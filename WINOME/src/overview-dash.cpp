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
#include "overview.h"
#include "app-index.h"
#include "config.h"
#include "easing.h"
#include "fonts.h"
#include "host-utils.h"
#include "i18n.h"
#include "monitor-manager.h"
#include "overview-secondary.h"
#include "overview-trigger.h"
#include "st-engine.h"
#include "system-status.h"
#include "virtual-desktop.h"
#include "workspace-manager.h"

#include <gdk/gdk.h>
#include <gdk/win32/gdkwin32.h>
#include <dwmapi.h>

#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cwchar>
#include <new>
#include <string>
#include <vector>
#include <wchar.h>

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
  std::vector<int> window_desktops; // parallel to windows (-1 if unknown)
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
  int item_gap = kItemGap; // logical px horizontal tile spacing

  int pill_x = 0, pill_y = 0, pill_w = 0, pill_h = 0; // widget-relative
  std::vector<DashTileRect> tile_rects;
  int hover_index = -1;    // -1 = none
  int pressed_index = -1;  // -1 = none
  bool show_apps_active = false;

  HMONITOR last_monitor = nullptr;
  int last_max_height = 96;
  int last_active_desktop = -1;

  void (*show_apps_cb) (void *) = nullptr;
  void *show_apps_data = nullptr;
  void (*changed_cb) (void *) = nullptr;
  void *changed_data = nullptr;

  GdkRGBA pill_color;
  GdkRGBA pill_border_color;
  GdkRGBA tile_color;
  GdkRGBA tile_hover_color;
  GdkRGBA tile_active_color;
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
  self->pill_border_color = (GdkRGBA){0.98f, 0.98f, 0.98f, 0.08f};
  self->tile_color = self->pill_color;                        // flat on dash
  self->tile_hover_color = (GdkRGBA){0.286f, 0.286f, 0.302f, 1.0f}; // #49494d
  self->tile_active_color = (GdkRGBA){0.345f, 0.345f, 0.365f, 1.0f}; // #58585d
  self->tile_checked_color = (GdkRGBA){0.294f, 0.294f, 0.310f, 1.0f}; // #4b4b4f
  self->sep_color = (GdkRGBA){0.98f, 0.98f, 0.98f, 0.14f};
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

DWORD
dash_cloaked_flags (HWND hwnd)
{
  DWORD cloaked = 0;
  DwmGetWindowAttribute (hwnd, DWMWA_CLOAKED, &cloaked, sizeof (cloaked));
  return cloaked;
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
  if (n <= 0)
    return {};
  std::string out (n, 0);
  WideCharToMultiByte (CP_UTF8, 0, w.c_str (), (int)w.size (), out.data (),
                       n, nullptr, nullptr);
  return out;
}

std::string
exe_file_description (const std::wstring &exe)
{
  if (exe.empty ())
    return {};
  DWORD zero = 0;
  DWORD size = GetFileVersionInfoSizeW (exe.c_str (), &zero);
  if (size == 0)
    return {};
  std::vector<char> buffer (size);
  if (!GetFileVersionInfoW (exe.c_str (), 0, size, buffer.data ()))
    return {};

  const char *buf_begin = buffer.data ();
  const char *buf_end = buf_begin + size;

  struct LangCodePage {
    WORD language;
    WORD code_page;
  } *langs = nullptr;
  UINT count = 0;
  std::string result;
  if (VerQueryValueW (buffer.data (), L"\\VarFileInfo\\Translation",
                      reinterpret_cast<LPVOID *> (&langs), &count) &&
      langs != nullptr && count >= sizeof (LangCodePage) &&
      reinterpret_cast<const char *> (langs) >= buf_begin &&
      reinterpret_cast<const char *> (langs) + sizeof (LangCodePage) <= buf_end) {
    wchar_t query[128];
    std::swprintf (query, 128, L"\\StringFileInfo\\%04x%04x\\FileDescription",
                   langs[0].language, langs[0].code_page);
    wchar_t *desc = nullptr;
    UINT len = 0;
    if (VerQueryValueW (buffer.data (), query,
                        reinterpret_cast<LPVOID *> (&desc), &len) &&
        desc != nullptr && len > 0) {
      const char *desc_bytes = reinterpret_cast<const char *> (desc);
      if (desc_bytes >= buf_begin && desc_bytes < buf_end) {
        size_t max_wchars = std::min<size_t> (
            static_cast<size_t> (len),
            static_cast<size_t> (buf_end - desc_bytes) / sizeof (wchar_t));
        size_t bounded = ::wcsnlen (desc, max_wchars);
        if (bounded > 0)
          result = utf8 (std::wstring (desc, bounded));
      }
    }
  }
  return result;
}

struct DashWindowEntry {
  HWND hwnd = nullptr;
  int desktop_index = -1;
};

struct EnumCtx {
  HMONITOR monitor = nullptr;
  std::vector<DashWindowEntry> *windows = nullptr;
  bool vd_available = false;
  bool isolate_workspaces = false;
  int target_desktop = -1;
  bool use_blacklist = true;
};

