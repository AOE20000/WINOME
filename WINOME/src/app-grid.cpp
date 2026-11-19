// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors

#include "app-grid.h"
#include "app-index.h"
#include "config.h"
#include "fonts.h"
#include "i18n.h"
#include "st-engine.h"

#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace winome {

namespace {

// Lightweight texture widget rendering cached app icons with GPU trilinear
// mipmap downscaling so 2x HiDPI icons never look pixelated or blurry.
struct WinomeAppIcon {
  GtkWidget parent_instance;
  GdkTexture *texture;
  GtkIconPaintable *fallback;
  int icon_size;
};

struct WinomeAppIconClass {
  GtkWidgetClass parent_class;
};

#define WINOME_APP_ICON(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), winome_app_icon_get_type (), WinomeAppIcon))

G_DEFINE_TYPE (WinomeAppIcon, winome_app_icon, GTK_TYPE_WIDGET)

void
winome_app_icon_measure (GtkWidget *widget, GtkOrientation orientation G_GNUC_UNUSED,
                         int for_size G_GNUC_UNUSED, int *minimum,
                         int *natural, int *minimum_baseline,
                         int *natural_baseline)
{
  WinomeAppIcon *self = WINOME_APP_ICON (widget);
  int sz = self->icon_size > 0 ? self->icon_size : 64;
  if (minimum != nullptr)
    *minimum = sz;
  if (natural != nullptr)
    *natural = sz;
  if (minimum_baseline != nullptr)
    *minimum_baseline = -1;
  if (natural_baseline != nullptr)
    *natural_baseline = -1;
}

void
winome_app_icon_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  WinomeAppIcon *self = WINOME_APP_ICON (widget);
  int w = gtk_widget_get_width (widget);
  int h = gtk_widget_get_height (widget);
  if (w <= 0 || h <= 0)
    return;

  if (self->texture != nullptr) {
    double tw = gdk_texture_get_width (self->texture);
    double th = gdk_texture_get_height (self->texture);
    if (tw > 0.0 && th > 0.0) {
      double s = std::min ((double)w / tw, (double)h / th);
      double dw = tw * s;
      double dh = th * s;
      graphene_rect_t r;
      graphene_rect_init (&r, (float)((w - dw) * 0.5), (float)((h - dh) * 0.5),
                          (float)dw, (float)dh);
      gtk_snapshot_append_scaled_texture (
          snapshot, self->texture, GSK_SCALING_FILTER_TRILINEAR, &r);
    }
    return;
  }

  if (self->fallback == nullptr) {
    GtkIconTheme *theme =
        gtk_icon_theme_get_for_display (gtk_widget_get_display (widget));
    if (theme != nullptr) {
      int scale = std::max (gtk_widget_get_scale_factor (widget), 1);
      int sz = std::max (std::min (w, h), 16);
      self->fallback = gtk_icon_theme_lookup_icon (
          theme, "application-x-executable-symbolic", nullptr, sz, scale,
          GTK_TEXT_DIR_NONE, GTK_ICON_LOOKUP_FORCE_SYMBOLIC);
    }
  }
  if (self->fallback != nullptr) {
    if (GTK_IS_SYMBOLIC_PAINTABLE (self->fallback)) {
      GdkRGBA fg{};
      gtk_widget_get_color (widget, &fg);
      gtk_symbolic_paintable_snapshot_symbolic (
          GTK_SYMBOLIC_PAINTABLE (self->fallback), snapshot, w, h, &fg, 1);
    } else {
      gdk_paintable_snapshot (GDK_PAINTABLE (self->fallback), snapshot, w, h);
    }
  }
}

void
winome_app_icon_dispose (GObject *object)
{
  WinomeAppIcon *self = WINOME_APP_ICON (object);
  g_clear_object (&self->texture);
  g_clear_object (&self->fallback);
  G_OBJECT_CLASS (winome_app_icon_parent_class)->dispose (object);
}

void
winome_app_icon_init (WinomeAppIcon *self)
{
  self->texture = nullptr;
  self->fallback = nullptr;
  self->icon_size = 64;
}

void
winome_app_icon_class_init (WinomeAppIconClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = winome_app_icon_dispose;
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  widget_class->measure = winome_app_icon_measure;
  widget_class->snapshot = winome_app_icon_snapshot;
}

GtkWidget *
winome_app_icon_new (int icon_size)
{
  auto *w = static_cast<WinomeAppIcon *> (
      g_object_new (winome_app_icon_get_type (), nullptr));
  w->icon_size = std::max (icon_size, 16);
  gtk_widget_set_size_request (GTK_WIDGET (w), w->icon_size, w->icon_size);
  return GTK_WIDGET (w);
}

void
winome_app_icon_set_texture (GtkWidget *widget, GdkTexture *texture)
{
  if (widget == nullptr || !GTK_IS_WIDGET (widget))
    return;
  WinomeAppIcon *self = WINOME_APP_ICON (widget);
  if (g_set_object (&self->texture, texture)) {
    g_clear_object (&self->fallback);
    if (gtk_widget_get_parent (widget) != nullptr &&
        gtk_widget_get_root (widget) != nullptr) {
      gtk_widget_queue_draw (widget);
    }
  }
}

struct PendingIconJob {
  AppEntry entry;
  int page = 0;
};

struct AppGridAsyncState {
  bool built_valid = false;
  std::string built_query;
  int built_avail_w = 0;
  int built_avail_h = 0;
  int built_scale = 0;
  int built_load_px = 0;
  uint64_t built_revision = 0;
  bool prewarm_active = false;
  uint64_t icon_gen = 0;
  int active_workers = 0;
  std::shared_ptr<std::atomic<bool>> alive =
      std::make_shared<std::atomic<bool>> (true);
  std::shared_ptr<std::atomic<uint64_t>> shared_gen =
      std::make_shared<std::atomic<uint64_t>> (0);
  std::vector<PendingIconJob> pending_jobs;
  std::unordered_map<std::string, std::vector<GtkWidget *>> icon_targets;

  ~AppGridAsyncState () {
    for (auto &kv : icon_targets) {
      for (GtkWidget *w : kv.second) {
        if (w != nullptr)
          g_object_unref (w);
      }
    }
    icon_targets.clear ();
  }
};

void
add_icon_target (AppGridAsyncState *st, const std::string &id,
                 GtkWidget *icon_w)
{
  if (st == nullptr || icon_w == nullptr)
    return;
  st->icon_targets[id].push_back (
      static_cast<GtkWidget *> (g_object_ref (icon_w)));
}

void
clear_icon_targets (AppGridAsyncState *st)
{
  if (st == nullptr)
    return;
  for (auto &kv : st->icon_targets) {
    for (GtkWidget *w : kv.second) {
      if (w != nullptr)
        g_object_unref (w);
    }
  }
  st->icon_targets.clear ();
}

void
prune_detached_icon_targets (AppGridAsyncState *st)
{
  if (st == nullptr)
    return;
  for (auto it = st->icon_targets.begin (); it != st->icon_targets.end ();) {
    auto &vec = it->second;
    size_t write_idx = 0;
    for (size_t i = 0; i < vec.size (); ++i) {
      GtkWidget *w = vec[i];
      if (w != nullptr && GTK_IS_WIDGET (w) &&
          gtk_widget_get_parent (w) != nullptr) {
        vec[write_idx++] = w;
      } else if (w != nullptr) {
        g_object_unref (w);
      }
    }
    vec.resize (write_idx);
    if (vec.empty ())
      it = st->icon_targets.erase (it);
    else
      ++it;
  }
}

void
apply_texture_to_targets (AppGridAsyncState *st, const std::string &id,
                          GdkTexture *tex)
{
  if (st == nullptr || tex == nullptr)
    return;
  auto it = st->icon_targets.find (id);
  if (it == st->icon_targets.end ())
    return;
  auto &vec = it->second;
  size_t write_idx = 0;
  for (size_t i = 0; i < vec.size (); ++i) {
    GtkWidget *w = vec[i];
    if (w != nullptr && GTK_IS_WIDGET (w) &&
        gtk_widget_get_parent (w) != nullptr) {
      winome_app_icon_set_texture (w, tex);
      vec[write_idx++] = w;
    } else if (w != nullptr) {
      g_object_unref (w);
    }
  }
  vec.resize (write_idx);
  if (vec.empty ())
    st->icon_targets.erase (it);
}

struct WinomeAppGrid {
  GtkBox parent_instance;

  GtkWidget *root_overlay = nullptr;
  GtkWidget *content_box = nullptr;
  GtkWidget *body_row = nullptr;
  GtkWidget *prev_btn = nullptr;
  GtkWidget *next_btn = nullptr;
  GtkWidget *center_box = nullptr;
  GtkWidget *pages_stack = nullptr;
  GtkWidget *empty_label = nullptr;
  GtkWidget *indicators_box = nullptr;
  GtkWidget *active_menu = nullptr;
  GtkWidget *menu_tile = nullptr;
  GtkWidget *folder_backdrop = nullptr;
  GtkWidget *folder_dialog_grid = nullptr;

