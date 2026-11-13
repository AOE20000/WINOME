// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors

#include "app-grid.h"
#include "app-index.h"
#include "config.h"
#include "i18n.h"

#include <algorithm>
#include <vector>

namespace winome {

namespace {

constexpr const char *kAppGridCss =
    "#winome-app-tile {"
    "  background-image: none;"
    "  border: none;"
    "  box-shadow: none;"
    "  min-width: 112px;"
    "  min-height: 112px;"
    "}"
    "#winome-app-nav-arrow {"
    "  background-image: none;"
    "  border: none;"
    "  box-shadow: none;"
    "}"
    "#winome-page-dot {"
    "  background: none;"
    "  border: none;"
    "  box-shadow: none;"
    "  padding: 6px;"
    "  min-width: 12px;"
    "  min-height: 12px;"
    "}"
    "#winome-app-ctx-item {"
    "  background: none;"
    "  border: none;"
    "  box-shadow: none;"
    "  color: #fafafb;"
    "  padding: 6px 12px;"
    "  border-radius: 6px;"
    "}"
    "#winome-app-ctx-item:hover {"
    "  background-color: rgba(255, 255, 255, 0.12);"
    "}";

struct WinomeAppGrid {
  GtkBox parent_instance;

  GtkWidget *body_row = nullptr;
  GtkWidget *prev_btn = nullptr;
  GtkWidget *next_btn = nullptr;
  GtkWidget *center_box = nullptr;
  GtkWidget *grid = nullptr;
  GtkWidget *empty_label = nullptr;
  GtkWidget *indicators_box = nullptr;
  GtkWidget *active_menu = nullptr;
  GtkCssProvider *css = nullptr;

