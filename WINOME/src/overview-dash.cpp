// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Overview dash, visual port of gnome-shell js/ui/dash.js. See the header
// for the geometry contract. Applications are discovered by enumerating the
// same alt-tab-eligible windows the overview picker shows, then grouped by
// AppUserModelID (UWP apps) or the owner process image (win32). Icons come
// from (in order) WM_GETICON, the window class icon and
// IShellItemImageFactory on the exe path, converted to GdkTexture.

#include "overview-dash.h"
#include "app-index.h"
#include "config.h"
#include "easing.h"
#include "i18n.h"
#include "overview-trigger.h"
#include "st-engine.h"
#include "system-status.h"

#include <gdk/gdk.h>
#include <dwmapi.h>

#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <string>
#include <vector>

namespace winome {

namespace {

// dash.js baseIconSizes
constexpr int kBaseIconSizes[] = {16, 22, 24, 32, 48, 64};
constexpr int kBaseIconSizesCount = 6;

// Compiled theme values (see _dash.scss / gnome-shell-dark.css).
constexpr double kDashPillRadius = 28.0;
constexpr int kDashPadV = 12;
constexpr int kDashPadH = 10;
constexpr int kDashSidePadding = 6;   // #dash padding-left/right
constexpr int kDashBottomMargin = 12; // .dash-background margin-bottom
constexpr int kTilePad = 6;           // .overview-icon padding
constexpr double kTileRadius = 16.0;
constexpr int kItemGap = 4;           // margin 0 2px on both neighbours
constexpr int kSeparatorGap = 9;      // .dash-separator (1px + 4px margin * 2)
constexpr int kDotSize = 5;
constexpr int kLabelHoverTimeoutMs = 300; // DASH_ITEM_HOVER_TIMEOUT
constexpr int kLabelShowMs = 150;         // DASH_ITEM_LABEL_SHOW_TIME
constexpr int kLabelHideMs = 100;         // dash.js label hideFade
constexpr int kLabelOffset = 8;           // .dash-label -y-offset

// PKEY_AppUserModel_ID (propsys), defined locally to avoid header churn.
constexpr GUID kPKEY_AppUserModel_ID_fmt = {
    0x9F4C2855, 0x9F79, 0x4B39,
    {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}};

struct DashApp {
  std::string id;            // canonical normalized app ID (AUMID / exe / lnk)
  std::wstring exe_path;     // primary exe path if running or resolved
  std::string name;          // hover label text (UTF-8)
  GdkTexture *icon = nullptr;
  std::vector<HWND> windows; // z-order, topmost first
  bool pinned = false;
};

struct DashTileRect {
  double x, y, w, h; // widget-relative
};

struct WinomeDash {
  GtkWidget parent_instance;

  GtkWidget *root_fixed;  // overview root (owns the hover label)
  GtkWidget *label;       // .dash-label GtkLabel
  guint label_timeout;    // hover delay source
  uint64_t label_anim_gen = 0;
  GtkWidget *active_menu; // right-click GtkPopover

  std::vector<DashApp> apps;
  int pinned_count = 0;    // items [0..pinned_count) are persisted favorites
  double separator_x = -1; // >= 0 when a separator separates favorites & running
  int icon_size = 64;      // logical px

  int pill_x = 0, pill_y = 0, pill_w = 0, pill_h = 0; // widget-relative
  std::vector<DashTileRect> tile_rects;
  int hover_index = -1;    // -1 = none
  bool show_apps_active = false;

  HMONITOR last_monitor = nullptr;
  int last_max_height = 96;

  void (*show_apps_cb) (void *) = nullptr;
  void *show_apps_data = nullptr;
  void (*changed_cb) (void *) = nullptr;
  void *changed_data = nullptr;