BOOL CALLBACK
dash_collect (HWND hwnd, LPARAM lparam)
{
  EnumCtx *ctx = reinterpret_cast<EnumCtx *> (lparam);
  if (dash_is_our_process (hwnd) || !dash_is_alt_tab_window (hwnd) ||
      is_overview_default_blacklisted_hwnd (hwnd, ctx->use_blacklist))
    return TRUE;

  const DWORD cloaked = dash_cloaked_flags (hwnd);
  int win_ws = -1;
  bool on_cur = false;
  if (ctx->vd_available) {
    win_ws = ws::window_workspace (hwnd, ctx->monitor);
    if (win_ws < 0 && ctx->isolate_workspaces)
      on_cur = ws::window_on_active_workspace (hwnd, ctx->monitor);
  }
  if (!should_include_dash_window (ctx->isolate_workspaces, ctx->vd_available,
                                   ctx->target_desktop, win_ws, on_cur,
                                   cloaked)) {
    return TRUE;
  }

  RECT r = {};
  if (IsIconic (hwnd)) {
    WINDOWPLACEMENT wp {};
    wp.length = sizeof (wp);
    if (GetWindowPlacement (hwnd, &wp))
      r = wp.rcNormalPosition;
  } else if (!SUCCEEDED (DwmGetWindowAttribute (
                 hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof (r)))) {
    GetWindowRect (hwnd, &r);
  }
  if (r.right <= r.left || r.bottom <= r.top)
    return TRUE;
  // GNOME Shell dash.js shows running apps across all monitors so users can
  // focus/switch running applications regardless of which monitor hosts the
  // overview.

  ctx->windows->push_back (DashWindowEntry{hwnd, win_ws});
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

int
dash_total_width_for (int n_apps, int icon_size, int item_gap, bool has_split)
{
  int tiles = std::max (n_apps, 0) + 1;
  int tile = icon_size + 2 * kTilePad;
  int pill_w = 2 * kDashPadH + tiles * tile +
               std::max (tiles - 1, 0) * item_gap +
               (has_split ? kSeparatorGap : 0);
  return pill_w + 2 * kDashSidePadding;
}

int
dash_monitor_logical_width (HMONITOR monitor, int scale)
{
  int safe_scale = std::max (scale, 1);
  int mw = 0;
  if (monitor != nullptr) {
    MONITORINFO mi{};
    mi.cbSize = sizeof (mi);
    if (GetMonitorInfoW (monitor, &mi))
      mw = mi.rcMonitor.right - mi.rcMonitor.left;
  }
  if (mw <= 0)
    mw = GetSystemMetrics (SM_CXSCREEN);
  return mw > 0 ? std::max (mw / safe_scale, 1) : 0;
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

  self->pill_w = 2 * kDashPadH + tiles * tile +
                 std::max (tiles - 1, 0) * self->item_gap +
                 (has_split ? kSeparatorGap : 0);
  self->pill_h = 2 * kDashPadV + tile;
  self->pill_x = kDashSidePadding;
  self->pill_y = 0;

  double x = self->pill_x + kDashPadH;
  double y = self->pill_y + kDashPadV;
  for (int i = 0; i < tiles; ++i) {
    if (has_split && i == self->pinned_count) {
      self->separator_x = x + (kSeparatorGap - self->item_gap) / 2.0 - 0.5;
      x += kSeparatorGap;
    }
    self->tile_rects.push_back ({x, y, (double)tile, (double)tile});
    x += tile + self->item_gap;
  }
}

void
place_label (WinomeDash *self, int index)
{
  if (self->label == nullptr || self->root_fixed == nullptr ||
      index < 0 || index >= (int)self->tile_rects.size ())
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
  if (gtk_widget_get_parent (self->label) == self->root_fixed &&
      gtk_widget_compute_point (GTK_WIDGET (self), self->root_fixed,
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
  if (self->root_fixed != nullptr &&
      gtk_widget_get_parent (self->label) == self->root_fixed) {
    // Keep .dash-label above g_ws_bg / g_app_grid_clip inside root GtkFixed.
    gtk_widget_insert_before (self->label, self->root_fixed, nullptr);
  }
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
dismiss_overview_from_dash (WinomeDash *self, const char *reason)
{
  if (!get_config ().multi_monitor_overview_linked && self != nullptr &&
      self->last_monitor != nullptr &&
      overview_secondary_is_open_for_monitor (self->last_monitor) &&
      self->last_monitor != overview_main_monitor ()) {
    overview_close_secondary_for_monitor (self->last_monitor);
    return;
  }
  close_overview (reason);
}

void
activate_app (WinomeDash *self, int index)
{
  if (index < 0 || index >= (int)self->apps.size ())
    return;

  const DashApp &app = self->apps[index];
  const int cur_ws =
      self->last_active_desktop >= 0 ? self->last_active_desktop
                                     : ws::active_for_monitor (nullptr);
  HWND best = nullptr;
  int best_ws = -1;
  // Prefer a window on the currently active workspace if available; otherwise
  // fall back to the topmost window across all workspaces and switch to it.
  for (size_t i = 0; i < app.windows.size (); ++i) {
    HWND hwnd = app.windows[i];
    if (!IsWindow (hwnd))
      continue;
    int ws = (i < app.window_desktops.size ()) ? app.window_desktops[i] : -1;
    if (best == nullptr) {
      best = hwnd;
      best_ws = ws;
    }
    if (cur_ws >= 0 && ws == cur_ws) {
      best = hwnd;
      best_ws = ws;
      break;
    }
  }
  if (best == nullptr) {
    overview_discard_restore_focus ();
    HMONITOR target_mon =
        (self != nullptr && self->last_monitor != nullptr)
            ? self->last_monitor
            : monitor_manager_monitor_for_widget (GTK_WIDGET (self));
    bool launched = app_index_launch_id (app.id, false, target_mon);
    if (!launched && !app.exe_path.empty ()) {
      watch_launched_window_for_monitor (target_mon);
      SHELLEXECUTEINFOW sei {};
      sei.cbSize = sizeof (sei);
      sei.fMask = SEE_MASK_FLAG_NO_UI;
      if (target_mon != nullptr) {
        sei.fMask |= SEE_MASK_HMONITOR;
        sei.hMonitor = target_mon;
      }
      sei.lpVerb = L"open";
      sei.lpFile = app.exe_path.c_str ();
      sei.nShow = SW_SHOWNORMAL;
      launched = ShellExecuteExW (&sei) != FALSE;
    }
    if (launched)
      dismiss_overview_from_dash (self, "dash-launch-favorite");
    return;
  }

  overview_discard_restore_focus ();
  HMONITOR win_mon = MonitorFromWindow (best, MONITOR_DEFAULTTONEAREST);
  if (best_ws < 0 && vd::available ())
    best_ws = ws::window_workspace (best, win_mon);
  if (best_ws >= 0 && best_ws != ws::active_for_monitor (win_mon))
    ws::switch_to (win_mon, best_ws);
  if (IsIconic (best))
    ShowWindow (best, SW_RESTORE);
  SetForegroundWindow (best);
  dismiss_overview_from_dash (self, "dash-activate");
}

void
open_start_menu_and_close ()
{
  overview_discard_restore_focus ();
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
  gtk_widget_set_focus_on_click (btn, FALSE);
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
  WinomeDash *self = ctx->self;
  int index = ctx->index;
  overview_dash_close_menu (GTK_WIDGET (self));
  activate_app (self, index);
}

void
on_dash_menu_new_window (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  auto *ctx = static_cast<DashMenuCtx *> (user_data);
  WinomeDash *self = ctx->self;
  std::string app_id = ctx->app_id;
  std::wstring exe_path = ctx->exe_path;
  overview_dash_close_menu (GTK_WIDGET (self));
  overview_discard_restore_focus ();
  HMONITOR target_mon =
      (self != nullptr && self->last_monitor != nullptr)
          ? self->last_monitor
          : monitor_manager_monitor_for_widget (GTK_WIDGET (self));
  bool launched = app_index_launch_id (app_id, false, target_mon);
  if (!launched && !exe_path.empty ()) {
    watch_launched_window_for_monitor (target_mon);
    SHELLEXECUTEINFOW sei {};
    sei.cbSize = sizeof (sei);
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    if (target_mon != nullptr) {
      sei.fMask |= SEE_MASK_HMONITOR;
      sei.hMonitor = target_mon;
    }
    sei.lpVerb = L"open";
    sei.lpFile = exe_path.c_str ();
    sei.nShow = SW_SHOWNORMAL;
    launched = ShellExecuteExW (&sei) != FALSE;
  }
  if (launched)
    dismiss_overview_from_dash (self, "dash-new-window");
}

void
on_dash_menu_toggle_pin (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  auto *ctx = static_cast<DashMenuCtx *> (user_data);
  WinomeDash *self = ctx->self;
  toggle_app_id_pinned (ctx->app_id);
  overview_dash_close_menu (GTK_WIDGET (self));
  overview_dash_repopulate (GTK_WIDGET (self), self->last_monitor,
                            self->last_max_height, self->last_active_desktop);
  if (self->changed_cb != nullptr)
    self->changed_cb (self->changed_data);
}

void
on_dash_menu_close_all (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  auto *ctx = static_cast<DashMenuCtx *> (user_data);
  WinomeDash *self = ctx->self;
  std::vector<HWND> hwnds = ctx->windows;
  overview_dash_close_menu (GTK_WIDGET (self));
  app_index_close_windows (hwnds);
  g_object_ref (self);
  g_timeout_add (140, deferred_notify_changed, self);
}

struct DashWindowCtx {
  WinomeDash *self = nullptr;
  HWND hwnd = nullptr;
  int desktop_index = -1;
};

void
on_dash_menu_window (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  auto *wctx = static_cast<DashWindowCtx *> (user_data);
  WinomeDash *self = wctx->self;
  HWND hwnd = wctx->hwnd;
  int ws = wctx->desktop_index;
  overview_dash_close_menu (GTK_WIDGET (self));
  overview_discard_restore_focus ();
  HMONITOR win_mon = MonitorFromWindow (hwnd, MONITOR_DEFAULTTONEAREST);
  if (ws < 0 && vd::available ())
    ws = ws::window_workspace (hwnd, win_mon);
  if (ws >= 0 && ws != ws::active_for_monitor (win_mon))
    ws::switch_to (win_mon, ws);
  if (app_index_activate_hwnd (hwnd))
    dismiss_overview_from_dash (self, "dash-activate-window");
}

void
on_dash_menu_location (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  auto *ctx = static_cast<DashMenuCtx *> (user_data);
  WinomeDash *self = ctx->self;
  std::string app_id = ctx->app_id;
  overview_dash_close_menu (GTK_WIDGET (self));
  AppEntry entry;
  if (app_index_find_by_id (app_id, &entry)) {
    overview_discard_restore_focus ();
    if (app_index_open_location (entry))
      close_overview ("dash-open-location");
  }
}

GtkWidget *
make_dash_sep (void)
{
  GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class (sep, "popup-separator-menu-item");
  return sep;
}

static void
promote_dash_popover_hwnd_topmost (GtkWidget *popover)
{
  if (popover == nullptr || !gtk_widget_get_visible (popover))
    return;
  GdkSurface *surf = gtk_native_get_surface (GTK_NATIVE (popover));
  if (surf == nullptr)
    return;
  HWND hwnd = static_cast<HWND> (gdk_win32_surface_get_handle (surf));
  if (hwnd != nullptr && IsWindow (hwnd)) {
    LONG_PTR ex = GetWindowLongPtrW (hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_TOPMOST) == 0)
      SetWindowLongPtrW (hwnd, GWL_EXSTYLE, ex | WS_EX_TOPMOST);
    SetWindowPos (hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                      SWP_NOOWNERZORDER);
  }
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
  install_bundled_fonts (popover);
  gtk_widget_add_css_class (popover, "winome-context-popover");
  gtk_widget_set_parent (popover, GTK_WIDGET (self));
  gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
  gtk_popover_set_position (GTK_POPOVER (popover), GTK_POS_TOP);
  gtk_popover_set_autohide (GTK_POPOVER (popover), TRUE);
  GdkRectangle target = {(int)r.x, (int)r.y - 6, (int)r.w, (int)r.h};
  gtk_popover_set_pointing_to (GTK_POPOVER (popover), &target);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);

  auto *ctx = new DashMenuCtx{self, app.id, app.exe_path, app.windows, index};
  g_object_set_data_full (
      G_OBJECT (popover), "winome-dash-ctx", ctx,
      [] (gpointer p) { delete static_cast<DashMenuCtx *> (p); });

  bool is_running = !app.windows.empty ();
  AppEntry matched_entry;
  bool has_entry = app_index_find_by_id (app.id, &matched_entry);

  // Section 1 (upstream AppMenu): open window list when running.
  if (is_running) {
    GtkWidget *hdr = gtk_label_new (tr ("Open Windows", "打开的窗口"));
    gtk_label_set_xalign (GTK_LABEL (hdr), 0.0f);
    gtk_widget_add_css_class (hdr, "popup-menu-item");
    gtk_widget_add_css_class (hdr, "popup-inactive-menu-item");
    gtk_widget_set_sensitive (hdr, FALSE);
    gtk_box_append (GTK_BOX (box), hdr);

    const bool show_ws_tag =
        !get_config ().dash_isolate_workspaces &&
        ws::count_for_monitor (nullptr) > 1;
    const int cur_ws =
        self->last_active_desktop >= 0 ? self->last_active_desktop
                                       : ws::active_for_monitor (nullptr);
    int shown = 0;
    for (size_t wi = 0; wi < app.windows.size () && shown < 8; ++wi) {
      HWND hwnd = app.windows[wi];
      if (!IsWindow (hwnd))
        continue;
      int ws = (wi < app.window_desktops.size ()) ? app.window_desktops[wi] : -1;
      wchar_t wtitle[256] = L"";
      GetWindowTextW (hwnd, wtitle, 256);
      std::string title = app_wide_to_utf8 (wtitle);
      if (title.empty ())
        title = app.name;
      if (show_ws_tag && ws >= 0 && ws != cur_ws) {
        title += std::string (" · ") + tr ("Workspace ", "工作区 ") +
                 std::to_string (ws + 1);
      }
      auto *wctx = new DashWindowCtx{self, hwnd, ws};
      GtkWidget *wbtn = gtk_button_new_with_label (title.c_str ());
      gtk_widget_set_name (wbtn, "winome-dash-ctx-item");
      gtk_widget_add_css_class (wbtn, "popup-menu-item");
      gtk_widget_set_focus_on_click (wbtn, FALSE);
      GtkWidget *lbl = gtk_button_get_child (GTK_BUTTON (wbtn));
      if (GTK_IS_LABEL (lbl)) {
        gtk_label_set_xalign (GTK_LABEL (lbl), 0.0f);
        gtk_label_set_max_width_chars (GTK_LABEL (lbl), 30);
        gtk_label_set_ellipsize (GTK_LABEL (lbl), PANGO_ELLIPSIZE_END);
      }
      g_object_set_data_full (
          G_OBJECT (wbtn), "winome-dash-win-ctx", wctx,
          [] (gpointer p) { delete static_cast<DashWindowCtx *> (p); });
      g_signal_connect (wbtn, "clicked", G_CALLBACK (on_dash_menu_window),
                        wctx);
      gtk_box_append (GTK_BOX (box), wbtn);
      ++shown;
    }
    gtk_box_append (GTK_BOX (box), make_dash_sep ());
  }

  // Section 2: New Window / Open.
  if (is_running) {
    if (!app.exe_path.empty () || has_entry) {
      gtk_box_append (
          GTK_BOX (box),
          make_dash_menu_btn (tr ("New Window", "新建窗口"),
                              G_CALLBACK (on_dash_menu_new_window), ctx));
      gtk_box_append (GTK_BOX (box), make_dash_sep ());
    }
  } else {
    gtk_box_append (
        GTK_BOX (box),
        make_dash_menu_btn (tr ("Open", "打开"),
                            G_CALLBACK (on_dash_menu_activate), ctx));
    gtk_box_append (GTK_BOX (box), make_dash_sep ());
  }

  // Section 3: Pin / Unpin & Open File Location.
  WinomeConfig cfg = get_config ();
  bool pinned = app.pinned || is_app_id_pinned (cfg.pinned_apps, app.id);
  gtk_box_append (
      GTK_BOX (box),
      make_dash_menu_btn (pinned ? tr ("Unpin from Dash", "从程序坞取消固定")
                                 : tr ("Pin to Dash", "固定到程序坞"),
                          G_CALLBACK (on_dash_menu_toggle_pin), ctx));

  if (has_entry && !matched_entry.is_uwp &&
      (!matched_entry.target_exe.empty () ||
       (!matched_entry.launch_path.empty () &&
        matched_entry.launch_path.rfind (L"shell:", 0) != 0))) {
    gtk_box_append (
        GTK_BOX (box),
        make_dash_menu_btn (tr ("Open File Location", "打开文件所在位置"),
                            G_CALLBACK (on_dash_menu_location), ctx));
  }

  // Section 4: Quit (close all windows) when running.
  if (is_running) {
    gtk_box_append (GTK_BOX (box), make_dash_sep ());
    gtk_box_append (
        GTK_BOX (box),
        make_dash_menu_btn (tr ("Quit", "退出"),
                            G_CALLBACK (on_dash_menu_close_all), ctx));
  }

  gtk_popover_set_child (GTK_POPOVER (popover), box);
  g_signal_connect (
      popover, "closed", G_CALLBACK (+[] (GtkPopover *p, gpointer d) {
        g_print ("[menu] dash closed\n");
        auto *dash = WINOME_DASH (d);
        if (dash->active_menu == GTK_WIDGET (p))
          dash->active_menu = nullptr;
        if (gtk_widget_get_parent (GTK_WIDGET (p)) != nullptr)
          gtk_widget_unparent (GTK_WIDGET (p));
        overview_restore_focus_if_active ();
      }),
      self);
  g_signal_connect (
      popover, "map", G_CALLBACK (+[] (GtkWidget *p, gpointer G_GNUC_UNUSED) {
        promote_dash_popover_hwnd_topmost (p);
        g_object_ref (p);
        g_idle_add_full (
            G_PRIORITY_HIGH_IDLE,
            [] (gpointer data) -> gboolean {
              GtkWidget *pop = GTK_WIDGET (data);
              promote_dash_popover_hwnd_topmost (pop);
              g_object_unref (pop);
              return G_SOURCE_REMOVE;
            },
            p, nullptr);
      }),
      nullptr);
  self->active_menu = popover;
  g_print ("[menu] dash open index=%d id=%s\n", index, app.id.c_str ());
  gtk_popover_popup (GTK_POPOVER (popover));
  promote_dash_popover_hwnd_topmost (popover);
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

struct DashScopeCtx {
  WinomeDash *self = nullptr;
  bool isolate = false;
};

void
on_dash_scope_menu_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  auto *sctx = static_cast<DashScopeCtx *> (user_data);
  WinomeDash *self = sctx->self;
  bool isolate = sctx->isolate;
  overview_dash_close_menu (GTK_WIDGET (self));
  WinomeConfig cfg = get_config ();
  if (cfg.dash_isolate_workspaces != isolate) {
    cfg.dash_isolate_workspaces = isolate;
    update_config (cfg);
  }
}

void
show_dash_scope_menu (WinomeDash *self, double x, double y)
{
  overview_dash_close_menu (GTK_WIDGET (self));
  hide_label (self);

  GtkWidget *popover = gtk_popover_new ();
  install_bundled_fonts (popover);
  gtk_widget_add_css_class (popover, "winome-context-popover");
  gtk_widget_set_parent (popover, GTK_WIDGET (self));
  gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
  gtk_popover_set_position (GTK_POPOVER (popover), GTK_POS_TOP);
  gtk_popover_set_autohide (GTK_POPOVER (popover), TRUE);
  GdkRectangle target = {(int)x - 4, (int)y - 10, 8, 8};
  gtk_popover_set_pointing_to (GTK_POPOVER (popover), &target);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *hdr =
      gtk_label_new (tr ("Dash Applications", "程序坞应用范围"));
  gtk_label_set_xalign (GTK_LABEL (hdr), 0.0f);
  gtk_widget_add_css_class (hdr, "popup-menu-item");
  gtk_widget_add_css_class (hdr, "popup-inactive-menu-item");
  gtk_widget_set_sensitive (hdr, FALSE);
  gtk_box_append (GTK_BOX (box), hdr);

  const bool iso = get_config ().dash_isolate_workspaces;
  auto add_opt = [&] (const char *en, const char *zh, bool want_iso) {
    std::string label =
        std::string (iso == want_iso ? "✓  " : "     ") + tr (en, zh);
    auto *sctx = new DashScopeCtx{self, want_iso};
    GtkWidget *btn = gtk_button_new_with_label (label.c_str ());
    gtk_widget_set_name (btn, "winome-dash-ctx-item");
    gtk_widget_add_css_class (btn, "popup-menu-item");
    gtk_widget_set_focus_on_click (btn, FALSE);
    GtkWidget *lbl = gtk_button_get_child (GTK_BUTTON (btn));
    if (GTK_IS_LABEL (lbl))
      gtk_label_set_xalign (GTK_LABEL (lbl), 0.0f);
    g_object_set_data_full (
        G_OBJECT (btn), "winome-dash-scope-ctx", sctx,
        [] (gpointer p) { delete static_cast<DashScopeCtx *> (p); });
    g_signal_connect (btn, "clicked", G_CALLBACK (on_dash_scope_menu_clicked),
                      sctx);
    gtk_box_append (GTK_BOX (box), btn);
  };
  add_opt ("Show Apps from All Workspaces", "显示所有工作区的应用", false);
  add_opt ("Current Workspace Only", "仅显示当前工作区的应用", true);

  gtk_popover_set_child (GTK_POPOVER (popover), box);
  g_signal_connect (
      popover, "closed", G_CALLBACK (+[] (GtkPopover *p, gpointer d) {
        auto *dash = WINOME_DASH (d);
        if (dash->active_menu == GTK_WIDGET (p))
          dash->active_menu = nullptr;
        if (gtk_widget_get_parent (GTK_WIDGET (p)) != nullptr)
          gtk_widget_unparent (GTK_WIDGET (p));
        overview_restore_focus_if_active ();
      }),
      self);
  g_signal_connect (
      popover, "map", G_CALLBACK (+[] (GtkWidget *p, gpointer G_GNUC_UNUSED) {
        promote_dash_popover_hwnd_topmost (p);
      }),
      nullptr);
  self->active_menu = popover;
  gtk_popover_popup (GTK_POPOVER (popover));
  promote_dash_popover_hwnd_topmost (popover);
}

void
on_dash_press (GtkGestureClick *gesture, int n_press G_GNUC_UNUSED, double x,
               double y, gpointer user_data)
{
  WinomeDash *self = WINOME_DASH (user_data);
  guint button =
      gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));
  if (button != GDK_BUTTON_PRIMARY)
    return;
  int idx = tile_at (self, x, y);
  if (idx != self->pressed_index) {
    self->pressed_index = idx;
    gtk_widget_queue_draw (GTK_WIDGET (self));
  }
}