  std::string query_lower;
  std::vector<AppEntry> all_apps;
  std::vector<StartMenuPinnedItem> pinned_layout;
  std::vector<AppGridItem> filtered;
  std::vector<RunningWindow> running_windows;
  std::vector<GtkWidget *> page_dots;
  AppGridAsyncState *async = nullptr;
  int page = 0;
  int total_pages = 1;
  int avail_w = 1000;
  int avail_h = 560;
  bool index_loaded = false;
  bool can_scroll = true;
  guint scroll_timer = 0;
  guint refresh_timer = 0;

  void (*on_launch_done) (void *) = nullptr;
  void (*on_pins_changed) (void *) = nullptr;
  void *user_data = nullptr;
};

struct WinomeAppGridClass {
  GtkBoxClass parent_class;
};

#define WINOME_APP_GRID(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), winome_app_grid_get_type (), WinomeAppGrid))

G_DEFINE_TYPE (WinomeAppGrid, winome_app_grid, GTK_TYPE_BOX)

struct TileActionCtx {
  WinomeAppGrid *self = nullptr;
  AppEntry entry;
};

struct FolderTileCtx {
  WinomeAppGrid *self = nullptr;
  AppGridItem item;
};

void rebuild_all_pages (WinomeAppGrid *self);
void update_running_dots_in_place (WinomeAppGrid *self);
bool close_folder_dialog (WinomeAppGrid *self);

void
clear_box_children (GtkWidget *box)
{
  if (box == nullptr)
    return;
  while (GtkWidget *child = gtk_widget_get_first_child (box))
    gtk_box_remove (GTK_BOX (box), child);
}

void
clear_stack_children (GtkWidget *stack)
{
  if (stack == nullptr)
    return;
  while (GtkWidget *child = gtk_widget_get_first_child (stack))
    gtk_stack_remove (GTK_STACK (stack), child);
}

void
clear_menu_tile_highlight (WinomeAppGrid *self)
{
  if (self == nullptr || self->menu_tile == nullptr)
    return;
  GtkWidget *tile = self->menu_tile;
  g_object_remove_weak_pointer (G_OBJECT (tile),
                                reinterpret_cast<gpointer *> (&self->menu_tile));
  self->menu_tile = nullptr;
  if (GTK_IS_WIDGET (tile))
    gtk_widget_remove_css_class (tile, "checked");
}

WinomeAppGrid *
close_tile_menu_extract (gpointer user_data, AppEntry *out_entry)
{
  auto *ctx = static_cast<TileActionCtx *> (user_data);
  WinomeAppGrid *self = ctx->self;
  if (out_entry != nullptr)
    *out_entry = ctx->entry;
  app_grid_close_menu (GTK_WIDGET (self));
  return self;
}

bool
activate_or_launch_entry (const AppEntry &entry, bool force_new_window)
{
  if (!force_new_window) {
    std::vector<RunningWindow> wins =
        app_index_running_windows_for_entry (entry);
    for (const RunningWindow &w : wins) {
      if (app_index_activate_hwnd (w.hwnd))
        return true;
    }
  }
  return app_index_launch (entry, false);
}

void
on_tile_action_launch (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  AppEntry entry;
  WinomeAppGrid *self = close_tile_menu_extract (user_data, &entry);
  if (app_index_launch (entry, false) && self->on_launch_done != nullptr)
    self->on_launch_done (self->user_data);
}

void
on_tile_action_admin (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  AppEntry entry;
  WinomeAppGrid *self = close_tile_menu_extract (user_data, &entry);
  if (app_index_launch (entry, true) && self->on_launch_done != nullptr)
    self->on_launch_done (self->user_data);
}

void
on_tile_action_pin (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  AppEntry entry;
  WinomeAppGrid *self = close_tile_menu_extract (user_data, &entry);
  toggle_app_id_pinned (entry.id);
  if (self->on_pins_changed != nullptr)
    self->on_pins_changed (self->user_data);
}

void
on_tile_action_location (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  AppEntry entry;
  WinomeAppGrid *self = close_tile_menu_extract (user_data, &entry);
  app_index_open_location (entry);
  if (self->on_launch_done != nullptr)
    self->on_launch_done (self->user_data);
}

struct WindowMenuCtx {
  WinomeAppGrid *self = nullptr;
  HWND hwnd = nullptr;
};

void
on_tile_action_window (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  auto *wctx = static_cast<WindowMenuCtx *> (user_data);
  WinomeAppGrid *self = wctx->self;
  HWND hwnd = wctx->hwnd;
  app_grid_close_menu (GTK_WIDGET (self));
  if (app_index_activate_hwnd (hwnd) && self->on_launch_done != nullptr)
    self->on_launch_done (self->user_data);
}

struct QuitMenuCtx {
  WinomeAppGrid *self = nullptr;
  std::vector<HWND> hwnds;
};

gboolean
on_quit_refresh_cb (gpointer user_data)
{
  WinomeAppGrid *self = WINOME_APP_GRID (user_data);
  self->refresh_timer = 0;
  app_grid_refresh_running (GTK_WIDGET (self));
  if (self->on_pins_changed != nullptr)
    self->on_pins_changed (self->user_data);
  return G_SOURCE_REMOVE;
}

void
on_tile_action_quit (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  auto *qctx = static_cast<QuitMenuCtx *> (user_data);
  WinomeAppGrid *self = qctx->self;
  std::vector<HWND> hwnds = qctx->hwnds;
  app_grid_close_menu (GTK_WIDGET (self));
  app_index_close_windows (hwnds);
  if (self->refresh_timer != 0) {
    g_source_remove (self->refresh_timer);
    self->refresh_timer = 0;
  }
  self->refresh_timer = g_timeout_add (180, on_quit_refresh_cb, self);
}

GtkWidget *
make_menu_button (const char *text, GCallback cb, gpointer data,
                  bool ellipsize = false)
{
  GtkWidget *btn = gtk_button_new_with_label (text);
  gtk_widget_set_name (btn, "winome-app-ctx-item");
  gtk_widget_add_css_class (btn, "popup-menu-item");
  gtk_widget_set_focus_on_click (btn, FALSE);
  GtkWidget *lbl = gtk_button_get_child (GTK_BUTTON (btn));
  if (GTK_IS_LABEL (lbl)) {
    gtk_label_set_xalign (GTK_LABEL (lbl), 0.0f);
    if (ellipsize) {
      gtk_label_set_max_width_chars (GTK_LABEL (lbl), 30);
      gtk_label_set_ellipsize (GTK_LABEL (lbl), PANGO_ELLIPSIZE_END);
    }
  }
  g_signal_connect (btn, "clicked", cb, data);
  return btn;
}

GtkWidget *
make_menu_separator (void)
{
  GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class (sep, "popup-separator-menu-item");
  return sep;
}