  GdkRGBA pill_color;
  GdkRGBA tile_color;
  GdkRGBA tile_hover_color;
  GdkRGBA tile_checked_color;
  GdkRGBA sep_color;
  GdkRGBA fg_color;
  bool colors_resolved = false;
};

struct WinomeDashClass {
  GtkWidgetClass parent_class;
};

#define WINOME_DASH(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), winome_dash_get_type (), WinomeDash))

G_DEFINE_TYPE (WinomeDash, winome_dash, GTK_TYPE_WIDGET)

// --- theme lookups (StEngine, with compiled-CSS fallbacks) -----------------

static void
resolve_colors (WinomeDash *self)
{
  if (self->colors_resolved)
    return;
  self->colors_resolved = true;

  self->pill_color = (GdkRGBA){0.22f, 0.22f, 0.23f, 1.0f};   // #38383b
  self->tile_color = self->pill_color;                        // flat on dash
  self->tile_hover_color = (GdkRGBA){0.286f, 0.286f, 0.302f, 1.0f}; // #49494d
  self->tile_checked_color = (GdkRGBA){0.294f, 0.294f, 0.310f, 1.0f}; // #4b4b4f
  self->sep_color = (GdkRGBA){0.98f, 0.98f, 0.98f, 0.12f};
  self->fg_color = (GdkRGBA){0.98f, 0.98f, 0.98f, 1.0f};      // #fafafb

  StEngine *engine = StEngine::get ();
  if (engine == nullptr)
    return;

  // #dash .dash-background { background-color }
  StEngine::Node dash (*engine, nullptr, "dash", "", "");
  StEngine::Node bg (*engine, &dash, "", "dash-background", "");
  std::string color;
  if (bg.lookup_color ("background-color", &color) &&
      gdk_rgba_parse (&self->pill_color, color.c_str ()))
    self->tile_color = self->pill_color;

  StEngine::Node sep (*engine, &dash, "", "dash-separator", "");
  if (sep.lookup_color ("background-color", &color))
    gdk_rgba_parse (&self->sep_color, color.c_str ());

  // Hover / checked: .overview-icon
  StEngine::Node item (*engine, &dash, "", "dash-item-container", "");
  StEngine::Node tile (*engine, &item, "", "overview-tile", "hover");
  StEngine::Node icon (*engine, &tile, "", "overview-icon", "");
  if (icon.lookup_color ("background-color", &color))
    gdk_rgba_parse (&self->tile_hover_color, color.c_str ());

  StEngine::Node tile_chk (*engine, &item, "", "overview-tile", "checked");
  StEngine::Node icon_chk (*engine, &tile_chk, "", "overview-icon", "");
  if (icon_chk.lookup_color ("background-color", &color))
    gdk_rgba_parse (&self->tile_checked_color, color.c_str ());

  StEngine::Node tile_plain (*engine, &item, "", "overview-tile", "");
  StEngine::Node icon_plain (*engine, &tile_plain, "", "overview-icon", "");
  if (icon_plain.lookup_color ("color", &color))
    gdk_rgba_parse (&self->fg_color, color.c_str ());
}

// --- app discovery ----------------------------------------------------------

bool
dash_is_our_process (HWND hwnd)
{
  DWORD pid = 0;
  GetWindowThreadProcessId (hwnd, &pid);
  return pid == GetCurrentProcessId ();
}

bool
dash_is_alt_tab_window (HWND hwnd)
{
  if (!IsWindowVisible (hwnd))
    return false;

  HWND try_ = GetAncestor (hwnd, GA_ROOTOWNER);
  HWND walk = nullptr;
  while (try_ != walk) {
    walk = try_;
    try_ = GetLastActivePopup (walk);
    if (IsWindowVisible (try_))
      break;
  }
  if (walk != hwnd)
    return false;

  if (GetWindowLongPtrW (hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
    return false;

  return true;
}

bool
dash_is_cloaked (HWND hwnd)
{
  BOOL cloaked = FALSE;
  DwmGetWindowAttribute (hwnd, DWMWA_CLOAKED, &cloaked, sizeof (cloaked));
  return cloaked != FALSE;
}

std::wstring
window_aumid (HWND hwnd)
{
  std::wstring aumid;
  IPropertyStore *store = nullptr;
  // Local definition to dodge propsys propagation into every TU.
  struct WinomePropKey {
    GUID fmtid;
    DWORD pid;
  };
  WinomePropKey key{kPKEY_AppUserModel_ID_fmt, 5};

  if (SUCCEEDED (SHGetPropertyStoreForWindow (hwnd, IID_PPV_ARGS (&store)))) {
    PROPVARIANT pv;
    PropVariantInit (&pv);
    if (SUCCEEDED (store->GetValue (
            *reinterpret_cast<PROPERTYKEY *> (&key), &pv)) &&
        (pv.vt == VT_LPWSTR) && pv.pwszVal != nullptr) {
      aumid = pv.pwszVal;
    }
    PropVariantClear (&pv);
    store->Release ();
  }
  return aumid;
}

std::wstring
window_exe_path (HWND hwnd)
{
  DWORD pid = 0;
  GetWindowThreadProcessId (hwnd, &pid);
  // `return {}` rather than `return L""`: GCC's LTO whole-program analysis
  // misjudges the empty wide literal's length and emits -Wstringop-overread
  // while inlining the wstring copy. Behaviour is identical.
  if (pid == 0)
    return {};

  HANDLE process = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (process == nullptr)
    return {};

  wchar_t path[MAX_PATH];
  DWORD size = MAX_PATH;
  std::wstring result;
  if (QueryFullProcessImageNameW (process, 0, path, &size))
    result.assign (path, size);
  CloseHandle (process);
  return result;
}

std::string
utf8 (const std::wstring &w)
{
  if (w.empty ())
    return {};
  int n = WideCharToMultiByte (CP_UTF8, 0, w.c_str (), (int)w.size (),
                               nullptr, 0, nullptr, nullptr);
  std::string out (n, 0);
  WideCharToMultiByte (CP_UTF8, 0, w.c_str (), (int)w.size (), out.data (),
                       n, nullptr, nullptr);
  return out;
}

std::string
exe_file_description (const std::wstring &exe)
{
  DWORD zero = 0;
  DWORD size = GetFileVersionInfoSizeW (exe.c_str (), &zero);
  if (size == 0)
    return {};
  std::vector<char> buffer (size);
  if (!GetFileVersionInfoW (exe.c_str (), 0, size, buffer.data ()))
    return {};

  struct LangCodePage {
    WORD language;
    WORD code_page;
  } *langs = nullptr;
  UINT count = 0;
  std::string result;
  if (VerQueryValueW (buffer.data (), L"\\VarFileInfo\\Translation",
                      (LPVOID *)&langs, &count) &&
      count > 0) {
    wchar_t query[128];
    swprintf (query, 128, L"\\StringFileInfo\\%04x%04x\\FileDescription",
              langs[0].language, langs[0].code_page);
    wchar_t *desc = nullptr;
    UINT len = 0;
    if (VerQueryValueW (buffer.data (), query, (LPVOID *)&desc, &len) &&
        desc != nullptr)
      result = utf8 (desc);
  }
  return result;
}

struct EnumCtx {
  HMONITOR monitor;
  std::vector<HWND> *windows;
};

BOOL CALLBACK
dash_collect (HWND hwnd, LPARAM lparam)
{
  EnumCtx *ctx = reinterpret_cast<EnumCtx *> (lparam);
  if (dash_is_our_process (hwnd) || dash_is_cloaked (hwnd) ||
      !dash_is_alt_tab_window (hwnd))
    return TRUE;

  RECT r = {};
  if (IsIconic (hwnd)) {
    WINDOWPLACEMENT wp {};
    wp.length = sizeof (wp);
    if (GetWindowPlacement (hwnd, &wp))
      r = wp.rcNormalPosition;
  } else {
    GetWindowRect (hwnd, &r);
  }
  if (r.right <= r.left || r.bottom <= r.top)
    return TRUE;
  if (MonitorFromRect (&r, MONITOR_DEFAULTTONULL) != ctx->monitor)
    return TRUE;

  ctx->windows->push_back (hwnd);
  return TRUE;
}

void
clear_apps (WinomeDash *self)
{
  for (DashApp &app : self->apps) {
    if (app.icon != nullptr)
      g_object_unref (app.icon);
  }
  self->apps.clear ();
}

void
recompute_layout (WinomeDash *self)
{
  self->tile_rects.clear ();
  self->separator_x = -1;

  int n_apps = (int)self->apps.size ();
  int tiles = n_apps + 1; // apps + show-apps
  int tile = self->icon_size + 2 * kTilePad;
  bool has_split =
      self->pinned_count > 0 && self->pinned_count < n_apps;

  self->pill_w = 2 * kDashPadH + tiles * tile + (tiles - 1) * kItemGap +
                 (has_split ? kSeparatorGap : 0);
  self->pill_h = 2 * kDashPadV + tile;
  self->pill_x = kDashSidePadding;
  self->pill_y = 0;

  double x = self->pill_x + kDashPadH;
  double y = self->pill_y + kDashPadV;
  for (int i = 0; i < tiles; ++i) {
    if (has_split && i == self->pinned_count) {
      self->separator_x = x + (kSeparatorGap - kItemGap) / 2.0 - 0.5;
      x += kSeparatorGap;
    }
    self->tile_rects.push_back ({x, y, (double)tile, (double)tile});
    x += tile + kItemGap;
  }
}

void
place_label (WinomeDash *self, int index)
{
  if (index < 0 || index >= (int)self->tile_rects.size ())
    return;

  DashApp *app =
      index < (int)self->apps.size () ? &self->apps[index] : nullptr;
  const char *text = app ? app->name.c_str ()
                         : winome::tr ("Show Apps", "显示应用");
  gtk_label_set_text (GTK_LABEL (self->label), text);

  GtkRequisition natural;
  gtk_widget_get_preferred_size (self->label, nullptr, &natural);

  double tile_cx = self->tile_rects[index].x + self->tile_rects[index].w / 2;
  double tile_top = self->tile_rects[index].y;

  graphene_point_t src_pt = GRAPHENE_POINT_INIT (0.f, 0.f);
  graphene_point_t dst_pt = GRAPHENE_POINT_INIT (0.f, 0.f);
  if (gtk_widget_compute_point (GTK_WIDGET (self), self->root_fixed,
                                &src_pt, &dst_pt)) {
    double cx = dst_pt.x + tile_cx;
    double top = dst_pt.y + tile_top;
    double x = cx - natural.width / 2.0;
    int root_w = gtk_widget_get_width (self->root_fixed);
    if (root_w > 0) {
      double max_x = std::max (8.0, (double)root_w - natural.width - 8.0);
      x = std::clamp (x, 8.0, max_x);
    }
    double y = std::max (8.0, top - natural.height - kLabelOffset);
    gtk_fixed_move (GTK_FIXED (self->root_fixed), self->label,
                    (int)x, (int)y);
  }

  uint64_t gen = ++self->label_anim_gen;
  g_object_set_data (G_OBJECT (self->label), "dash-fade-gen",
                     GUINT_TO_POINTER ((guint)gen));
  double start_opacity = gtk_widget_get_visible (self->label)
                             ? gtk_widget_get_opacity (self->label)
                             : 0.0;
  gtk_widget_set_opacity (self->label, start_opacity);
  gtk_widget_set_visible (self->label, TRUE);
  tween_opacity (self->label, start_opacity, 1.0, kLabelShowMs,
                 Ease::ease_out_quad);
}

void
hide_label (WinomeDash *self)
{
  if (self->label_timeout != 0) {
    g_source_remove (self->label_timeout);
    self->label_timeout = 0;
  }
  if (self->label == nullptr || !gtk_widget_get_visible (self->label))
    return;

  GtkWidget *lbl = self->label;
  uint64_t gen = ++self->label_anim_gen;
  g_object_set_data (G_OBJECT (lbl), "dash-fade-gen",
                     GUINT_TO_POINTER ((guint)gen));
  tween_opacity (lbl, gtk_widget_get_opacity (lbl), 0.0, kLabelHideMs,
                 Ease::ease_out_quad, [lbl, gen] {
                   if (GPOINTER_TO_UINT (g_object_get_data (
                           G_OBJECT (lbl), "dash-fade-gen")) == (guint)gen) {
                     gtk_widget_set_visible (lbl, FALSE);
                   }
                 });
}

gboolean
label_timeout_cb (gpointer user_data)
{
  WinomeDash *self = WINOME_DASH (user_data);
  self->label_timeout = 0;
  place_label (self, self->hover_index);
  return G_SOURCE_REMOVE;
}

int
tile_at (WinomeDash *self, double x, double y)
{
  for (int i = 0; i < (int)self->tile_rects.size (); ++i) {
    const DashTileRect &r = self->tile_rects[i];
    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
      return i;
  }
  return -1;
}

void
activate_app (WinomeDash *self, int index)
{
  if (index < 0 || index >= (int)self->apps.size ())
    return;

  HWND best = nullptr;
  for (HWND hwnd : self->apps[index].windows) {
    if (!IsWindow (hwnd))
      continue;
    best = hwnd;
    break;
  }
  if (best == nullptr) {
    if (app_index_launch_id (self->apps[index].id, false) ||
        (!self->apps[index].exe_path.empty () &&
         reinterpret_cast<INT_PTR> (ShellExecuteW (
             nullptr, L"open", self->apps[index].exe_path.c_str (), nullptr,
             nullptr, SW_SHOWNORMAL)) > 32)) {
      close_overview ("dash-launch-favorite");
    }
    return;
  }

  if (IsIconic (best))
    ShowWindow (best, SW_RESTORE);
  SetForegroundWindow (best);
  close_overview ("dash-activate");
}

void
open_start_menu_and_close ()
{
  close_overview ("show-apps");
  open_start_menu ();
}

void
activate_show_apps (WinomeDash *self, GdkModifierType mods)
{
  if (!(mods & GDK_SHIFT_MASK) && self->show_apps_cb != nullptr) {
    self->show_apps_cb (self->show_apps_data);
    return;
  }
  open_start_menu_and_close ();
}

struct DashMenuCtx {
  WinomeDash *self = nullptr;
  std::string app_id;
  std::wstring exe_path;
  std::vector<HWND> windows;
  int index = -1;
};

gboolean
deferred_notify_changed (gpointer user_data)
{
  WinomeDash *self = static_cast<WinomeDash *> (user_data);
  if (GTK_IS_WIDGET (self) && self->changed_cb != nullptr)
    self->changed_cb (self->changed_data);
  g_object_unref (self);
  return G_SOURCE_REMOVE;
}

GtkWidget *
make_dash_menu_btn (const char *text, GCallback cb, DashMenuCtx *ctx)
{
  GtkWidget *btn = gtk_button_new_with_label (text);
  gtk_widget_set_name (btn, "winome-dash-ctx-item");
  gtk_widget_add_css_class (btn, "popup-menu-item");
  GtkWidget *lbl = gtk_button_get_child (GTK_BUTTON (btn));
  if (GTK_IS_LABEL (lbl))
    gtk_label_set_xalign (GTK_LABEL (lbl), 0.0f);
  g_signal_connect (btn, "clicked", cb, ctx);
  return btn;
}

void
on_dash_menu_activate (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  auto *ctx = static_cast<DashMenuCtx *> (user_data);
  overview_dash_close_menu (GTK_WIDGET (ctx->self));
  activate_app (ctx->self, ctx->index);
}

void
on_dash_menu_new_window (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  auto *ctx = static_cast<DashMenuCtx *> (user_data);
  overview_dash_close_menu (GTK_WIDGET (ctx->self));
  if (app_index_launch_id (ctx->app_id, false) ||
      (!ctx->exe_path.empty () &&
       reinterpret_cast<INT_PTR> (ShellExecuteW (
           nullptr, L"open", ctx->exe_path.c_str (), nullptr, nullptr,
           SW_SHOWNORMAL)) > 32)) {
    close_overview ("dash-new-window");
  }
}

void
on_dash_menu_toggle_pin (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  auto *ctx = static_cast<DashMenuCtx *> (user_data);
  WinomeDash *self = ctx->self;
  toggle_app_id_pinned (ctx->app_id);
  overview_dash_close_menu (GTK_WIDGET (self));
  overview_dash_repopulate (GTK_WIDGET (self), self->last_monitor,
                            self->last_max_height);
  if (self->changed_cb != nullptr)
    self->changed_cb (self->changed_data);
}

void
on_dash_menu_close_all (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  auto *ctx = static_cast<DashMenuCtx *> (user_data);
  WinomeDash *self = ctx->self;
  for (HWND hwnd : ctx->windows) {
    if (IsWindow (hwnd))
      PostMessageW (hwnd, WM_CLOSE, 0, 0);
  }
  overview_dash_close_menu (GTK_WIDGET (self));
  g_object_ref (self);
  g_timeout_add (140, deferred_notify_changed, self);
}

void
show_dash_context_menu (WinomeDash *self, int index)
{
  if (index < 0 || index >= (int)self->apps.size () ||
      index >= (int)self->tile_rects.size ()) {
    return;
  }

  overview_dash_close_menu (GTK_WIDGET (self));
  hide_label (self);

  const DashApp &app = self->apps[index];
  const DashTileRect &r = self->tile_rects[index];

  GtkWidget *popover = gtk_popover_new ();
  gtk_widget_add_css_class (popover, "winome-context-popover");
  gtk_widget_set_parent (popover, GTK_WIDGET (self));
  gtk_popover_set_has_arrow (GTK_POPOVER (popover), TRUE);
  gtk_popover_set_position (GTK_POPOVER (popover), GTK_POS_TOP);
  gtk_popover_set_autohide (GTK_POPOVER (popover), TRUE);
  GdkRectangle target = {(int)r.x, (int)r.y, (int)r.w, (int)r.h};
  gtk_popover_set_pointing_to (GTK_POPOVER (popover), &target);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);

  auto *ctx = new DashMenuCtx{self, app.id, app.exe_path, app.windows, index};
  g_object_set_data_full (
      G_OBJECT (popover), "winome-dash-ctx", ctx,
      [] (gpointer p) { delete static_cast<DashMenuCtx *> (p); });

  bool is_running = !app.windows.empty ();
  gtk_box_append (
      GTK_BOX (box),
      make_dash_menu_btn (is_running ? tr ("Show Window", "显示窗口")
                                     : tr ("Open", "打开"),
                          G_CALLBACK (on_dash_menu_activate), ctx));

  if (is_running) {
    AppEntry tmp;
    if (!app.exe_path.empty () || app_index_find_by_id (app.id, &tmp)) {
      gtk_box_append (
          GTK_BOX (box),
          make_dash_menu_btn (tr ("New Window", "新建窗口"),
                              G_CALLBACK (on_dash_menu_new_window), ctx));
    }
  }

  WinomeConfig cfg = get_config ();
  bool pinned = app.pinned || is_app_id_pinned (cfg.pinned_apps, app.id);
  gtk_box_append (
      GTK_BOX (box),
      make_dash_menu_btn (pinned ? tr ("Unpin from Dash", "从程序坞取消固定")
                                 : tr ("Pin to Dash", "固定到程序坞"),
                          G_CALLBACK (on_dash_menu_toggle_pin), ctx));

  if (is_running) {
    gtk_box_append (
        GTK_BOX (box),
        make_dash_menu_btn (tr ("Close All Windows", "关闭全部窗口"),
                            G_CALLBACK (on_dash_menu_close_all), ctx));
  }

  gtk_popover_set_child (GTK_POPOVER (popover), box);
  g_signal_connect (
      popover, "closed", G_CALLBACK (+[] (GtkPopover *p, gpointer d) {
        auto *dash = WINOME_DASH (d);
        if (dash->active_menu == GTK_WIDGET (p))
          dash->active_menu = nullptr;
        if (gtk_widget_get_parent (GTK_WIDGET (p)) != nullptr)
          gtk_widget_unparent (GTK_WIDGET (p));
      }),
      self);
  self->active_menu = popover;
  gtk_popover_popup (GTK_POPOVER (popover));
}

void
on_dash_motion_enter (GtkEventControllerMotion *motion, double x, double y,
                      gpointer user_data)
{
  (void)motion;
  WinomeDash *self = WINOME_DASH (user_data);
  int index = tile_at (self, x, y);
  if (index != self->hover_index) {
    self->hover_index = index;
    gtk_widget_queue_draw (GTK_WIDGET (self));

    if (self->label_timeout != 0) {
      g_source_remove (self->label_timeout);
      self->label_timeout = 0;
    }
    if (index >= 0 && self->active_menu == nullptr)
      self->label_timeout =
          g_timeout_add (kLabelHoverTimeoutMs, label_timeout_cb, self);
    else
      hide_label (self);
  }
}

void
on_dash_motion (GtkEventControllerMotion *motion, double x, double y,
                gpointer user_data)
{
  (void)motion;
  on_dash_motion_enter (nullptr, x, y, user_data);
}

void
on_dash_motion_leave (GtkEventControllerMotion *motion, gpointer user_data)
{
  (void)motion;
  WinomeDash *self = WINOME_DASH (user_data);
  if (self->hover_index != -1) {
    self->hover_index = -1;
    gtk_widget_queue_draw (GTK_WIDGET (self));
  }
  hide_label (self);
}

void
on_dash_click (GtkGestureClick *gesture, int n_press, double x, double y,
               gpointer user_data)
{
  (void)n_press;
  WinomeDash *self = WINOME_DASH (user_data);
  guint button =
      gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));
  int index = tile_at (self, x, y);
  if (index < 0)
    return;