void
on_dash_click_stopped (GtkGestureClick *gesture G_GNUC_UNUSED,
                       gpointer user_data)
{
  WinomeDash *self = WINOME_DASH (user_data);
  if (self->pressed_index != -1) {
    self->pressed_index = -1;
    gtk_widget_queue_draw (GTK_WIDGET (self));
  }
}

void
on_dash_click (GtkGestureClick *gesture, int n_press, double x, double y,
               gpointer user_data)
{
  (void)n_press;
  WinomeDash *self = WINOME_DASH (user_data);
  if (self->pressed_index != -1) {
    self->pressed_index = -1;
    gtk_widget_queue_draw (GTK_WIDGET (self));
  }
  guint button =
      gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));
  int index = tile_at (self, x, y);

  if (button == GDK_BUTTON_SECONDARY) {
    if (index >= 0 && index < (int)self->apps.size ()) {
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
      show_dash_context_menu (self, index);
    } else if (x >= self->pill_x && x < self->pill_x + self->pill_w &&
               y >= self->pill_y && y < self->pill_y + self->pill_h) {
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
      show_dash_scope_menu (self, x, y);
    }
    return;
  }

  if (index < 0 || button != GDK_BUTTON_PRIMARY)
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

  // .dash-background pill with subtle 1px GNOME border.
  graphene_rect_t pill_rect;
  graphene_rect_init (&pill_rect, self->pill_x, self->pill_y, self->pill_w,
                      self->pill_h);
  GskRoundedRect pill;
  gsk_rounded_rect_init_from_rect (&pill, &pill_rect, (float)kDashPillRadius);
  gtk_snapshot_push_rounded_clip (snapshot, &pill);
  gtk_snapshot_append_color (snapshot, &self->pill_color, &pill_rect);
  gtk_snapshot_pop (snapshot);
  const float border_w[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  const GdkRGBA border_c[4] = {self->pill_border_color, self->pill_border_color,
                               self->pill_border_color, self->pill_border_color};
  gtk_snapshot_append_border (snapshot, &pill, border_w, border_c);

  // Separator between Pinned Favorites and Unpinned Running Apps.
  if (self->separator_x >= 0) {
    graphene_rect_t sep_rect;
    graphene_rect_init (&sep_rect, (float)self->separator_x,
                        (float)(self->pill_y + kDashPadV + 6), 1.0f,
                        (float)std::max (self->pill_h - 2 * kDashPadV - 12, 8));
    gtk_snapshot_append_color (snapshot, &self->sep_color, &sep_rect);
  }

  // Tiles.
  for (int i = 0; i < (int)self->tile_rects.size (); ++i) {
    const DashTileRect &r = self->tile_rects[i];
    bool is_show_apps = (i == (int)self->apps.size ());

    // .overview-icon tile background (active / hover / show-apps checked).
    const GdkRGBA *bg_tile = nullptr;
    if (i == self->pressed_index)
      bg_tile = &self->tile_active_color;
    else if (is_show_apps && self->show_apps_active && i != self->hover_index)
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
      gtk_snapshot_append_scaled_texture (snapshot, texture,
                                          GSK_SCALING_FILTER_TRILINEAR,
                                          &icon_rect);
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

    // Running dot: centered cleanly in the bottom padding BELOW the tile
    // (upstream _dash.scss .app-grid-running-dot), never overlapping the icon.
    if (i < (int)self->apps.size () && !self->apps[i].windows.empty ()) {
      double cx = r.x + r.w / 2.0;
      double dy = r.y + r.h + 2.5;
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
  // g_object_new zero-fills C structs without running C++ constructors or
  // default member initializers.
  new (&self->apps) std::vector<DashApp> ();
  new (&self->tile_rects) std::vector<DashTileRect> ();
  self->separator_x = -1;
  self->icon_size = 64;
  self->item_gap = kItemGap;
  self->hover_index = -1;
  self->pressed_index = -1;
  self->last_max_height = 96;
  self->last_active_desktop = -1;
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
    GtkWidget *lbl = self->label;
    g_object_remove_weak_pointer (G_OBJECT (lbl),
                                  reinterpret_cast<gpointer *> (&self->label));
    self->label = nullptr;
    gtk_widget_set_visible (lbl, FALSE);
    if (self->root_fixed != nullptr &&
        gtk_widget_get_parent (lbl) == self->root_fixed) {
      gtk_fixed_remove (GTK_FIXED (self->root_fixed), lbl);
    }
  }
  if (self->root_fixed != nullptr) {
    g_object_remove_weak_pointer (
        G_OBJECT (self->root_fixed),
        reinterpret_cast<gpointer *> (&self->root_fixed));
    self->root_fixed = nullptr;
  }
  clear_apps (self);
  G_OBJECT_CLASS (winome_dash_parent_class)->dispose (object);
}

static void
winome_dash_finalize (GObject *object)
{
  WinomeDash *self = WINOME_DASH (object);
  using DashAppVecT = std::vector<DashApp>;
  using RectVecT = std::vector<DashTileRect>;
  self->tile_rects.~RectVecT ();
  self->apps.~DashAppVecT ();
  G_OBJECT_CLASS (winome_dash_parent_class)->finalize (object);
}

static void
winome_dash_size_allocate (GtkWidget *widget, int width, int height,
                           int baseline)
{
  GTK_WIDGET_CLASS (winome_dash_parent_class)
      ->size_allocate (widget, width, height, baseline);
  WinomeDash *self = WINOME_DASH (widget);
  if (self->active_menu != nullptr)
    gtk_popover_present (GTK_POPOVER (self->active_menu));
}

static void
winome_dash_class_init (WinomeDashClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  widget_class->snapshot = winome_dash_snapshot;
  widget_class->measure = winome_dash_measure;
  widget_class->size_allocate = winome_dash_size_allocate;

  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = winome_dash_dispose;
  object_class->finalize = winome_dash_finalize;
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
  if (self->root_fixed != nullptr) {
    g_object_add_weak_pointer (
        G_OBJECT (self->root_fixed),
        reinterpret_cast<gpointer *> (&self->root_fixed));
  }

  // Hover label (.dash-label pill), floating above the dash.
  self->label = gtk_label_new ("");
  gtk_widget_add_css_class (self->label, "dash-label");
  gtk_widget_set_visible (self->label, FALSE);
  gtk_fixed_put (GTK_FIXED (root_fixed), self->label, 0, 0);
  g_object_add_weak_pointer (G_OBJECT (self->label),
                             reinterpret_cast<gpointer *> (&self->label));

  // Controllers: motion for hover + label, left/right click for activation/menu.
  GtkEventController *motion =
      gtk_event_controller_motion_new ();
  g_signal_connect (motion, "enter", G_CALLBACK (on_dash_motion_enter), self);
  g_signal_connect (motion, "motion", G_CALLBACK (on_dash_motion), self);
  g_signal_connect (motion, "leave", G_CALLBACK (on_dash_motion_leave), self);
  gtk_widget_add_controller (GTK_WIDGET (self), motion);

  GtkGesture *click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);
  g_signal_connect (click, "pressed", G_CALLBACK (on_dash_press), self);
  g_signal_connect (click, "stopped", G_CALLBACK (on_dash_click_stopped), self);
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
    // Hold a temporary ref across popdown so a synchronous "closed" signal
    // unparenting the popover cannot finalize `m` before we check its parent.
    g_object_ref (m);
    gtk_popover_popdown (GTK_POPOVER (m));
    if (gtk_widget_get_parent (m) != nullptr)
      gtk_widget_unparent (m);
    g_object_unref (m);
  }
}