void
show_tile_context_menu (GtkWidget *tile, WinomeAppGrid *self,
                        const AppEntry &entry)
{
  app_grid_close_menu (GTK_WIDGET (self));

  GtkWidget *popover = gtk_popover_new ();
  install_bundled_fonts (popover);
  gtk_widget_add_css_class (popover, "winome-context-popover");
  gtk_widget_set_parent (popover, tile);
  gtk_popover_set_has_arrow (GTK_POPOVER (popover), TRUE);
  gtk_popover_set_position (GTK_POPOVER (popover), GTK_POS_BOTTOM);
  gtk_popover_set_autohide (GTK_POPOVER (popover), TRUE);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);

  auto *ctx = new TileActionCtx{self, entry};
  g_object_set_data_full (
      G_OBJECT (popover), "winome-tile-ctx", ctx,
      [] (gpointer p) { delete static_cast<TileActionCtx *> (p); });

  std::vector<RunningWindow> running =
      app_index_running_windows_for_entry (entry);

  // Section 1 (upstream AppMenu): open window list when running.
  if (!running.empty ()) {
    GtkWidget *hdr = gtk_label_new (tr ("Open Windows", "打开的窗口"));
    gtk_label_set_xalign (GTK_LABEL (hdr), 0.0f);
    gtk_widget_add_css_class (hdr, "popup-menu-item");
    gtk_widget_add_css_class (hdr, "popup-inactive-menu-item");
    gtk_widget_set_sensitive (hdr, FALSE);
    gtk_box_append (GTK_BOX (box), hdr);

    int shown = 0;
    for (const RunningWindow &rw : running) {
      if (shown >= 8)
        break;
      const char *title =
          !rw.title.empty () ? rw.title.c_str () : entry.name.c_str ();
      auto *wctx = new WindowMenuCtx{self, rw.hwnd};
      GtkWidget *wbtn =
          make_menu_button (title, G_CALLBACK (on_tile_action_window), wctx,
                            true);
      g_object_set_data_full (
          G_OBJECT (wbtn), "winome-win-ctx", wctx,
          [] (gpointer p) { delete static_cast<WindowMenuCtx *> (p); });
      gtk_box_append (GTK_BOX (box), wbtn);
      ++shown;
    }
    gtk_box_append (GTK_BOX (box), make_menu_separator ());
  }

  // Section 2: Open / New Window + Run as Administrator (for classic apps).
  bool is_running = !running.empty ();
  gtk_box_append (
      GTK_BOX (box),
      make_menu_button (is_running ? tr ("New Window", "新建窗口")
                                   : tr ("Open", "打开"),
                        G_CALLBACK (on_tile_action_launch), ctx));

  bool has_disk_path =
      !entry.is_uwp &&
      (!entry.target_exe.empty () ||
       (!entry.launch_path.empty () &&
        entry.launch_path.rfind (L"shell:", 0) != 0));
  if (has_disk_path) {
    gtk_box_append (
        GTK_BOX (box),
        make_menu_button (tr ("Run as Administrator", "以管理员身份运行"),
                          G_CALLBACK (on_tile_action_admin), ctx));
  }

  gtk_box_append (GTK_BOX (box), make_menu_separator ());

  // Section 3: Pin to Dash / Unpin & Open File Location.
  WinomeConfig cfg = get_config ();
  bool pinned = is_app_id_pinned (cfg.pinned_apps, entry.id);
  gtk_box_append (
      GTK_BOX (box),
      make_menu_button (pinned ? tr ("Unpin from Dash", "从程序坞取消固定")
                               : tr ("Pin to Dash", "固定到程序坞"),
                        G_CALLBACK (on_tile_action_pin), ctx));

  if (has_disk_path) {
    gtk_box_append (
        GTK_BOX (box),
        make_menu_button (tr ("Open File Location", "打开文件所在位置"),
                          G_CALLBACK (on_tile_action_location), ctx));
  }

  // Section 4: Quit (close all windows) when running.
  if (is_running) {
    gtk_box_append (GTK_BOX (box), make_menu_separator ());
    std::vector<HWND> hwnds;
    hwnds.reserve (running.size ());
    for (const RunningWindow &rw : running)
      hwnds.push_back (rw.hwnd);
    auto *qctx = new QuitMenuCtx{self, std::move (hwnds)};
    GtkWidget *qbtn = make_menu_button (tr ("Quit", "退出"),
                                        G_CALLBACK (on_tile_action_quit), qctx);
    g_object_set_data_full (
        G_OBJECT (qbtn), "winome-quit-ctx", qctx,
        [] (gpointer p) { delete static_cast<QuitMenuCtx *> (p); });
    gtk_box_append (GTK_BOX (box), qbtn);
  }

  gtk_popover_set_child (GTK_POPOVER (popover), box);
  g_signal_connect (
      popover, "closed", G_CALLBACK (+[] (GtkPopover *p, gpointer d) {
        auto *g = WINOME_APP_GRID (d);
        if (g->active_menu == GTK_WIDGET (p))
          g->active_menu = nullptr;
        clear_menu_tile_highlight (g);
        if (gtk_widget_get_parent (GTK_WIDGET (p)) != nullptr)
          gtk_widget_unparent (GTK_WIDGET (p));
      }),
      self);

  self->menu_tile = tile;
  gtk_widget_add_css_class (tile, "checked");
  g_object_add_weak_pointer (G_OBJECT (tile),
                             reinterpret_cast<gpointer *> (&self->menu_tile));

  self->active_menu = popover;
  gtk_popover_popup (GTK_POPOVER (popover));
}

void
on_tile_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  auto *ctx = static_cast<TileActionCtx *> (user_data);
  bool ctrl = (GetKeyState (VK_CONTROL) & 0x8000) != 0;
  if (activate_or_launch_entry (ctx->entry, ctrl)) {
    close_folder_dialog (ctx->self);
    if (ctx->self->on_launch_done != nullptr)
      ctx->self->on_launch_done (ctx->self->user_data);
  }
}

void
on_tile_middle_click (GtkGestureClick *gesture G_GNUC_UNUSED,
                      int n_press G_GNUC_UNUSED, double x G_GNUC_UNUSED,
                      double y G_GNUC_UNUSED, gpointer user_data)
{
  auto *ctx = static_cast<TileActionCtx *> (user_data);
  if (app_index_launch (ctx->entry, false)) {
    close_folder_dialog (ctx->self);
    if (ctx->self->on_launch_done != nullptr)
      ctx->self->on_launch_done (ctx->self->user_data);
  }
}

void
on_tile_right_click (GtkGestureClick *gesture, int n_press G_GNUC_UNUSED,
                     double x G_GNUC_UNUSED, double y G_GNUC_UNUSED,
                     gpointer user_data)
{
  auto *ctx = static_cast<TileActionCtx *> (user_data);
  GtkWidget *tile =
      gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
  show_tile_context_menu (tile, ctx->self, ctx->entry);
}

gboolean
on_tile_key_pressed (GtkEventControllerKey *ctrl, guint keyval,
                     guint keycode G_GNUC_UNUSED, GdkModifierType state,
                     gpointer user_data)
{
  bool is_menu_key =
      keyval == GDK_KEY_Menu ||
      (keyval == GDK_KEY_F10 && (state & GDK_SHIFT_MASK) != 0);
  if (!is_menu_key)
    return GDK_EVENT_PROPAGATE;
  auto *ctx = static_cast<TileActionCtx *> (user_data);
  GtkWidget *tile =
      gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (ctrl));
  show_tile_context_menu (tile, ctx->self, ctx->entry);
  return GDK_EVENT_STOP;
}

void
on_prev_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  app_grid_prev_page (GTK_WIDGET (user_data));
}

void
on_next_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  app_grid_next_page (GTK_WIDGET (user_data));
}

struct PageDotCtx {
  WinomeAppGrid *self = nullptr;
  int page = 0;
};

void
update_page_controls (WinomeAppGrid *self)
{
  int total = std::max (self->total_pages, 1);
  bool multi_page = total > 1;
  bool has_prev = self->page > 0;
  bool has_next = self->page + 1 < total;

  gtk_widget_set_visible (self->prev_btn, multi_page);
  gtk_widget_set_visible (self->next_btn, multi_page);
  gtk_widget_set_sensitive (self->prev_btn, has_prev);
  gtk_widget_set_can_target (self->prev_btn, has_prev);
  gtk_widget_set_opacity (self->prev_btn, has_prev ? 1.0 : 0.0);
  gtk_widget_set_sensitive (self->next_btn, has_next);
  gtk_widget_set_can_target (self->next_btn, has_next);
  gtk_widget_set_opacity (self->next_btn, has_next ? 1.0 : 0.0);
  gtk_widget_set_visible (self->indicators_box, multi_page);

  for (int p = 0; p < (int)self->page_dots.size (); ++p) {
    GtkWidget *dot_btn = self->page_dots[p];
    if (dot_btn == nullptr)
      continue;
    if (p == self->page)
      gtk_widget_add_css_class (dot_btn, "active");
    else
      gtk_widget_remove_css_class (dot_btn, "active");
  }
}

void
switch_to_page (WinomeAppGrid *self, int target_page)
{
  if (self == nullptr)
    return;
  int clamped = std::clamp (target_page, 0, std::max (self->total_pages - 1, 0));
  if (clamped == self->page)
    return;
  app_grid_close_menu (GTK_WIDGET (self));
  self->page = clamped;
  if (self->pages_stack != nullptr) {
    std::string page_name = "page-" + std::to_string (self->page);
    gtk_stack_set_visible_child_full (
        GTK_STACK (self->pages_stack), page_name.c_str (),
        GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
  }
  update_page_controls (self);
}

void
on_page_dot_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  auto *ctx = static_cast<PageDotCtx *> (user_data);
  switch_to_page (ctx->self, ctx->page);
}

gboolean
on_scroll_cooldown_cb (gpointer user_data)
{
  WinomeAppGrid *self = WINOME_APP_GRID (user_data);
  self->scroll_timer = 0;
  self->can_scroll = true;
  return G_SOURCE_REMOVE;
}