  if (button == GDK_BUTTON_SECONDARY) {
    if (index < (int)self->apps.size ())
      show_dash_context_menu (self, index);
    return;
  }

  if (button != GDK_BUTTON_PRIMARY)
    return;

  if (index < (int)self->apps.size ()) {
    activate_app (self, index);
  } else {
    GdkEvent *ev = gtk_gesture_get_last_event (
        GTK_GESTURE (gesture),
        gtk_gesture_single_get_current_sequence (GTK_GESTURE_SINGLE (gesture)));
    GdkModifierType mods =
        ev != nullptr ? gdk_event_get_modifier_state (ev)
                      : static_cast<GdkModifierType> (0);
    activate_show_apps (self, mods);
  }
}

// --- widget ------------------------------------------------------------------

static void
winome_dash_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  WinomeDash *self = WINOME_DASH (widget);
  resolve_colors (self);

  int w = gtk_widget_get_width (widget);
  int h = gtk_widget_get_height (widget);
  if (w <= 0 || h <= 0)
    return;

  // .dash-background pill.
  graphene_rect_t pill_rect;
  graphene_rect_init (&pill_rect, self->pill_x, self->pill_y, self->pill_w,
                      self->pill_h);
  GskRoundedRect pill;
  gsk_rounded_rect_init_from_rect (&pill, &pill_rect, (float)kDashPillRadius);
  gtk_snapshot_push_rounded_clip (snapshot, &pill);
  gtk_snapshot_append_color (snapshot, &self->pill_color, &pill_rect);
  gtk_snapshot_pop (snapshot);