  std::string query_lower;
  std::vector<AppEntry> all_apps;
  std::vector<AppEntry> filtered;
  int page = 0;
  int avail_w = 1000;
  int avail_h = 560;
  bool index_loaded = false;

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

void rebuild_page_contents (WinomeAppGrid *self);

void
clear_box_children (GtkWidget *box)
{
  if (box == nullptr)
    return;
  while (GtkWidget *child = gtk_widget_get_first_child (box))
    gtk_box_remove (GTK_BOX (box), child);
}

void
clear_grid_children (GtkWidget *grid)
{
  if (grid == nullptr)
    return;
  while (GtkWidget *child = gtk_widget_get_first_child (grid))
    gtk_grid_remove (GTK_GRID (grid), child);
}

void
on_tile_action_launch (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  auto *ctx = static_cast<TileActionCtx *> (user_data);
  if (ctx->self->active_menu != nullptr)
    gtk_popover_popdown (GTK_POPOVER (ctx->self->active_menu));
  if (app_index_launch (ctx->entry, false) &&
      ctx->self->on_launch_done != nullptr) {
    ctx->self->on_launch_done (ctx->self->user_data);
  }
}

void
on_tile_action_admin (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  auto *ctx = static_cast<TileActionCtx *> (user_data);
  if (ctx->self->active_menu != nullptr)
    gtk_popover_popdown (GTK_POPOVER (ctx->self->active_menu));
  if (app_index_launch (ctx->entry, true) &&
      ctx->self->on_launch_done != nullptr) {
    ctx->self->on_launch_done (ctx->self->user_data);
  }
}

void
on_tile_action_pin (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  auto *ctx = static_cast<TileActionCtx *> (user_data);
  toggle_app_id_pinned (ctx->entry.id);
  if (ctx->self->active_menu != nullptr)
    gtk_popover_popdown (GTK_POPOVER (ctx->self->active_menu));
  if (ctx->self->on_pins_changed != nullptr)
    ctx->self->on_pins_changed (ctx->self->user_data);
}

void
on_tile_action_location (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  auto *ctx = static_cast<TileActionCtx *> (user_data);
  if (ctx->self->active_menu != nullptr)
    gtk_popover_popdown (GTK_POPOVER (ctx->self->active_menu));
  app_index_open_location (ctx->entry);
  if (ctx->self->on_launch_done != nullptr)
    ctx->self->on_launch_done (ctx->self->user_data);
}

GtkWidget *
make_menu_button (const char *text, GCallback cb, TileActionCtx *ctx)
{
  GtkWidget *btn = gtk_button_new_with_label (text);
  gtk_widget_set_name (btn, "winome-app-ctx-item");
  gtk_widget_add_css_class (btn, "popup-menu-item");
  GtkWidget *lbl = gtk_button_get_child (GTK_BUTTON (btn));
  if (GTK_IS_LABEL (lbl))
    gtk_label_set_xalign (GTK_LABEL (lbl), 0.0f);
  g_signal_connect (btn, "clicked", cb, ctx);
  return btn;
}

void
show_tile_context_menu (GtkWidget *tile, WinomeAppGrid *self,
                        const AppEntry &entry)
{
  app_grid_close_menu (GTK_WIDGET (self));

  GtkWidget *popover = gtk_popover_new ();
  gtk_widget_set_parent (popover, tile);
  gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
  gtk_popover_set_autohide (GTK_POPOVER (popover), TRUE);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_add_css_class (box, "popup-menu-content");

  auto *ctx = new TileActionCtx{self, entry};
  g_object_set_data_full (
      G_OBJECT (popover), "winome-tile-ctx", ctx,
      [] (gpointer p) { delete static_cast<TileActionCtx *> (p); });

  WinomeConfig cfg = get_config ();
  bool pinned = is_app_id_pinned (cfg.pinned_apps, entry.id);

  gtk_box_append (GTK_BOX (box),
                  make_menu_button (tr ("Open", "打开"),
                                    G_CALLBACK (on_tile_action_launch), ctx));
  gtk_box_append (
      GTK_BOX (box),
      make_menu_button (tr ("Run as Administrator", "以管理员身份运行"),
                        G_CALLBACK (on_tile_action_admin), ctx));
  gtk_box_append (
      GTK_BOX (box),
      make_menu_button (pinned ? tr ("Unpin from Dash", "从程序坞取消固定")
                               : tr ("Pin to Dash", "固定到程序坞"),
                        G_CALLBACK (on_tile_action_pin), ctx));
  gtk_box_append (
      GTK_BOX (box),
      make_menu_button (tr ("Open File Location", "打开文件所在位置"),
                        G_CALLBACK (on_tile_action_location), ctx));

  gtk_popover_set_child (GTK_POPOVER (popover), box);
  g_signal_connect (popover, "closed", G_CALLBACK (+[] (GtkPopover *p, gpointer d) {
                      auto *g = WINOME_APP_GRID (d);
                      if (g->active_menu == GTK_WIDGET (p))
                        g->active_menu = nullptr;
                      if (gtk_widget_get_parent (GTK_WIDGET (p)) != nullptr)
                        gtk_widget_unparent (GTK_WIDGET (p));
                    }),
                    self);
  self->active_menu = popover;
  gtk_popover_popup (GTK_POPOVER (popover));
}

void
on_tile_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  auto *ctx = static_cast<TileActionCtx *> (user_data);
  if (app_index_launch (ctx->entry, false) &&
      ctx->self->on_launch_done != nullptr) {
    ctx->self->on_launch_done (ctx->self->user_data);
  }
}

void
on_tile_right_click (GtkGestureClick *gesture G_GNUC_UNUSED, int n_press G_GNUC_UNUSED,
                     double x G_GNUC_UNUSED, double y G_GNUC_UNUSED,
                     gpointer user_data)
{
  auto *ctx = static_cast<TileActionCtx *> (user_data);
  GtkWidget *tile = gtk_event_controller_get_widget (
      GTK_EVENT_CONTROLLER (gesture));
  show_tile_context_menu (tile, ctx->self, ctx->entry);
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
on_page_dot_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  auto *ctx = static_cast<PageDotCtx *> (user_data);
  if (ctx->self->page != ctx->page) {
    ctx->self->page = ctx->page;
    rebuild_page_contents (ctx->self);
  }
}

gboolean
on_grid_scroll (GtkEventControllerScroll *ctrl G_GNUC_UNUSED, double dx,
                double dy, gpointer user_data)
{
  WinomeAppGrid *self = WINOME_APP_GRID (user_data);
  double delta = (dy != 0.0) ? dy : dx;
  if (delta > 0.1) {
    app_grid_next_page (GTK_WIDGET (self));
    return TRUE;
  }
  if (delta < -0.1) {
    app_grid_prev_page (GTK_WIDGET (self));
    return TRUE;
  }
  return FALSE;
}

GtkWidget *
build_app_tile (WinomeAppGrid *self, const AppEntry &entry, int icon_size,
                int tile_w, int tile_h)
{
  GtkWidget *btn = gtk_button_new ();
  gtk_widget_set_name (btn, "winome-app-tile");
  gtk_widget_add_css_class (btn, "overview-tile");
  gtk_widget_set_size_request (btn, tile_w, tile_h);
  gtk_widget_set_halign (btn, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (btn, GTK_ALIGN_CENTER);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class (content, "overview-icon");
  gtk_widget_add_css_class (content, "overview-icon-with-label");
  gtk_widget_set_halign (content, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (content, GTK_ALIGN_CENTER);

  GdkTexture *tex = app_icon_for_entry (entry, icon_size);
  GtkWidget *icon_w = nullptr;
  if (tex != nullptr) {
    icon_w = gtk_image_new_from_paintable (GDK_PAINTABLE (tex));
    g_object_unref (tex);
  } else {
    icon_w = gtk_image_new_from_icon_name ("application-x-executable-symbolic");
  }
  gtk_image_set_pixel_size (GTK_IMAGE (icon_w), icon_size);
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

  gtk_button_set_child (GTK_BUTTON (btn), content);

  auto *ctx = new TileActionCtx{self, entry};
  g_object_set_data_full (
      G_OBJECT (btn), "winome-tile-action", ctx,
      [] (gpointer p) { delete static_cast<TileActionCtx *> (p); });
  g_signal_connect (btn, "clicked", G_CALLBACK (on_tile_clicked), ctx);

  GtkGesture *rclick = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (rclick),
                                 GDK_BUTTON_SECONDARY);
  g_signal_connect (rclick, "pressed", G_CALLBACK (on_tile_right_click), ctx);
  gtk_widget_add_controller (btn, GTK_EVENT_CONTROLLER (rclick));

  return btn;
}

void
rebuild_page_contents (WinomeAppGrid *self)
{
  app_grid_close_menu (GTK_WIDGET (self));
  clear_grid_children (self->grid);
  clear_box_children (self->indicators_box);

  AppGridDimensions dim =
      compute_app_grid_dimensions (self->avail_w, self->avail_h);
  AppGridSlice slice = compute_app_grid_slice ((int)self->filtered.size (),
                                                self->page, dim.page_size);
  self->page = slice.page;

  bool empty = self->filtered.empty ();
  gtk_widget_set_visible (self->empty_label, empty);
  gtk_widget_set_visible (self->grid, !empty);

  if (empty) {
    gtk_label_set_text (GTK_LABEL (self->empty_label),
                        tr ("No results.", "未找到匹配的应用"));
  } else {
    // Keep tile bounding box proportional to available grid width cleanly.
    int grid_w = std::max (self->avail_w - 160, 480);
    int tile_w = std::clamp ((grid_w - (dim.cols - 1) * 18) / dim.cols, 112, 152);
    int tile_h = dim.icon_size + 60;

    int idx = 0;
    for (int i = slice.begin; i < slice.end; ++i) {
      int r = idx / dim.cols;
      int c = idx % dim.cols;
      GtkWidget *tile =
          build_app_tile (self, self->filtered[i], dim.icon_size, tile_w, tile_h);
      gtk_grid_attach (GTK_GRID (self->grid), tile, c, r, 1, 1);
      ++idx;
    }
  }

  // Page navigation arrows & dots.
  bool multi_page = slice.total_pages > 1;
  gtk_widget_set_visible (self->prev_btn, multi_page);
  gtk_widget_set_visible (self->next_btn, multi_page);
  gtk_widget_set_sensitive (self->prev_btn, slice.page > 0);
  gtk_widget_set_opacity (self->prev_btn, slice.page > 0 ? 1.0 : 0.28);
  gtk_widget_set_sensitive (self->next_btn, slice.page + 1 < slice.total_pages);
  gtk_widget_set_opacity (self->next_btn,
                          slice.page + 1 < slice.total_pages ? 1.0 : 0.28);
  gtk_widget_set_visible (self->indicators_box, multi_page);

  if (multi_page) {
    for (int p = 0; p < slice.total_pages; ++p) {
      GtkWidget *dot_btn = gtk_button_new ();
      gtk_widget_set_name (dot_btn, "winome-page-dot");
      gtk_widget_add_css_class (dot_btn, "page-indicator");
      GtkWidget *dot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_add_css_class (dot, "page-indicator-icon");
      gtk_widget_set_opacity (dot, (p == slice.page) ? 1.0 : 0.35);
      gtk_button_set_child (GTK_BUTTON (dot_btn), dot);

      auto *dctx = new PageDotCtx{self, p};
      g_object_set_data_full (
          G_OBJECT (dot_btn), "winome-dot-ctx", dctx,
          [] (gpointer ptr) { delete static_cast<PageDotCtx *> (ptr); });
      g_signal_connect (dot_btn, "clicked", G_CALLBACK (on_page_dot_clicked),
                        dctx);
      gtk_box_append (GTK_BOX (self->indicators_box), dot_btn);
    }
  }
}

void
winome_app_grid_dispose (GObject *object)
{
  WinomeAppGrid *self = WINOME_APP_GRID (object);
  app_grid_close_menu (GTK_WIDGET (self));
  if (self->css != nullptr) {
    gtk_style_context_remove_provider_for_display (
        gdk_display_get_default (), GTK_STYLE_PROVIDER (self->css));
    g_clear_object (&self->css);
  }
  G_OBJECT_CLASS (winome_app_grid_parent_class)->dispose (object);
}

void
winome_app_grid_init (WinomeAppGrid *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                  GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing (GTK_BOX (self), 12);
  gtk_widget_set_halign (GTK_WIDGET (self), GTK_ALIGN_CENTER);
  gtk_widget_set_valign (GTK_WIDGET (self), GTK_ALIGN_CENTER);

  if (GdkDisplay *disp = gdk_display_get_default ()) {
    self->css = gtk_css_provider_new ();
    gtk_css_provider_load_from_string (self->css, kAppGridCss);
    gtk_style_context_add_provider_for_display (
        disp, GTK_STYLE_PROVIDER (self->css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
  }

  self->body_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_halign (self->body_row, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (self->body_row, GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand (self->body_row, TRUE);

  self->prev_btn = gtk_button_new ();
  gtk_widget_set_name (self->prev_btn, "winome-app-nav-arrow");
  gtk_widget_add_css_class (self->prev_btn, "page-navigation-arrow");
  gtk_widget_set_valign (self->prev_btn, GTK_ALIGN_CENTER);
  GtkWidget *prev_img = gtk_image_new_from_icon_name ("go-previous-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (prev_img), 24);
  gtk_button_set_child (GTK_BUTTON (self->prev_btn), prev_img);
  g_signal_connect (self->prev_btn, "clicked", G_CALLBACK (on_prev_clicked),
                    self);
  gtk_box_append (GTK_BOX (self->body_row), self->prev_btn);

  self->center_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_halign (self->center_box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (self->center_box, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (self->center_box, TRUE);

  self->grid = gtk_grid_new ();
  gtk_widget_add_css_class (self->grid, "icon-grid");
  gtk_grid_set_column_spacing (GTK_GRID (self->grid), 18);
  gtk_grid_set_row_spacing (GTK_GRID (self->grid), 18);
  gtk_grid_set_column_homogeneous (GTK_GRID (self->grid), TRUE);
  gtk_grid_set_row_homogeneous (GTK_GRID (self->grid), TRUE);
  gtk_widget_set_halign (self->grid, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (self->grid, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (self->center_box), self->grid);

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
  gtk_widget_set_valign (self->next_btn, GTK_ALIGN_CENTER);
  GtkWidget *next_img = gtk_image_new_from_icon_name ("go-next-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (next_img), 24);
  gtk_button_set_child (GTK_BUTTON (self->next_btn), next_img);
  g_signal_connect (self->next_btn, "clicked", G_CALLBACK (on_next_clicked),
                    self);
  gtk_box_append (GTK_BOX (self->body_row), self->next_btn);

  gtk_box_append (GTK_BOX (self), self->body_row);

  self->indicators_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class (self->indicators_box, "page-indicators");
  gtk_widget_set_halign (self->indicators_box, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (self), self->indicators_box);

  GtkEventController *scroll = gtk_event_controller_scroll_new (
      GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect (scroll, "scroll", G_CALLBACK (on_grid_scroll), self);
  gtk_widget_add_controller (GTK_WIDGET (self), scroll);
}

void
winome_app_grid_class_init (WinomeAppGridClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = winome_app_grid_dispose;
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
  if (avail_w >= 1150 && avail_h >= 580) {
    d.cols = 6;
    d.rows = 4;
    d.icon_size = 64;
  } else if (avail_w >= 860 && avail_h >= 440) {
    d.cols = 5;
    d.rows = 3;
    d.icon_size = 56;
  } else {
    d.cols = 4;
    d.rows = 3;
    d.icon_size = 48;
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
  if (self->query_lower == query_lower && self->index_loaded)
    return;
  self->query_lower = query_lower;
  self->page = 0;
  if (!self->index_loaded) {
    self->all_apps = app_index_get_all ();
    self->index_loaded = true;
  }
  self->filtered = filter_and_sort_apps (self->all_apps, self->query_lower);
  rebuild_page_contents (self);
}

void
app_grid_rebuild (GtkWidget *grid, int avail_w, int avail_h)
{
  WinomeAppGrid *self = WINOME_APP_GRID (grid);
  self->avail_w = std::max (avail_w, 400);
  self->avail_h = std::max (avail_h, 300);
  self->all_apps = app_index_get_all ();
  self->index_loaded = true;
  self->filtered = filter_and_sort_apps (self->all_apps, self->query_lower);
  rebuild_page_contents (self);
}

void
app_grid_next_page (GtkWidget *grid)
{
  WinomeAppGrid *self = WINOME_APP_GRID (grid);
  AppGridDimensions dim =
      compute_app_grid_dimensions (self->avail_w, self->avail_h);
  int pages = compute_app_grid_page_count ((int)self->filtered.size (),
                                           dim.page_size);
  if (self->page + 1 < pages) {
    ++self->page;
    rebuild_page_contents (self);
  }
}

void
app_grid_prev_page (GtkWidget *grid)
{
  WinomeAppGrid *self = WINOME_APP_GRID (grid);
  if (self->page > 0) {
    --self->page;
    rebuild_page_contents (self);
  }
}

void
app_grid_reset_page (GtkWidget *grid)
{
  WinomeAppGrid *self = WINOME_APP_GRID (grid);
  if (self->page != 0) {
    self->page = 0;
    rebuild_page_contents (self);
  }
}

bool
app_grid_launch_first (GtkWidget *grid)
{
  WinomeAppGrid *self = WINOME_APP_GRID (grid);
  if (self->filtered.empty ())
    return false;
  return app_index_launch (self->filtered[0], false);
}

void
app_grid_close_menu (GtkWidget *grid)
{
  if (grid == nullptr)
    return;
  WinomeAppGrid *self = WINOME_APP_GRID (grid);
  if (self->active_menu != nullptr) {
    GtkWidget *m = self->active_menu;
    self->active_menu = nullptr;
    gtk_popover_popdown (GTK_POPOVER (m));
    if (gtk_widget_get_parent (m) != nullptr)
      gtk_widget_unparent (m);
  }
}

}  // namespace winome