gboolean
on_grid_scroll (GtkEventControllerScroll *ctrl G_GNUC_UNUSED, double dx,
                double dy, gpointer user_data)
{
  WinomeAppGrid *self = WINOME_APP_GRID (user_data);
  double delta = (std::abs (dy) >= std::abs (dx)) ? dy : dx;
  if (std::abs (delta) <= 0.1)
    return FALSE;
  if (!self->can_scroll)
    return TRUE;

  if (delta > 0.1)
    app_grid_next_page (GTK_WIDGET (self));
  else if (delta < -0.1)
    app_grid_prev_page (GTK_WIDGET (self));

  // SCROLL_TIMEOUT_TIME = 150ms (upstream appDisplay.js).
  self->can_scroll = false;
  if (self->scroll_timer != 0) {
    g_source_remove (self->scroll_timer);
    self->scroll_timer = 0;
  }
  self->scroll_timer = g_timeout_add (150, on_scroll_cooldown_cb, self);
  return TRUE;
}

int
lookup_icon_label_spacing (void)
{
  if (StEngine *e = StEngine::get ()) {
    StEngine::Node tile (*e, nullptr, "", "overview-tile", "");
    StEngine::Node icon (*e, &tile, "", "overview-icon",
                         "overview-icon-with-label");
    StEngine::Node box (*e, &icon, "StBoxLayout", "", "");
    double v = 0.0;
    if (box.lookup_length ("spacing", &v) && v >= 0.0)
      return (int)(v + 0.5);
  }
  return 6;
}

void maybe_start_icon_worker (WinomeAppGrid *self);

struct IconBatchPayload {
  WinomeAppGrid *self = nullptr;
  std::shared_ptr<std::atomic<bool>> alive;
  uint64_t gen = 0;
  int size = 0;
  bool worker_done = false;
  std::vector<RawDecodedIcon> items;
};

gboolean
on_icon_batch_idle (gpointer user_data)
{
  auto *payload = static_cast<IconBatchPayload *> (user_data);
  bool alive =
      payload->alive && payload->alive->load (std::memory_order_acquire);
  if (alive && payload->self != nullptr && payload->self->async != nullptr) {
    AppGridAsyncState *st = payload->self->async;
    bool matching_size = (payload->size == st->built_load_px);
    for (RawDecodedIcon &raw : payload->items) {
      if (raw.bytes == nullptr)
        continue;
      GdkTexture *tex = app_icon_texture_from_raw (raw);
      if (tex != nullptr) {
        app_icon_cache_store (raw.id, raw.size, tex);
        if (matching_size)
          apply_texture_to_targets (st, raw.id, tex);
        g_object_unref (tex);
      }
    }
    if (payload->worker_done) {
      if (st->active_workers > 0)
        --st->active_workers;
      if (!st->pending_jobs.empty ())
        maybe_start_icon_worker (payload->self);
    }
  }
  for (RawDecodedIcon &raw : payload->items)
    app_icon_raw_clear (raw);
  delete payload;
  return G_SOURCE_REMOVE;
}