  // Separator between Pinned Favorites and Unpinned Running Apps.
  if (self->separator_x >= 0) {
    graphene_rect_t sep_rect;
    graphene_rect_init (&sep_rect, (float)self->separator_x,
                        (float)(self->pill_y + kDashPadV), 1.0f,
                        (float)(self->pill_h - 2 * kDashPadV));
    gtk_snapshot_append_color (snapshot, &self->sep_color, &sep_rect);
  }

  // Tiles.
  for (int i = 0; i < (int)self->tile_rects.size (); ++i) {
    const DashTileRect &r = self->tile_rects[i];
    bool is_show_apps = (i == (int)self->apps.size ());

    // .overview-icon tile background (hover / show-apps checked state).
    const GdkRGBA *bg_tile = nullptr;
    if (is_show_apps && self->show_apps_active && i != self->hover_index)
      bg_tile = &self->tile_checked_color;
    else if (i == self->hover_index)
      bg_tile = &self->tile_hover_color;

    if (bg_tile != nullptr) {
      graphene_rect_t rect;
      graphene_rect_init (&rect, r.x, r.y, r.w, r.h);
      GskRoundedRect rounded;
      gsk_rounded_rect_init_from_rect (&rounded, &rect, (float)kTileRadius);
      gtk_snapshot_push_rounded_clip (snapshot, &rounded);
      gtk_snapshot_append_color (snapshot, bg_tile, &rect);
      gtk_snapshot_pop (snapshot);
    }

    // Icon.
    GdkTexture *texture = nullptr;
    if (i < (int)self->apps.size ())
      texture = self->apps[i].icon;

    if (texture != nullptr) {
      graphene_rect_t icon_rect;
      graphene_rect_init (&icon_rect, r.x + kTilePad, r.y + kTilePad,
                          self->icon_size, self->icon_size);
      // Letterbox the texture inside the square.
      double tw = gdk_texture_get_width (texture);
      double th = gdk_texture_get_height (texture);
      double scale = std::min (self->icon_size / tw, self->icon_size / th);
      double dw = tw * scale, dh = th * scale;
      graphene_rect_init (&icon_rect, r.x + kTilePad + (self->icon_size - dw) / 2,
                          r.y + kTilePad + (self->icon_size - dh) / 2, dw, dh);
      gtk_snapshot_append_texture (snapshot, texture, &icon_rect);
    } else if (i == (int)self->apps.size ()) {
      // Show Apps: view-app-grid-symbolic as a crisp 3x3 dot grid.
      double pad = self->icon_size * 0.16;
      double grid = self->icon_size - 2 * pad;
      double dot_r = grid / 8.5;
      double step = grid / 3.0;
      double x0 = r.x + kTilePad + pad, y0 = r.y + kTilePad + pad;
      for (int dy = 0; dy < 3; ++dy) {
        for (int dx = 0; dx < 3; ++dx) {
          graphene_rect_t dot;
          graphene_rect_init (&dot, x0 + dx * step + step / 2 - dot_r,
                              y0 + dy * step + step / 2 - dot_r,
                              2 * dot_r, 2 * dot_r);
          GskRoundedRect rr;
          gsk_rounded_rect_init_from_rect (&rr, &dot, (float)dot_r);
          gtk_snapshot_push_rounded_clip (snapshot, &rr);
          gtk_snapshot_append_color (snapshot, &self->fg_color, &dot);
          gtk_snapshot_pop (snapshot);
        }
      }
    }

    // Running dot: only shown when the app has open windows on this monitor
    // (favorites with no active windows show cleanly without a dot).
    if (i < (int)self->apps.size () && !self->apps[i].windows.empty ()) {
      double cx = r.x + r.w / 2.0;
      double dy = r.y + r.h - kTilePad - kDotSize / 2.0 - 0.5;
      graphene_rect_t dot;
      graphene_rect_init (&dot, cx - kDotSize / 2.0, dy, kDotSize, kDotSize);
      GskRoundedRect rr;
      gsk_rounded_rect_init_from_rect (&rr, &dot, (float)(kDotSize / 2.0));
      gtk_snapshot_push_rounded_clip (snapshot, &rr);
      gtk_snapshot_append_color (snapshot, &self->fg_color, &dot);
      gtk_snapshot_pop (snapshot);
    }
  }
}