bool
overview_dash_has_open_menu (GtkWidget *dash)
{
  if (dash == nullptr)
    return false;
  WinomeDash *self = WINOME_DASH (dash);
  return self->active_menu != nullptr && gtk_widget_get_visible (self->active_menu);
}

void
overview_dash_repopulate (GtkWidget *dash, HMONITOR monitor, int max_height,
                          int active_desktop)
{
  if (dash == nullptr)
    return;
  WinomeDash *self = WINOME_DASH (dash);
  overview_dash_close_menu (dash);
  hide_label (self);
  self->hover_index = -1;
  self->pressed_index = -1;
  self->last_monitor = monitor;
  self->last_max_height = max_height;
  self->last_active_desktop = active_desktop;
  clear_apps (self);

  // Icon size ceiling from available dash height.
  int scale = std::max (gtk_widget_get_scale_factor (dash), 1);
  int avail = max_height - 2 * kDashPadV - 2 * kTilePad - kDashBottomMargin;
  int max_v_icon = kBaseIconSizes[0];
  for (int i = 0; i < kBaseIconSizesCount; ++i) {
    if (kBaseIconSizes[i] <= avail)
      max_v_icon = kBaseIconSizes[i];
  }

  // Enumerate windows top-down and group into running applications.
  WinomeConfig cfg = get_config ();
  const bool vd_avail = vd::available ();
  const int target_ws =
      (active_desktop >= 0) ? active_desktop
                            : (vd_avail ? ws::active_for_monitor (monitor) : -1);
  std::vector<DashWindowEntry> windows;
  EnumCtx ctx{monitor, &windows, vd_avail, cfg.dash_isolate_workspaces,
              target_ws, cfg.overview_window_blacklist};
  EnumWindows (dash_collect, reinterpret_cast<LPARAM> (&ctx));

  struct RunningGroup {
    std::string id;
    std::wstring exe;
    std::string name;
    std::vector<HWND> windows;
    std::vector<int> window_desktops;
  };
  std::vector<RunningGroup> running;

  for (const DashWindowEntry &we : windows) {
    HWND hwnd = we.hwnd;
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
    running[idx].window_desktops.push_back (we.desktop_index);
  }

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

  struct DashCandidate {
    DashApp app;
    AppEntry entry;
    bool has_entry = false;
    bool from_running = false;
    bool fallback_path = false;
  };
  std::vector<DashCandidate> pinned_cands;
  std::vector<DashCandidate> running_cands;

  // 1. Persisted Favorites (in configured order).
  for (const std::string &raw_pin : cfg.pinned_apps) {
    std::string pin_id = normalize_app_id (raw_pin);
    if (pin_id.empty ())
      continue;

    bool dup = false;
    for (const DashCandidate &existing : pinned_cands) {
      if (existing.app.id == pin_id) {
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

    DashCandidate cand;
    cand.app.id = pin_id;
    cand.app.pinned = true;
    cand.has_entry = has_entry;
    if (has_entry)
      cand.entry = std::move (entry);

    if (ridx != SIZE_MAX) {
      running_consumed[ridx] = true;
      cand.app.exe_path = running[ridx].exe;
      cand.app.windows = std::move (running[ridx].windows);
      cand.app.window_desktops = std::move (running[ridx].window_desktops);
      cand.app.name = cand.has_entry ? cand.entry.name : running[ridx].name;
      cand.from_running = true;
    } else if (cand.has_entry) {
      cand.app.exe_path = cand.entry.target_exe;
      cand.app.name = cand.entry.name;
    } else {
      std::wstring wpath = app_utf8_to_wide (pin_id);
      if (GetFileAttributesW (wpath.c_str ()) == INVALID_FILE_ATTRIBUTES)
        continue; // ignore stale uninstalled favorite path
      cand.app.exe_path = std::move (wpath);
      cand.app.name = pinned_fallback_label (pin_id);
      cand.fallback_path = true;
    }

    pinned_cands.push_back (std::move (cand));
  }

  // 2. Remaining running apps not in favorites.
  for (size_t i = 0; i < running.size (); ++i) {
    if (running_consumed[i])
      continue;
    RunningGroup &g = running[i];
    DashCandidate cand;
    cand.app.id = g.id;
    cand.app.exe_path = g.exe;
    cand.app.name = g.name;
    cand.app.windows = std::move (g.windows);
    cand.app.window_desktops = std::move (g.window_desktops);
    cand.app.pinned = false;
    cand.from_running = true;
    running_cands.push_back (std::move (cand));
  }

  // Fit dash horizontally to the target monitor width so dash_x never goes
  // negative and the trailing Show Apps button always remains visible on screen.
  int max_logical_w = dash_monitor_logical_width (monitor, scale);
  int chosen_icon = max_v_icon;
  int chosen_gap = kItemGap;
  int keep_pinned = (int)pinned_cands.size ();
  int keep_running = (int)running_cands.size ();

  if (max_logical_w > 0) {
    auto fits = [&] (int np, int nr, int isize, int gap) {
      bool split = (np > 0 && nr > 0);
      return dash_total_width_for (np + nr, isize, gap, split) <= max_logical_w;
    };

    bool matched = false;
    for (int i = kBaseIconSizesCount - 1; i >= 0; --i) {
      if (kBaseIconSizes[i] > max_v_icon)
        continue;
      if (fits (keep_pinned, keep_running, kBaseIconSizes[i], kItemGap)) {
        chosen_icon = kBaseIconSizes[i];
        chosen_gap = kItemGap;
        matched = true;
        break;
      }
    }

    if (!matched) {
      chosen_icon = kBaseIconSizes[0];
      static constexpr int kCompactGaps[] = {2, 0};
      for (int gap : kCompactGaps) {
        if (fits (keep_pinned, keep_running, chosen_icon, gap)) {
          chosen_gap = gap;
          matched = true;
          break;
        }
      }
    }

    if (!matched) {
      chosen_icon = kBaseIconSizes[0];
      chosen_gap = 0;
      while (keep_running > 0 &&
             !fits (keep_pinned, keep_running, chosen_icon, chosen_gap)) {
        --keep_running;
      }
      while (keep_pinned > 0 &&
             !fits (keep_pinned, keep_running, chosen_icon, chosen_gap)) {
        --keep_pinned;
      }
    }
  }

  self->icon_size = chosen_icon;
  self->item_gap = chosen_gap;
  int super_scale = std::max (scale, 2);
  int icon_px = std::clamp (self->icon_size * super_scale, 96, 256);

  auto finalize_candidate = [&] (DashCandidate &cand) {
    if (cand.from_running) {
      cand.app.icon = app_icon_for_window (cand.app.windows[0],
                                           cand.app.exe_path, icon_px,
                                           cand.app.id);
    } else if (cand.has_entry) {
      cand.app.icon = app_icon_for_entry (cand.entry, icon_px);
    } else if (cand.fallback_path) {
      cand.app.icon = app_icon_for_path (cand.app.exe_path, icon_px,
                                         cand.app.id);
    }
    self->apps.push_back (std::move (cand.app));
  };

  for (int i = 0; i < keep_pinned; ++i)
    finalize_candidate (pinned_cands[i]);
  self->pinned_count = keep_pinned;
  for (int i = 0; i < keep_running; ++i)
    finalize_candidate (running_cands[i]);

  const int prev_pill_w = self->pill_w;
  const int prev_pill_h = self->pill_h;
  recompute_layout (self);
  if (self->pill_w != prev_pill_w || self->pill_h != prev_pill_h)
    gtk_widget_queue_resize (dash);
  else
    gtk_widget_queue_draw (dash);
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