void
maybe_start_icon_worker (WinomeAppGrid *self)
{
  if (self == nullptr || self->async == nullptr)
    return;
  AppGridAsyncState *st = self->async;
  if (st->pending_jobs.empty ())
    return;

  // Apply any icons that entered the cache (e.g. from Dash or prewarm) first.
  std::vector<PendingIconJob> remaining;
  remaining.reserve (st->pending_jobs.size ());
  int load_px = st->built_load_px;
  for (const PendingIconJob &job : st->pending_jobs) {
    if (GdkTexture *tex = app_icon_cache_lookup (job.entry.id, load_px)) {
      apply_texture_to_targets (st, job.entry.id, tex);
      g_object_unref (tex);
    } else {
      remaining.push_back (job);
    }
  }
  st->pending_jobs = std::move (remaining);
  if (st->pending_jobs.empty ())
    return;

  // Never spawn detached worker threads inside unmapped headless unit tests.
  if (!gtk_widget_get_realized (GTK_WIDGET (self)) && !st->prewarm_active)
    return;
  if (st->active_workers > 0)
    return;

  int active_page = self->page;
  std::vector<PendingIconJob> jobs;
  jobs.reserve (st->pending_jobs.size ());
  std::unordered_set<std::string> seen;
  for (const PendingIconJob &job : st->pending_jobs) {
    if (seen.insert (job.entry.id).second)
      jobs.push_back (job);
  }
  st->pending_jobs.clear ();
  std::stable_sort (
      jobs.begin (), jobs.end (),
      [active_page] (const PendingIconJob &a, const PendingIconJob &b) {
        int da = std::abs (a.page - active_page);
        int db = std::abs (b.page - active_page);
        if ((da == 0) != (db == 0))
          return da == 0;
        if (da != db)
          return da < db;
        return a.page < b.page;
      });

  ++st->active_workers;
  uint64_t gen = st->icon_gen;
  std::shared_ptr<std::atomic<bool>> alive = st->alive;
  std::shared_ptr<std::atomic<uint64_t>> shared_gen = st->shared_gen;
  std::thread ([self, jobs = std::move (jobs), gen, load_px, alive,
                shared_gen] () {
    HRESULT hr = CoInitializeEx (
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool did_co = SUCCEEDED (hr);

    constexpr size_t kBatchSize = 8;
    std::vector<RawDecodedIcon> batch;
    batch.reserve (kBatchSize);

    auto flush_batch = [&] (bool done) {
      if (batch.empty () && !done)
        return;
      auto *payload = new IconBatchPayload ();
      payload->self = self;
      payload->alive = alive;
      payload->gen = gen;
      payload->size = load_px;
      payload->worker_done = done;
      payload->items = std::move (batch);
      batch.clear ();
      g_idle_add (on_icon_batch_idle, payload);
    };

    for (const PendingIconJob &job : jobs) {
      if (!alive->load (std::memory_order_acquire) ||
          shared_gen->load (std::memory_order_acquire) != gen) {
        break;
      }
      RawDecodedIcon raw = app_icon_decode_entry_raw (job.entry, load_px);
      if (raw.bytes != nullptr) {
        batch.push_back (std::move (raw));
        if (batch.size () >= kBatchSize)
          flush_batch (false);
      } else {
        app_icon_raw_clear (raw);
      }
    }
    flush_batch (true);

    if (did_co)
      CoUninitialize ();
  }).detach ();
}

void
on_grid_realize (GtkWidget *widget, gpointer user_data G_GNUC_UNUSED)
{
  maybe_start_icon_worker (WINOME_APP_GRID (widget));
}

GtkWidget *
build_app_tile (WinomeAppGrid *self, const AppEntry &entry, int icon_size,
                int load_px, int page_idx, int tile_w, int tile_h,
                int label_spacing, bool is_running)
{
  GtkWidget *btn = gtk_button_new ();
  gtk_widget_set_name (btn, "winome-app-tile");
  gtk_widget_add_css_class (btn, "overview-tile");
  gtk_widget_set_size_request (btn, tile_w, tile_h);
  gtk_widget_set_halign (btn, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (btn, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (btn, entry.name.c_str ());

  GtkWidget *overlay = gtk_overlay_new ();

  GtkWidget *content =
      gtk_box_new (GTK_ORIENTATION_VERTICAL, label_spacing);
  gtk_widget_add_css_class (content, "overview-icon");
  gtk_widget_add_css_class (content, "overview-icon-with-label");
  gtk_widget_set_halign (content, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (content, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_bottom (content, 6);

  GtkWidget *icon_w = winome_app_icon_new (icon_size);
  if (GdkTexture *tex = app_icon_cache_lookup (entry.id, load_px)) {
    winome_app_icon_set_texture (icon_w, tex);
    g_object_unref (tex);
  } else if (self->async != nullptr) {
    self->async->pending_jobs.push_back (PendingIconJob{entry, page_idx});
    add_icon_target (self->async, entry.id, icon_w);
  }
  gtk_widget_set_halign (icon_w, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (content), icon_w);

  GtkWidget *lbl = gtk_label_new (entry.name.c_str ());
  gtk_label_set_lines (GTK_LABEL (lbl), 2);
  gtk_label_set_wrap (GTK_LABEL (lbl), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (lbl), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_ellipsize (GTK_LABEL (lbl), PANGO_ELLIPSIZE_END);
  gtk_label_set_justify (GTK_LABEL (lbl), GTK_JUSTIFY_CENTER);
  gtk_label_set_max_width_chars (GTK_LABEL (lbl), 14);
  gtk_widget_set_halign (lbl, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (content), lbl);

  gtk_overlay_set_child (GTK_OVERLAY (overlay), content);

  GtkWidget *dot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (dot, "app-grid-running-dot");
  gtk_widget_set_halign (dot, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (dot, GTK_ALIGN_END);
  gtk_widget_set_visible (dot, is_running);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), dot);

  gtk_button_set_child (GTK_BUTTON (btn), overlay);

  auto *ctx = new TileActionCtx{self, entry};
  g_object_set_data_full (
      G_OBJECT (btn), "winome-tile-action", ctx,
      [] (gpointer p) { delete static_cast<TileActionCtx *> (p); });
  g_object_set_data (G_OBJECT (btn), "winome-running-dot", dot);
  g_signal_connect (btn, "clicked", G_CALLBACK (on_tile_clicked), ctx);

  GtkGesture *mclick = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (mclick),
                                 GDK_BUTTON_MIDDLE);
  g_signal_connect (mclick, "pressed", G_CALLBACK (on_tile_middle_click), ctx);
  gtk_widget_add_controller (btn, GTK_EVENT_CONTROLLER (mclick));

  GtkGesture *rclick = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (rclick),
                                 GDK_BUTTON_SECONDARY);
  g_signal_connect (rclick, "pressed", G_CALLBACK (on_tile_right_click), ctx);
  gtk_widget_add_controller (btn, GTK_EVENT_CONTROLLER (rclick));

  GtkEventController *keys = gtk_event_controller_key_new ();
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_tile_key_pressed), ctx);
  gtk_widget_add_controller (btn, keys);

  return btn;
}

bool
close_folder_dialog (WinomeAppGrid *self)
{
  if (self == nullptr || self->folder_backdrop == nullptr)
    return false;
  app_grid_close_menu (GTK_WIDGET (self));
  if (self->root_overlay != nullptr)
    gtk_overlay_remove_overlay (GTK_OVERLAY (self->root_overlay),
                                self->folder_backdrop);
  self->folder_backdrop = nullptr;
  self->folder_dialog_grid = nullptr;
  if (self->async != nullptr)
    prune_detached_icon_targets (self->async);
  return true;
}

void
on_folder_backdrop_pressed (GtkGestureClick *gesture G_GNUC_UNUSED,
                            int n_press G_GNUC_UNUSED, double x, double y,
                            gpointer user_data)
{
  auto *self = WINOME_APP_GRID (user_data);
  if (self->folder_backdrop == nullptr)
    return;
  GtkWidget *dialog = GTK_WIDGET (
      g_object_get_data (G_OBJECT (self->folder_backdrop),
                         "winome-folder-card"));
  if (dialog != nullptr) {
    graphene_rect_t bounds{};
    if (gtk_widget_compute_bounds (dialog, self->folder_backdrop, &bounds) &&
        x >= bounds.origin.x &&
        x <= bounds.origin.x + bounds.size.width &&
        y >= bounds.origin.y &&
        y <= bounds.origin.y + bounds.size.height) {
      return;
    }
  }
  close_folder_dialog (self);
}

void
open_folder_dialog (WinomeAppGrid *self, const AppGridItem &folder)
{
  if (self == nullptr || self->root_overlay == nullptr)
    return;
  close_folder_dialog (self);

  GtkWidget *backdrop = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name (backdrop, "winome-folder-scrim");
  gtk_widget_set_hexpand (backdrop, TRUE);
  gtk_widget_set_vexpand (backdrop, TRUE);
  gtk_widget_set_halign (backdrop, GTK_ALIGN_FILL);
  gtk_widget_set_valign (backdrop, GTK_ALIGN_FILL);

  GtkGesture *outside_click = gtk_gesture_click_new ();
  g_signal_connect (outside_click, "pressed",
                    G_CALLBACK (on_folder_backdrop_pressed), self);
  gtk_widget_add_controller (backdrop, GTK_EVENT_CONTROLLER (outside_click));

  int dlg_w = std::clamp (self->avail_w - 80, 360, 680);
  int dlg_h = std::clamp (self->avail_h - 12, 260, 560);

  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class (card, "app-folder-dialog");
  gtk_widget_set_size_request (card, dlg_w, dlg_h);
  gtk_widget_set_halign (card, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (card, GTK_ALIGN_CENTER);
  g_object_set_data (G_OBJECT (backdrop), "winome-folder-card", card);

  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class (header, "folder-name-container");

  GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request (spacer, 32, 32);
  gtk_box_append (GTK_BOX (header), spacer);

  GtkWidget *title_lbl = gtk_label_new (folder.title.c_str ());
  gtk_widget_add_css_class (title_lbl, "folder-name-label");
  gtk_widget_set_hexpand (title_lbl, TRUE);
  gtk_widget_set_halign (title_lbl, GTK_ALIGN_CENTER);
  gtk_label_set_ellipsize (GTK_LABEL (title_lbl), PANGO_ELLIPSIZE_END);
  PangoAttrList *attrs = pango_attr_list_new ();
  pango_attr_list_insert (attrs, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
  pango_attr_list_insert (attrs, pango_attr_scale_new (1.35));
  gtk_label_set_attributes (GTK_LABEL (title_lbl), attrs);
  pango_attr_list_unref (attrs);
  gtk_box_append (GTK_BOX (header), title_lbl);

  GtkWidget *close_btn = gtk_button_new_from_icon_name ("window-close-symbolic");
  gtk_widget_add_css_class (close_btn, "icon-button");
  gtk_widget_set_focus_on_click (close_btn, FALSE);
  g_signal_connect (
      close_btn, "clicked",
      G_CALLBACK (+[] (GtkButton *b G_GNUC_UNUSED, gpointer d) {
        close_folder_dialog (WINOME_APP_GRID (d));
      }),
      self);
  gtk_box_append (GTK_BOX (header), close_btn);
  gtk_box_append (GTK_BOX (card), header);

  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_widget_set_hexpand (scroll, TRUE);
  gtk_widget_set_margin_start (scroll, 24);
  gtk_widget_set_margin_end (scroll, 24);
  gtk_widget_set_margin_bottom (scroll, 24);

  GtkWidget *grid = gtk_grid_new ();
  gtk_widget_add_css_class (grid, "icon-grid");
  gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
  gtk_grid_set_row_spacing (GTK_GRID (grid), 12);
  gtk_grid_set_column_homogeneous (GTK_GRID (grid), TRUE);
  gtk_widget_set_halign (grid, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (grid, GTK_ALIGN_START);

  int scale = std::max (gtk_widget_get_scale_factor (GTK_WIDGET (self)), 1);
  int icon_size = std::clamp ((dlg_w - 140) / 5, 48, 64);
  int load_px =
      (self->async != nullptr && self->async->built_load_px > 0)
          ? self->async->built_load_px
          : std::clamp (icon_size * std::max (scale, 2), 96, 256);
  int tile_w = std::clamp ((dlg_w - 96) / 4, 108, 148);
  int tile_h = icon_size + 68;
  int label_spacing = lookup_icon_label_spacing ();
  int cols = std::max (2, (dlg_w - 64) / (tile_w + 12));

  self->running_windows = app_index_enumerate_running_windows ();
  int idx = 0;
  for (const AppEntry &child : folder.children) {
    int r = idx / cols;
    int c = idx % cols;
    bool running =
        !match_entry_running_windows (child, self->running_windows).empty ();
    GtkWidget *tile = build_app_tile (self, child, icon_size, load_px,
                                      self->page, tile_w, tile_h, label_spacing,
                                      running);
    gtk_grid_attach (GTK_GRID (grid), tile, c, r, 1, 1);
    ++idx;
  }

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), grid);
  gtk_box_append (GTK_BOX (card), scroll);
  gtk_box_append (GTK_BOX (backdrop), card);

  self->folder_backdrop = backdrop;
  self->folder_dialog_grid = grid;
  gtk_overlay_add_overlay (GTK_OVERLAY (self->root_overlay), backdrop);
  maybe_start_icon_worker (self);
}

void
on_folder_tile_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  auto *fctx = static_cast<FolderTileCtx *> (user_data);
  open_folder_dialog (fctx->self, fctx->item);
}

GtkWidget *
build_folder_tile (WinomeAppGrid *self, const AppGridItem &folder,
                   int icon_size, int load_px, int page_idx, int tile_w,
                   int tile_h, int label_spacing, bool any_running)
{
  GtkWidget *btn = gtk_button_new ();
  gtk_widget_set_name (btn, "winome-app-folder-tile");
  gtk_widget_add_css_class (btn, "overview-tile");
  gtk_widget_add_css_class (btn, "app-folder");
  gtk_widget_set_size_request (btn, tile_w, tile_h);
  gtk_widget_set_halign (btn, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (btn, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (btn, folder.title.c_str ());

  GtkWidget *overlay = gtk_overlay_new ();

  GtkWidget *content =
      gtk_box_new (GTK_ORIENTATION_VERTICAL, label_spacing);
  gtk_widget_add_css_class (content, "overview-icon");
  gtk_widget_add_css_class (content, "overview-icon-with-label");
  gtk_widget_set_halign (content, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (content, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_bottom (content, 6);

  GtkWidget *preview_frame = gtk_grid_new ();
  gtk_grid_set_column_spacing (GTK_GRID (preview_frame), 4);
  gtk_grid_set_row_spacing (GTK_GRID (preview_frame), 4);
  gtk_grid_set_column_homogeneous (GTK_GRID (preview_frame), TRUE);
  gtk_grid_set_row_homogeneous (GTK_GRID (preview_frame), TRUE);
  gtk_widget_set_size_request (preview_frame, icon_size, icon_size);
  gtk_widget_set_halign (preview_frame, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (preview_frame, GTK_ALIGN_CENTER);

  int mini_px = std::max ((icon_size - 8) / 2, 16);
  size_t limit = std::min<size_t> (folder.children.size (), 4);
  for (size_t i = 0; i < limit; ++i) {
    const AppEntry &child = folder.children[i];
    GtkWidget *mini = winome_app_icon_new (mini_px);
    if (GdkTexture *tex = app_icon_cache_lookup (child.id, load_px)) {
      winome_app_icon_set_texture (mini, tex);
      g_object_unref (tex);
    } else if (self->async != nullptr) {
      self->async->pending_jobs.push_back (PendingIconJob{child, page_idx});
      add_icon_target (self->async, child.id, mini);
    }
    gtk_widget_set_halign (mini, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (mini, GTK_ALIGN_CENTER);
    gtk_grid_attach (GTK_GRID (preview_frame), mini,
                     static_cast<int> (i % 2), static_cast<int> (i / 2), 1, 1);
  }
  gtk_box_append (GTK_BOX (content), preview_frame);

  GtkWidget *lbl = gtk_label_new (folder.title.c_str ());
  gtk_label_set_lines (GTK_LABEL (lbl), 2);
  gtk_label_set_wrap (GTK_LABEL (lbl), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (lbl), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_ellipsize (GTK_LABEL (lbl), PANGO_ELLIPSIZE_END);
  gtk_label_set_justify (GTK_LABEL (lbl), GTK_JUSTIFY_CENTER);
  gtk_label_set_max_width_chars (GTK_LABEL (lbl), 14);
  gtk_widget_set_halign (lbl, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (content), lbl);

  gtk_overlay_set_child (GTK_OVERLAY (overlay), content);

  GtkWidget *dot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (dot, "app-grid-running-dot");
  gtk_widget_set_halign (dot, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (dot, GTK_ALIGN_END);
  gtk_widget_set_visible (dot, any_running);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), dot);

  gtk_button_set_child (GTK_BUTTON (btn), overlay);

  auto *fctx = new FolderTileCtx{self, folder};
  g_object_set_data_full (
      G_OBJECT (btn), "winome-folder-action", fctx,
      [] (gpointer p) { delete static_cast<FolderTileCtx *> (p); });
  g_object_set_data (G_OBJECT (btn), "winome-running-dot", dot);
  g_signal_connect (btn, "clicked", G_CALLBACK (on_folder_tile_clicked), fctx);

  return btn;
}

void
update_tile_dots_in_grid (GtkWidget *grid,
                          const std::vector<RunningWindow> &running_windows)
{
  if (grid == nullptr)
    return;
  for (GtkWidget *tile = gtk_widget_get_first_child (grid); tile != nullptr;
       tile = gtk_widget_get_next_sibling (tile)) {
    auto *dot = GTK_WIDGET (
        g_object_get_data (G_OBJECT (tile), "winome-running-dot"));
    if (dot == nullptr)
      continue;
    if (auto *ctx = static_cast<TileActionCtx *> (
            g_object_get_data (G_OBJECT (tile), "winome-tile-action"))) {
      bool running = !match_entry_running_windows (ctx->entry, running_windows)
                          .empty ();
      gtk_widget_set_visible (dot, running);
    } else if (auto *fctx = static_cast<FolderTileCtx *> (
                   g_object_get_data (G_OBJECT (tile), "winome-folder-action"))) {
      bool any_running = false;
      for (const AppEntry &child : fctx->item.children) {
        if (!match_entry_running_windows (child, running_windows).empty ()) {
          any_running = true;
          break;
        }
      }
      gtk_widget_set_visible (dot, any_running);
    }
  }
}

void
update_running_dots_in_place (WinomeAppGrid *self)
{
  if (self == nullptr || self->pages_stack == nullptr)
    return;
  self->running_windows = app_index_enumerate_running_windows ();
  for (GtkWidget *page_w = gtk_widget_get_first_child (self->pages_stack);
       page_w != nullptr; page_w = gtk_widget_get_next_sibling (page_w)) {
    update_tile_dots_in_grid (page_w, self->running_windows);
  }
  update_tile_dots_in_grid (self->folder_dialog_grid, self->running_windows);
}

void
rebuild_all_pages (WinomeAppGrid *self)
{
  if (self->async != nullptr) {
    uint64_t next_gen = ++self->async->icon_gen;
    self->async->shared_gen->store (next_gen, std::memory_order_release);
    self->async->pending_jobs.clear ();
    clear_icon_targets (self->async);
  }

  app_grid_close_menu (GTK_WIDGET (self));
  clear_stack_children (self->pages_stack);
  clear_box_children (self->indicators_box);
  self->page_dots.clear ();

  self->running_windows = app_index_enumerate_running_windows ();

  AppGridDimensions dim =
      compute_app_grid_dimensions (self->avail_w, self->avail_h);
  int scale = std::max (gtk_widget_get_scale_factor (GTK_WIDGET (self)), 1);
  int load_px = std::clamp (dim.icon_size * std::max (scale, 2), 96, 256);
  if (self->async != nullptr)
    self->async->built_load_px = load_px;

  self->total_pages =
      compute_app_grid_page_count ((int)self->filtered.size (), dim.page_size);
  self->page = std::clamp (self->page, 0, self->total_pages - 1);

  bool empty = self->filtered.empty ();
  gtk_widget_set_visible (self->empty_label, empty);
  gtk_widget_set_visible (self->pages_stack, !empty);

  if (empty) {
    gtk_label_set_text (GTK_LABEL (self->empty_label),
                        tr ("No results.", "未找到匹配的应用"));
  } else {
    int base_col_gap = 12;
    int base_row_gap = 12;
    int max_col_gap = 36;
    int max_row_gap = 36;
    if (StEngine *e = StEngine::get ()) {
      StEngine::Node grid_node (*e, nullptr, "", "icon-grid", "");
      double v = 0.0;
      if (grid_node.lookup_length ("column-spacing", &v) && v > 0.0)
        base_col_gap = (int)(v + 0.5);
      if (grid_node.lookup_length ("row-spacing", &v) && v > 0.0)
        base_row_gap = (int)(v + 0.5);
      if (grid_node.lookup_length ("max-column-spacing", &v) && v > 0.0)
        max_col_gap = (int)(v + 0.5);
      if (grid_node.lookup_length ("max-row-spacing", &v) && v > 0.0)
        max_row_gap = (int)(v + 0.5);
    }

    int inner_w = std::max (self->avail_w - 144, 360);
    int inner_h = std::max (self->avail_h - 36, 220);
    int tile_w = std::clamp (
        (inner_w - (dim.cols - 1) * base_col_gap) / dim.cols, 108, 168);
    int tile_h = dim.icon_size + 72;
    int col_spacing =
        dim.cols > 1
            ? std::clamp ((inner_w - dim.cols * tile_w) / (dim.cols - 1),
                          base_col_gap, std::max (base_col_gap, max_col_gap))
            : base_col_gap;
    int row_spacing =
        dim.rows > 1
            ? std::clamp ((inner_h - dim.rows * tile_h) / (dim.rows - 1),
                          base_row_gap, std::max (base_row_gap, max_row_gap))
            : base_row_gap;
    int label_spacing = lookup_icon_label_spacing ();

    for (int p = 0; p < self->total_pages; ++p) {
      AppGridSlice slice = compute_app_grid_slice ((int)self->filtered.size (),
                                                   p, dim.page_size);
      GtkWidget *page_grid = gtk_grid_new ();
      gtk_widget_add_css_class (page_grid, "icon-grid");
      gtk_grid_set_column_spacing (GTK_GRID (page_grid), col_spacing);
      gtk_grid_set_row_spacing (GTK_GRID (page_grid), row_spacing);
      gtk_grid_set_column_homogeneous (GTK_GRID (page_grid), TRUE);
      gtk_grid_set_row_homogeneous (GTK_GRID (page_grid), TRUE);
      gtk_widget_set_halign (page_grid, GTK_ALIGN_CENTER);
      gtk_widget_set_valign (page_grid, GTK_ALIGN_CENTER);

      int idx = 0;
      for (int i = slice.begin; i < slice.end; ++i) {
        int r = idx / dim.cols;
        int c = idx % dim.cols;
        const AppGridItem &item = self->filtered[i];
        GtkWidget *tile = nullptr;
        if (item.kind == AppGridItemKind::Folder) {
          bool any_running = false;
          for (const AppEntry &child : item.children) {
            if (!match_entry_running_windows (child, self->running_windows)
                     .empty ()) {
              any_running = true;
              break;
            }
          }
          tile = build_folder_tile (self, item, dim.icon_size, load_px, p,
                                    tile_w, tile_h, label_spacing, any_running);
        } else {
          bool running =
              !match_entry_running_windows (item.app, self->running_windows)
                   .empty ();
          tile = build_app_tile (self, item.app, dim.icon_size, load_px, p,
                                 tile_w, tile_h, label_spacing, running);
        }
        gtk_grid_attach (GTK_GRID (page_grid), tile, c, r, 1, 1);
        ++idx;
      }

      std::string page_name = "page-" + std::to_string (p);
      gtk_stack_add_named (GTK_STACK (self->pages_stack), page_grid,
                           page_name.c_str ());
    }

    std::string active_name = "page-" + std::to_string (self->page);
    gtk_stack_set_visible_child_full (GTK_STACK (self->pages_stack),
                                      active_name.c_str (),
                                      GTK_STACK_TRANSITION_TYPE_NONE);
  }

  if (self->total_pages > 1) {
    int dot_size = 10;
    if (StEngine *e = StEngine::get ()) {
      StEngine::Node ind (*e, nullptr, "", "page-indicator", "");
      StEngine::Node icon (*e, &ind, "", "page-indicator-icon", "");
      double v = 0.0;
      if (icon.lookup_length ("width", &v) && v > 0.0)
        dot_size = (int)(v + 0.5);
    }
    for (int p = 0; p < self->total_pages; ++p) {
      GtkWidget *dot_btn = gtk_button_new ();
      gtk_widget_set_name (dot_btn, "winome-page-dot");
      gtk_widget_add_css_class (dot_btn, "page-indicator");
      GtkWidget *dot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_add_css_class (dot, "page-indicator-icon");
      gtk_widget_set_size_request (dot, dot_size, dot_size);
      gtk_button_set_child (GTK_BUTTON (dot_btn), dot);

      auto *dctx = new PageDotCtx{self, p};
      g_object_set_data_full (
          G_OBJECT (dot_btn), "winome-dot-ctx", dctx,
          [] (gpointer ptr) { delete static_cast<PageDotCtx *> (ptr); });
      g_signal_connect (dot_btn, "clicked", G_CALLBACK (on_page_dot_clicked),
                        dctx);
      gtk_box_append (GTK_BOX (self->indicators_box), dot_btn);
      self->page_dots.push_back (dot_btn);
    }
  }
  update_page_controls (self);
  if (self->async != nullptr) {
    self->async->built_valid = true;
    self->async->built_query = self->query_lower;
    self->async->built_avail_w = self->avail_w;
    self->async->built_avail_h = self->avail_h;
    self->async->built_scale = scale;
    self->async->built_revision = app_index_revision ();
  }
  maybe_start_icon_worker (self);
}

void
winome_app_grid_dispose (GObject *object)
{
  WinomeAppGrid *self = WINOME_APP_GRID (object);
  if (self->async != nullptr) {
    self->async->alive->store (false, std::memory_order_release);
    uint64_t next_gen = ++self->async->icon_gen;
    self->async->shared_gen->store (next_gen, std::memory_order_release);
    self->async->pending_jobs.clear ();
    clear_icon_targets (self->async);
  }
  if (self->scroll_timer != 0) {
    g_source_remove (self->scroll_timer);
    self->scroll_timer = 0;
  }
  if (self->refresh_timer != 0) {
    g_source_remove (self->refresh_timer);
    self->refresh_timer = 0;
  }
  app_grid_close_menu (GTK_WIDGET (self));
  close_folder_dialog (self);
  clear_menu_tile_highlight (self);
  self->page_dots.clear ();
  G_OBJECT_CLASS (winome_app_grid_parent_class)->dispose (object);
}

void
winome_app_grid_finalize (GObject *object)
{
  WinomeAppGrid *self = WINOME_APP_GRID (object);
  using StringT = std::string;
  using AppVecT = std::vector<AppEntry>;
  using PinnedVecT = std::vector<StartMenuPinnedItem>;
  using GridItemVecT = std::vector<AppGridItem>;
  using RunVecT = std::vector<RunningWindow>;
  using DotVecT = std::vector<GtkWidget *>;
  delete self->async;
  self->async = nullptr;
  self->page_dots.~DotVecT ();
  self->running_windows.~RunVecT ();
  self->filtered.~GridItemVecT ();
  self->pinned_layout.~PinnedVecT ();
  self->all_apps.~AppVecT ();
  self->query_lower.~StringT ();
  G_OBJECT_CLASS (winome_app_grid_parent_class)->finalize (object);
}

void
winome_app_grid_init (WinomeAppGrid *self)
{
  // g_object_new zero-fills C structs without running C++ constructors or
  // default member initializers.
  new (&self->query_lower) std::string ();
  new (&self->all_apps) std::vector<AppEntry> ();
  new (&self->pinned_layout) std::vector<StartMenuPinnedItem> ();
  new (&self->filtered) std::vector<AppGridItem> ();
  new (&self->running_windows) std::vector<RunningWindow> ();
  new (&self->page_dots) std::vector<GtkWidget *> ();
  self->async = new AppGridAsyncState ();
  self->total_pages = 1;
  self->avail_w = 1000;
  self->avail_h = 560;
  self->can_scroll = true;

  gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                  GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing (GTK_BOX (self), 0);
  gtk_widget_set_halign (GTK_WIDGET (self), GTK_ALIGN_CENTER);
  gtk_widget_set_valign (GTK_WIDGET (self), GTK_ALIGN_CENTER);

  self->root_overlay = gtk_overlay_new ();
  gtk_widget_set_hexpand (self->root_overlay, TRUE);
  gtk_widget_set_vexpand (self->root_overlay, TRUE);

  self->content_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_halign (self->content_box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (self->content_box, GTK_ALIGN_CENTER);

  int arrow_size = 24;
  if (StEngine *e = StEngine::get ()) {
    StEngine::Node arrow (*e, nullptr, "", "page-navigation-arrow", "");
    double v = 0.0;
    if (arrow.lookup_length ("width", &v) && v > 0.0)
      arrow_size = (int)(v + 0.5);
  }

  self->body_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign (self->body_row, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (self->body_row, GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand (self->body_row, TRUE);

  self->prev_btn = gtk_button_new ();
  gtk_widget_set_name (self->prev_btn, "winome-app-nav-arrow");
  gtk_widget_add_css_class (self->prev_btn, "page-navigation-arrow");
  gtk_widget_set_focus_on_click (self->prev_btn, FALSE);
  gtk_widget_set_valign (self->prev_btn, GTK_ALIGN_CENTER);
  GtkWidget *prev_img =
      gtk_image_new_from_icon_name ("carousel-arrow-previous-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (prev_img), arrow_size);
  gtk_button_set_child (GTK_BUTTON (self->prev_btn), prev_img);
  g_signal_connect (self->prev_btn, "clicked", G_CALLBACK (on_prev_clicked),
                    self);
  gtk_box_append (GTK_BOX (self->body_row), self->prev_btn);

  self->center_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_halign (self->center_box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (self->center_box, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (self->center_box, TRUE);

  self->pages_stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (self->pages_stack),
                                 GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
  gtk_stack_set_transition_duration (GTK_STACK (self->pages_stack), 250);
  gtk_stack_set_hhomogeneous (GTK_STACK (self->pages_stack), TRUE);
  gtk_stack_set_vhomogeneous (GTK_STACK (self->pages_stack), TRUE);
  gtk_widget_set_overflow (self->pages_stack, GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_halign (self->pages_stack, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (self->pages_stack, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (self->center_box), self->pages_stack);

  self->empty_label = gtk_label_new ("");
  gtk_widget_add_css_class (self->empty_label, "search-statustext");
  gtk_widget_set_halign (self->empty_label, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (self->empty_label, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (self->empty_label, FALSE);
  gtk_box_append (GTK_BOX (self->center_box), self->empty_label);

  gtk_box_append (GTK_BOX (self->body_row), self->center_box);

  self->next_btn = gtk_button_new ();
  gtk_widget_set_name (self->next_btn, "winome-app-nav-arrow");
  gtk_widget_add_css_class (self->next_btn, "page-navigation-arrow");
  gtk_widget_set_focus_on_click (self->next_btn, FALSE);
  gtk_widget_set_valign (self->next_btn, GTK_ALIGN_CENTER);
  GtkWidget *next_img =
      gtk_image_new_from_icon_name ("carousel-arrow-next-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (next_img), arrow_size);
  gtk_button_set_child (GTK_BUTTON (self->next_btn), next_img);
  g_signal_connect (self->next_btn, "clicked", G_CALLBACK (on_next_clicked),
                    self);
  gtk_box_append (GTK_BOX (self->body_row), self->next_btn);

  gtk_box_append (GTK_BOX (self->content_box), self->body_row);

  self->indicators_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (self->indicators_box, "page-indicators");
  gtk_widget_set_halign (self->indicators_box, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (self->content_box), self->indicators_box);

  gtk_overlay_set_child (GTK_OVERLAY (self->root_overlay), self->content_box);
  gtk_box_append (GTK_BOX (self), self->root_overlay);

  GtkEventController *scroll = gtk_event_controller_scroll_new (
      GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  g_signal_connect (scroll, "scroll", G_CALLBACK (on_grid_scroll), self);
  gtk_widget_add_controller (GTK_WIDGET (self), scroll);

  g_signal_connect (self, "realize", G_CALLBACK (on_grid_realize), nullptr);
}

void
winome_app_grid_class_init (WinomeAppGridClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = winome_app_grid_dispose;
  object_class->finalize = winome_app_grid_finalize;
}

}  // namespace

int
compute_app_grid_page_count (int total_items, int page_size)
{
  if (page_size <= 0 || total_items <= 0)
    return 1;
  return (total_items + page_size - 1) / page_size;
}

AppGridSlice
compute_app_grid_slice (int total_items, int page, int page_size)
{
  AppGridSlice s;
  int effective_size = std::max (page_size, 1);
  s.total_pages = compute_app_grid_page_count (total_items, effective_size);
  if (total_items <= 0) {
    s.page = 0;
    s.begin = 0;
    s.end = 0;
    return s;
  }
  s.page = std::clamp (page, 0, s.total_pages - 1);
  s.begin = s.page * effective_size;
  s.end = std::min (total_items, s.begin + effective_size);
  return s;
}

AppGridDimensions
compute_app_grid_dimensions (int avail_w, int avail_h)
{
  AppGridDimensions d;
  int inner_w = std::max (avail_w - 144, 320);
  int inner_h = std::max (avail_h - 36, 220);

  if (avail_w < avail_h && inner_w < 700) {
    d.cols = 4;
    d.rows = (inner_h >= 580) ? 5 : 4;
  } else if (inner_w >= 900 && inner_h >= 560) {
    d.cols = 6;
    d.rows = 4;
  } else if (inner_w >= 960 && inner_h >= 400) {
    d.cols = 6;
    d.rows = 3;
  } else if (inner_w >= 680 && inner_h >= 380) {
    d.cols = 5;
    d.rows = 3;
  } else {
    d.cols = 4;
    d.rows = 3;
  }

  constexpr int kMinGap = 12;
  int max_cell_w = (inner_w - (d.cols - 1) * kMinGap) / d.cols;
  int max_cell_h = (inner_h - (d.rows - 1) * kMinGap) / d.rows;

  constexpr int kCandidates[] = {96, 80, 72, 64, 56, 48};
  d.icon_size = 48;
  for (int sz : kCandidates) {
    if (sz + 24 <= max_cell_w && sz + 72 <= max_cell_h) {
      d.icon_size = sz;
      break;
    }
  }
  d.page_size = d.cols * d.rows;
  return d;
}

GtkWidget *
app_grid_new (void (*on_launch_done) (void *),
              void (*on_pins_changed) (void *), void *user_data)
{
  WinomeAppGrid *self =
      static_cast<WinomeAppGrid *> (g_object_new (winome_app_grid_get_type (),
                                                  nullptr));
  self->on_launch_done = on_launch_done;
  self->on_pins_changed = on_pins_changed;
  self->user_data = user_data;
  return GTK_WIDGET (self);
}

void
app_grid_set_query (GtkWidget *grid, const std::string &query_lower)
{
  WinomeAppGrid *self = WINOME_APP_GRID (grid);
  uint64_t rev = app_index_revision ();
  if (self->query_lower == query_lower && self->index_loaded &&
      self->async != nullptr && self->async->built_valid &&
      self->async->built_revision == rev) {
    return;
  }
  self->query_lower = query_lower;
  self->page = 0;
  if (!self->index_loaded ||
      (self->async != nullptr && self->async->built_revision != rev)) {
    self->all_apps = app_index_get_all ();
    self->pinned_layout = app_index_get_pinned_layout ();
    self->index_loaded = true;
  }
  close_folder_dialog (self);
  self->filtered =
      build_app_grid_items (self->all_apps, self->pinned_layout,
                            self->query_lower);
  rebuild_all_pages (self);
}

void
app_grid_rebuild (GtkWidget *grid, int avail_w, int avail_h)
{
  WinomeAppGrid *self = WINOME_APP_GRID (grid);
  int eff_w = std::max (avail_w, 400);
  int eff_h = std::max (avail_h, 300);
  int scale = std::max (gtk_widget_get_scale_factor (grid), 1);
  uint64_t rev = app_index_revision ();

  if (self->async != nullptr && self->async->built_valid &&
      self->index_loaded && self->async->built_query == self->query_lower &&
      self->async->built_avail_w == eff_w &&
      self->async->built_avail_h == eff_h &&
      self->async->built_scale == scale &&
      self->async->built_revision == rev) {
    update_running_dots_in_place (self);
    maybe_start_icon_worker (self);
    return;
  }

  self->avail_w = eff_w;
  self->avail_h = eff_h;
  if (!self->index_loaded ||
      (self->async != nullptr && self->async->built_revision != rev)) {
    self->all_apps = app_index_get_all ();
    self->pinned_layout = app_index_get_pinned_layout ();
    self->index_loaded = true;
  }
  close_folder_dialog (self);
  self->filtered =
      build_app_grid_items (self->all_apps, self->pinned_layout,
                            self->query_lower);
  rebuild_all_pages (self);
}

void
app_grid_next_page (GtkWidget *grid)
{
  WinomeAppGrid *self = WINOME_APP_GRID (grid);
  if (self->page + 1 < self->total_pages)
    switch_to_page (self, self->page + 1);
}

void
app_grid_prev_page (GtkWidget *grid)
{
  WinomeAppGrid *self = WINOME_APP_GRID (grid);
  if (self->page > 0)
    switch_to_page (self, self->page - 1);
}

void
app_grid_first_page (GtkWidget *grid)
{
  WinomeAppGrid *self = WINOME_APP_GRID (grid);
  switch_to_page (self, 0);
}

void
app_grid_last_page (GtkWidget *grid)
{
  WinomeAppGrid *self = WINOME_APP_GRID (grid);
  switch_to_page (self, std::max (self->total_pages - 1, 0));
}

void
app_grid_reset_page (GtkWidget *grid)
{
  WinomeAppGrid *self = WINOME_APP_GRID (grid);
  if (self->page != 0) {
    self->page = 0;
    if (self->pages_stack != nullptr) {
      gtk_stack_set_visible_child_full (GTK_STACK (self->pages_stack), "page-0",
                                        GTK_STACK_TRANSITION_TYPE_NONE);
    }
    update_page_controls (self);
  }
}

void
app_grid_refresh_running (GtkWidget *grid)
{
  if (grid == nullptr)
    return;
  update_running_dots_in_place (WINOME_APP_GRID (grid));
}

void
app_grid_prewarm (GtkWidget *grid, int avail_w, int avail_h)
{
  if (grid == nullptr)
    return;
  WinomeAppGrid *self = WINOME_APP_GRID (grid);
  if (self->async != nullptr)
    self->async->prewarm_active = true;
  app_grid_rebuild (grid, avail_w, avail_h);
}

bool
app_grid_launch_first (GtkWidget *grid)
{
  WinomeAppGrid *self = WINOME_APP_GRID (grid);
  if (self->filtered.empty ())
    return false;
  if (self->filtered[0].kind == AppGridItemKind::Folder) {
    open_folder_dialog (self, self->filtered[0]);
    return false;
  }
  return activate_or_launch_entry (self->filtered[0].app, false);
}

void
app_grid_close_menu (GtkWidget *grid)
{
  if (grid == nullptr)
    return;
  WinomeAppGrid *self = WINOME_APP_GRID (grid);
  clear_menu_tile_highlight (self);
  if (self->active_menu != nullptr) {
    GtkWidget *m = self->active_menu;
    self->active_menu = nullptr;
    g_object_ref (m);
    gtk_popover_popdown (GTK_POPOVER (m));
    if (gtk_widget_get_parent (m) != nullptr)
      gtk_widget_unparent (m);
    g_object_unref (m);
  }
}

bool
app_grid_close_open_folder (GtkWidget *grid)
{
  if (grid == nullptr)
    return false;
  return close_folder_dialog (WINOME_APP_GRID (grid));
}

}  // namespace winome