static void
winome_dash_measure (GtkWidget *widget, GtkOrientation orientation,
                     int for_size, int *minimum, int *natural,
                     int *minimum_baseline, int *natural_baseline)
{
  (void)for_size;
  WinomeDash *self = WINOME_DASH (widget);
  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    *natural = self->pill_w + 2 * kDashSidePadding;
  else
    *natural = self->pill_h + kDashBottomMargin;
  *minimum = *natural;
  *minimum_baseline = *natural_baseline = -1;
}

static void
winome_dash_init (WinomeDash *self)
{
  self->hover_index = -1;
  gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "default");
}

static void
winome_dash_dispose (GObject *object)
{
  WinomeDash *self = WINOME_DASH (object);
  overview_dash_close_menu (GTK_WIDGET (self));
  if (self->label_timeout != 0) {
    g_source_remove (self->label_timeout);
    self->label_timeout = 0;
  }
  ++self->label_anim_gen;
  if (self->label != nullptr) {
    gtk_widget_set_visible (self->label, FALSE);
    gtk_fixed_remove (GTK_FIXED (self->root_fixed), self->label);
    self->label = nullptr;
  }
  clear_apps (self);
  G_OBJECT_CLASS (winome_dash_parent_class)->dispose (object);
}

static void
winome_dash_class_init (WinomeDashClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  widget_class->snapshot = winome_dash_snapshot;
  widget_class->measure = winome_dash_measure;

  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = winome_dash_dispose;
}

std::string
display_name_for_window (HWND hwnd, const std::wstring &exe)
{
  wchar_t title[256] = L"";
  GetWindowTextW (hwnd, title, 256);

  std::wstring exe_lower = exe;
  std::transform (exe_lower.begin (), exe_lower.end (), exe_lower.begin (),
                  towlower);
  bool is_frame_host =
      exe_lower.find (L"applicationframehost.exe") != std::wstring::npos;

  std::string name;
  if (is_frame_host || exe.empty ())
    name = utf8 (title);
  else {
    name = exe_file_description (exe);
    if (name.empty ())
      name = utf8 (title);
  }
  return name.empty () ? std::string ("Application") : name;
}

std::string
pinned_fallback_label (const std::string &id)
{
  size_t sep = id.find_last_of ("\\/");
  std::string base = (sep != std::string::npos) ? id.substr (sep + 1) : id;
  if (base.size () > 4 &&
      (base.rfind (".exe") == base.size () - 4 ||
       base.rfind (".lnk") == base.size () - 4)) {
    base.resize (base.size () - 4);
  }
  if (!base.empty ())
    base[0] = (char)toupper ((unsigned char)base[0]);
  return base.empty () ? std::string ("App") : base;
}

}  // namespace

// Exported for the overview window-preview hover chrome (windowPreview.js
// shows the window's application icon at ICON_SIZE 64 logical px on hover).
GdkTexture *
winome_window_app_icon (HWND hwnd, int size)
{
  std::wstring exe = window_exe_path (hwnd);
  std::string key = normalize_app_id (utf8 (exe));
  return app_icon_for_window (hwnd, exe, size, key);
}

GtkWidget *
overview_dash_new (GtkWidget *root_fixed)
{
  WinomeDash *self =
      (WinomeDash *)g_object_new (winome_dash_get_type (), nullptr);
  self->root_fixed = root_fixed;

  // Hover label (.dash-label pill), floating above the dash.
  self->label = gtk_label_new ("");
  gtk_widget_add_css_class (self->label, "dash-label");
  gtk_widget_set_visible (self->label, FALSE);
  gtk_fixed_put (GTK_FIXED (root_fixed), self->label, 0, 0);

  // Controllers: motion for hover + label, left/right click for activation/menu.
  GtkEventController *motion =
      gtk_event_controller_motion_new ();
  g_signal_connect (motion, "enter", G_CALLBACK (on_dash_motion_enter), self);
  g_signal_connect (motion, "motion", G_CALLBACK (on_dash_motion), self);
  g_signal_connect (motion, "leave", G_CALLBACK (on_dash_motion_leave), self);
  gtk_widget_add_controller (GTK_WIDGET (self), motion);

  GtkGesture *click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);
  g_signal_connect (click, "released", G_CALLBACK (on_dash_click), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));

  return GTK_WIDGET (self);
}

void
overview_dash_set_show_apps_cb (GtkWidget *dash, void (*cb) (void *),
                                void *user_data)
{
  WinomeDash *self = WINOME_DASH (dash);
  self->show_apps_cb = cb;
  self->show_apps_data = user_data;
}

void
overview_dash_set_apps_active (GtkWidget *dash, bool active)
{
  WinomeDash *self = WINOME_DASH (dash);
  if (self->show_apps_active != active) {
    self->show_apps_active = active;
    gtk_widget_queue_draw (dash);
  }
}

void
overview_dash_set_changed_cb (GtkWidget *dash, void (*cb) (void *),
                              void *user_data)
{
  WinomeDash *self = WINOME_DASH (dash);
  self->changed_cb = cb;
  self->changed_data = user_data;
}

void
overview_dash_close_menu (GtkWidget *dash)
{
  if (dash == nullptr)
    return;
  WinomeDash *self = WINOME_DASH (dash);
  if (self->active_menu != nullptr) {
    GtkWidget *m = self->active_menu;
    self->active_menu = nullptr;
    gtk_popover_popdown (GTK_POPOVER (m));
    if (gtk_widget_get_parent (m) != nullptr)
      gtk_widget_unparent (m);
  }
}

void
overview_dash_repopulate (GtkWidget *dash, HMONITOR monitor, int max_height)
{
  WinomeDash *self = WINOME_DASH (dash);
  overview_dash_close_menu (dash);
  hide_label (self);
  self->hover_index = -1;
  self->last_monitor = monitor;
  self->last_max_height = max_height;
  clear_apps (self);

  // Icon size: largest base size that fits the available dash height.
  double scale = gtk_widget_get_scale_factor (dash);
  int avail = max_height - 2 * kDashPadV - 2 * kTilePad - kDashBottomMargin;
  self->icon_size = kBaseIconSizes[0];
  for (int i = 0; i < kBaseIconSizesCount; ++i) {
    if (kBaseIconSizes[i] <= avail)
      self->icon_size = kBaseIconSizes[i];
  }
  int icon_px = (int)(self->icon_size * scale + 0.5);

  // Enumerate windows top-down and group into running applications.
  std::vector<HWND> windows;
  EnumCtx ctx{monitor, &windows};
  EnumWindows (dash_collect, reinterpret_cast<LPARAM> (&ctx));

  struct RunningGroup {
    std::string id;
    std::wstring exe;
    std::string name;
    std::vector<HWND> windows;
  };
  std::vector<RunningGroup> running;

  for (HWND hwnd : windows) {
    std::wstring aumid = window_aumid (hwnd);
    std::wstring exe = window_exe_path (hwnd);
    std::string id = canonical_app_id (aumid, exe, std::wstring{});
    if (id.empty ())
      continue;

    size_t idx = SIZE_MAX;
    for (size_t i = 0; i < running.size (); ++i) {
      if (running[i].id == id) {
        idx = i;
        break;
      }
    }
    if (idx == SIZE_MAX) {
      RunningGroup g;
      g.id = id;
      g.exe = exe;
      g.name = display_name_for_window (hwnd, exe);
      running.push_back (std::move (g));
      idx = running.size () - 1;
    }
    running[idx].windows.push_back (hwnd);
  }

  WinomeConfig cfg = get_config ();
  std::vector<bool> running_consumed (running.size (), false);

  auto find_running_match = [&] (const std::string &pin_id) -> size_t {
    for (size_t i = 0; i < running.size (); ++i) {
      if (running_consumed[i])
        continue;
      if (running[i].id == pin_id)
        return i;
      if (normalize_app_id (utf8 (running[i].exe)) == pin_id)
        return i;
    }
    return SIZE_MAX;
  };

  // 1. Persisted Favorites (in configured order).
  for (const std::string &raw_pin : cfg.pinned_apps) {
    std::string pin_id = normalize_app_id (raw_pin);
    if (pin_id.empty ())
      continue;

    bool dup = false;
    for (const DashApp &existing : self->apps) {
      if (existing.id == pin_id) {
        dup = true;
        break;
      }
    }
    if (dup)
      continue;

    AppEntry entry;
    bool has_entry = app_index_find_by_id (pin_id, &entry);
    size_t ridx = find_running_match (pin_id);
    if (ridx == SIZE_MAX && has_entry && !entry.target_exe.empty ())
      ridx = find_running_match (normalize_app_id (utf8 (entry.target_exe)));

    DashApp app;
    app.id = pin_id;
    app.pinned = true;

    if (ridx != SIZE_MAX) {
      running_consumed[ridx] = true;
      app.exe_path = running[ridx].exe;
      app.windows = std::move (running[ridx].windows);
      app.name = has_entry ? entry.name : running[ridx].name;
      app.icon = app_icon_for_window (app.windows[0], app.exe_path, icon_px,
                                      app.id);
    } else if (has_entry) {
      app.exe_path = entry.target_exe;
      app.name = entry.name;
      app.icon = app_icon_for_entry (entry, icon_px);
    } else {
      std::wstring wpath = app_utf8_to_wide (pin_id);
      if (GetFileAttributesW (wpath.c_str ()) == INVALID_FILE_ATTRIBUTES)
        continue; // ignore stale uninstalled favorite path
      app.exe_path = wpath;
      app.name = pinned_fallback_label (pin_id);
      app.icon = app_icon_for_path (wpath, icon_px, pin_id);
    }

    self->apps.push_back (std::move (app));
  }
  self->pinned_count = (int)self->apps.size ();

  // 2. Remaining running apps not in favorites.
  for (size_t i = 0; i < running.size (); ++i) {
    if (running_consumed[i])
      continue;
    RunningGroup &g = running[i];
    DashApp app;
    app.id = g.id;
    app.exe_path = g.exe;
    app.name = g.name;
    app.windows = std::move (g.windows);
    app.pinned = false;
    app.icon =
        app_icon_for_window (app.windows[0], app.exe_path, icon_px, app.id);
    self->apps.push_back (std::move (app));
  }

  recompute_layout (self);
  gtk_widget_queue_resize (dash);
}

int
overview_dash_get_width (GtkWidget *dash)
{
  WinomeDash *self = WINOME_DASH (dash);
  return self->pill_w + 2 * kDashSidePadding;
}

int
overview_dash_get_height (GtkWidget *dash)
{
  WinomeDash *self = WINOME_DASH (dash);
  return self->pill_h + kDashBottomMargin;
}

}  // namespace winome
