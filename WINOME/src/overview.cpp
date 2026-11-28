// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Activities overview rendered in the host process: a faithful port of the
// GNOME Shell overviewControls.js WINDOW_PICKER state.
//
//   #overviewGroup (root fixed)      background-color: #222226
//     .search-entry (GtkEntry)       top center, width 24em, margins 12/6
//     .workspace-background          the WORKAREA-ASPECT box from
//                                    WorkspacesView._getFirstFitSingle-
//                                    WorkspaceBox (as tall as the picker
//                                    container, width = height * workarea
//                                    aspect, centered), wallpaper drawn
//                                    cover-style inside an animated
//                                    0..30px rounded clip
//     #dash (overview-dash.cpp)      bottom center
//     .window-caption (GtkLabel)     hover title pill below a preview
//
// Window previews are DWM live thumbnails on the TOP-LEVEL overview window
// (DwmRegisterThumbnail requires a top-level destination), laid out by the
// ported workspace.js strategy (overview-layout.cpp) inside the workspace
// box with the effective spacing of WorkspaceLayout._adjustSpacingAndPadding
// (theme 6px + windowPreview chrome oversize ~= 30px).
//
// The overview transition mirrors Overview.ANIMATION_TIME = 250ms: opening
// EASE_OUT_SINE, closing EASE_OUT_QUAD. The workspace background interpolates
// between the full work area (radius 0) and the picker box (radius 30) —
// both rects share the workarea aspect so the interpolation keeps it — and
// window clones fly between their real rects and the slots. Minimized
// windows are not on their workspace: they grow from a zero-size point at
// the workspace corner while fading in (WorkspaceLayout.vfunc_allocate's
// "not showing on its workspace" branch + _syncOpacity). The dash and search
// entry do not fade (only the startup animation moves them upstream); they
// appear at their final positions.
//
// Hover chrome (windowPreview.js): the preview scales up by
// WINDOW_ACTIVE_SIZE_INC*2 around its center (200ms EASE_OUT_QUAD), the
// application icon (64px, ICON_OVERLAP 0.7 straddling the bottom edge) and
// the .window-caption pill fade in below, and the 32px .window-close circle
// sits centered on the top-right corner. Because DWM composites thumbnails
// ABOVE everything the window paints, the close button lives in a tiny
// interactive chrome window, the app icon in a second one, and the
// .window-caption pill in a third one (clicking the icon or caption activates
// the window, like event propagation to the preview actor); all three sit above
// the overview, below the panel. The overlay hides 750ms
// after the pointer leaves (WINDOW_OVERLAY_IDLE_HIDE_TIMEOUT), fading out.
//
// Interactions: click a preview (or its icon/title) restores/focuses that
// window and closes the overview; the close button sends WM_CLOSE and
// relayouts; a click elsewhere closes. Esc / a second Win press close it
// through overview-trigger.cpp. A click that dismissed a panel popover
// opened over the overview is swallowed (GNOME's menu grab consumes it).

#include "overview.h"
#include "overview-secondary.h"
#include "app-grid.h"
#include "app-index.h"
#include "config.h"
#include "debug-runtime.h"
#include "easing.h"
#include "fullscreen-watcher.h"
#include "host-utils.h"
#include "monitor-manager.h"
#include "workspace-manager.h"
#include "i18n.h"
#include "live-wallpaper.h"
#include "perf-metrics.h"
#include "overview-layout.h"
#include "overview-dash.h"
#include "overview-search.h"
#include "overview-thumbs.h"
#include "overview-trigger.h"
#include "settings-window.h"
#include "shell-panel.h"
#include "st-engine.h"
#include "system-status.h"
#include "fonts.h"
#include "virtual-desktop.h"

#include <gtk/gtk.h>
#include <gdk/win32/gdkwin32.h>
#include <dwmapi.h>

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace winome {

namespace {

// --- theme constants (compiled gnome-shell-dark.css; StEngine-resolved) ----

// .search-entry carries CSS margin-top: 12px / margin-bottom: 6px directly
// in gtk_widget_get_preferred_size(), matching ControlsManagerLayout startY.
constexpr double kWorkspaceRadiusFallback = 30.0;
constexpr int kCloseButtonSize = 32;     // .window-close width/height
constexpr int kCloseIconSize = 16;       // .window-close StIcon (medium)

// windowPreview.js constants (logical px).
constexpr int kWindowIconSize = 64;      // ICON_SIZE
constexpr double kIconOverlap = 0.7;     // ICON_OVERLAP
constexpr int kIconTitleSpacing = 6;     // ICON_TITLE_SPACING
constexpr int kWindowActiveSizeInc = 5;  // WINDOW_ACTIVE_SIZE_INC
constexpr int kOverlayFadeMs = 200;      // WINDOW_OVERLAY_FADE_TIME
constexpr int kWindowScaleMs = 200;      // WINDOW_SCALE_TIME
constexpr int kOverlayIdleHideMs = 750;  // WINDOW_OVERLAY_IDLE_HIDE_TIMEOUT

// .window-picker { spacing: 6px } (theme) and the chrome oversize added by
// WorkspaceLayout._adjustSpacingAndPadding: max(close 32/2, icon 64*0.3) +
// WINDOW_ACTIVE_SIZE_INC, in logical px.
constexpr double kWindowPickerSpacing = 6.0;

struct OvThemeMetrics {
  bool resolved = false;
  int close_btn = kCloseButtonSize;
  int close_ico = kCloseIconSize;
  double picker_spacing = kWindowPickerSpacing;
};

static const OvThemeMetrics &
ov_theme_metrics (void)
{
  static OvThemeMetrics m;
  if (!m.resolved) {
    m.resolved = true;
    if (StEngine *engine = StEngine::get ()) {
      StEngine::Node close_node (*engine, nullptr, "", "window-close", "");
      double v = 0;
      if (close_node.lookup_length ("width", &v) && v > 0)
        m.close_btn = (int)(v + 0.5);
      v = 0;
      if (close_node.lookup_length ("icon-size", &v) && v > 0)
        m.close_ico = (int)(v + 0.5);

      StEngine::Node picker_node (*engine, nullptr, "", "window-picker", "");
      v = 0;
      if (picker_node.lookup_length ("spacing", &v) && v >= 0)
        m.picker_spacing = v;
    }
  }
  return m;
}

static int close_button_size () { return ov_theme_metrics ().close_btn; }
static int close_icon_size () { return ov_theme_metrics ().close_ico; }
static double window_picker_spacing () { return ov_theme_metrics ().picker_spacing; }

// overviewControls.js & workspacesView.js constants.
constexpr double kVerticalSpacingRatio = 0.02;
constexpr double kThumbnailsAdjustTop = 0.6;
constexpr double kThumbnailsAdjustBottom = 0.4;  // THUMBNAILS_SPACING_ADJ_
                                                  // USTMENT_BOTTOM
constexpr double kMaxThumbnailScale = 0.08;      // MAX_THUMBNAIL_SCALE
constexpr double kDashMaxHeightRatio = 0.16;
constexpr double kSmallWorkspaceRatio = 0.15;    // SMALL_WORKSPACE_RATIO
constexpr int kWorkspaceMinSpacing = 24;         // WORKSPACE_MIN_SPACING
constexpr int kWorkspaceMaxSpacing = 80;         // WORKSPACE_MAX_SPACING
constexpr double kWorkspaceInactiveScale = 0.94; // WORKSPACE_INACTIVE_SCALE
constexpr int kWindowDndSize = 256;              // WINDOW_DND_SIZE
constexpr BYTE kDraggingWindowOpacity = 215;
constexpr guint kDragScaleMs = 250;              // SCALE_ANIMATION_TIME
constexpr guint kDragSnapBackMs = 250;           // SNAP_BACK_ANIMATION_TIME
constexpr guint kRelayoutMs = 200;               // workspace.js layout transition

// Overview.ANIMATION_TIME: opening EASE_OUT_SINE, closing EASE_OUT_QUAD.
constexpr guint kOpenMs = 250;
constexpr guint kCloseMs = 250;



struct OverviewView;

// --- Shared process-wide overview state --------------------------------------

HWND g_panel_hwnd = nullptr;
std::vector<std::unique_ptr<OverviewView>> g_views;
OverviewView *g_focused_view = nullptr;
OverviewView *g_chrome_owner_view = nullptr;
OverviewView *g_live_wallpaper_owner = nullptr;
bool g_global_initialized = false;

std::atomic<HMONITOR> g_requested_monitor{nullptr};
HWND g_prev_foreground_hwnd = nullptr;
std::atomic<bool> g_restore_focus_discarded{false};

enum class ControlsState {
  HIDDEN = 0,
  WINDOW_PICKER = 1,
  APP_GRID = 2,
};
constexpr double kGridSlideOffsetPx = 48.0;
constexpr guint kSearchFadeMs = 200;
constexpr guint kSwitchMs = 250;
constexpr guint kWorkspaceScrollTimeoutMs = 150;

std::atomic<bool> g_atomic_can_shift_app_grid{false};
std::atomic<bool> g_pending_shift_app_grid{false};

GdkTexture *g_wallpaper = nullptr;
GdkTexture *g_wallpaper_thumb = nullptr;
GdkTexture *g_live_wallpaper = nullptr;
GdkTexture *g_live_wallpaper_thumb = nullptr;
bool g_using_live_wallpaper = false;

struct WallpaperCacheKey {
  std::wstring path;
  FILETIME mtime{};
  uint64_t size = 0;
  int target_w = 0;
  int target_h = 0;
  bool valid = false;
};
WallpaperCacheKey g_wallpaper_cache;

// Shared hover-chrome toplevels (at most one monitor is hovered at a time).
GtkWidget *g_chrome_window = nullptr;
GtkWidget *g_chrome_button = nullptr;
HWND g_chrome_hwnd = nullptr;
HWND g_chrome_target = nullptr;
RECT g_chrome_want = {0, 0, 0, 0};

GtkWidget *g_icon_window = nullptr;
GtkWidget *g_icon_picture = nullptr;
HWND g_icon_hwnd = nullptr;
HWND g_icon_target = nullptr;
RECT g_icon_want = {0, 0, 0, 0};

GtkWidget *g_caption_window = nullptr;
GtkWidget *g_caption_wrap = nullptr;
GtkWidget *g_caption = nullptr;
HWND g_caption_hwnd = nullptr;
RECT g_caption_want = {0, 0, 0, 0};

// Shared runtime timers / drag tracking.
bool g_last_overview_linked = false;
bool g_suppress_ws_change_rebuild = false;
HWND g_ext_drag_hwnd = nullptr;
RECT g_ext_drag_start_rect{};
guint g_dyn_ws_startup_timer = 0;
guint g_dash_resync_timer = 0;
guint g_idle_trim_timer = 0;
guint g_prewarm_timer = 0;
int g_prewarm_step = 0;
guint g_post_hide_workarea_timer = 0;
guint g_app_index_defer_timer = 0;
FrameTimeTracker g_anim_tracker (20000);
std::shared_ptr<std::atomic<bool>> g_overview_alive;

struct SlotEntry {
  RECT canonical_start{};
  RECT start{};
  RECT canonical_picker_rect{};
  RECT picker_rect{};
  RECT grid_rect{};
  RECT rect{};
  RECT anim_from{};
  RECT anim_to{};
  RECT current{};
  HWND target = nullptr;
  HTHUMBNAIL thumb = nullptr;
  int desktop_index = 0;
  int z_order = 0;
  bool minimized = false;
  bool minimized_any = false;
  BYTE grid_opacity = 255;
  BYTE anim_from_opacity = 255;
  BYTE anim_to_opacity = 255;
  BYTE current_opacity = 255;
  double current_scale = 1.0;
  bool thumb_applied = false;
  bool last_visible = false;
  RECT pending_rect{};
  BYTE pending_opacity = 255;
  bool has_pending_dwm = false;
};

struct MiniSlot {
  HTHUMBNAIL thumb = nullptr;
  RECT rect{};
  SIZE src_size{0, 0};
  bool has_src_size = false;
  RECT last_dest{};
  RECT last_src{};
  bool last_visible = false;
  bool last_has_src = false;
  BYTE last_opacity = 0;
  bool props_applied = false;
};

struct Animation {
  guint source = 0;
  bool started = false;
  ControlsState from_state = ControlsState::HIDDEN;
  ControlsState to_state = ControlsState::HIDDEN;
  gint64 start_us = 0;
  guint duration_ms = 0;
  RECT bg_from{};
  RECT bg_to{};
  double radius_from = 0.0;
  double radius_to = 0.0;
  double thumbs_from = 0.0;
  double thumbs_to = 0.0;
  double thumbs_scale_from = 1.0;
  double thumbs_scale_to = 1.0;
  double thumbs_ty_from = 0.0;
  double thumbs_ty_to = 0.0;
  double grid_opacity_from = 0.0;
  double grid_opacity_to = 0.0;
  double grid_slide_from = 0.0;
  double grid_slide_to = 0.0;
  double panel_from = 0.0;
  double panel_to = 0.0;
};

struct ScaleAnim {
  int index;
  double from, to;
  gint64 start_us;
};

static bool
is_valid_monitor (HMONITOR mon)
{
  if (mon == nullptr)
    return false;
  MONITORINFO mi {};
  mi.cbSize = sizeof (mi);
  return GetMonitorInfoW (mon, &mi) != FALSE;
}

static void
set_hwnd_cloaked (HWND hwnd, bool cloak)
{
  if (hwnd == nullptr || !IsWindow (hwnd))
    return;
  BOOL val = cloak ? TRUE : FALSE;
  DwmSetWindowAttribute (hwnd, DWMWA_CLOAK, &val, sizeof (val));
}

static bool
is_any_panel_hwnd (HWND h)
{
  return h != nullptr && (h == g_panel_hwnd || monitor_manager_is_panel_hwnd (h));
}

// Forward declarations for cross-view / shared helpers.
static bool is_any_overview_view_hwnd (HWND h);
static OverviewView *active_chrome_or_focused_view (void);
static bool view_can_auto_focus_search (OverviewView *v);
static HWND view_hwnd (OverviewView *v);
static HWND view_panel_hwnd (OverviewView *v);
static OverviewView *find_any_open_view (OverviewView *exclude = nullptr);
static void restack_locked (void);
static void dump_zorder (OverviewView *v, const char *tag);
static void ensure_overview_subclass (HWND hwnd, OverviewView *view);
static void unsubclass_overview_hwnd (OverviewView *view);
static void chrome_hide (void);
static void icon_hide (void);
static void caption_hide (void);
static void overlay_hide_all (void);
static void chrome_show_at (OverviewView *v, const RECT &preview_rect);
static void icon_show_at (OverviewView *v, const RECT &preview_rect, HWND target);
static void caption_show_at (OverviewView *v, const RECT &preview_rect);
static void chrome_keep_hover_alive (void);
static void chrome_schedule_hover_idle_hide (void);
static void set_external_drop_highlight_all (POINT screen_pt, OverviewView *exclude);
static void clear_external_drop_highlight_all (OverviewView *exclude = nullptr);
static bool hit_test_any_open_view (POINT screen_pt, OverviewView *exclude,
                                    HMONITOR *out_mon, int *out_ws,
                                    bool *out_create_new);
static void refresh_other_views_dash (OverviewView *exclude);
static void sync_sibling_views_workspace_switch (OverviewView *origin, int target,
                                                 int known_desktop_count);
static bool has_multiple_open_views (void);
static void schedule_dash_resync (guint delay_ms = 120);
static void cancel_post_hide_workarea (void);
static void schedule_post_hide_workarea (guint delay_ms);
static gboolean overview_idle_trim_cb (gpointer user_data);
static void on_live_wallpaper_frame (GdkTexture *full_tex, GdkTexture *thumb_tex,
                                     void *user_data);
static void on_overview_desktop_changed_global (HMONITOR monitor, int count,
                                                int active_index, void *user_data);
static void on_overview_config_changed_global (void *user_data);
static void schedule_dynamic_workspaces_check (guint delay_ms = 250);
static bool is_external_foreground_target (HWND fg);

// --- helpers -----------------------------------------------------------------

double
ease_out_quad (double t)
{
  return 1.0 - (1.0 - t) * (1.0 - t);
}

double
ease_out_sine (double t)
{
  return std::sin (t * G_PI / 2.0);
}

// Clutter.AnimationMode.EASE_OUT_CUBIC (workspace switch scroll).
double
ease_out_cubic (double t)
{
  double u = 1.0 - t;
  return 1.0 - u * u * u;
}

// Horizontal slide of a physical-px rect.
RECT
translate_rect_x (const RECT &r, LONG dx)
{
  RECT out = r;
  out.left += dx;
  out.right += dx;
  return out;
}

RECT
lerp_rect (const RECT &a, const RECT &b, double t)
{
  RECT r;
  r.left = (LONG)(a.left + (b.left - a.left) * t);
  r.top = (LONG)(a.top + (b.top - a.top) * t);
  r.right = (LONG)(a.right + (b.right - a.right) * t);
  r.bottom = (LONG)(a.bottom + (b.bottom - a.bottom) * t);
  return r;
}

// windowPreview.js showOverlay: uniform scale around the center so the
// preview grows by WINDOW_ACTIVE_SIZE_INC on each side of its larger side.
double
slot_hover_target_scale (const RECT &r, int scale_factor)
{
  double w = r.right - r.left;
  double h = r.bottom - r.top;
  double orig = std::max (w, h);
  if (orig <= 0)
    return 1.0;
  return (orig + kWindowActiveSizeInc * 2.0 * scale_factor) / orig;
}

RECT
scale_rect_around_center (const RECT &r, double s)
{
  double cx = (r.left + r.right) / 2.0;
  double cy = (r.top + r.bottom) / 2.0;
  double w = (r.right - r.left) * s;
  double h = (r.bottom - r.top) * s;
  RECT out;
  out.left = (LONG)(cx - w / 2.0);
  out.top = (LONG)(cy - h / 2.0);
  out.right = (LONG)(cx + w / 2.0);
  out.bottom = (LONG)(cy + h / 2.0);
  return out;
}


std::string
hwnd_title_utf8 (HWND hwnd)
{
  wchar_t title[256] = L"";
  GetWindowTextW (hwnd, title, 256);
  if (title[0] == L'\0')
    return {};
  int n = WideCharToMultiByte (CP_UTF8, 0, title, -1, nullptr, 0, nullptr,
                               nullptr);
  std::string out (n > 0 ? n - 1 : 0, '\0');
  if (n > 0)
    WideCharToMultiByte (CP_UTF8, 0, title, -1, out.data (), n, nullptr,
                         nullptr);
  return out;
}

std::string
wide_to_utf8 (const wchar_t *ws)
{
  if (ws == nullptr || ws[0] == L'\0')
    return {};
  int n = WideCharToMultiByte (CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
  std::string out (n > 0 ? n - 1 : 0, '\0');
  if (n > 0)
    WideCharToMultiByte (CP_UTF8, 0, ws, -1, out.data (), n, nullptr, nullptr);
  return out;
}

std::string
hwnd_process_name_utf8 (HWND hwnd)
{
  DWORD pid = 0;
  GetWindowThreadProcessId (hwnd, &pid);
  if (pid == 0)
    return {};
  HANDLE proc = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (proc == nullptr)
    return {};
  wchar_t buf[MAX_PATH] = L"";
  DWORD len = MAX_PATH;
  BOOL ok = QueryFullProcessImageNameW (proc, 0, buf, &len);
  CloseHandle (proc);
  if (!ok || len == 0)
    return {};
  const wchar_t *base = wcsrchr (buf, L'\\');
  return wide_to_utf8 (base != nullptr ? base + 1 : buf);
}

// --- window enumeration (alt-tab eligible, DWM visible bounds) ---------------

bool
is_our_process (HWND hwnd)
{
  DWORD pid = 0;
  GetWindowThreadProcessId (hwnd, &pid);
  return pid == GetCurrentProcessId ();
}

bool
is_alt_tab_window (HWND hwnd)
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

  if (GetWindowLong (hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
    return false;

  return true;
}

DWORD
window_cloaked_flags (HWND hwnd)
{
  DWORD cloaked = 0;
  DwmGetWindowAttribute (hwnd, DWMWA_CLOAKED, &cloaked, sizeof (cloaked));
  return cloaked;
}

bool
is_cloaked (HWND hwnd)
{
  return window_cloaked_flags (hwnd) != 0;
}

// The visible frame bounds (no invisible DWM resize borders); the restored
// placement for minimized windows. Matches MetaWindow's frame rect usage in
// workspace.js bounding boxes.
RECT
window_rect (HWND hwnd)
{
  RECT r = {0, 0, 0, 0};
  if (IsIconic (hwnd)) {
    WINDOWPLACEMENT wp {};
    wp.length = sizeof (wp);
    if (GetWindowPlacement (hwnd, &wp)) {
      // rcNormalPosition includes the invisible borders; accept the slight
      // inflation, same trade-off as the extracted WinOverview.
      return wp.rcNormalPosition;
    }
  }

  if (SUCCEEDED (DwmGetWindowAttribute (hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                        &r, sizeof (r))))
    return r;

  GetWindowRect (hwnd, &r);
  return r;
}

struct EnumContext {
  HMONITOR monitor;
  RECT monitor_rect;
  std::vector<OvWindowInfo> *windows;
  // Desktop-aware filtering (virtual-desktop.cpp). When the virtual desktop
  // API is reachable, membership replaces the cloak filter: a window belongs
  // in the picker when it is on the CURRENT desktop — this shows suspended
  // (cloaked) apps on the active desktop exactly like Meta's workspace
  // windows, and pinned windows too.
  bool vd_filter = false;
  // Include cloaked windows (every desktop): used for the thumbnail
  // mini-previews, which bucket windows by their desktop GUID afterwards.
  bool include_cloaked = false;
  bool use_blacklist = true;
  bool filter_monitor = true;
};

BOOL CALLBACK
collect_windows (HWND hwnd, LPARAM lparam)
{
  EnumContext *ctx = reinterpret_cast<EnumContext *> (lparam);

  if (is_our_process (hwnd) || !is_alt_tab_window (hwnd))
    return TRUE;

  if (is_overview_default_blacklisted_hwnd (hwnd, ctx->use_blacklist))
    return TRUE;

  const DWORD cloaked = window_cloaked_flags (hwnd);
  // App-cloaked (DWM_CLOAKED_APP = 0x1, e.g. suspended/pre-launched UWP) and
  // inherited-cloaked (0x4) windows are never real user windows on any desktop.
  if ((cloaked & (0x1u | 0x4u)) != 0)
    return TRUE;

  if (ctx->vd_filter) {
    if (!ws::window_on_active_workspace (hwnd, ctx->monitor) || cloaked != 0)
      return TRUE;
  } else if (!ctx->include_cloaked) {
    if (cloaked != 0)
      return TRUE;
  }

  RECT r = window_rect (hwnd);
  if (r.right - r.left <= 0 || r.bottom - r.top <= 0)
    return TRUE;

  if (ctx->filter_monitor &&
      MonitorFromRect (&r, MONITOR_DEFAULTTONULL) != ctx->monitor)
    return TRUE;

  OffsetRect (&r, -ctx->monitor_rect.left, -ctx->monitor_rect.top);
  ctx->windows->push_back (OvWindowInfo{hwnd, r});
  return TRUE;
}



// --- workspace background widget ----------------------------------------------

typedef struct {
  GtkWidget parent_instance;
  OverviewView *owner;
} WinomeWorkspaceBg;

typedef struct {
  GtkWidgetClass parent_class;
} WinomeWorkspaceBgClass;

#define WINOME_WORKSPACE_BG(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), winome_workspace_bg_get_type (), \
                               WinomeWorkspaceBg))

G_DEFINE_TYPE (WinomeWorkspaceBg, winome_workspace_bg, GTK_TYPE_WIDGET)

static void winome_workspace_bg_snapshot_impl (WinomeWorkspaceBg *self,
                                               GtkWidget *widget,
                                               GtkSnapshot *snapshot);

static void
winome_workspace_bg_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  winome_workspace_bg_snapshot_impl (WINOME_WORKSPACE_BG (widget), widget, snapshot);
}

static void
winome_workspace_bg_class_init (WinomeWorkspaceBgClass *klass)
{
  GTK_WIDGET_CLASS (klass)->snapshot = winome_workspace_bg_snapshot;
}

static void
winome_workspace_bg_init (WinomeWorkspaceBg *self)
{
  self->owner = nullptr;
}

// --- clipped App Grid ControlsManager container ------------------------------

typedef struct {
  GtkWidget parent_instance;
  OverviewView *owner;
  GtkWidget *child;
  int target_w;
  int target_h;
  float slide_y;
  float transition_opacity;
  bool transition_active;
  GdkTexture *transition_tex;
  GdkTexture *cached_page0_tex;
  uint64_t cached_page0_version;
  int cached_page0_w;
  int cached_page0_h;
  int cached_page0_scale;
  guint capture_idle_source;
} WinomeAppGridClip;

typedef struct {
  GtkWidgetClass parent_class;
} WinomeAppGridClipClass;

#define WINOME_APP_GRID_CLIP(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), winome_app_grid_clip_get_type (), \
                               WinomeAppGridClip))

G_DEFINE_TYPE (WinomeAppGridClip, winome_app_grid_clip, GTK_TYPE_WIDGET)

static bool app_grid_clip_can_capture (OverviewView *owner);

static void
winome_app_grid_clip_measure (GtkWidget *widget, GtkOrientation orientation,
                              int for_size, int *minimum, int *natural,
                              int *minimum_baseline, int *natural_baseline)
{
  (void)for_size;
  WinomeAppGridClip *self = WINOME_APP_GRID_CLIP (widget);
  int sz = (orientation == GTK_ORIENTATION_HORIZONTAL) ? self->target_w
                                                       : self->target_h;
  if (sz > 0) {
    *minimum = sz;
    *natural = sz;
  } else if (self->child != nullptr && gtk_widget_should_layout (self->child)) {
    gtk_widget_measure (self->child, orientation, for_size, minimum, natural,
                        nullptr, nullptr);
  } else {
    *minimum = 0;
    *natural = 0;
  }
  *minimum_baseline = -1;
  *natural_baseline = -1;
}

static void
winome_app_grid_clip_size_allocate (GtkWidget *widget, int width, int height,
                                    int baseline)
{
  WinomeAppGridClip *self = WINOME_APP_GRID_CLIP (widget);
  if (self->child == nullptr || !gtk_widget_should_layout (self->child))
    return;
  // Keep child allocation static at (0,0) and apply slide_y purely inside
  // snapshot so 250ms transitions never trigger recursive layout/allocate passes.
  gtk_widget_allocate (self->child, width, height, baseline, nullptr);
}

static void
winome_app_grid_clip_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  WinomeAppGridClip *self = WINOME_APP_GRID_CLIP (widget);
  const int w = gtk_widget_get_width (widget);
  const int h = gtk_widget_get_height (widget);
  if (w <= 0 || h <= 0)
    return;

  if (!self->transition_active) {
    if (self->child == nullptr || !gtk_widget_get_visible (self->child))
      return;
    if (std::abs (self->slide_y) > 0.5f) {
      graphene_rect_t clip_rect =
          GRAPHENE_RECT_INIT (0.0f, 0.0f, (float)w, (float)h);
      gtk_snapshot_push_clip (snapshot, &clip_rect);
      graphene_point_t pt = GRAPHENE_POINT_INIT (0.0f, self->slide_y);
      gtk_snapshot_save (snapshot);
      gtk_snapshot_translate (snapshot, &pt);
      gtk_widget_snapshot_child (widget, self->child, snapshot);
      gtk_snapshot_restore (snapshot);
      gtk_snapshot_pop (snapshot);
    } else {
      gtk_widget_snapshot_child (widget, self->child, snapshot);
    }
    return;
  }

  const float alpha = std::clamp (self->transition_opacity, 0.0f, 1.0f);
  if (alpha <= 0.004f)
    return;

  // Fast path: slide & fade a pre-cached GPU texture quad without touching the
  // live App Grid widget tree (zero re-entrant gsk_renderer_render_texture
  // stalls inside snapshot(), zero per-frame layout/FBO overhead).
  graphene_rect_t clip_rect =
      GRAPHENE_RECT_INIT (0.0f, 0.0f, (float)w, (float)h);
  gtk_snapshot_push_clip (snapshot, &clip_rect);
  if (self->transition_tex != nullptr) {
    if (alpha < 0.996f)
      gtk_snapshot_push_opacity (snapshot, alpha);
    graphene_rect_t dst =
        GRAPHENE_RECT_INIT (0.0f, self->slide_y, (float)w, (float)h);
    gtk_snapshot_append_scaled_texture (snapshot, self->transition_tex,
                                        GSK_SCALING_FILTER_LINEAR, &dst);
    if (alpha < 0.996f)
      gtk_snapshot_pop (snapshot);
  } else if (self->child != nullptr && gtk_widget_get_visible (self->child)) {
    // Cold fallback (first open before page-0 cache exists, or non-page-0):
    // slide the live child directly without wrapping it in a full-screen
    // GskOpacityNode offscreen FBO (which stalls integrated GPUs 100-180ms).
    graphene_point_t pt = GRAPHENE_POINT_INIT (0.0f, self->slide_y);
    gtk_snapshot_save (snapshot);
    gtk_snapshot_translate (snapshot, &pt);
    gtk_widget_snapshot_child (widget, self->child, snapshot);
    gtk_snapshot_restore (snapshot);
  }
  gtk_snapshot_pop (snapshot);
}

static void
app_grid_clip_cancel_capture (WinomeAppGridClip *self)
{
  if (self != nullptr && self->capture_idle_source != 0) {
    g_source_remove (self->capture_idle_source);
    self->capture_idle_source = 0;
  }
}

static void
winome_app_grid_clip_dispose (GObject *object)
{
  WinomeAppGridClip *self = WINOME_APP_GRID_CLIP (object);
  app_grid_clip_cancel_capture (self);
  g_clear_object (&self->transition_tex);
  g_clear_object (&self->cached_page0_tex);
  g_clear_pointer (&self->child, gtk_widget_unparent);
  G_OBJECT_CLASS (winome_app_grid_clip_parent_class)->dispose (object);
}

static void
winome_app_grid_clip_class_init (WinomeAppGridClipClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = winome_app_grid_clip_dispose;
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS (klass);
  wclass->measure = winome_app_grid_clip_measure;
  wclass->size_allocate = winome_app_grid_clip_size_allocate;
  wclass->snapshot = winome_app_grid_clip_snapshot;
}

static void
winome_app_grid_clip_init (WinomeAppGridClip *self)
{
  self->owner = nullptr;
  self->child = nullptr;
  self->target_w = 0;
  self->target_h = 0;
  self->slide_y = 0.0f;
  self->transition_opacity = 1.0f;
  self->transition_active = false;
  self->transition_tex = nullptr;
  self->cached_page0_tex = nullptr;
  self->cached_page0_version = 0;
  self->cached_page0_w = 0;
  self->cached_page0_h = 0;
  self->cached_page0_scale = 0;
  self->capture_idle_source = 0;
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
}

static GtkWidget *
app_grid_clip_new (GtkWidget *child, OverviewView *owner)
{
  auto *clip = static_cast<WinomeAppGridClip *> (
      g_object_new (winome_app_grid_clip_get_type (), nullptr));
  clip->owner = owner;
  clip->child = child;
  if (child != nullptr)
    gtk_widget_set_parent (child, GTK_WIDGET (clip));
  return GTK_WIDGET (clip);
}

static void
app_grid_clip_set_bounds (GtkWidget *clip_widget, int w, int h)
{
  if (clip_widget == nullptr)
    return;
  WinomeAppGridClip *self = WINOME_APP_GRID_CLIP (clip_widget);
  if (self->target_w == w && self->target_h == h)
    return;
  self->target_w = w;
  self->target_h = h;
  app_grid_clip_cancel_capture (self);
  g_clear_object (&self->transition_tex);
  g_clear_object (&self->cached_page0_tex);
  self->cached_page0_version = 0;
  gtk_widget_set_size_request (clip_widget, w, h);
  gtk_widget_queue_resize (clip_widget);
}

// Capture page 0 into a persistent GPU texture OUTSIDE snapshot() once the
// App Grid has settled and laid out cleanly, so every subsequent transition
// slides & fades a single cached GPU quad with zero capture latency.
static gboolean
app_grid_clip_capture_page0_cb (gpointer user_data)
{
  WinomeAppGridClip *self = WINOME_APP_GRID_CLIP (user_data);
  self->capture_idle_source = 0;
  if (self->child == nullptr || self->transition_active ||
      !app_grid_clip_can_capture (self->owner)) {
    return G_SOURCE_REMOVE;
  }
  GtkWidget *widget = GTK_WIDGET (self);
  if (!gtk_widget_get_visible (widget) ||
      !gtk_widget_get_visible (self->child) ||
      app_grid_current_page (self->child) != 0) {
    return G_SOURCE_REMOVE;
  }
  const int w = gtk_widget_get_width (widget);
  const int h = gtk_widget_get_height (widget);
  if (w <= 0 || h <= 0 || gtk_widget_get_width (self->child) <= 0 ||
      gtk_widget_get_height (self->child) <= 0) {
    return G_SOURCE_REMOVE;
  }
  const int scale = std::clamp (gtk_widget_get_scale_factor (widget), 1, 2);
  const uint64_t ver = app_grid_visual_version (self->child);
  if (self->cached_page0_tex != nullptr &&
      self->cached_page0_w == w && self->cached_page0_h == h &&
      self->cached_page0_scale == scale &&
      self->cached_page0_version == ver) {
    return G_SOURCE_REMOVE;
  }
  GtkNative *native = gtk_widget_get_native (widget);
  GskRenderer *renderer =
      native != nullptr ? gtk_native_get_renderer (native) : nullptr;
  if (renderer == nullptr)
    return G_SOURCE_REMOVE;

  GtkSnapshot *cap = gtk_snapshot_new ();
  if (scale > 1)
    gtk_snapshot_scale (cap, (float)scale, (float)scale);
  gtk_widget_snapshot_child (widget, self->child, cap);
  GskRenderNode *node = gtk_snapshot_free_to_node (cap);
  if (node != nullptr) {
    graphene_rect_t bounds = GRAPHENE_RECT_INIT (
        0.0f, 0.0f, (float)(w * scale), (float)(h * scale));
    GdkTexture *tex = gsk_renderer_render_texture (renderer, node, &bounds);
    gsk_render_node_unref (node);
    if (tex != nullptr) {
      g_clear_object (&self->cached_page0_tex);
      self->cached_page0_tex = tex;
      self->cached_page0_w = w;
      self->cached_page0_h = h;
      self->cached_page0_scale = scale;
      self->cached_page0_version = ver;
    }
  }
  return G_SOURCE_REMOVE;
}

static void
app_grid_clip_schedule_page0_capture (GtkWidget *clip_widget)
{
  if (clip_widget == nullptr)
    return;
  WinomeAppGridClip *self = WINOME_APP_GRID_CLIP (clip_widget);
  if (self->child == nullptr || app_grid_current_page (self->child) != 0)
    return;
  const int w = gtk_widget_get_width (clip_widget);
  const int h = gtk_widget_get_height (clip_widget);
  const int scale =
      std::clamp (gtk_widget_get_scale_factor (clip_widget), 1, 2);
  const uint64_t ver = app_grid_visual_version (self->child);
  if (self->cached_page0_tex != nullptr &&
      self->cached_page0_w == w && self->cached_page0_h == h &&
      self->cached_page0_scale == scale &&
      self->cached_page0_version == ver) {
    return;
  }
  app_grid_clip_cancel_capture (self);
  self->capture_idle_source = g_timeout_add_full (
      G_PRIORITY_LOW, 40, app_grid_clip_capture_page0_cb, self, nullptr);
}

// Begin a transition on WinomeAppGridClip. Returns true when a valid cached
// page-0 GPU texture was bound (so the caller can keep g_app_grid hidden until
// t>=1.0 and skip all App Grid widget-tree layout/draw work during animation).
static bool
app_grid_clip_begin_transition (GtkWidget *clip_widget, double opacity,
                                double slide_y)
{
  if (clip_widget == nullptr)
    return false;
  WinomeAppGridClip *self = WINOME_APP_GRID_CLIP (clip_widget);
  app_grid_clip_cancel_capture (self);
  g_clear_object (&self->transition_tex);

  const int scale =
      std::clamp (gtk_widget_get_scale_factor (clip_widget), 1, 2);
  const bool can_use_cache =
      (self->cached_page0_tex != nullptr && self->child != nullptr &&
       app_grid_current_page (self->child) == 0 &&
       self->cached_page0_w == self->target_w &&
       self->cached_page0_h == self->target_h &&
       self->cached_page0_scale == scale);
  if (can_use_cache) {
    self->transition_tex = GDK_TEXTURE (g_object_ref (self->cached_page0_tex));
  }

  self->transition_active = true;
  self->transition_opacity =
      static_cast<float> (std::clamp (opacity, 0.0, 1.0));
  self->slide_y = static_cast<float> (slide_y);
  gtk_widget_queue_draw (clip_widget);
  return self->transition_tex != nullptr;
}

static void
app_grid_clip_set_transition (GtkWidget *clip_widget, bool active,
                              double opacity, double slide_y,
                              bool invalidate_cache = false)
{
  if (clip_widget == nullptr)
    return;
  WinomeAppGridClip *self = WINOME_APP_GRID_CLIP (clip_widget);
  const float alpha = static_cast<float> (std::clamp (opacity, 0.0, 1.0));
  const float sy = static_cast<float> (slide_y);
  if (!active) {
    g_clear_object (&self->transition_tex);
  } else if (invalidate_cache) {
    g_clear_object (&self->transition_tex);
  }
  const bool changed =
      (self->transition_active != active) ||
      (std::abs (self->transition_opacity - alpha) > 0.004f) ||
      (std::abs (self->slide_y - sy) > 0.25f);
  self->transition_active = active;
  self->transition_opacity = alpha;
  self->slide_y = sy;
  if (changed)
    gtk_widget_queue_draw (clip_widget);
}


// --- wallpaper ----------------------------------------------------------------

// Pack a GdkPixbuf into a tight, opaque 3-channel RGB GdkTexture (compositing
// any alpha against COLOR_DESKTOP so g_ws_bg never exposes controls beneath it
// and saves 25% GPU texture bandwidth on integrated GPUs).
static GdkTexture *
pixbuf_to_opaque_rgb_texture (GdkPixbuf *pb)
{
  if (pb == nullptr)
    return nullptr;
  const int n_channels = gdk_pixbuf_get_n_channels (pb);
  const int bps = gdk_pixbuf_get_bits_per_sample (pb);
  const int rowstride = gdk_pixbuf_get_rowstride (pb);
  const int pw = gdk_pixbuf_get_width (pb);
  const int ph = gdk_pixbuf_get_height (pb);
  if (pw <= 0 || ph <= 0 || bps != 8 || (n_channels != 3 && n_channels != 4))
    return nullptr;
  const gsize src_line_bytes =
      static_cast<gsize> (pw) * static_cast<gsize> (n_channels);
  const gsize pix_bytes = gdk_pixbuf_get_byte_length (pb);
  const gsize required_src =
      static_cast<gsize> (ph - 1) * static_cast<gsize> (rowstride) +
      src_line_bytes;
  if (static_cast<gsize> (rowstride) < src_line_bytes ||
      pix_bytes < required_src) {
    return nullptr;
  }
  const gsize dst_stride = static_cast<gsize> (pw) * 3u;
  guint8 *packed = static_cast<guint8 *> (
      g_try_malloc_n (static_cast<gsize> (ph), dst_stride));
  if (packed == nullptr)
    return nullptr;

  const guint8 *src = gdk_pixbuf_read_pixels (pb);
  if (n_channels == 3) {
    for (int y = 0; y < ph; ++y) {
      std::memcpy (packed + static_cast<gsize> (y) * dst_stride,
                   src + static_cast<gsize> (y) * static_cast<gsize> (rowstride),
                   dst_stride);
    }
  } else {
    DWORD bg = GetSysColor (COLOR_DESKTOP);
    const unsigned bg_r = (bg >> 0) & 0xffu;
    const unsigned bg_g = (bg >> 8) & 0xffu;
    const unsigned bg_b = (bg >> 16) & 0xffu;
    for (int y = 0; y < ph; ++y) {
      const guint8 *srow =
          src + static_cast<gsize> (y) * static_cast<gsize> (rowstride);
      guint8 *drow = packed + static_cast<gsize> (y) * dst_stride;
      for (int x = 0; x < pw; ++x) {
        const unsigned a = srow[x * 4 + 3];
        if (a == 255u) {
          drow[x * 3 + 0] = srow[x * 4 + 0];
          drow[x * 3 + 1] = srow[x * 4 + 1];
          drow[x * 3 + 2] = srow[x * 4 + 2];
        } else {
          const unsigned inv = 255u - a;
          drow[x * 3 + 0] =
              static_cast<guint8> ((srow[x * 4 + 0] * a + bg_r * inv) / 255u);
          drow[x * 3 + 1] =
              static_cast<guint8> ((srow[x * 4 + 1] * a + bg_g * inv) / 255u);
          drow[x * 3 + 2] =
              static_cast<guint8> ((srow[x * 4 + 2] * a + bg_b * inv) / 255u);
        }
      }
    }
  }
  GBytes *bytes =
      g_bytes_new_take (packed, static_cast<gsize> (ph) * dst_stride);
  GdkTexture *tex = gdk_memory_texture_new (pw, ph, GDK_MEMORY_R8G8B8, bytes,
                                            dst_stride);
  g_bytes_unref (bytes);
  return tex;
}

static cairo_surface_t *g_wallpaper_cairo = nullptr;
static cairo_surface_t *g_wallpaper_thumb_cairo = nullptr;

static cairo_surface_t *
texture_to_cairo_rgb24_surface (GdkTexture *tex)
{
  if (tex == nullptr)
    return nullptr;
  const int w = gdk_texture_get_width (tex);
  const int h = gdk_texture_get_height (tex);
  if (w <= 0 || h <= 0)
    return nullptr;
  cairo_surface_t *surf =
      cairo_image_surface_create (CAIRO_FORMAT_RGB24, w, h);
  if (cairo_surface_status (surf) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy (surf);
    return nullptr;
  }
  const int dst_stride = cairo_image_surface_get_stride (surf);
  unsigned char *dst = cairo_image_surface_get_data (surf);
  // gdk_texture_download() outputs B8G8R8A8_PREMULTIPLIED, which matches
  // little-endian CAIRO_FORMAT_RGB24 (B, G, R, X bytes per pixel).
  gdk_texture_download (tex, dst, static_cast<gsize> (dst_stride));
  cairo_surface_mark_dirty (surf);
  return surf;
}

static void
clear_wallpaper_surfaces (void)
{
  if (g_wallpaper_cairo != nullptr) {
    cairo_surface_destroy (g_wallpaper_cairo);
    g_wallpaper_cairo = nullptr;
  }
  if (g_wallpaper_thumb_cairo != nullptr) {
    cairo_surface_destroy (g_wallpaper_thumb_cairo);
    g_wallpaper_thumb_cairo = nullptr;
  }
}

static void
sync_wallpaper_cairo_surfaces (GdkTexture *full_tex, GdkTexture *thumb_tex)
{
  clear_wallpaper_surfaces ();
  g_wallpaper_cairo = texture_to_cairo_rgb24_surface (full_tex);
  g_wallpaper_thumb_cairo = texture_to_cairo_rgb24_surface (thumb_tex);
}

void
load_wallpaper (bool force = false)
{
  wchar_t path[MAX_PATH] = L"";
  SystemParametersInfoW (SPI_GETDESKWALLPAPER, MAX_PATH, path, 0);

  int target_w = GetSystemMetrics (SM_CXSCREEN);
  int target_h = GetSystemMetrics (SM_CYSCREEN);
  for (const auto &md : enumerate_monitors ()) {
    target_w = std::max<int> (target_w, md.monitor_rect.right - md.monitor_rect.left);
    target_h = std::max<int> (target_h, md.monitor_rect.bottom - md.monitor_rect.top);
  }
  target_w = std::max (target_w, 1280);
  target_h = std::max (target_h, 720);

  if (path[0] != L'\0') {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    FILETIME mtime{};
    uint64_t file_size = 0;
    if (GetFileAttributesExW (path, GetFileExInfoStandard, &fad)) {
      mtime = fad.ftLastWriteTime;
      file_size = (static_cast<uint64_t> (fad.nFileSizeHigh) << 32) |
                  fad.nFileSizeLow;
    }
    if (!force && g_wallpaper != nullptr && g_wallpaper_cache.valid &&
        g_wallpaper_cache.path == path &&
        g_wallpaper_cache.mtime.dwLowDateTime == mtime.dwLowDateTime &&
        g_wallpaper_cache.mtime.dwHighDateTime == mtime.dwHighDateTime &&
        g_wallpaper_cache.size == file_size &&
        g_wallpaper_cache.target_w == target_w &&
        g_wallpaper_cache.target_h == target_h) {
      return;
    }

    char utf8[MAX_PATH * 3];
    int n = WideCharToMultiByte (CP_UTF8, 0, path, -1, utf8, sizeof (utf8),
                                 nullptr, nullptr);
    if (n > 0) {
      // Bound uncompressed wallpaper memory to monitor cover resolution and
      // build a tiny <=320x180 RGB thumbnail texture for g_thumbs so workspace
      // pills never thrash the GPU texture cache sampling full 1080p/4K images.
      GdkTexture *texture = nullptr;
      GdkTexture *thumb_tex = nullptr;
      int img_w = 0, img_h = 0;
      GdkPixbuf *base_pb = nullptr;
      if (gdk_pixbuf_get_file_info (utf8, &img_w, &img_h) != nullptr &&
          img_w > 0 && img_h > 0) {
        if (img_w > target_w || img_h > target_h) {
          double cover = std::max (static_cast<double> (target_w) / img_w,
                                   static_cast<double> (target_h) / img_h);
          int scaled_w =
              std::max (1, static_cast<int> (std::ceil (img_w * std::min (cover, 1.0))));
          int scaled_h =
              std::max (1, static_cast<int> (std::ceil (img_h * std::min (cover, 1.0))));
          base_pb = gdk_pixbuf_new_from_file_at_scale (utf8, scaled_w, scaled_h,
                                                       FALSE, nullptr);
        } else {
          base_pb = gdk_pixbuf_new_from_file (utf8, nullptr);
        }
      }

      if (base_pb != nullptr) {
        texture = pixbuf_to_opaque_rgb_texture (base_pb);
        const int bw = gdk_pixbuf_get_width (base_pb);
        const int bh = gdk_pixbuf_get_height (base_pb);
        if (bw > 0 && bh > 0) {
          double ts = std::min ({320.0 / bw, 180.0 / bh, 1.0});
          int tw = std::max (1, static_cast<int> (std::lround (bw * ts)));
          int th = std::max (1, static_cast<int> (std::lround (bh * ts)));
          GdkPixbuf *tpb =
              (tw < bw || th < bh)
                  ? gdk_pixbuf_scale_simple (base_pb, tw, th, GDK_INTERP_BILINEAR)
                  : g_object_ref (base_pb);
          if (tpb != nullptr) {
            thumb_tex = pixbuf_to_opaque_rgb_texture (tpb);
            g_object_unref (tpb);
          }
        }
        g_object_unref (base_pb);
      }

      if (texture == nullptr) {
        GFile *file = g_file_new_for_path (utf8);
        texture = gdk_texture_new_from_file (file, nullptr);
        g_object_unref (file);
      }

      if (texture != nullptr) {
        g_clear_object (&g_wallpaper);
        g_clear_object (&g_wallpaper_thumb);
        g_wallpaper = texture;
        g_wallpaper_thumb = thumb_tex;
        sync_wallpaper_cairo_surfaces (g_wallpaper, g_wallpaper_thumb);
        g_wallpaper_cache = {path, mtime, file_size, target_w, target_h, true};
        return;
      }
      g_clear_object (&thumb_tex);
    }
  }
  g_wallpaper_cache = WallpaperCacheKey{};
  clear_wallpaper_surfaces ();
  g_clear_object (&g_wallpaper);
  g_clear_object (&g_wallpaper_thumb);
}

struct AsyncWallpaperResult {
  std::shared_ptr<std::atomic<bool>> alive;
  WallpaperCacheKey key;
  GBytes *full_bytes = nullptr;
  int full_w = 0;
  int full_h = 0;
  gsize full_stride = 0;
  GBytes *thumb_bytes = nullptr;
  int thumb_w = 0;
  int thumb_h = 0;
  gsize thumb_stride = 0;
};

static bool
pixbuf_to_opaque_rgb_bytes (GdkPixbuf *pb, GBytes **out_bytes, int *out_w,
                            int *out_h, gsize *out_stride)
{
  if (pb == nullptr || out_bytes == nullptr || out_w == nullptr ||
      out_h == nullptr || out_stride == nullptr)
    return false;
  const int n_channels = gdk_pixbuf_get_n_channels (pb);
  const int bps = gdk_pixbuf_get_bits_per_sample (pb);
  const int rowstride = gdk_pixbuf_get_rowstride (pb);
  const int pw = gdk_pixbuf_get_width (pb);
  const int ph = gdk_pixbuf_get_height (pb);
  if (pw <= 0 || ph <= 0 || bps != 8 || (n_channels != 3 && n_channels != 4))
    return false;
  const gsize src_line_bytes =
      static_cast<gsize> (pw) * static_cast<gsize> (n_channels);
  const gsize pix_bytes = gdk_pixbuf_get_byte_length (pb);
  const gsize required_src =
      static_cast<gsize> (ph - 1) * static_cast<gsize> (rowstride) +
      src_line_bytes;
  if (static_cast<gsize> (rowstride) < src_line_bytes ||
      pix_bytes < required_src) {
    return false;
  }
  const gsize dst_stride = static_cast<gsize> (pw) * 3u;
  guint8 *packed = static_cast<guint8 *> (
      g_try_malloc_n (static_cast<gsize> (ph), dst_stride));
  if (packed == nullptr)
    return false;

  const guint8 *src = gdk_pixbuf_read_pixels (pb);
  if (n_channels == 3) {
    for (int y = 0; y < ph; ++y) {
      std::memcpy (packed + static_cast<gsize> (y) * dst_stride,
                   src + static_cast<gsize> (y) * static_cast<gsize> (rowstride),
                   dst_stride);
    }
  } else {
    DWORD bg = GetSysColor (COLOR_DESKTOP);
    const unsigned bg_r = (bg >> 0) & 0xffu;
    const unsigned bg_g = (bg >> 8) & 0xffu;
    const unsigned bg_b = (bg >> 16) & 0xffu;
    for (int y = 0; y < ph; ++y) {
      const guint8 *srow =
          src + static_cast<gsize> (y) * static_cast<gsize> (rowstride);
      guint8 *drow = packed + static_cast<gsize> (y) * dst_stride;
      for (int x = 0; x < pw; ++x) {
        const unsigned a = srow[x * 4 + 3];
        if (a == 255u) {
          drow[x * 3 + 0] = srow[x * 4 + 0];
          drow[x * 3 + 1] = srow[x * 4 + 1];
          drow[x * 3 + 2] = srow[x * 4 + 2];
        } else {
          const unsigned inv = 255u - a;
          drow[x * 3 + 0] =
              static_cast<guint8> ((srow[x * 4 + 0] * a + bg_r * inv) / 255u);
          drow[x * 3 + 1] =
              static_cast<guint8> ((srow[x * 4 + 1] * a + bg_g * inv) / 255u);
          drow[x * 3 + 2] =
              static_cast<guint8> ((srow[x * 4 + 2] * a + bg_b * inv) / 255u);
        }
      }
    }
  }
  *out_bytes = g_bytes_new_take (packed, static_cast<gsize> (ph) * dst_stride);
  *out_w = pw;
  *out_h = ph;
  *out_stride = dst_stride;
  return true;
}

static gboolean
on_async_wallpaper_ready (gpointer user_data)
{
  auto *res = static_cast<AsyncWallpaperResult *> (user_data);
  const bool alive = res->alive && res->alive->load (std::memory_order_acquire);
  if (alive && g_global_initialized && g_wallpaper == nullptr &&
      res->full_bytes != nullptr && res->full_w > 0 && res->full_h > 0) {
    GdkTexture *full_tex = gdk_memory_texture_new (
        res->full_w, res->full_h, GDK_MEMORY_R8G8B8, res->full_bytes,
        res->full_stride);
    GdkTexture *thumb_tex = nullptr;
    if (res->thumb_bytes != nullptr && res->thumb_w > 0 && res->thumb_h > 0) {
      thumb_tex = gdk_memory_texture_new (
          res->thumb_w, res->thumb_h, GDK_MEMORY_R8G8B8, res->thumb_bytes,
          res->thumb_stride);
    }
    if (full_tex != nullptr) {
      g_clear_object (&g_wallpaper);
      g_clear_object (&g_wallpaper_thumb);
      g_wallpaper = full_tex;
      g_wallpaper_thumb = thumb_tex;
      sync_wallpaper_cairo_surfaces (g_wallpaper, g_wallpaper_thumb);
      g_wallpaper_cache = res->key;
    } else {
      g_clear_object (&thumb_tex);
    }
  }
  if (res->full_bytes != nullptr)
    g_bytes_unref (res->full_bytes);
  if (res->thumb_bytes != nullptr)
    g_bytes_unref (res->thumb_bytes);
  delete res;
  return G_SOURCE_REMOVE;
}

static std::atomic<bool> g_wallpaper_prewarm_in_flight{false};

static void
start_wallpaper_prewarm_async (void)
{
  if (g_wallpaper != nullptr || !g_overview_alive)
    return;
  if (g_wallpaper_prewarm_in_flight.exchange (true, std::memory_order_acq_rel))
    return;
  int max_w = GetSystemMetrics (SM_CXSCREEN);
  int max_h = GetSystemMetrics (SM_CYSCREEN);
  for (const auto &md : enumerate_monitors ()) {
    max_w = std::max<int> (max_w, md.monitor_rect.right - md.monitor_rect.left);
    max_h = std::max<int> (max_h, md.monitor_rect.bottom - md.monitor_rect.top);
  }
  const int target_w = std::max (max_w, 1280);
  const int target_h = std::max (max_h, 720);
  std::shared_ptr<std::atomic<bool>> alive = g_overview_alive;
  std::thread ([alive, target_w, target_h] () {
    struct PrewarmResetGuard {
      ~PrewarmResetGuard () {
        g_wallpaper_prewarm_in_flight.store (false, std::memory_order_release);
      }
    } reset_guard;
    if (!alive->load (std::memory_order_acquire))
      return;
    wchar_t path[MAX_PATH] = L"";
    SystemParametersInfoW (SPI_GETDESKWALLPAPER, MAX_PATH, path, 0);
    if (path[0] == L'\0')
      return;

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    FILETIME mtime{};
    uint64_t file_size = 0;
    if (GetFileAttributesExW (path, GetFileExInfoStandard, &fad)) {
      mtime = fad.ftLastWriteTime;
      file_size = (static_cast<uint64_t> (fad.nFileSizeHigh) << 32) |
                  fad.nFileSizeLow;
    }
    char utf8[MAX_PATH * 3];
    int n = WideCharToMultiByte (CP_UTF8, 0, path, -1, utf8, sizeof (utf8),
                                 nullptr, nullptr);
    if (n <= 0)
      return;

    int img_w = 0, img_h = 0;
    GdkPixbuf *base_pb = nullptr;
    if (gdk_pixbuf_get_file_info (utf8, &img_w, &img_h) != nullptr &&
        img_w > 0 && img_h > 0) {
      if (img_w > target_w || img_h > target_h) {
        double cover = std::max (static_cast<double> (target_w) / img_w,
                                 static_cast<double> (target_h) / img_h);
        int scaled_w = std::max (
            1, static_cast<int> (std::ceil (img_w * std::min (cover, 1.0))));
        int scaled_h = std::max (
            1, static_cast<int> (std::ceil (img_h * std::min (cover, 1.0))));
        base_pb = gdk_pixbuf_new_from_file_at_scale (utf8, scaled_w, scaled_h,
                                                     FALSE, nullptr);
      } else {
        base_pb = gdk_pixbuf_new_from_file (utf8, nullptr);
      }
    }
    if (base_pb == nullptr)
      return;

    auto *res = new AsyncWallpaperResult ();
    res->alive = alive;
    res->key = {path, mtime, file_size, target_w, target_h, true};
    bool ok = pixbuf_to_opaque_rgb_bytes (
        base_pb, &res->full_bytes, &res->full_w, &res->full_h,
        &res->full_stride);
    if (ok) {
      const int bw = gdk_pixbuf_get_width (base_pb);
      const int bh = gdk_pixbuf_get_height (base_pb);
      if (bw > 0 && bh > 0) {
        double ts = std::min ({320.0 / bw, 180.0 / bh, 1.0});
        int tw = std::max (1, static_cast<int> (std::lround (bw * ts)));
        int th = std::max (1, static_cast<int> (std::lround (bh * ts)));
        GdkPixbuf *tpb =
            (tw < bw || th < bh)
                ? gdk_pixbuf_scale_simple (base_pb, tw, th, GDK_INTERP_BILINEAR)
                : g_object_ref (base_pb);
        if (tpb != nullptr) {
          pixbuf_to_opaque_rgb_bytes (tpb, &res->thumb_bytes, &res->thumb_w,
                                      &res->thumb_h, &res->thumb_stride);
          g_object_unref (tpb);
        }
      }
    }
    g_object_unref (base_pb);
    if (!ok || !alive->load (std::memory_order_acquire)) {
      if (res->full_bytes != nullptr)
        g_bytes_unref (res->full_bytes);
      if (res->thumb_bytes != nullptr)
        g_bytes_unref (res->thumb_bytes);
      delete res;
      return;
    }
    g_idle_add_full (G_PRIORITY_LOW, on_async_wallpaper_ready, res, nullptr);
  }).detach ();
}


// --- chrome windows (close button, app icon, window caption) ----------------

static bool
is_visible_hwnd (HWND hwnd)
{
  return hwnd != nullptr && IsWindow (hwnd) && IsWindowVisible (hwnd);
}

static constexpr const wchar_t *kChromeOrigProcProp =
    L"WinomeChromeOrigWndProc";

static std::string
hwnd_class_utf8_diag (HWND h)
{
  if (h == nullptr || !IsWindow (h))
    return "<null>";
  wchar_t cls[96] = L"";
  if (GetClassNameW (h, cls, 96) <= 0)
    return "<unknown>";
  std::string u8 = wide_to_utf8 (cls);
  return u8.empty () ? "<empty>" : u8;
}

static bool
is_overview_chrome_or_panel_hwnd (HWND h)
{
  return h != nullptr &&
         (h == g_chrome_hwnd || h == g_icon_hwnd || h == g_caption_hwnd ||
          is_any_panel_hwnd (h));
}

static constexpr const wchar_t *kChromeHooksProp =
    L"WinomeChromeHooksPtr";

struct ChromeSurfaceHooks {
  GtkWidget **window;
  HWND *hwnd;
  guint layout_idle;
  guint reenable_idle;
  guint reenable_fallback_timer;
  bool pending_reenable;
  GdkFrameClock *frame_clock;
  gulong after_paint_id;
  void (*apply_want) (void);
  const RECT *want_rect = nullptr;
  bool pending_uncloak = false;
};

static LRESULT CALLBACK
chrome_subclass_wndproc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  WNDPROC orig = reinterpret_cast<WNDPROC> (
      GetPropW (hwnd, kChromeOrigProcProp));
  if (msg == WM_MOUSEACTIVATE)
    return MA_NOACTIVATE;
  if (msg == WM_NCACTIVATE && wParam != FALSE)
    return FALSE;
  if (msg == WM_STYLECHANGING && wParam == static_cast<WPARAM> (GWL_EXSTYLE) &&
      lParam != 0) {
    auto *ss = reinterpret_cast<STYLESTRUCT *> (lParam);
    ss->styleNew = (ss->styleNew | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW) &
                   ~WS_EX_APPWINDOW;
  }
  if (msg == WM_WINDOWPOSCHANGING && lParam != 0) {
    auto *wp = reinterpret_cast<WINDOWPOS *> (lParam);
    wp->flags |= SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    auto *hooks = reinterpret_cast<ChromeSurfaceHooks *> (
        GetPropW (hwnd, kChromeHooksProp));
    if (hooks != nullptr && hooks->want_rect != nullptr &&
        (wp->flags & SWP_HIDEWINDOW) == 0) {
      const RECT want = *hooks->want_rect;
      const int want_w = want.right - want.left;
      const int want_h = want.bottom - want.top;
      if (want_w > 0 && want_h > 0) {
        wp->x = want.left;
        wp->y = want.top;
        wp->cx = want_w;
        wp->cy = want_h;
        wp->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
      }
    }
  }
  if ((msg == WM_ACTIVATE && LOWORD (wParam) != WA_INACTIVE) ||
      msg == WM_SETFOCUS) {
    OverviewView *ov = active_chrome_or_focused_view ();
    if (ov != nullptr && view_can_auto_focus_search (ov)) {
      HWND ov_hwnd = view_hwnd (ov);
      if (ov_hwnd != nullptr && IsWindow (ov_hwnd)) {
        if (GetForegroundWindow () != ov_hwnd) {
          g_print ("[ov-focus] blocked chrome activation hwnd=%p msg=0x%x\n",
                   static_cast<void *> (hwnd), msg);
          SetForegroundWindow (ov_hwnd);
        } else if (GetFocus () != ov_hwnd) {
          SetFocus (ov_hwnd);
        }
        return 0;
      }
    }
  }
  if (msg == WM_NCDESTROY) {
    RemovePropW (hwnd, kChromeOrigProcProp);
    RemovePropW (hwnd, kChromeHooksProp);
    if (orig != nullptr &&
        GetWindowLongPtrW (hwnd, GWLP_WNDPROC) ==
            reinterpret_cast<LONG_PTR> (chrome_subclass_wndproc)) {
      SetWindowLongPtrW (hwnd, GWLP_WNDPROC,
                         reinterpret_cast<LONG_PTR> (orig));
    }
  }
  return orig != nullptr ? CallWindowProcW (orig, hwnd, msg, wParam, lParam)
                         : DefWindowProcW (hwnd, msg, wParam, lParam);
}

static void
ensure_chrome_subclass (HWND hwnd, ChromeSurfaceHooks *hooks)
{
  if (hwnd == nullptr || !IsWindow (hwnd))
    return;
  if (hooks != nullptr)
    SetPropW (hwnd, kChromeHooksProp, reinterpret_cast<HANDLE> (hooks));
  if (GetPropW (hwnd, kChromeOrigProcProp) != nullptr)
    return;
  WNDPROC cur = reinterpret_cast<WNDPROC> (
      GetWindowLongPtrW (hwnd, GWLP_WNDPROC));
  if (cur != nullptr && cur != chrome_subclass_wndproc) {
    SetPropW (hwnd, kChromeOrigProcProp, reinterpret_cast<HANDLE> (cur));
    SetWindowLongPtrW (hwnd, GWLP_WNDPROC,
                       reinterpret_cast<LONG_PTR> (chrome_subclass_wndproc));
  }
}

static void
chrome_apply_base_ex_styles (HWND hwnd)
{
  LONG_PTR ex = GetWindowLongPtrW (hwnd, GWL_EXSTYLE);
  LONG_PTR want = (ex | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW) &
                  ~WS_EX_APPWINDOW;
  if (want != ex)
    SetWindowLongPtrW (hwnd, GWL_EXSTYLE, want);
}

static void
ensure_hwnd_topmost (HWND hwnd)
{
  if (hwnd == nullptr || !IsWindow (hwnd))
    return;
  if ((GetWindowLongPtrW (hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) {
    SetWindowPos (hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                      SWP_NOOWNERZORDER);
  }
}

static void
chrome_bind_owner (HWND hwnd)
{
  // Keep chrome popups unowned (GWLP_HWNDPARENT == 0): restack_locked()
  // explicitly manages the topmost z-order chain (popover > panel > chrome >
  // overview), and Win32 owner-child z-order coupling would otherwise drag
  // g_overview_hwnd above g_panel_hwnd whenever GDK or SetForegroundWindow
  // touches any window in the group without SWP_NOOWNERZORDER.
  if (hwnd == nullptr || !IsWindow (hwnd))
    return;
  if (GetWindowLongPtrW (hwnd, GWLP_HWNDPARENT) != 0)
    SetWindowLongPtrW (hwnd, GWLP_HWNDPARENT, 0);
}

static HWND active_folder_hwnd (void);

static void
chrome_detach_owners (void)
{
  HWND chrome_hwnds[] = {active_folder_hwnd (), g_chrome_hwnd, g_icon_hwnd,
                         g_caption_hwnd};
  for (HWND ch : chrome_hwnds) {
    if (ch != nullptr && IsWindow (ch) &&
        GetWindowLongPtrW (ch, GWLP_HWNDPARENT) != 0) {
      SetWindowLongPtrW (ch, GWLP_HWNDPARENT, 0);
    }
  }
}

static void
chrome_window_apply_want (GtkWidget *win, HWND hwnd, const RECT &want,
                          HWND insert_after)
{
  if (win == nullptr || !gtk_widget_get_visible (win) ||
      hwnd == nullptr || !IsWindow (hwnd))
    return;

  chrome_apply_base_ex_styles (hwnd);
  chrome_bind_owner (hwnd);
  ensure_hwnd_topmost (hwnd);

  int want_w = want.right - want.left;
  int want_h = want.bottom - want.top;
  if (want_w <= 0 || want_h <= 0)
    return;

  HWND after = (insert_after != nullptr && IsWindow (insert_after))
                   ? insert_after
                   : HWND_TOPMOST;
  RECT cur{};
  bool same_rect = GetWindowRect (hwnd, &cur) &&
                   cur.left == want.left && cur.top == want.top &&
                   cur.right == want.right && cur.bottom == want.bottom;
  bool vis = IsWindowVisible (hwnd) != FALSE;
  HWND prev = GetWindow (hwnd, GW_HWNDPREV);
  bool same_z = (after == HWND_TOPMOST) ? (prev == nullptr) : (prev == after);
  if (!same_rect || !vis || !same_z) {
    UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                 (vis ? 0 : SWP_SHOWWINDOW) |
                 (same_rect ? (SWP_NOMOVE | SWP_NOSIZE) : 0);
    SetWindowPos (hwnd, after, want.left, want.top, want_w, want_h, flags);
  }
}

static void
chrome_finish_reenable (ChromeSurfaceHooks *hooks)
{
  if (hooks->reenable_idle != 0) {
    g_source_remove (hooks->reenable_idle);
    hooks->reenable_idle = 0;
  }
  if (hooks->reenable_fallback_timer != 0) {
    g_source_remove (hooks->reenable_fallback_timer);
    hooks->reenable_fallback_timer = 0;
  }
  hooks->pending_reenable = false;
  if (hooks->pending_uncloak && hooks->hwnd != nullptr &&
      *hooks->hwnd != nullptr && IsWindow (*hooks->hwnd) &&
      hooks->window != nullptr && *hooks->window != nullptr &&
      gtk_widget_get_visible (*hooks->window)) {
    set_hwnd_cloaked (*hooks->hwnd, false);
    hooks->pending_uncloak = false;
  }
  if (hooks->hwnd != nullptr && *hooks->hwnd != nullptr &&
      IsWindow (*hooks->hwnd) && !IsWindowEnabled (*hooks->hwnd)) {
    EnableWindow (*hooks->hwnd, TRUE);
  }
}

static gboolean
chrome_reenable_idle_cb (gpointer user_data)
{
  auto *hooks = static_cast<ChromeSurfaceHooks *> (user_data);
  hooks->reenable_idle = 0;
  chrome_finish_reenable (hooks);
  return G_SOURCE_REMOVE;
}

static gboolean
chrome_reenable_fallback_cb (gpointer user_data)
{
  auto *hooks = static_cast<ChromeSurfaceHooks *> (user_data);
  hooks->reenable_fallback_timer = 0;
  chrome_finish_reenable (hooks);
  return G_SOURCE_REMOVE;
}

static void
chrome_schedule_reenable (ChromeSurfaceHooks *hooks)
{
  if (hooks->pending_reenable && hooks->reenable_idle == 0) {
    // Schedule strictly below GDK_PRIORITY_REDRAW (120) so GDK4 finishes
    // its frame-clock paint/ShowWindow pass before WS_DISABLED is cleared.
    hooks->reenable_idle = g_idle_add_full (
        G_PRIORITY_DEFAULT_IDLE, chrome_reenable_idle_cb, hooks, nullptr);
  }
}

static void
chrome_disconnect_after_paint (ChromeSurfaceHooks *hooks)
{
  if (hooks->frame_clock != nullptr && hooks->after_paint_id != 0)
    g_signal_handler_disconnect (hooks->frame_clock, hooks->after_paint_id);
  hooks->frame_clock = nullptr;
  hooks->after_paint_id = 0;
}

static void
on_chrome_after_paint (GdkFrameClock *clock G_GNUC_UNUSED, gpointer user_data)
{
  auto *hooks = static_cast<ChromeSurfaceHooks *> (user_data);
  if (hooks->window != nullptr && *hooks->window != nullptr &&
      gtk_widget_get_visible (*hooks->window)) {
    if (hooks->apply_want != nullptr)
      hooks->apply_want ();
    if (hooks->pending_uncloak && hooks->hwnd != nullptr &&
        *hooks->hwnd != nullptr && IsWindow (*hooks->hwnd)) {
      set_hwnd_cloaked (*hooks->hwnd, false);
      hooks->pending_uncloak = false;
    }
  }
  if (hooks->pending_reenable)
    chrome_schedule_reenable (hooks);
}

static void
chrome_ensure_after_paint_hook (ChromeSurfaceHooks *hooks)
{
  if (hooks == nullptr || hooks->window == nullptr || *hooks->window == nullptr)
    return;
  GdkSurface *surf = gtk_native_get_surface (GTK_NATIVE (*hooks->window));
  GdkFrameClock *clock =
      surf != nullptr ? gdk_surface_get_frame_clock (surf) : nullptr;
  if (clock == nullptr)
    return;
  if (hooks->frame_clock == clock && hooks->after_paint_id != 0)
    return;
  chrome_disconnect_after_paint (hooks);
  hooks->frame_clock = clock;
  hooks->after_paint_id = g_signal_connect (
      clock, "after-paint", G_CALLBACK (on_chrome_after_paint), hooks);
}

static gboolean
chrome_surface_idle_cb (gpointer user_data)
{
  auto *hooks = static_cast<ChromeSurfaceHooks *> (user_data);
  hooks->layout_idle = 0;
  if (*hooks->window != nullptr && gtk_widget_get_visible (*hooks->window) &&
      !hooks->pending_uncloak) {
    hooks->apply_want ();
    restack_locked ();
  }
  return G_SOURCE_REMOVE;
}

static void
chrome_schedule_apply (ChromeSurfaceHooks *hooks)
{
  if (hooks->layout_idle != 0)
    return;
  hooks->layout_idle = g_idle_add_full (
      G_PRIORITY_HIGH_IDLE, chrome_surface_idle_cb, hooks, nullptr);
}

static void
chrome_cancel_apply (ChromeSurfaceHooks *hooks)
{
  if (hooks->layout_idle != 0) {
    g_source_remove (hooks->layout_idle);
    hooks->layout_idle = 0;
  }
  chrome_finish_reenable (hooks);
}

static void
on_chrome_surface_layout (GdkSurface *surface G_GNUC_UNUSED,
                          int width G_GNUC_UNUSED,
                          int height G_GNUC_UNUSED,
                          gpointer user_data)
{
  auto *hooks = static_cast<ChromeSurfaceHooks *> (user_data);
  if (*hooks->window != nullptr && gtk_widget_get_visible (*hooks->window))
    chrome_schedule_apply (hooks);
}

static void
chrome_present_window (ChromeSurfaceHooks *hooks)
{
  if (hooks == nullptr || hooks->window == nullptr || *hooks->window == nullptr)
    return;
  GtkWidget *win = *hooks->window;
  if (!gtk_widget_get_realized (win))
    gtk_widget_realize (win);
  if (!gtk_widget_get_visible (win)) {
    // GDK4's Win32 backend defers ShowWindow(SW_SHOWNORMAL) to its frame-clock
    // paint tick (GDK_PRIORITY_REDRAW = 120), which ignores WS_EX_NOACTIVATE
    // and initially places the HWND using GDK's stale cached coordinates.
    // Cloak across the first paint while keeping WS_DISABLED and pinning
    // want_rect in WM_WINDOWPOSCHANGING, then apply_want + uncloak in
    // after-paint so the close/icon/caption never flashes at a stale location.
    if (hooks->reenable_idle != 0) {
      g_source_remove (hooks->reenable_idle);
      hooks->reenable_idle = 0;
    }
    if (hooks->reenable_fallback_timer != 0) {
      g_source_remove (hooks->reenable_fallback_timer);
      hooks->reenable_fallback_timer = 0;
    }
    if (GdkSurface *surf = gtk_native_get_surface (GTK_NATIVE (win))) {
      HWND hwnd = static_cast<HWND> (gdk_win32_surface_get_handle (surf));
      if (hooks->hwnd != nullptr)
        *hooks->hwnd = hwnd;
      if (hwnd != nullptr && IsWindow (hwnd)) {
        chrome_apply_base_ex_styles (hwnd);
        chrome_bind_owner (hwnd);
        ensure_hwnd_topmost (hwnd);
        set_hwnd_cloaked (hwnd, true);
        hooks->pending_uncloak = true;
        if (hooks->want_rect != nullptr) {
          const RECT &wr = *hooks->want_rect;
          if (wr.right > wr.left && wr.bottom > wr.top) {
            SetWindowPos (hwnd, HWND_TOPMOST, wr.left, wr.top,
                          wr.right - wr.left, wr.bottom - wr.top,
                          SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER);
          }
        }
        if (IsWindowEnabled (hwnd) != FALSE)
          EnableWindow (hwnd, FALSE);
      }
    }
    hooks->pending_reenable = true;
    hooks->reenable_fallback_timer =
        g_timeout_add (160, chrome_reenable_fallback_cb, hooks);
    gtk_widget_set_visible (win, TRUE);
    gtk_widget_queue_draw (win);
    chrome_ensure_after_paint_hook (hooks);
    return;
  }
  if (hooks->pending_uncloak)
    return;
  if (hooks->apply_want != nullptr)
    hooks->apply_want ();
}

static void
on_chrome_window_realize (GtkWidget *widget, gpointer user_data)
{
  auto *hooks = static_cast<ChromeSurfaceHooks *> (user_data);
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (widget));
  if (surface == nullptr)
    return;
  HWND hwnd = static_cast<HWND> (gdk_win32_surface_get_handle (surface));
  *hooks->hwnd = hwnd;
  chrome_apply_base_ex_styles (hwnd);
  harden_shell_overlay_hwnd (hwnd);
  ensure_chrome_subclass (hwnd, hooks);
  chrome_bind_owner (hwnd);
  ensure_hwnd_topmost (hwnd);
  winome::vd::pin_window_async (hwnd);
  g_signal_connect (surface, "layout", G_CALLBACK (on_chrome_surface_layout),
                    hooks);
}

static void
on_chrome_window_map (GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
  auto *hooks = static_cast<ChromeSurfaceHooks *> (user_data);
  chrome_ensure_after_paint_hook (hooks);
  chrome_schedule_apply (hooks);
}

static GtkWidget *
make_chrome_toplevel (ChromeSurfaceHooks *hooks)
{
  GtkWidget *win = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (win), "WINOME Overview Chrome");
  gtk_window_set_decorated (GTK_WINDOW (win), FALSE);
  gtk_window_set_resizable (GTK_WINDOW (win), FALSE);
  gtk_window_set_hide_on_close (GTK_WINDOW (win), TRUE);
  gtk_widget_set_focusable (win, FALSE);
  gtk_widget_add_css_class (win, "winome-overview-chrome");
  install_bundled_fonts (win);
  g_signal_connect (win, "realize", G_CALLBACK (on_chrome_window_realize),
                    hooks);
  g_signal_connect (win, "map", G_CALLBACK (on_chrome_window_map), hooks);
  g_signal_connect (
      win, "close-request",
      G_CALLBACK (+[] (GtkWindow *w, gpointer ud G_GNUC_UNUSED) -> gboolean {
        gtk_widget_set_visible (GTK_WIDGET (w), FALSE);
        return TRUE;
      }),
      nullptr);
  return win;
}

static void
attach_chrome_hover_motion (GtkWidget *widget);
static gboolean
on_chrome_scroll_cb (GtkEventControllerScroll *ctrl, double dx, double dy,
                     gpointer user_data);

static void
attach_chrome_hover_motion (GtkWidget *widget)
{
  GtkEventController *motion = gtk_event_controller_motion_new ();
  g_signal_connect (motion, "enter",
                    G_CALLBACK (+[] (GtkEventControllerMotion *m G_GNUC_UNUSED,
                                     double x G_GNUC_UNUSED,
                                     double y G_GNUC_UNUSED,
                                     gpointer d G_GNUC_UNUSED) {
                      chrome_keep_hover_alive ();
                    }),
                    nullptr);
  g_signal_connect (motion, "motion",
                    G_CALLBACK (+[] (GtkEventControllerMotion *m G_GNUC_UNUSED,
                                     double x G_GNUC_UNUSED,
                                     double y G_GNUC_UNUSED,
                                     gpointer d G_GNUC_UNUSED) {
                      chrome_keep_hover_alive ();
                    }),
                    nullptr);
  g_signal_connect (motion, "leave",
                    G_CALLBACK (+[] (GtkEventControllerMotion *m G_GNUC_UNUSED,
                                     gpointer d G_GNUC_UNUSED) {
                      chrome_schedule_hover_idle_hide ();
                    }),
                    nullptr);
  gtk_widget_add_controller (widget, motion);

  GtkEventController *scroll = gtk_event_controller_scroll_new (
      GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  g_signal_connect (scroll, "scroll", G_CALLBACK (on_chrome_scroll_cb), nullptr);
  gtk_widget_add_controller (widget, scroll);
}

static bool
cursor_over_chrome_window (void)
{
  POINT pt;
  if (!GetCursorPos (&pt))
    return false;
  HWND hwnds[] = {g_chrome_hwnd, g_icon_hwnd, g_caption_hwnd};
  for (HWND h : hwnds) {
    if (!is_visible_hwnd (h))
      continue;
    RECT r;
    if (GetWindowRect (h, &r) && PtInRect (&r, pt) != FALSE)
      return true;
  }
  return false;
}


// Synchronize Windows virtual desktops with GNOME Shell's dynamic workspace
// rules (windowManager.js WorkspaceTracker._checkWorkspaces):
// - Minimum 2 workspaces at all times (1 workspace -> 2 workspaces on startup).
// - Always 1 trailing empty workspace at the end (if last workspace has a
//   window, append a new empty workspace).
// - Any empty intermediate or extra trailing workspace is removed.
static bool g_in_dynamic_ws_sync = false;

struct SharedOpenSnapshot {
  bool valid = false;
  ws::MultiMonitorWorkspaceMode mode = ws::MultiMonitorWorkspaceMode::Span;
  std::vector<vd::DesktopInfo> desktops;
  int active_index = -1;
  std::vector<OvWindowInfo> global_windows;  // global screen coordinates
  std::vector<HMONITOR> window_monitors;
  std::vector<int> window_desktops;
  std::vector<DWORD> window_cloaked;
  std::unordered_map<HWND, int> hwnd_desktop_map;
};

static SharedOpenSnapshot
capture_shared_open_snapshot (void)
{
  const gint64 snap_t0 = g_get_monotonic_time ();
  SharedOpenSnapshot snap{};
  snap.mode = ws::effective_mode ();
  if (snap.mode == ws::MultiMonitorWorkspaceMode::Independent)
    return snap;

  // Single global EnumWindows + single batched COM roundtrip across ALL
  // monitors so linked multi-monitor opening never repeats N COM queries.
  std::vector<OvWindowInfo> raw_global;
  EnumContext ctx{nullptr, RECT{0, 0, 0, 0}, &raw_global, false, true,
                  winome::get_config ().overview_window_blacklist, false};
  EnumWindows (collect_windows, reinterpret_cast<LPARAM> (&ctx));
  const gint64 snap_t_enum = g_get_monotonic_time ();

  std::vector<HWND> hwnds;
  hwnds.reserve (raw_global.size ());
  for (const OvWindowInfo &w : raw_global)
    hwnds.push_back (w.hwnd);

  std::vector<vd::DesktopInfo> ds;
  int active_idx = -1;
  std::vector<int> raw_desks;
  vd::query_desktops_and_windows_batch (hwnds, &ds, &active_idx, &raw_desks);
  const gint64 snap_t_vd = g_get_monotonic_time ();

  std::vector<int> global_counts (ds.size (), 0);
  snap.global_windows.reserve (raw_global.size ());
  snap.window_monitors.reserve (raw_global.size ());
  snap.window_desktops.reserve (raw_global.size ());
  snap.window_cloaked.reserve (raw_global.size ());
  snap.hwnd_desktop_map.reserve (raw_global.size ());

  for (size_t i = 0; i < raw_global.size (); ++i) {
    const OvWindowInfo &w = raw_global[i];
    const int d = (i < raw_desks.size () && !ds.empty ()) ? raw_desks[i] : -1;
    const DWORD cloaked = window_cloaked_flags (w.hwnd);
    if (cloaked != 0 && (ds.empty () || d < 0))
      continue;
    if (d >= 0 && d < static_cast<int> (global_counts.size ()))
      ++global_counts[static_cast<size_t> (d)];
    snap.global_windows.push_back (w);
    snap.window_monitors.push_back (
        MonitorFromRect (&w.rect, MONITOR_DEFAULTTONULL));
    snap.window_desktops.push_back (d);
    snap.window_cloaked.push_back (cloaked);
    if (d >= 0)
      snap.hwnd_desktop_map[w.hwnd] = d;
  }

  const bool can_manage =
      !ds.empty () &&
      vd::vtable_slots_for_build (vd::windows_build_number ())
          .can_manage_desktops;
  bool changed = false;
  if (can_manage) {
    DynamicWorkspacePlan plan = compute_dynamic_workspace_plan (global_counts);
    g_in_dynamic_ws_sync = true;
    for (int a = 0; a < plan.append_count; ++a) {
      if (vd::create_desktop ())
        changed = true;
    }
    for (int rm_idx : plan.remove_indices) {
      if (vd::remove_desktop (rm_idx))
        changed = true;
    }
    g_in_dynamic_ws_sync = false;

    if (changed) {
      if (plan.remove_indices.empty ()) {
        // Pure trailing desktop creation only appends empty workspaces; no
        // existing window's desktop index changed, so skip per-HWND RPCs.
        std::vector<vd::DesktopInfo> refreshed = vd::desktops ();
        if (!refreshed.empty ())
          ds = std::move (refreshed);
      } else {
        std::vector<HWND> kept_hwnds;
        kept_hwnds.reserve (snap.global_windows.size ());
        for (const OvWindowInfo &w : snap.global_windows)
          kept_hwnds.push_back (w.hwnd);
        std::vector<int> refreshed_desks;
        vd::query_desktops_and_windows_batch (kept_hwnds, &ds, &active_idx,
                                              &refreshed_desks);
        if (refreshed_desks.size () == snap.window_desktops.size ()) {
          snap.window_desktops = std::move (refreshed_desks);
          snap.hwnd_desktop_map.clear ();
          for (size_t i = 0; i < snap.global_windows.size (); ++i) {
            if (snap.window_desktops[i] >= 0)
              snap.hwnd_desktop_map[snap.global_windows[i].hwnd] =
                  snap.window_desktops[i];
          }
        }
      }
    }
  }
  const gint64 snap_t_end = g_get_monotonic_time ();
  const double total_ms = (snap_t_end - snap_t0) / 1000.0;
  if (total_ms >= 35.0) {
    g_print ("[ov-perf] shared_snap total=%.1fms enum=%.1fms vd=%.1fms "
             "dyn=%.1fms win=%zu changed=%d\n",
             total_ms, (snap_t_enum - snap_t0) / 1000.0,
             (snap_t_vd - snap_t_enum) / 1000.0,
             (snap_t_end - snap_t_vd) / 1000.0, raw_global.size (),
             changed ? 1 : 0);
  }

  snap.desktops = std::move (ds);
  snap.active_index = active_idx;
  snap.valid = true;
  return snap;
}

static bool
sync_dynamic_workspaces (HMONITOR monitor, const RECT &monitor_rect,
                         std::vector<OvWindowInfo> *out_all = nullptr,
                         std::vector<int> *out_window_desktop = nullptr,
                         std::vector<vd::DesktopInfo> *out_desktops = nullptr,
                         int *out_active_index = nullptr,
                         std::unordered_map<HWND, int> *out_hwnd_map = nullptr,
                         const SharedOpenSnapshot *shared_snap = nullptr)
{
  const ws::MultiMonitorWorkspaceMode mode = ws::effective_mode ();
  if (mode == ws::MultiMonitorWorkspaceMode::Independent) {
    const bool changed = ws::sync_dynamic_workspaces (monitor);
    std::vector<OvWindowInfo> raw_mon;
    EnumContext ctx{monitor, monitor_rect, &raw_mon, false, true,
                    winome::get_config ().overview_window_blacklist, true};
    EnumWindows (collect_windows, reinterpret_cast<LPARAM> (&ctx));

    std::vector<vd::DesktopInfo> ds = ws::desktops_for_monitor (monitor);
    std::vector<OvWindowInfo> all;
    std::vector<int> win_desk;
    all.reserve (raw_mon.size ());
    win_desk.reserve (raw_mon.size ());
    for (const OvWindowInfo &w : raw_mon) {
      const int d = ws::window_workspace (w.hwnd, monitor);
      if (d < 0)
        continue;
      all.push_back (w);
      win_desk.push_back (d);
      if (out_hwnd_map != nullptr)
        (*out_hwnd_map)[w.hwnd] = d;
    }
    if (out_all != nullptr)
      *out_all = std::move (all);
    if (out_window_desktop != nullptr)
      *out_window_desktop = std::move (win_desk);
    if (out_desktops != nullptr)
      *out_desktops = std::move (ds);
    if (out_active_index != nullptr)
      *out_active_index = ws::active_for_monitor (monitor);
    return changed;
  }

  if (mode == ws::MultiMonitorWorkspaceMode::PrimaryOnly &&
      monitor != monitor_manager_primary_monitor ()) {
    // GNOME SecondaryMonitorDisplay: fixed secondary monitor without workspace
    // switcher pills.
    std::vector<OvWindowInfo> all;
    if (shared_snap != nullptr && shared_snap->valid) {
      for (size_t i = 0; i < shared_snap->global_windows.size (); ++i) {
        if (shared_snap->window_monitors[i] != monitor ||
            shared_snap->window_cloaked[i] != 0)
          continue;
        OvWindowInfo w = shared_snap->global_windows[i];
        OffsetRect (&w.rect, -monitor_rect.left, -monitor_rect.top);
        all.push_back (w);
      }
      if (out_hwnd_map != nullptr)
        *out_hwnd_map = shared_snap->hwnd_desktop_map;
    } else {
      EnumContext ctx{monitor, monitor_rect, &all, false, false,
                      winome::get_config ().overview_window_blacklist, true};
      EnumWindows (collect_windows, reinterpret_cast<LPARAM> (&ctx));
    }
    std::vector<vd::DesktopInfo> ds = ws::desktops_for_monitor (monitor);
    std::vector<int> win_desk (all.size (), 0);
    if (out_all != nullptr)
      *out_all = std::move (all);
    if (out_window_desktop != nullptr)
      *out_window_desktop = std::move (win_desk);
    if (out_desktops != nullptr)
      *out_desktops = std::move (ds);
    if (out_active_index != nullptr)
      *out_active_index = 0;
    return false;
  }

  // Fast path: reuse a single shared snapshot captured once for all monitors.
  if (shared_snap != nullptr && shared_snap->valid && shared_snap->mode == mode) {
    std::vector<OvWindowInfo> all;
    std::vector<int> win_desk;
    all.reserve (shared_snap->global_windows.size ());
    win_desk.reserve (shared_snap->global_windows.size ());
    for (size_t i = 0; i < shared_snap->global_windows.size (); ++i) {
      if (shared_snap->window_monitors[i] != monitor)
        continue;
      OvWindowInfo w = shared_snap->global_windows[i];
      OffsetRect (&w.rect, -monitor_rect.left, -monitor_rect.top);
      all.push_back (w);
      win_desk.push_back (shared_snap->window_desktops[i]);
    }
    if (out_all != nullptr)
      *out_all = std::move (all);
    if (out_window_desktop != nullptr)
      *out_window_desktop = std::move (win_desk);
    if (out_desktops != nullptr)
      *out_desktops = shared_snap->desktops;
    if (out_active_index != nullptr)
      *out_active_index = shared_snap->active_index;
    if (out_hwnd_map != nullptr)
      *out_hwnd_map = shared_snap->hwnd_desktop_map;
    return false;
  }

  // Single-monitor path: use ONE batched COM query instead of N per-window RPCs.
  SharedOpenSnapshot local = capture_shared_open_snapshot ();
  std::vector<OvWindowInfo> all;
  std::vector<int> win_desk;
  all.reserve (local.global_windows.size ());
  win_desk.reserve (local.global_windows.size ());
  for (size_t i = 0; i < local.global_windows.size (); ++i) {
    if (local.window_monitors[i] != monitor)
      continue;
    OvWindowInfo w = local.global_windows[i];
    OffsetRect (&w.rect, -monitor_rect.left, -monitor_rect.top);
    all.push_back (w);
    win_desk.push_back (local.window_desktops[i]);
  }
  if (out_all != nullptr)
    *out_all = std::move (all);
  if (out_window_desktop != nullptr)
    *out_window_desktop = std::move (win_desk);
  if (out_desktops != nullptr)
    *out_desktops = std::move (local.desktops);
  if (out_active_index != nullptr)
    *out_active_index = local.active_index;
  if (out_hwnd_map != nullptr)
    *out_hwnd_map = std::move (local.hwnd_desktop_map);
  return false;
}


// --- layout ----------------------------------------------------------------------

static int
picker_workspace_spacing (const RECT &base_picker, int mw, int scale)
{
  int pw = base_picker.right - base_picker.left;
  int avail_space = (mw - pw) / 2;
  int raw = avail_space - static_cast<int> (pw * 0.4);
  return std::clamp (raw, kWorkspaceMinSpacing * scale,
                     kWorkspaceMaxSpacing * scale);
}

// workspacesView.js _getFirstFitSingleWorkspaceBox + _updateWorkspacesState:
// Inactive workspaces scale by WORKSPACE_INACTIVE_SCALE (0.94) and sit at a
// visual edge spacing of [24..80]*scale from the active workspace so the left
// and right adjacent workspaces peek into the overview.
static RECT
picker_box_for_desktop (int d, double scroll_pos, const RECT &base_picker,
                        int mw, int scale)
{
  double pw = static_cast<double> (base_picker.right - base_picker.left);
  double ph = static_cast<double> (base_picker.bottom - base_picker.top);
  double base_cx = (base_picker.left + base_picker.right) * 0.5;
  double base_cy = (base_picker.top + base_picker.bottom) * 0.5;
  int gap = picker_workspace_spacing (base_picker, mw, scale);
  double dist = static_cast<double> (d) - scroll_pos;
  double s_prog = 1.0 - std::clamp (std::abs (dist), 0.0, 1.0);
  double ws_scale =
      kWorkspaceInactiveScale + (1.0 - kWorkspaceInactiveScale) * s_prog;
  double step = 0.5 * (1.0 + kWorkspaceInactiveScale) * pw + gap;
  double cx = base_cx + dist * step;
  double w = pw * ws_scale;
  double h = ph * ws_scale;
  RECT out;
  out.left = static_cast<LONG> (std::lround (cx - w * 0.5));
  out.top = static_cast<LONG> (std::lround (base_cy - h * 0.5));
  out.right = static_cast<LONG> (std::lround (cx + w * 0.5));
  out.bottom = static_cast<LONG> (std::lround (base_cy + h * 0.5));
  return out;
}

// workspacesView.js _getFirstFitAllWorkspaceBox (FitMode.ALL in APP_GRID):
// All workspaces line up horizontally inside the 15% top workspace bar.
static RECT
grid_box_for_desktop (int d, int count, const RECT &base_grid, int mw,
                      int scale)
{
  int n = std::max (count, 1);
  int base_w = base_grid.right - base_grid.left;
  int base_h = base_grid.bottom - base_grid.top;
  if (n <= 1)
    return base_grid;
  int gap = kWorkspaceMinSpacing * scale;
  int max_total_w = std::max (mw - 2 * gap, n * 64);
  int avail_for_boxes = std::max (max_total_w - (n - 1) * gap, n * 48);
  int ws_w = std::min (base_w, avail_for_boxes / n);
  double aspect =
      base_h > 0 ? static_cast<double> (base_w) / base_h : 1.7777778;
  int ws_h = std::min (
      base_h, std::max (static_cast<int> (ws_w / aspect + 0.5), 48));
  ws_w = std::max (static_cast<int> (ws_h * aspect + 0.5), 48);
  int total_w = n * ws_w + (n - 1) * gap;
  int start_x = (mw - total_w) / 2;
  int top_y = base_grid.top + (base_h - ws_h) / 2;
  int left_x = start_x + d * (ws_w + gap);
  return RECT{left_x, top_y, left_x + ws_w, top_y + ws_h};
}

static RECT
full_box_for_desktop (int d, int active_idx, const RECT &full_rect,
                      const RECT &base_picker, int mw, int scale)
{
  int gap = picker_workspace_spacing (base_picker, mw, scale);
  int stride = (full_rect.right - full_rect.left) + gap;
  return translate_rect_x (full_rect,
                           static_cast<LONG> ((d - active_idx) * stride));
}

struct PlacedGeometry {
  int search_x, search_y;      // logical
  int search_w, search_h;      // logical
  int search_view_x, search_view_y; // logical (#searchResults box)
  int search_view_w, search_view_h; // logical
  int dash_x, dash_y;          // logical
  int thumbs_x, thumbs_y;      // logical
  int thumbs_h;                // physical (0 = hidden)
  RECT ws_box_picker;          // physical WINDOW_PICKER workspace box
  RECT ws_box_grid;            // physical APP_GRID workspace box
  RECT app_grid_box;           // physical App Grid container box
  double radius_picker = kWorkspaceRadiusFallback;
  double radius_grid = 5.0;
};

RECT
remap_rect_between_boxes (const RECT &r, const RECT &from_box,
                          const RECT &to_box)
{
  double fw = std::max ((double)(from_box.right - from_box.left), 1.0);
  double fh = std::max ((double)(from_box.bottom - from_box.top), 1.0);
  double tw = std::max ((double)(to_box.right - to_box.left), 1.0);
  double th = std::max ((double)(to_box.bottom - to_box.top), 1.0);
  RECT out;
  out.left = to_box.left + (LONG)((r.left - from_box.left) / fw * tw);
  out.top = to_box.top + (LONG)((r.top - from_box.top) / fh * th);
  out.right = std::max (
      out.left + 1, to_box.left + (LONG)((r.right - from_box.left) / fw * tw));
  out.bottom = std::max (
      out.top + 1, to_box.top + (LONG)((r.bottom - from_box.top) / fh * th));
  return out;
}


// --- clicks --------------------------------------------------------------------------

static bool
is_shell_core_window (HWND hwnd)
{
  if (hwnd == nullptr || !IsWindow (hwnd))
    return false;
  wchar_t cls[64] = {};
  const int n = GetClassNameW (hwnd, cls, 63);
  return n > 0 && wcscmp (cls, L"Windows.UI.Core.CoreWindow") == 0;
}



// --- Unified Per-Monitor OverviewView ----------------------------------------

struct OverviewView {
  HMONITOR m_monitor = nullptr;
  GtkWidget *m_window = nullptr;
  HWND m_hwnd = nullptr;
  WNDPROC m_orig_overview_wndproc = nullptr;
  bool m_overview_surface_hooked = false;
  int m_panel_height = 0;

  GtkWidget *m_root = nullptr;
  GtkWidget *m_focus_sink = nullptr;
  GtkWidget *m_search = nullptr;
  GtkWidget *m_search_view = nullptr;
  GtkWidget *m_ws_bg = nullptr;
  GtkWidget *m_app_grid_clip = nullptr;
  GtkWidget *m_app_grid = nullptr;
  GtkWidget *m_dash = nullptr;
  GtkWidget *m_thumbs = nullptr;

  bool m_show_dash = true;
  bool m_want_focus_on_open = true;
  bool m_cloaked_for_open = false;
  bool m_pending_reveal = false;
  bool m_uncloak_on_after_paint = false;
  guint m_uncloak_fallback_timer = 0;
  guint m_vd_cloak_guard_timer = 0;
  bool m_app_grid_mode = false;
  bool m_search_mode = false;
  bool m_folder_dialog_open = false;
  ControlsState m_target_state = ControlsState::HIDDEN;
  ControlsState m_pending_show_state = ControlsState::WINDOW_PICKER;
  bool m_placed = false;
  bool m_thumbs_has_pills = false;
  int m_grid_x = 0, m_grid_y = 0, m_grid_w = 0, m_grid_h = 0;
  double m_thumbs_cur_opacity = 0.0;
  double m_thumbs_cur_scale = 1.0;
  double m_thumbs_cur_ty = 0.0;
  double m_grid_cur_opacity = 0.0;
  double m_grid_cur_slide_y = kGridSlideOffsetPx;
  double m_panel_cur_progress = 0.0;

  int m_drag_slot = -1;
  HWND m_drag_hwnd = nullptr;
  RECT m_drag_orig_rect{};
  int m_drag_start_px = 0;
  int m_drag_start_py = 0;
  int m_drag_offset_x = 0;
  int m_drag_offset_y = 0;
  int m_drag_cur_px = 0;
  int m_drag_cur_py = 0;
  double m_drag_target_scale = 0.55;
  gint64 m_drag_scale_start_us = 0;
  guint m_drag_tick_source = 0;
  int m_drag_snapback_slot = -1;
  RECT m_drag_snapback_from{};
  BYTE m_drag_snapback_from_op = 215;
  gint64 m_drag_snapback_start_us = 0;
  guint m_drag_snapback_source = 0;
  int m_drag_drop_ws_peek = -1;
  bool m_drag_active = false;
  bool m_drag_just_ended = false;
  bool m_drag_thumbs_revealed = false;

  std::string m_search_query;
  std::string m_search_raw_text;
  bool m_search_preedit_active = false;
  guint m_search_fade_source = 0;
  gint64 m_search_fade_start_us = 0;
  double m_search_fade_from = 0.0;
  double m_search_fade_to = 0.0;
  double m_search_fade_cur = 0.0;

  std::vector<vd::DesktopInfo> m_desktops;
  int m_desktop_active = -1;
  bool m_pending_desktop_sync = false;
  int m_pending_desktop_target = -1;
  guint m_place_opening_source = 0;
  guint m_place_idle_source = 0;
  guint m_close_rebuild_timer = 0;

  int m_grid_last_moved_y = INT_MIN;
  BYTE m_mini_target_opacity = 0;
  double m_ws_bg_last_radius = -1.0;
  bool m_ws_bg_settled_shadow_drawn = false;
  guint m_settle_hover_timer = 0;
  guint m_focus_verify_timer = 0;
  GdkFrameClock *m_overview_clock = nullptr;
  gulong m_overview_after_paint_id = 0;
  bool m_dwm_frame_pending = false;
  bool m_dwm_pending_has_mini = false;
  BYTE m_dwm_pending_mini_opacity = 0;
  gint64 m_anim_tick_cpu_max_us = 0;
  gint64 m_anim_dwm_flush_max_us = 0;
  int m_anim_panel_dirty_frames = 0;
  bool m_gl_backing_sanitized = false;
  bool m_open_perf_pending = false;
  gint64 m_open_perf_show_us = 0;
  gint64 m_open_perf_trans_us = 0;
  gint64 m_open_perf_tick0_enter_us = 0;
  gint64 m_open_perf_tick0_exit_us = 0;
  gint64 m_open_perf_paint0_us = 0;
  gint64 m_open_perf_tick1_enter_us = 0;

  RECT m_bg_full = {0, 0, 0, 0};
  RECT m_bg_picker = {0, 0, 0, 0};
  RECT m_bg_grid = {0, 0, 0, 0};
  RECT m_bg_final = {0, 0, 0, 0};
  RECT m_bg_cur = {0, 0, 0, 0};
  std::vector<RECT> m_ws_full_boxes;
  std::vector<RECT> m_ws_picker_boxes;
  std::vector<RECT> m_ws_grid_boxes;
  std::vector<RECT> m_ws_cur_boxes;
  std::vector<RECT> m_ws_hist1_boxes;
  std::vector<RECT> m_ws_hist2_boxes;
  std::vector<RECT> m_ws_anim_from_boxes;
  std::vector<RECT> m_ws_anim_to_boxes;
  int m_ext_drop_highlight_ws = -1;
  double m_bg_radius_target = kWorkspaceRadiusFallback;
  double m_radius_picker = kWorkspaceRadiusFallback;
  double m_radius_grid = 12.0;
  double m_bg_radius = kWorkspaceRadiusFallback;

  std::vector<SlotEntry> m_slots;
  int m_hover_index = -1;
  std::vector<MiniSlot> m_mini_slots;

  int m_monitor_w = 0, m_monitor_h = 0;
  int m_last_layout_w = 0, m_last_layout_h = 0;
  int m_placed_scale = 0;
  int m_placed_search_h = 0;
  int m_dash_top = 0;
  RECT m_search_rect = {0, 0, 0, 0};
  RECT m_thumbs_rect = {0, 0, 0, 0};
  RECT m_ws_box = {0, 0, 0, 0};
  RECT m_want_hwnd_rect = {0, 0, 0, 0};

  Animation m_anim;
  std::vector<SlotEntry> m_leaving;
  guint m_switch_source = 0;
  bool m_switch_started = false;
  gint64 m_switch_start_us = 0;
  double m_switch_from = -1.0, m_switch_to = -1.0, m_switch_cur_pos = 0.0;
  int m_switch_width = 0;
  gint64 m_last_ws_scroll_us = 0;
  gint64 m_last_internal_switch_us = 0;
  int m_last_internal_switch_from = -1;
  int m_last_internal_switch_to = -1;

  std::vector<ScaleAnim> m_scale_anims;
  guint m_scale_anim_source = 0;
  int m_last_hovered_slot = -1;
  double m_overlay_opacity = 0.0;
  double m_overlay_from = 0.0, m_overlay_to = 0.0;
  gint64 m_overlay_anim_start = 0;
  guint m_overlay_anim_ms = 0;
  guint m_hover_idle_source = 0;

  // --- Monitor & wallpaper helpers ---
  HMONITOR resolve_monitor () const
  {
    if (is_valid_monitor (m_monitor))
      return m_monitor;
    HMONITOR active = monitor_manager_active_monitor ();
    if (is_valid_monitor (active))
      return active;
    if (g_panel_hwnd != nullptr && IsWindow (g_panel_hwnd))
      return MonitorFromWindow (g_panel_hwnd, MONITOR_DEFAULTTOPRIMARY);
    return MonitorFromPoint (POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  }

  HWND panel_hwnd_for_view () const
  {
    HMONITOR mon = resolve_monitor ();
    if (HWND managed = monitor_manager_panel_for_monitor (mon))
      return managed;
    if (g_panel_hwnd != nullptr && IsWindow (g_panel_hwnd) &&
        MonitorFromWindow (g_panel_hwnd, MONITOR_DEFAULTTONEAREST) == mon) {
      return g_panel_hwnd;
    }
    return nullptr;
  }

  int resolve_panel_inset () const
  {
    HWND ph = panel_hwnd_for_view ();
    if (ph == nullptr || !IsWindow (ph))
      return 0;
    RECT pr{};
    if (IsWindowVisible (ph) && GetWindowRect (ph, &pr) && pr.bottom > pr.top)
      return pr.bottom - pr.top;
    return std::max (winome_shell_panel_height (), 0);
  }

  bool is_open () const
  {
    return m_window != nullptr && gtk_widget_get_visible (m_window) &&
           m_target_state != ControlsState::HIDDEN;
  }

  void uncloak_if_needed ()
  {
    if (m_cloaked_for_open) {
      m_cloaked_for_open = false;
      winome::sanitize_gdk_gl_backing_windows ();
      set_hwnd_cloaked (m_hwnd, false);
      winome::sanitize_gdk_gl_backing_windows ();
    }
  }

  void cancel_uncloak_fallback ()
  {
    if (m_uncloak_fallback_timer != 0) {
      g_source_remove (m_uncloak_fallback_timer);
      m_uncloak_fallback_timer = 0;
    }
  }

  void cancel_vd_cloak_guard ()
  {
    if (m_vd_cloak_guard_timer != 0) {
      g_source_remove (m_vd_cloak_guard_timer);
      m_vd_cloak_guard_timer = 0;
    }
  }

  void ensure_overview_visible_on_active_desktop (int reported_active_index = -1)
  {
    if (m_hwnd == nullptr || !IsWindow (m_hwnd) || m_window == nullptr ||
        !gtk_widget_get_visible (m_window) ||
        m_target_state == ControlsState::HIDDEN) {
      return;
    }
    winome::vd::pin_window_async (m_hwnd);
    HWND chrome_layers[] = {g_chrome_hwnd, g_icon_hwnd, g_caption_hwnd,
                            active_folder_hwnd ()};
    for (HWND ch : chrome_layers) {
      if (is_visible_hwnd (ch))
        winome::vd::pin_window_async (ch);
    }

    DWORD cloaked = 0;
    DwmGetWindowAttribute (m_hwnd, DWMWA_CLOAKED, &cloaked, sizeof (cloaked));
    if (should_rescue_overview_vd_cloak (cloaked, true, true)) {
      const auto snap = winome::vd::cached_snapshot ();
      int target_vd = resolve_native_desktop_for_overview_rescue (
          ws::effective_mode (), reported_active_index,
          snap.valid ? snap.active_index : -1);
      if (target_vd < 0)
        target_vd = winome::vd::current_index ();
      if (target_vd >= 0) {
        winome::vd::move_window_to_desktop (m_hwnd, target_vd);
        winome::vd::pin_window_async (m_hwnd);
        for (HWND ch : chrome_layers) {
          if (is_visible_hwnd (ch)) {
            DWORD ch_cloak = 0;
            DwmGetWindowAttribute (ch, DWMWA_CLOAKED, &ch_cloak,
                                   sizeof (ch_cloak));
            if ((ch_cloak & 0x2u /* DWM_CLOAKED_SHELL */) != 0) {
              winome::vd::move_window_to_desktop (ch, target_vd);
              winome::vd::pin_window_async (ch);
            }
          }
        }
        if (!m_cloaked_for_open) {
          SetWindowPos (m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                            SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
          restack_locked ();
        }
        g_print ("[ov] rescued shell-cloaked overview mon=%p hwnd=%p -> vd=%d\n",
                 static_cast<void *> (m_monitor),
                 static_cast<void *> (m_hwnd), target_vd);
      }
    }
  }

  void schedule_vd_cloak_guard (guint delay_ms)
  {
    cancel_vd_cloak_guard ();
    m_vd_cloak_guard_timer =
        g_timeout_add (delay_ms, vd_cloak_guard_static_cb, this);
  }

  void complete_open_uncloak ()
  {
    m_uncloak_on_after_paint = false;
    cancel_uncloak_fallback ();
    if (m_window == nullptr || !gtk_widget_get_visible (m_window) ||
        m_target_state == ControlsState::HIDDEN) {
      return;
    }
    uncloak_if_needed ();
    ensure_overview_visible_on_active_desktop (m_desktop_active);
    schedule_vd_cloak_guard (100);
    HMONITOR mon = resolve_monitor ();
    if (m_want_focus_on_open &&
        winome_shell_panel_top_popover_hwnd_for_monitor (mon) == nullptr) {
      focus_overview_window (false);
    }
    restack_locked ();
    if (m_want_focus_on_open && can_auto_focus_overview_search ()) {
      if (GetForegroundWindow () == m_hwnd)
        ensure_search_entry_focus ();
      else
        schedule_focus_verify ();
    }
  }

  void arm_uncloak_after_paint ()
  {
    m_uncloak_on_after_paint = true;
    if (m_window != nullptr) {
      if (GdkSurface *s = gtk_native_get_surface (GTK_NATIVE (m_window)))
        ensure_after_paint_hook (gdk_surface_get_frame_clock (s));
      gtk_widget_queue_draw (m_window);
    }
    if (m_ws_bg != nullptr)
      gtk_widget_queue_draw (m_ws_bg);
    cancel_uncloak_fallback ();
    m_uncloak_fallback_timer =
        g_timeout_add (120, uncloak_fallback_static_cb, this);
  }

  GdkTexture *active_wallpaper_texture () const
  {
    if (g_using_live_wallpaper && g_live_wallpaper_owner == this &&
        g_live_wallpaper != nullptr) {
      return g_live_wallpaper;
    }
    return g_wallpaper;
  }

  GdkTexture *active_wallpaper_thumb_texture () const
  {
    if (g_using_live_wallpaper && g_live_wallpaper_owner == this) {
      if (g_live_wallpaper_thumb != nullptr)
        return g_live_wallpaper_thumb;
      if (g_live_wallpaper != nullptr)
        return g_live_wallpaper;
    }
    return g_wallpaper_thumb != nullptr ? g_wallpaper_thumb : g_wallpaper;
  }

  void dismiss (const char *reason)
  {
    if (winome::get_config ().multi_monitor_overview_linked)
      close_overview (reason);
    else
      close_overview_for_monitor (reason, resolve_monitor ());
  }

  // Static GTK/GLib callback trampolines forwarding to `this`.
  static gboolean uncloak_fallback_static_cb (gpointer ud) {
    auto *self = static_cast<OverviewView *> (ud);
    self->m_uncloak_fallback_timer = 0;
    self->complete_open_uncloak ();
    return G_SOURCE_REMOVE;
  }
  static gboolean vd_cloak_guard_static_cb (gpointer ud) {
    auto *self = static_cast<OverviewView *> (ud);
    self->m_vd_cloak_guard_timer = 0;
    self->ensure_overview_visible_on_active_desktop (-1);
    return G_SOURCE_REMOVE;
  }
  static gboolean hover_anim_tick_cb (GtkWidget *w, GdkFrameClock *c, gpointer ud) {
    return static_cast<OverviewView *> (ud)->hover_anim_tick (w, c, nullptr);
  }
  static gboolean hover_idle_hide_static_cb (gpointer ud) {
    return static_cast<OverviewView *> (ud)->hover_idle_hide_cb (nullptr);
  }
  static void on_overview_after_paint_cb (GdkFrameClock *c, gpointer ud) {
    static_cast<OverviewView *> (ud)->on_overview_after_paint (c, nullptr);
  }
  static gboolean settle_hover_static_cb (gpointer ud) {
    return static_cast<OverviewView *> (ud)->settle_hover_cb (nullptr);
  }
  static gboolean overview_place_idle_cb (gpointer ud) {
    return static_cast<OverviewView *> (ud)->overview_place_idle (nullptr);
  }
  static gboolean overview_place_opening_idle_cb (gpointer ud) {
    return static_cast<OverviewView *> (ud)->overview_place_opening_idle (nullptr);
  }
  static gboolean anim_tick_cb (GtkWidget *w, GdkFrameClock *c, gpointer ud) {
    return static_cast<OverviewView *> (ud)->anim_tick (w, c, nullptr);
  }
  static gboolean switch_tick_cb (GtkWidget *w, GdkFrameClock *c, gpointer ud) {
    return static_cast<OverviewView *> (ud)->switch_tick (w, c, nullptr);
  }
  static gboolean delayed_rebuild_static_cb (gpointer ud) {
    return static_cast<OverviewView *> (ud)->delayed_rebuild_cb (nullptr);
  }
  static gboolean drag_scale_tick_cb (GtkWidget *w, GdkFrameClock *c, gpointer ud) {
    return static_cast<OverviewView *> (ud)->drag_scale_tick (w, c, nullptr);
  }
  static gboolean drag_snapback_tick_cb (GtkWidget *w, GdkFrameClock *c, gpointer ud) {
    return static_cast<OverviewView *> (ud)->drag_snapback_tick (w, c, nullptr);
  }
  static gboolean focus_verify_static_cb (gpointer ud) {
    return static_cast<OverviewView *> (ud)->focus_verify_cb (nullptr);
  }
  static void on_overview_surface_layout_cb (GdkSurface *s, int w, int h, gpointer ud) {
    static_cast<OverviewView *> (ud)->on_overview_surface_layout (s, w, h, nullptr);
  }
  static gboolean search_fade_tick_cb (GtkWidget *w, GdkFrameClock *c, gpointer ud) {
    return static_cast<OverviewView *> (ud)->search_fade_tick (w, c, nullptr);
  }
  static void on_realize_cb (GtkWidget *w, gpointer ud) {
    static_cast<OverviewView *> (ud)->on_realize (w, nullptr);
  }
  static void on_overview_map_cb (GtkWidget *w, gpointer ud) {
    static_cast<OverviewView *> (ud)->on_overview_map (w, nullptr);
  }
  static void on_root_motion_cb (GtkEventControllerMotion *m, double x, double y, gpointer ud) {
    static_cast<OverviewView *> (ud)->on_root_motion (m, x, y, nullptr);
  }
  static void on_root_motion_leave_cb (GtkEventControllerMotion *m, gpointer ud) {
    static_cast<OverviewView *> (ud)->on_root_motion_leave (m, nullptr);
  }
  static void on_drag_begin_cb (GtkGestureDrag *d, double x, double y, gpointer ud) {
    static_cast<OverviewView *> (ud)->on_drag_begin (d, x, y, nullptr);
  }
  static void on_drag_update_cb (GtkGestureDrag *d, double dx, double dy, gpointer ud) {
    static_cast<OverviewView *> (ud)->on_drag_update (d, dx, dy, nullptr);
  }
  static void on_drag_end_cb (GtkGestureDrag *d, double dx, double dy, gpointer ud) {
    static_cast<OverviewView *> (ud)->on_drag_end (d, dx, dy, nullptr);
  }
  static void on_click_released_cb (GtkGestureClick *g, int n, double x, double y, gpointer ud) {
    static_cast<OverviewView *> (ud)->on_click_released (g, n, x, y, nullptr);
  }
  static gboolean on_overview_key_pressed_cb (GtkEventControllerKey *c, guint kv, guint kc, GdkModifierType st, gpointer ud) {
    return static_cast<OverviewView *> (ud)->on_overview_key_pressed (c, kv, kc, st, nullptr);
  }
  static gboolean on_overview_scroll_cb (GtkEventControllerScroll *c, double dx, double dy, gpointer ud) {
    return static_cast<OverviewView *> (ud)->on_overview_scroll (c, dx, dy, nullptr);
  }
  static void on_thumb_clicked_cb (int idx, void *ud) {
    static_cast<OverviewView *> (ud)->on_thumb_clicked (idx, nullptr);
  }
  static void on_thumb_add_desktop_cb (void *ud) {
    static_cast<OverviewView *> (ud)->on_thumb_add_desktop (nullptr);
  }
  static void on_thumb_remove_desktop_cb (int idx, void *ud) {
    static_cast<OverviewView *> (ud)->on_thumb_remove_desktop (idx, nullptr);
  }
  static void on_dash_show_apps_clicked_cb (void *ud) {
    static_cast<OverviewView *> (ud)->on_dash_show_apps_clicked (nullptr);
  }
  static void on_dash_changed_cb (void *ud) {
    static_cast<OverviewView *> (ud)->on_dash_changed (nullptr);
  }
  static void on_app_grid_launch_done_cb (void *ud) {
    static_cast<OverviewView *> (ud)->on_app_grid_launch_done (nullptr);
  }
  static void on_app_grid_pins_changed_cb (void *ud) {
    static_cast<OverviewView *> (ud)->on_app_grid_pins_changed (nullptr);
  }
  static void on_app_grid_folder_toggled_cb (bool open, void *ud) {
    static_cast<OverviewView *> (ud)->on_app_grid_folder_toggled (open, nullptr);
  }
  static void on_search_changed_cb (GtkEditable *ed, gpointer ud) {
    static_cast<OverviewView *> (ud)->on_search_changed (ed, nullptr);
  }
  static void on_search_icon_press_cb (GtkEntry *e, GtkEntryIconPosition pos, gpointer ud) {
    static_cast<OverviewView *> (ud)->on_search_icon_press (e, pos, nullptr);
  }
  static void on_search_activate_cb (GtkEntry *e, gpointer ud) {
    static_cast<OverviewView *> (ud)->on_search_activate (e, nullptr);
  }
  static void on_search_view_close_cb (const char *reason, bool discard_focus, void *ud) {
    static_cast<OverviewView *> (ud)->on_search_view_close_overview (reason, discard_focus, nullptr);
  }
  static void on_search_view_activate_cb (HWND hwnd, int desktop_index, void *ud) {
    static_cast<OverviewView *> (ud)->on_search_view_activate_window (hwnd, desktop_index, nullptr);
  }
  static std::vector<SearchWindowCandidate> on_search_view_get_windows_cb (void *ud) {
    return static_cast<OverviewView *> (ud)->on_search_view_get_open_windows (nullptr);
  }
  static void on_search_view_open_settings_cb (const char *page, void *ud) {
    static_cast<OverviewView *> (ud)->on_search_view_open_settings (page, nullptr);
  }

// The chrome (icon, close button, caption) follows the SCALED preview rect
// (windowPreview.js _adjustOverlayOffsets).
RECT
slot_display_rect (int index)
{
  if (index < 0 || index >= (int)m_slots.size ())
    return RECT{0, 0, 0, 0};
  const SlotEntry &slot = m_slots[index];
  if (slot.current_scale != 1.0)
    return scale_rect_around_center (slot.rect, slot.current_scale);
  return slot.rect;
}

RECT
hover_display_rect (void)
{
  int index = m_hover_index >= 0 ? m_hover_index : m_last_hovered_slot;
  return slot_display_rect (index);
}

int
slot_at_cursor (void)
{
  if (m_hwnd == nullptr || m_drag_slot >= 0 || m_drag_active ||
      m_drag_snapback_source != 0)
    return -1;
  POINT pt;
  if (!GetCursorPos (&pt) || !ScreenToClient (m_hwnd, &pt))
    return -1;
  const int active_ws = std::max (m_desktop_active, 0);
  for (int i = 0; i < (int)m_slots.size (); ++i) {
    if (m_slots[i].desktop_index != active_ws)
      continue;
    const RECT r = slot_display_rect (i);
    if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom)
      return i;
  }
  return -1;
}

void
activate_slot_and_close (int index, const char *reason)
{
  overview_discard_restore_focus ();
  if (index >= 0 && index < (int)m_slots.size () &&
      IsWindow (m_slots[index].target)) {
    promote_slot_thumb_to_top (m_slots[index]);
    HWND target = m_slots[index].target;
    if (IsIconic (target))
      ShowWindow (target, SW_RESTORE);
    SetForegroundWindow (target);
  }
  dismiss (reason);
}


void
keep_hover_alive_from_chrome (void)
{
  if (m_app_grid_mode || m_search_mode || m_search_fade_cur > 0.005) {
    set_hover (-1, true);
    return;
  }
  cancel_hover_idle ();
  if (m_hover_index < 0 && m_last_hovered_slot >= 0 &&
      m_last_hovered_slot < (int)m_slots.size ()) {
    set_hover (m_last_hovered_slot);
  } else if (m_hover_index >= 0 && m_overlay_to < 1.0) {
    overlay_fade_start (1.0);
    hover_layout_chrome ();
  }
}

// Position the close button, app icon, and caption pill against the current
// scaled preview rect.
void
hover_layout_chrome (void)
{
  int index = m_hover_index >= 0 ? m_hover_index : m_last_hovered_slot;
  if (m_app_grid_mode || m_search_mode || m_search_fade_cur > 0.005 ||
      index < 0 || index >= (int)m_slots.size () ||
      !IsWindow (m_slots[index].target) ||
      (m_overlay_opacity <= 0.001 && m_overlay_to <= 0.01)) {
    if (g_chrome_owner_view == this) {
      overlay_hide_all ();
      g_chrome_owner_view = nullptr;
    }
    return;
  }
  GtkWidget *chrome_wins[] = {g_chrome_window, g_icon_window, g_caption_window};
  for (GtkWidget *cw : chrome_wins) {
    if (cw != nullptr && !gtk_widget_get_realized (cw))
      gtk_widget_realize (cw);
  }

  const SlotEntry &slot = m_slots[index];
  RECT disp = hover_display_rect ();
  const bool chrome_changed =
      (g_chrome_target != slot.target || g_chrome_owner_view != this ||
       !is_visible_hwnd (g_chrome_hwnd));
  g_chrome_target = slot.target;
  chrome_show_at (this, disp);
  icon_show_at (this, disp, slot.target);
  caption_show_at (this, disp);
  // Skip expensive EnumDisplayMonitors + 48-HWND Z-order scans on pure
  // hover-scale animation frames when chrome is already visible for this slot.
  if (chrome_changed)
    restack_locked ();
}

// Frame-clock driver for the hover scale + caption fade animations: locked
// to the display refresh instead of a fixed 16ms timer beating against it.
gboolean
hover_anim_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
  (void)widget;
  (void)user_data;
  if (m_app_grid_mode || m_search_mode || m_search_fade_cur > 0.005 ||
      m_anim.source != 0 || m_switch_source != 0 ||
      m_target_state == ControlsState::HIDDEN) {
    m_scale_anims.clear ();
    m_hover_index = -1;
    m_last_hovered_slot = -1;
    m_overlay_opacity = 0.0;
    m_overlay_from = 0.0;
    m_overlay_to = 0.0;
    if (g_chrome_owner_view == this) {
      overlay_hide_all ();
      g_chrome_owner_view = nullptr;
    }
    m_scale_anim_source = 0;
    return G_SOURCE_REMOVE;
  }
  gint64 now = gdk_frame_clock_get_frame_time (clock);

  bool active = false;
  for (ScaleAnim &anim : m_scale_anims) {
    double t = (now - anim.start_us) / 1000.0 / (double)kWindowScaleMs;
    if (t < 0.0)
      t = 0.0;   // frame timestamp can predate the anim start
    if (t > 1.0)
      t = 1.0;
    double e = ease_out_quad (t);
    double s = anim.from + (anim.to - anim.from) * e;
    double final_s = (t < 1.0) ? s : anim.to;
    if (anim.index >= 0 && anim.index < (int)m_slots.size () &&
        anim.index != m_drag_slot && anim.index != m_drag_snapback_slot) {
      SlotEntry &slot = m_slots[anim.index];
      slot.current_scale = final_s;
      apply_thumb_rect (slot, scale_rect_around_center (slot.rect, final_s),
                        255);
    }
    if (t < 1.0)
      active = true;
  }
  m_scale_anims.erase (
      std::remove_if (m_scale_anims.begin (), m_scale_anims.end (),
                      [now] (const ScaleAnim &a) {
                        return (now - a.start_us) / 1000.0 >=
                               (double)kWindowScaleMs;
                      }),
      m_scale_anims.end ());

  // Unified overlay fade across close button, app icon, and caption pill.
  if (m_overlay_opacity != m_overlay_to) {
    double t = m_overlay_anim_ms > 0
                   ? (now - m_overlay_anim_start) / 1000.0 /
                         (double)m_overlay_anim_ms
                   : 1.0;
    if (t < 0.0)
      t = 0.0;
    if (t > 1.0)
      t = 1.0;
    double e = ease_out_quad (t);
    m_overlay_opacity =
        m_overlay_from + (m_overlay_to - m_overlay_from) * e;
    if (t >= 1.0) {
      m_overlay_opacity = m_overlay_to;
      if (m_overlay_to <= 0.01)
        if (g_chrome_owner_view == this) {
      overlay_hide_all ();
      g_chrome_owner_view = nullptr;
    }
    } else {
      active = true;
    }
  }

  hover_layout_chrome ();

  if (!active && m_scale_anims.empty () &&
      m_overlay_opacity == m_overlay_to) {
    if (m_hover_index < 0)
      m_last_hovered_slot = -1;
    m_scale_anim_source = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

void
hover_anim_ensure (void)
{
  if (m_scale_anim_source == 0)
    m_scale_anim_source = gtk_widget_add_tick_callback (
        m_window, hover_anim_tick_cb, this, nullptr);
}

void
hover_scale_start (int index, double to)
{
  if (index < 0 || index >= (int)m_slots.size ())
    return;
  double from = m_slots[index].current_scale;
  for (ScaleAnim &a : m_scale_anims) {
    if (a.index == index) {
      a.from = from;
      a.to = to;
      a.start_us = g_get_monotonic_time ();
      hover_anim_ensure ();
      return;
    }
  }
  if (std::fabs (from - to) < 1e-6)
    return;
  m_scale_anims.push_back (
      ScaleAnim{index, from, to, g_get_monotonic_time ()});
  hover_anim_ensure ();
}

void
overlay_fade_start (double to)
{
  if (to > 0.5 && m_overlay_opacity <= 0.01)
    m_overlay_opacity = 0.02;
  m_overlay_from = m_overlay_opacity;
  m_overlay_to = to;
  m_overlay_anim_start = g_get_monotonic_time ();
  m_overlay_anim_ms = kOverlayFadeMs;
  hover_anim_ensure ();
}

// WINDOW_OVERLAY_IDLE_HIDE_TIMEOUT: hide the overlay 750ms after the pointer
// left, unless it is over the preview or one of the overlay pieces (GNOME's
// idle check looks at has-pointer on the close button / icon / title).
gboolean
hover_idle_hide_cb (gpointer user_data)
{
  (void)user_data;

  if (m_drag_slot >= 0 || m_drag_active || m_drag_snapback_source != 0 ||
      m_app_grid_mode || m_search_mode || m_search_fade_cur > 0.005) {
    m_hover_idle_source = 0;
    if (m_app_grid_mode || m_search_mode || m_search_fade_cur > 0.005)
      if (g_chrome_owner_view == this) {
      overlay_hide_all ();
      g_chrome_owner_view = nullptr;
    }
    return G_SOURCE_REMOVE;
  }

  if (cursor_over_chrome_window ())
    return G_SOURCE_CONTINUE;

  // Pointer back on the preview (it may have travelled over a chrome
  // window, which swallows the overview's motion events).
  if (m_hover_index >= 0 && m_hover_index < (int)m_slots.size () &&
      m_hwnd != nullptr) {
    RECT r = slot_display_rect (m_hover_index);
    POINT pt;
    if (GetCursorPos (&pt) && ScreenToClient (m_hwnd, &pt) &&
        pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom) {
      m_hover_idle_source = 0;
      return G_SOURCE_REMOVE;
    }
  }

  m_hover_idle_source = 0;
  int index = m_hover_index;
  m_last_hovered_slot = index;
  m_hover_index = -1;
  overlay_fade_start (0.0);
  hover_scale_start (index, 1.0);
  return G_SOURCE_REMOVE;
}

void
schedule_hover_idle_hide (void)
{
  if (m_hover_idle_source != 0)
    return;
  m_hover_idle_source =
      g_timeout_add (kOverlayIdleHideMs, hover_idle_hide_static_cb, this);
}

void
cancel_hover_idle (void)
{
  if (m_hover_idle_source != 0) {
    g_source_remove (m_hover_idle_source);
    m_hover_idle_source = 0;
  }
}

void
set_hover (int index, bool immediate = false)
{
  if (immediate) {
    cancel_hover_idle ();
    m_scale_anims.clear ();
    if (m_scale_anim_source != 0 && m_window != nullptr) {
      gtk_widget_remove_tick_callback (m_window, m_scale_anim_source);
      m_scale_anim_source = 0;
    }
    // Snap any scaled preview back to its slot rect (skipping any actively
    // dragged or snapping-back slot).
    for (int i = 0; i < (int)m_slots.size (); ++i) {
      SlotEntry &s = m_slots[i];
      if (i == m_drag_slot || i == m_drag_snapback_slot) {
        s.current_scale = 1.0;
        continue;
      }
      if (s.current_scale != 1.0) {
        BYTE op = (m_search_mode || m_search_fade_cur > 0.005)
                      ? 0
                      : (m_app_grid_mode ? s.grid_opacity : 255);
        apply_thumb_rect (s, s.rect, op);
        s.current_scale = 1.0;
      }
    }
    m_last_hovered_slot = -1;
    m_hover_index = -1;
    m_overlay_opacity = 0.0;
    m_overlay_from = 0.0;
    m_overlay_to = 0.0;
    if (g_chrome_owner_view == this) {
      overlay_hide_all ();
      g_chrome_owner_view = nullptr;
    }
    return;
  }

  if (m_drag_slot >= 0 || m_drag_active || m_drag_snapback_source != 0 ||
      m_app_grid_mode || m_search_mode || m_search_fade_cur > 0.005 ||
      m_anim.source != 0 || m_switch_source != 0 ||
      m_search_fade_source != 0 || m_target_state == ControlsState::HIDDEN)
    return;
  cancel_settle_hover ();

  if (index == m_hover_index) {
    if (index >= 0) {
      cancel_hover_idle ();
      bool missing_chrome =
          (g_chrome_window != nullptr &&
           !gtk_widget_get_visible (g_chrome_window)) ||
          (g_icon_window != nullptr &&
           !gtk_widget_get_visible (g_icon_window)) ||
          (g_caption_window != nullptr &&
           !gtk_widget_get_visible (g_caption_window));
      if (missing_chrome || m_overlay_to < 1.0) {
        overlay_fade_start (1.0);
        hover_layout_chrome ();
      }
    }
    return;
  }

  // windowPreview.js: overlayEnabled = active && stateAdjustment === 1 —
  // the hover chrome is disabled until open/transition/workspace-switch
  // animations settle on WINDOW_PICKER (and never active during search).
  if (m_anim.source != 0 || m_switch_source != 0 ||
      m_app_grid_mode || m_search_mode || m_search_fade_cur > 0.005 ||
      m_target_state != ControlsState::WINDOW_PICKER)
    return;

  int previous = m_hover_index;
  cancel_hover_idle ();

  if (index < 0 || index >= (int)m_slots.size ()) {
    // GNOME keeps the overlay for 750ms after the pointer left the preview.
    if (previous >= 0)
      schedule_hover_idle_hide ();
    return;
  }

  const SlotEntry &slot = m_slots[index];
  if (!IsWindow (slot.target) ||
      slot.desktop_index != std::max (m_desktop_active, 0)) {
    if (previous >= 0)
      schedule_hover_idle_hide ();
    return;
  }

  if (g_chrome_owner_view != nullptr && g_chrome_owner_view != this)
    g_chrome_owner_view->set_hover (-1, true);
  g_chrome_owner_view = this;
  m_hover_index = index;
  m_last_hovered_slot = index;

  // Caption pill below the preview.
  std::string cap_title = hwnd_title_utf8 (slot.target);
  if (cap_title.empty ())
    cap_title = hwnd_process_name_utf8 (slot.target);
  gtk_label_set_text (GTK_LABEL (g_caption), cap_title.c_str ());
  overlay_fade_start (1.0);

  // Scale up the preview (WINDOW_SCALE_TIME EASE_OUT_QUAD); the previous
  // one shrinks back at the same time.
  int scale = gtk_widget_get_scale_factor (m_window);
  hover_scale_start (index, slot_hover_target_scale (slot.rect, scale));
  if (previous >= 0 && previous != index)
    hover_scale_start (previous, 1.0);

  hover_layout_chrome ();
}

void
on_root_motion (GtkEventControllerMotion *motion, double x, double y,
                gpointer user_data)
{
  (void)motion;
  (void)user_data;
  if (m_window == nullptr)
    return;
  if (m_drag_slot >= 0 || m_drag_active || m_drag_snapback_source != 0 ||
      m_app_grid_mode || m_search_mode || m_search_fade_cur > 0.005 ||
      m_anim.source != 0 || m_switch_source != 0 ||
      m_search_fade_source != 0 || m_target_state == ControlsState::HIDDEN)
    return;
  cancel_settle_hover ();
  if (m_app_grid_mode || m_search_mode || m_search_fade_cur > 0.005) {
    set_hover (-1, true);
    return;
  }

  int scale = gtk_widget_get_scale_factor (m_window);
  int px = (int)(x * scale);
  int py = (int)(y * scale);
  const int active_ws = std::max (m_desktop_active, 0);

  for (int i = 0; i < (int)m_slots.size (); ++i) {
    if (m_slots[i].desktop_index != active_ws)
      continue;
    const RECT r = slot_display_rect (i);
    if (px >= r.left && px < r.right && py >= r.top && py < r.bottom) {
      set_hover (i);
      return;
    }
  }
  set_hover (-1);
}

void
on_root_motion_leave (GtkEventControllerMotion *motion, gpointer user_data)
{
  (void)motion;
  (void)user_data;
  if (m_drag_slot >= 0 || m_drag_active || m_drag_snapback_source != 0 ||
      m_app_grid_mode || m_search_mode || m_search_fade_cur > 0.005 ||
      m_anim.source != 0 || m_switch_source != 0 ||
      m_search_fade_source != 0 || m_target_state == ControlsState::HIDDEN)
    return;
  cancel_settle_hover ();
  // Keep the hover alive while the pointer is over the chrome windows (they
  // took the input; we would otherwise see a spurious leave).
  if (cursor_over_chrome_window ())
    return;
  set_hover (-1);
}


// --- previews -------------------------------------------------------------------

void
destroy_previews (void)
{
  clear_pending_dwm_frame ();
  for (SlotEntry &e : m_slots)
    if (e.thumb != nullptr)
      DwmUnregisterThumbnail (e.thumb);
  m_slots.clear ();
  m_scale_anims.clear ();
  m_hover_index = -1;
  m_last_hovered_slot = -1;
  reset_drag_state ();
}

void
apply_thumb_rect (SlotEntry &e, const RECT &rect, BYTE opacity, bool force = false)
{
  if (e.thumb == nullptr)
    return;
  const int ov_h = std::max (m_monitor_h - m_panel_height, 1);
  const bool on_screen =
      (m_monitor_w <= 0 ||
       (rect.right > 0 && rect.left < m_monitor_w && rect.bottom > 0 &&
        rect.top < ov_h));
  const bool now_visible = (on_screen && opacity > 0);
  if (!force && e.thumb_applied) {
    if (!now_visible && !e.last_visible) {
      // Skip redundant synchronous DWM ALPC calls while a slot remains
      // completely off-screen (e.g. inactive workspaces) or opacity 0.
      e.current = rect;
      e.current_opacity = opacity;
      return;
    }
    if (EqualRect (&e.current, &rect) && e.current_opacity == opacity &&
        e.last_visible == now_visible) {
      return;
    }
  }
  DWM_THUMBNAIL_PROPERTIES props = {};
  props.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_RECTDESTINATION |
                  DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
  props.fVisible = now_visible ? TRUE : FALSE;
  props.opacity = opacity;
  props.rcDestination = rect;
  props.fSourceClientAreaOnly = FALSE;
  DwmUpdateThumbnailProperties (e.thumb, &props);
  e.current = rect;
  e.current_opacity = opacity;
  e.last_visible = now_visible;
  e.thumb_applied = true;
}

void
promote_slot_thumb_to_top (SlotEntry &e)
{
  if (m_hwnd == nullptr || e.target == nullptr || e.thumb == nullptr)
    return;
  DwmUnregisterThumbnail (e.thumb);
  e.thumb = nullptr;
  e.thumb_applied = false;
  HTHUMBNAIL thumb = nullptr;
  if (SUCCEEDED (DwmRegisterThumbnail (m_hwnd, e.target, &thumb)) &&
      thumb != nullptr) {
    e.thumb = thumb;
    apply_thumb_rect (e, e.current, e.current_opacity, true);
  }
}

void
create_previews (bool at_start_rects, LONG x_offset, bool reorder_existing)
{
  if (m_hwnd == nullptr)
    return;

  // DWM composites thumbnails registered on the same destination HWND in
  // registration order (earlier = bottom, later = top). Register in reverse
  // desktop Z-order (backmost window first, frontmost window z_order == 0
  // last) so the active foreground window always renders on top of background
  // windows during the open/close transitions — and always AFTER m_mini_slots
  // so workspace thumbnail previews never float above real workspace windows.
  // Register inactive workspaces' windows first and active workspace windows
  // last (each group back-to-front by desktop z_order) so neighboring
  // workspaces never fly on top of active workspace windows during APP_GRID
  // or close transitions.
  const int ws_count = std::max (static_cast<int> (m_desktops.size ()), 1);
  const int active_ws = std::clamp (m_desktop_active, 0, ws_count - 1);
  std::vector<size_t> order (m_slots.size ());
  for (size_t i = 0; i < m_slots.size (); ++i)
    order[i] = i;
  std::stable_sort (order.begin (), order.end (),
                    [this, active_ws] (size_t a, size_t b) {
                      const bool a_act = (m_slots[a].desktop_index == active_ws);
                      const bool b_act = (m_slots[b].desktop_index == active_ws);
                      if (a_act != b_act)
                        return !a_act && b_act;
                      return m_slots[a].z_order > m_slots[b].z_order;
                    });

  for (size_t idx : order) {
    SlotEntry &e = m_slots[idx];
    const bool was_reused = (e.thumb != nullptr);
    if (reorder_existing && e.thumb != nullptr) {
      DwmUnregisterThumbnail (e.thumb);
      e.thumb = nullptr;
    }
    if (e.thumb == nullptr) {
      HTHUMBNAIL thumb = nullptr;
      if (SUCCEEDED (DwmRegisterThumbnail (m_hwnd, e.target, &thumb)) &&
          thumb != nullptr) {
        e.thumb = thumb;
      }
    }
    if (e.thumb != nullptr) {
      e.thumb_applied = false;
      if (was_reused && x_offset == 0) {
        apply_thumb_rect (e, e.current, e.current_opacity, true);
      } else if (m_search_mode) {
        apply_thumb_rect (e, translate_rect_x (e.rect, x_offset), 0, true);
      } else if (at_start_rects) {
        apply_thumb_rect (e, e.start, e.minimized ? 0 : 255, true);
      } else {
        BYTE op = m_app_grid_mode ? e.grid_opacity : 255;
        if (m_app_grid_mode && m_folder_dialog_open && op > 0)
          op = 76;
        apply_thumb_rect (e, translate_rect_x (e.rect, x_offset), op, true);
      }
    } else {
      e.thumb_applied = false;
    }
  }
}

// --- thumbnail mini-previews ------------------------------------------------------

void
destroy_mini_previews (void)
{
  for (MiniSlot &m : m_mini_slots)
    if (m.thumb != nullptr)
      DwmUnregisterThumbnail (m.thumb);
  m_mini_slots.clear ();
  m_mini_target_opacity = 0;
}

void
build_mini_occluder_boxes (std::vector<RECT> *out)
{
  out->clear ();
  if (!m_ws_cur_boxes.empty ())
    *out = m_ws_cur_boxes;
  else if (m_bg_cur.right > m_bg_cur.left && m_bg_cur.bottom > m_bg_cur.top)
    out->push_back (m_bg_cur);
}

void
flush_pending_dwm_frame (void)
{
  if (!m_dwm_frame_pending)
    return;
  const gint64 dwm_t0 = g_get_monotonic_time ();
  m_dwm_frame_pending = false;
  for (SlotEntry &s : m_slots) {
    if (s.has_pending_dwm) {
      s.has_pending_dwm = false;
      apply_thumb_rect (s, s.pending_rect, s.pending_opacity);
    }
  }
  if (m_dwm_pending_has_mini) {
    m_dwm_pending_has_mini = false;
    set_mini_opacity (m_dwm_pending_mini_opacity);
  }
  const gint64 dwm_dt = g_get_monotonic_time () - dwm_t0;
  if (dwm_dt > m_anim_dwm_flush_max_us)
    m_anim_dwm_flush_max_us = dwm_dt;
}

void
clear_pending_dwm_frame (void)
{
  m_dwm_frame_pending = false;
  m_dwm_pending_has_mini = false;
  for (SlotEntry &s : m_slots)
    s.has_pending_dwm = false;
}

void
on_overview_after_paint (GdkFrameClock *clock G_GNUC_UNUSED,
                         gpointer user_data G_GNUC_UNUSED)
{
  if (!m_gl_backing_sanitized) {
    m_gl_backing_sanitized = true;
    winome::sanitize_gdk_gl_backing_windows ();
  }
  if (m_open_perf_pending && m_open_perf_tick0_exit_us != 0 &&
      m_open_perf_paint0_us == 0) {
    m_open_perf_paint0_us = g_get_monotonic_time ();
  }
  flush_pending_dwm_frame ();
  if (m_uncloak_on_after_paint && m_window != nullptr &&
      gtk_widget_get_visible (m_window) &&
      m_target_state != ControlsState::HIDDEN) {
    complete_open_uncloak ();
  }
}

void
disconnect_after_paint_hook (void)
{
  if (m_overview_clock != nullptr && m_overview_after_paint_id != 0) {
    g_signal_handler_disconnect (m_overview_clock, m_overview_after_paint_id);
  }
  m_overview_clock = nullptr;
  m_overview_after_paint_id = 0;
}

void
ensure_after_paint_hook (GdkFrameClock *clock)
{
  if (clock == nullptr)
    return;
  if (m_overview_clock == clock && m_overview_after_paint_id != 0)
    return;
  disconnect_after_paint_hook ();
  m_overview_clock = clock;
  m_overview_after_paint_id = g_signal_connect (
      clock, "after-paint", G_CALLBACK (on_overview_after_paint_cb), this);
}

void
sync_mini_previews_visibility (BYTE opacity)
{
  if (m_mini_slots.empty ())
    return;

  std::vector<RECT> occluders;
  build_mini_occluder_boxes (&occluders);

  const int dpi_scale =
      m_window != nullptr ? std::max (gtk_widget_get_scale_factor (m_window), 1)
                          : 1;
  const double pivot_x =
      (static_cast<double> (m_thumbs_rect.left) + m_thumbs_rect.right) * 0.5;
  const double pivot_y =
      (static_cast<double> (m_thumbs_rect.top) + m_thumbs_rect.bottom) * 0.5;
  const double cur_scale = std::clamp (m_thumbs_cur_scale, 0.05, 2.0);
  const double cur_ty_px = m_thumbs_cur_ty * dpi_scale;
  const BYTE effective_opacity = opacity;

  for (MiniSlot &m : m_mini_slots) {
    if (m.thumb == nullptr)
      continue;
    if (effective_opacity == 0) {
      if (!m.props_applied || m.last_visible) {
        DWM_THUMBNAIL_PROPERTIES props = {};
        props.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_OPACITY |
                        DWM_TNP_SOURCECLIENTAREAONLY;
        props.fVisible = FALSE;
        props.opacity = 0;
        props.fSourceClientAreaOnly = FALSE;
        DwmUpdateThumbnailProperties (m.thumb, &props);
        m.last_visible = false;
        m.last_opacity = 0;
        m.props_applied = true;
      }
      continue;
    }

    if (!m.has_src_size) {
      SIZE sz{};
      if (SUCCEEDED (DwmQueryThumbnailSourceSize (m.thumb, &sz)) &&
          sz.cx > 0 && sz.cy > 0) {
        m.src_size = sz;
        m.has_src_size = true;
      }
    }

    const RECT transformed = transform_rect_around_pivot (
        m.rect, pivot_x, pivot_y, cur_scale, cur_ty_px);
    MiniThumbClipResult clip =
        compute_mini_thumb_clip (transformed, m.src_size, occluders);
    if (!clip.visible) {
      if (!m.props_applied || m.last_visible) {
        DWM_THUMBNAIL_PROPERTIES props = {};
        props.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_OPACITY |
                        DWM_TNP_SOURCECLIENTAREAONLY;
        props.fVisible = FALSE;
        props.opacity = 0;
        props.fSourceClientAreaOnly = FALSE;
        DwmUpdateThumbnailProperties (m.thumb, &props);
        m.last_visible = false;
        m.last_opacity = 0;
        m.props_applied = true;
      }
      continue;
    }

    const bool uncropped = EqualRect (&clip.dest_rect, &transformed);
    if (uncropped && m.last_has_src && m.has_src_size) {
      clip.src_rect = RECT{0, 0, m.src_size.cx, m.src_size.cy};
    }
    const bool set_src = m.has_src_size && (!uncropped || m.last_has_src);
    if (m.props_applied && m.last_visible &&
        m.last_opacity == effective_opacity && m.last_has_src == set_src &&
        EqualRect (&m.last_dest, &clip.dest_rect) &&
        (!set_src || EqualRect (&m.last_src, &clip.src_rect))) {
      continue;
    }

    DWM_THUMBNAIL_PROPERTIES props = {};
    props.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_RECTDESTINATION |
                    DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
    if (set_src)
      props.dwFlags |= DWM_TNP_RECTSOURCE;
    props.fVisible = TRUE;
    props.opacity = effective_opacity;
    props.rcDestination = clip.dest_rect;
    props.rcSource = clip.src_rect;
    props.fSourceClientAreaOnly = FALSE;
    DwmUpdateThumbnailProperties (m.thumb, &props);

    m.last_visible = true;
    m.last_opacity = effective_opacity;
    m.last_dest = clip.dest_rect;
    m.last_src = clip.src_rect;
    m.last_has_src = set_src;
    m.props_applied = true;
  }
}

void
set_mini_opacity (BYTE opacity)
{
  m_mini_target_opacity = opacity;
  sync_mini_previews_visibility (opacity);
}

// One DWM thumbnail per (desktop, window) pair inside the pills
// (WorkspaceThumbnail clones the windows of each workspace). Windows on
// other desktops are cloaked, but their thumbnails still render (the same
// mechanism the native Task View uses); registration failures just leave
// that pill wallpaper-only. @initial_opacity starts the fade-in during the
// open animation.
void
create_mini_previews (HMONITOR monitor, const RECT &monitor_rect,
                      int thumbs_x, int thumbs_y, BYTE initial_opacity,
                      const std::vector<OvWindowInfo> *pre_all = nullptr,
                      const std::vector<int> *pre_desktops = nullptr)
{
  destroy_mini_previews ();
  if (m_hwnd == nullptr || m_desktops.empty () ||
      overview_thumbs_get_height (m_thumbs) <= 0)
    return;

  std::vector<OvWindowInfo> local_all;
  std::vector<int> local_desktops;
  const std::vector<OvWindowInfo> *all_ptr = pre_all;
  const std::vector<int> *desk_ptr = pre_desktops;
  if (all_ptr == nullptr || desk_ptr == nullptr ||
      all_ptr->size () != desk_ptr->size ()) {
    std::vector<OvWindowInfo> raw_all;
    EnumContext ctx{monitor, monitor_rect, &raw_all, false, true,
                    winome::get_config ().overview_window_blacklist};
    EnumWindows (collect_windows, reinterpret_cast<LPARAM> (&ctx));
    local_all.reserve (raw_all.size ());
    local_desktops.reserve (raw_all.size ());
    for (const OvWindowInfo &w : raw_all) {
      int wd = ws::window_workspace (w.hwnd, monitor);
      if (window_cloaked_flags (w.hwnd) != 0 && wd < 0)
        continue;
      local_all.push_back (w);
      local_desktops.push_back (wd);
    }
    all_ptr = &local_all;
    desk_ptr = &local_desktops;
  }
  const std::vector<OvWindowInfo> &all = *all_ptr;
  const std::vector<int> &window_desktop = *desk_ptr;

  int scale = gtk_widget_get_scale_factor (m_window);
  int panel = m_panel_height;

  for (int d = 0; d < (int)m_desktops.size (); ++d) {
    GdkRectangle pill;
    if (!overview_thumbs_get_rect (m_thumbs, d, &pill))
      continue;

    const double pad = 2.0 * scale;
    // Pill origin in overview client space (physical px), inset slightly so
    // maximized windows never poke sharp corners outside the rounded pill clip.
    double ox = (double)thumbs_x * scale + pill.x * scale + pad;
    double oy = (double)thumbs_y * scale + pill.y * scale + pad;
    double pill_w = std::max ((double)pill.width * scale - 2.0 * pad, 1.0);
    double pill_h = std::max ((double)pill.height * scale - 2.0 * pad, 1.0);
    RECT porthole = {0, panel, m_monitor_w, m_monitor_h};
    double pw = std::max ((double)(porthole.right - porthole.left), 1.0);
    double ph = std::max ((double)(porthole.bottom - porthole.top), 1.0);
    double sx = pill_w / pw;
    double sy = pill_h / ph;

    // Register back-to-front so frontmost windows render on top inside pills.
    for (size_t i = all.size (); i-- > 0;) {
      if (window_desktop[i] != d &&
          !(window_desktop[i] < 0 && d == m_desktop_active))
        continue;
      // GNOME WorkspaceThumbnail._isMyWindow: minimized windows are not showing
      // on their workspace and must not render as un-minimized rectangles.
      if (IsIconic (all[i].hwnd))
        continue;

      // Clip the window rect to the porthole (the workarea) before scaling.
      RECT r = all[i].rect;
      RECT c;
      c.left = std::max (r.left, porthole.left);
      c.top = std::max (r.top, porthole.top);
      c.right = std::min (r.right, porthole.right);
      c.bottom = std::min (r.bottom, porthole.bottom);
      if (c.right - c.left <= 0 || c.bottom - c.top <= 0)
        continue;

      MiniSlot m;
      m.thumb = nullptr;
      m.rect.left = (LONG)std::lround (ox + (c.left - porthole.left) * sx);
      m.rect.top = (LONG)std::lround (oy + (c.top - porthole.top) * sy);
      m.rect.right = (LONG)std::lround (ox + (c.right - porthole.left) * sx);
      m.rect.bottom = (LONG)std::lround (oy + (c.bottom - porthole.top) * sy);
      if (m.rect.right - m.rect.left < 1 || m.rect.bottom - m.rect.top < 1)
        continue;

      HTHUMBNAIL thumb = nullptr;
      if (SUCCEEDED (DwmRegisterThumbnail (m_hwnd, all[i].hwnd,
                                           &thumb)) &&
          thumb != nullptr) {
        m.thumb = thumb;
        SIZE src_sz{};
        if (SUCCEEDED (DwmQueryThumbnailSourceSize (thumb, &src_sz)) &&
            src_sz.cx > 0 && src_sz.cy > 0) {
          m.src_size = src_sz;
          m.has_src_size = true;
        }
        m_mini_slots.push_back (m);
      }
    }
  }

  set_mini_opacity (m_search_mode ? 0 : initial_opacity);
}


bool
compute_geometry (HMONITOR monitor, const RECT &monitor_rect, int panel_height,
                  PlacedGeometry *out,
                  const std::unordered_map<HWND, int> *pre_window_desktops = nullptr)
{
  int scale = gtk_widget_get_scale_factor (m_window);
  int mw = monitor_rect.right - monitor_rect.left;
  int mh = monitor_rect.bottom - monitor_rect.top;
  int wa_h = mh - panel_height;
  if (wa_h <= 0 || scale <= 0)
    return false;

  int spacing = (int)(wa_h * kVerticalSpacingRatio + 0.5);
  int thumb_adjust = (int)(spacing * kThumbnailsAdjustTop + 0.5);
  int thumb_adjust_bottom = (int)(spacing * kThumbnailsAdjustBottom + 0.5);

  // Search entry: must be visible (and realized alongside m_window) before
  // measuring; an unmapped/hidden GtkEntry reports preferred height 0 on the
  // very first overview open, pushing ws_box_picker upward across the bar.
  if (m_search != nullptr && !gtk_widget_get_visible (m_search))
    gtk_widget_set_visible (m_search, TRUE);
  if (m_search != nullptr && gtk_widget_get_realized (m_window) &&
      !gtk_widget_get_realized (m_search)) {
    gtk_widget_realize (m_search);
  }
  GtkRequisition search_nat{};
  if (m_search != nullptr)
    gtk_widget_get_preferred_size (m_search, nullptr, &search_nat);
  static int s_last_valid_search_h = 0;
  if (search_nat.height > 0)
    s_last_valid_search_h = search_nat.height;
  int search_w = search_nat.width;
  StEngine *engine = StEngine::get ();
  if (engine != nullptr) {
    StEngine::Node node (*engine, nullptr, "", "search-entry", "");
    double v = 0;
    if (node.lookup_length ("width", &v) && v > 0)
      search_w = (int)(v + 0.5);
  }
  if (search_w <= 0)
    search_w = 352;
  int search_h = search_nat.height > 0
                     ? search_nat.height
                     : (s_last_valid_search_h > 0 ? s_last_valid_search_h : 56);
  m_placed_scale = scale;
  m_placed_search_h = search_h;
  int search_total = search_h * scale;

  // Unified per-monitor role: Search & App Grid always enabled; Dash and
  // multi-workspace controls follow workspace mode and dynamic Shell Primary
  // (Independent & Span -> all monitors; PrimaryOnly -> Shell Primary only).
  const OverviewMonitorRole role = resolve_monitor_overview_role (
      ws::effective_mode (), monitor_manager_is_shell_primary (monitor));

  // Workspace thumbnails (ThumbnailsBox, shown in WINDOW_PICKER for >1
  // desktop or during drag on multi-workspace monitors).
  bool can_manage = role.multi_workspace && ws::can_manage_workspaces ();
  int thumbs_h = 0;  // physical
  m_thumbs_has_pills =
      role.multi_workspace &&
      (m_desktops.size () > 1 || (m_drag_active && !m_desktops.empty ()));
  if (m_thumbs_has_pills) {
    int box_h = (int)(wa_h * kMaxThumbnailScale) / scale;  // logical
    const double panel_ratio =
        (panel_height > 0 && wa_h > 0)
            ? (static_cast<double> (panel_height) / static_cast<double> (wa_h))
            : 0.0;
    overview_thumbs_repopulate (
        m_thumbs, (int)m_desktops.size (), m_desktop_active, box_h,
        (double)mw / scale, (double)mw, (double)wa_h,
        active_wallpaper_thumb_texture (),
        can_manage, m_drag_active, panel_ratio);
    thumbs_h = overview_thumbs_get_height (m_thumbs) * scale;
  } else {
    overview_thumbs_repopulate (m_thumbs, 0, -1, 0, 0, 0, 0, nullptr, false,
                                false);
  }
  m_thumbs_has_pills = thumbs_h > 0;

  m_show_dash = role.show_dash;
  int dash_w = 0;
  int dash_h = 0;
  if (m_show_dash && m_dash != nullptr) {
    int dash_max = (int)(wa_h * kDashMaxHeightRatio + 0.5);
    overview_dash_repopulate (m_dash, monitor, dash_max / scale,
                              m_desktop_active, pre_window_desktops);
    dash_w = overview_dash_get_width (m_dash) * scale;
    dash_h = std::min (overview_dash_get_height (m_dash) * scale, dash_max);
  }

  // Picker container box (ControlsManagerLayout WINDOW_PICKER): the
  // thumbnails row sits between the search entry and the workspace box,
  // which yields to thumbnailsHeight + round(spacing * 0.4) on both edges.
  int avail_bottom = wa_h - dash_h - spacing;
  RECT box;
  box.left = 0;
  box.top = search_total + thumb_adjust + thumbs_h +
            thumb_adjust_bottom;
  box.right = mw;
  box.bottom = avail_bottom;

  double wa_aspect = (double)mw / (double)wa_h;

  // WINDOW_PICKER workspace box.
  int picker_ws_h = std::max ((int)(box.bottom - box.top), 72);
  int picker_ws_w = std::min ((int)(picker_ws_h * wa_aspect + 0.5), mw);
  int picker_ws_x = (mw - picker_ws_w) / 2;
  RECT ws_box_picker = {picker_ws_x, box.top, picker_ws_x + picker_ws_w,
                        box.top + picker_ws_h};

  // APP_GRID 15% workspace bar & AppGrid vertical band.
  // Note: search_total already includes .search-entry's CSS margin-top (24px)
  // and margin-bottom (18px), so grid_ws_top starts directly at search_total
  // in Overview client coordinates (y=0 sits immediately below the panel).
  int grid_ws_top = search_total;
  int max_ws_h = std::max (72, avail_bottom - grid_ws_top - spacing - 320);
  int grid_ws_h =
      std::clamp ((int)(wa_h * kSmallWorkspaceRatio + 0.5), 72, max_ws_h);
  int grid_ws_w = std::min ((int)(grid_ws_h * wa_aspect + 0.5), mw);
  int grid_ws_x = (mw - grid_ws_w) / 2;
  RECT ws_box_grid = {grid_ws_x, grid_ws_top, grid_ws_x + grid_ws_w,
                      grid_ws_top + grid_ws_h};
  RECT app_grid_box = {0, ws_box_grid.bottom + spacing, mw, avail_bottom};

  out->ws_box_picker = ws_box_picker;
  out->ws_box_grid = ws_box_grid;
  out->app_grid_box = app_grid_box;
  out->search_w = search_w;
  out->search_h = search_h;
  out->search_x = (mw / scale - search_w) / 2;
  out->search_y = 0;
  int sv_top = search_total + spacing;
  int sv_w = std::clamp (mw / scale - 48, 400, 1044);
  int sv_h = std::max ((avail_bottom - sv_top) / scale, 220);
  out->search_view_w = sv_w;
  out->search_view_h = sv_h;
  out->search_view_x = (mw / scale - sv_w) / 2;
  out->search_view_y = sv_top / scale;
  out->dash_x = (mw - dash_w) / 2 / scale;
  out->dash_y = (wa_h - dash_h) / scale;
  out->thumbs_h = thumbs_h;
  out->thumbs_x =
      thumbs_h > 0 ? (mw / scale - overview_thumbs_get_width (m_thumbs)) / 2
                   : 0;
  out->thumbs_y = (search_total + thumb_adjust) / scale;

  m_monitor_w = mw;
  m_monitor_h = mh;
  m_dash_top = wa_h - dash_h;

  m_bg_full = RECT{0, 0, mw, wa_h};
  m_bg_picker = ws_box_picker;
  m_bg_grid = ws_box_grid;
  m_bg_final = m_app_grid_mode ? ws_box_grid : ws_box_picker;

  const int ws_count = std::max (static_cast<int> (m_desktops.size ()), 1);
  const int active_ws = std::clamp (m_desktop_active, 0, ws_count - 1);
  m_ws_full_boxes.resize (static_cast<size_t> (ws_count));
  m_ws_picker_boxes.resize (static_cast<size_t> (ws_count));
  m_ws_grid_boxes.resize (static_cast<size_t> (ws_count));
  for (int d = 0; d < ws_count; ++d) {
    m_ws_full_boxes[static_cast<size_t> (d)] =
        full_box_for_desktop (d, active_ws, m_bg_full, ws_box_picker, mw,
                              scale);
    m_ws_picker_boxes[static_cast<size_t> (d)] =
        picker_box_for_desktop (d, static_cast<double> (active_ws),
                                ws_box_picker, mw, scale);
    m_ws_grid_boxes[static_cast<size_t> (d)] =
        grid_box_for_desktop (d, ws_count, ws_box_grid, mw, scale);
  }

  // .workspace-background border-radius (proportionally scaled in APP_GRID).
  double base_radius = kWorkspaceRadiusFallback;
  if (engine != nullptr) {
    StEngine::Node node (*engine, nullptr, "", "workspace-background", "");
    double v = 0;
    if (node.lookup_length ("border-radius", &v) && v > 0)
      base_radius = v;
  }
  out->radius_picker = base_radius;
  double ratio = wa_h > 0
                     ? (double)(ws_box_grid.bottom - ws_box_grid.top) /
                           (double)wa_h
                     : kSmallWorkspaceRatio;
  out->radius_grid = std::clamp (base_radius * ratio, 4.5, 6.0);
  m_radius_picker = out->radius_picker;
  m_radius_grid = out->radius_grid;
  m_bg_radius_target = m_app_grid_mode ? m_radius_grid : m_radius_picker;
  m_ws_box = m_app_grid_mode ? ws_box_grid : ws_box_picker;
  return true;
}

bool
apply_ws_boxes (const std::vector<RECT> &boxes, bool sync_mini = true)
{
  const bool settled = (m_anim.source == 0 && m_switch_source == 0);
  const int active_ws =
      !boxes.empty ()
          ? std::clamp (m_desktop_active, 0, static_cast<int> (boxes.size ()) - 1)
          : 0;
  const RECT active_rect =
      !boxes.empty () ? boxes[static_cast<size_t> (active_ws)] : m_bg_cur;
  bool same = (boxes.size () == m_ws_cur_boxes.size ()) &&
              EqualRect (&m_bg_cur, &active_rect) &&
              m_ws_bg_last_radius == m_bg_radius &&
              m_ws_bg_settled_shadow_drawn == settled;
  if (same) {
    for (size_t i = 0; i < boxes.size (); ++i) {
      if (!EqualRect (&boxes[i], &m_ws_cur_boxes[i])) {
        same = false;
        break;
      }
    }
  }
  if (same)
    return false;
  m_ws_cur_boxes = boxes;
  m_bg_cur = active_rect;
  m_ws_bg_last_radius = m_bg_radius;
  if (sync_mini)
    sync_mini_previews_visibility (m_mini_target_opacity);
  if (m_ws_bg != nullptr) {
    gtk_widget_queue_draw (m_ws_bg);
    return true;
  }
  return false;
}

bool
apply_bg_rect (const RECT &rect, bool sync_mini = true)
{
  const bool settled = (m_anim.source == 0 && m_switch_source == 0);
  if (m_ws_cur_boxes.size () <= 1 && EqualRect (&m_bg_cur, &rect) &&
      m_ws_bg_last_radius == m_bg_radius &&
      m_ws_bg_settled_shadow_drawn == settled) {
    return false;
  }
  m_bg_cur = rect;
  if (m_ws_cur_boxes.size () <= 1) {
    m_ws_cur_boxes.assign (1, rect);
  } else {
    const int active_ws = std::clamp (
        m_desktop_active, 0, static_cast<int> (m_ws_cur_boxes.size ()) - 1);
    m_ws_cur_boxes[static_cast<size_t> (active_ws)] = rect;
  }
  m_ws_bg_last_radius = m_bg_radius;
  if (sync_mini)
    sync_mini_previews_visibility (m_mini_target_opacity);
  if (m_ws_bg != nullptr) {
    gtk_widget_queue_draw (m_ws_bg);
    return true;
  }
  return false;
}

void
cancel_settle_hover (void)
{
  if (m_settle_hover_timer != 0) {
    g_source_remove (m_settle_hover_timer);
    m_settle_hover_timer = 0;
  }
}

gboolean
settle_hover_cb (gpointer user_data G_GNUC_UNUSED)
{
  m_settle_hover_timer = 0;
  if (m_window == nullptr || !gtk_widget_get_visible (m_window) || !m_placed ||
      m_anim.source != 0 || m_switch_source != 0 || m_search_fade_source != 0 ||
      m_app_grid_mode || m_search_mode ||
      m_target_state != ControlsState::WINDOW_PICKER) {
    return G_SOURCE_REMOVE;
  }
  set_hover (slot_at_cursor ());
  recheck_overview_search_focus_on_settle ("settle-hover");
  return G_SOURCE_REMOVE;
}

void
schedule_settle_hover (guint delay_ms)
{
  cancel_settle_hover ();
  m_settle_hover_timer = g_timeout_add_full (
      G_PRIORITY_LOW, delay_ms, settle_hover_static_cb, this, nullptr);
}

void
ensure_app_grid_synced (bool refresh_running = false)
{
  if (m_app_grid == nullptr || m_grid_w <= 0 || m_grid_h <= 0)
    return;
  app_grid_clip_set_bounds (m_app_grid_clip, m_grid_w, m_grid_h);
  gtk_widget_set_size_request (m_app_grid, m_grid_w, m_grid_h);
  app_grid_set_query (m_app_grid, "");
  app_grid_rebuild (m_app_grid, m_grid_w, m_grid_h, refresh_running);
}

void
sync_app_grid_visible (bool visible)
{
  if (m_app_grid_clip != nullptr)
    gtk_widget_set_visible (m_app_grid_clip, visible ? TRUE : FALSE);
  if (m_app_grid != nullptr)
    gtk_widget_set_visible (m_app_grid, visible ? TRUE : FALSE);
}

void
position_widgets (const PlacedGeometry &geo, bool opening, LONG switch_off_x = 0)
{
  gtk_widget_set_size_request (m_search, geo.search_w, geo.search_h);
  gtk_fixed_move (GTK_FIXED (m_root), m_search, geo.search_x, geo.search_y);

  // Keep m_ws_bg fixed to the full workarea allocation and paint/clip m_bg_cur
  // directly inside winome_workspace_bg_snapshot so 250ms animations do zero
  // GtkFixed re-layout or size_request churn.
  int scale = std::max (gtk_widget_get_scale_factor (m_window), 1);
  if (m_ws_bg != nullptr) {
    const int full_w = std::max (
        static_cast<int> (m_bg_full.right - m_bg_full.left) / scale, 1);
    const int full_h = std::max (
        static_cast<int> (m_bg_full.bottom - m_bg_full.top) / scale, 1);
    gtk_widget_set_size_request (m_ws_bg, full_w, full_h);
    gtk_fixed_move (GTK_FIXED (m_root), m_ws_bg,
                    static_cast<int> (m_bg_full.left) / scale,
                    static_cast<int> (m_bg_full.top) / scale);
  }

  // The background starts at the full work area (radius 0) when opening —
  // the open animation shrinks it into the target box — and sits at the
  // settled (or slide-in start) box on relayout.
  if (opening) {
    m_bg_radius = 0.0;
    apply_ws_boxes (m_ws_full_boxes);
  } else {
    const std::vector<RECT> &target_boxes =
        m_app_grid_mode ? m_ws_grid_boxes : m_ws_picker_boxes;
    m_bg_radius = m_app_grid_mode ? m_radius_grid : m_radius_picker;
    if (switch_off_x == 0) {
      apply_ws_boxes (target_boxes);
    } else {
      std::vector<RECT> shifted = target_boxes;
      for (RECT &r : shifted)
        r = translate_rect_x (r, switch_off_x);
      apply_ws_boxes (shifted);
    }
  }

  if (m_dash != nullptr && m_show_dash)
    gtk_fixed_move (GTK_FIXED (m_root), m_dash, geo.dash_x, geo.dash_y);

  // Thumbnails row (invisible for a single desktop, settled APP_GRID, or search).
  gtk_fixed_move (GTK_FIXED (m_root), m_thumbs, geo.thumbs_x, geo.thumbs_y);
  bool show_thumbs = !m_search_mode &&
                     (opening ? (geo.thumbs_h > 0)
                              : (!m_app_grid_mode && geo.thumbs_h > 0));
  gtk_widget_set_visible (m_thumbs, show_thumbs);

  if (m_search_view != nullptr) {
    gtk_widget_set_size_request (m_search_view, geo.search_view_w,
                                 geo.search_view_h);
    gtk_fixed_move (GTK_FIXED (m_root), m_search_view,
                    geo.search_view_x, geo.search_view_y);
    gtk_widget_set_visible (m_search_view,
                            (m_search_mode || m_search_fade_cur > 0.005)
                                ? TRUE
                                : FALSE);
  }

  m_search_rect.left = geo.search_x * scale;
  m_search_rect.top = geo.search_y * scale;
  m_search_rect.right = (geo.search_x + geo.search_w) * scale;
  m_search_rect.bottom = (geo.search_y + geo.search_h) * scale;

  if (geo.thumbs_h > 0) {
    int thumbs_w = overview_thumbs_get_width (m_thumbs);
    m_thumbs_rect.left = geo.thumbs_x * scale;
    m_thumbs_rect.top = geo.thumbs_y * scale;
    m_thumbs_rect.right = (geo.thumbs_x + thumbs_w) * scale;
    m_thumbs_rect.bottom = (geo.thumbs_y * scale) + geo.thumbs_h;
  } else {
    m_thumbs_rect = {0, 0, 0, 0};
  }

  gtk_widget_set_visible (m_search, TRUE);
  gtk_widget_set_visible (m_ws_bg, m_search_mode ? FALSE : TRUE);
  if (m_dash != nullptr)
    gtk_widget_set_visible (m_dash, m_show_dash ? TRUE : FALSE);
  if (!opening)
    sync_app_grid_visible (m_app_grid_mode && !m_search_mode);
}


void
apply_ex_styles (HWND hwnd)
{
  // Allow m_hwnd to activate/take keyboard focus so overview-wide
  // shortcuts (Ctrl+F, Ctrl+Shift+N, PageUp/Down, Home/End, type-to-search,
  // Esc) reach on_overview_key_pressed while staying a topmost tool window
  // excluded from Alt+Tab and the taskbar.
  LONG_PTR ex = GetWindowLongPtrW (hwnd, GWL_EXSTYLE);
  LONG_PTR want = (ex | WS_EX_TOOLWINDOW) & ~(WS_EX_NOACTIVATE | WS_EX_APPWINDOW);
  UINT swp_flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
  if (want != ex) {
    SetWindowLongPtrW (hwnd, GWL_EXSTYLE, want);
    swp_flags |= SWP_FRAMECHANGED;
  }
  LONG_PTR st = GetWindowLongPtrW (hwnd, GWL_STYLE);
  LONG_PTR want_st =
      st & ~static_cast<LONG_PTR> (WS_CAPTION | WS_THICKFRAME | WS_BORDER |
                                   WS_MAXIMIZEBOX | WS_MINIMIZEBOX);
  if (want_st != st) {
    SetWindowLongPtrW (hwnd, GWL_STYLE, want_st);
    swp_flags |= SWP_FRAMECHANGED;
  }
  DWORD corner_pref = 1;  // DWMWCP_DONOTROUND
  DwmSetWindowAttribute (hwnd, 33, &corner_pref, sizeof (corner_pref));
  COLORREF border_none = 0xFFFFFFFEu;  // DWMWA_COLOR_NONE
  DwmSetWindowAttribute (hwnd, 34, &border_none, sizeof (border_none));
  ensure_hwnd_topmost (hwnd);
  harden_shell_overlay_hwnd (hwnd);
  ensure_overview_subclass (hwnd, this);

  HWND panel = panel_hwnd_for_view ();
  HWND after =
      (panel != nullptr && IsWindow (panel) && IsWindowVisible (panel) &&
       (GetWindowLongPtrW (panel, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0)
          ? panel
          : HWND_TOPMOST;
  HWND chrome_layers[] = {active_folder_hwnd (), g_chrome_hwnd, g_icon_hwnd,
                          g_caption_hwnd};
  for (HWND h : chrome_layers) {
    if (is_visible_hwnd (h) &&
        (GetWindowLongPtrW (h, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
      after = h;
    }
  }
  HWND prev = GetWindow (hwnd, GW_HWNDPREV);
  const bool need_z =
      (after == HWND_TOPMOST) ? (prev != nullptr) : (prev != after);
  if ((swp_flags & SWP_FRAMECHANGED) != 0 || need_z)
    SetWindowPos (hwnd, after, 0, 0, 0, 0, swp_flags);
}

// Rebuild slots, thumbnails and widget positions for the current monitor.
// Must run after the overview window is mapped (GDK's first-map sequence
// would overwrite earlier placement).
void
place_overview_locked (bool opening, int switch_dir = 0,
                       const SharedOpenSnapshot *shared_snap = nullptr,
                       bool defer_reveal = false)
{
  if (m_window == nullptr || !gtk_widget_get_visible (m_window))
    return;

  // A plain relayout (window closed etc.) ends any in-flight workspace
  // switch instantly: the rebuilt slots are the new truth.
  if (m_switch_source != 0)
    switch_finish (false);

  HMONITOR monitor = resolve_monitor ();
  m_monitor = monitor;
  MONITORINFO mi {};
  mi.cbSize = sizeof (mi);
  if (!GetMonitorInfoW (monitor, &mi))
    return;

  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (m_window));
  if (surface == nullptr)
    return;
  m_hwnd =
      static_cast<HWND> (gdk_win32_surface_get_handle (surface));
  apply_ex_styles (m_hwnd);

  // Bind owner for any already-realized chrome windows; unrealized chrome
  // windows are realized lazily on first hover (or staggered low-priority
  // startup prewarm) so opening Overview right after startup never blocks on
  // 3 extra OpenGL window realizations.
  HWND chrome_hwnds[] = {g_chrome_hwnd, g_icon_hwnd, g_caption_hwnd};
  for (HWND ch : chrome_hwnds)
    chrome_bind_owner (ch);

  HWND panel_hwnd = panel_hwnd_for_view ();
  HWND place_after = (panel_hwnd != nullptr && IsWindow (panel_hwnd) &&
                      IsWindowVisible (panel_hwnd))
                         ? panel_hwnd
                         : HWND_TOPMOST;
  int w = mi.rcMonitor.right - mi.rcMonitor.left;
  int h = mi.rcMonitor.bottom - mi.rcMonitor.top;
  int panel_height = resolve_panel_inset ();
  RECT ov_rc = compute_overview_hwnd_rect (mi.rcMonitor, panel_height);
  m_want_hwnd_rect = ov_rc;
  int ov_h = ov_rc.bottom - ov_rc.top;
  SetWindowPos (m_hwnd, place_after, ov_rc.left, ov_rc.top, w, ov_h,
                SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);

  m_panel_height = panel_height;
  m_monitor_w = w;
  m_monitor_h = h;

  load_wallpaper ();

  // Hybrid third-party live wallpaper engine support (single WGC session,
  // bound to the focused/first view whose monitor runs a live wallpaper engine).
  const std::string live_mode =
      sanitize_live_wallpaper_mode (winome::get_config ().live_wallpaper_mode);
  if (live_mode != "off" && (g_live_wallpaper_owner == nullptr || g_live_wallpaper_owner == this)) {
    LiveWallpaperTarget target = live_wallpaper_detect_target (mi.rcMonitor);
    if (target.active) {
      g_using_live_wallpaper = true;
      g_live_wallpaper_owner = this;
      live_wallpaper_set_paused (opening || m_anim.source != 0 ||
                                 m_switch_source != 0);
      live_wallpaper_start (target, mi.rcMonitor, live_mode == "live",
                            on_live_wallpaper_frame, nullptr);
    } else if (g_live_wallpaper_owner == this) {
      g_using_live_wallpaper = false;
      g_live_wallpaper_owner = nullptr;
      live_wallpaper_stop ();
      g_clear_object (&g_live_wallpaper);
      g_clear_object (&g_live_wallpaper_thumb);
    }
  } else if (live_mode == "off" && g_live_wallpaper_owner == this) {
    g_using_live_wallpaper = false;
    g_live_wallpaper_owner = nullptr;
    live_wallpaper_stop ();
    g_clear_object (&g_live_wallpaper);
    g_clear_object (&g_live_wallpaper_thumb);
  }

  // Synchronize dynamic workspaces (GNOME WorkspaceTracker._checkWorkspaces)
  // and enumerate all desktops' eligible windows in a single batched pass.
  std::vector<OvWindowInfo> all_windows;
  std::vector<int> window_desktop;
  std::vector<vd::DesktopInfo> synced_desktops;
  int synced_active = -1;
  std::unordered_map<HWND, int> local_hwnd_map;
  sync_dynamic_workspaces (monitor, mi.rcMonitor, &all_windows,
                           &window_desktop, &synced_desktops, &synced_active,
                           &local_hwnd_map, shared_snap);

  m_desktops = std::move (synced_desktops);
  m_desktop_active =
      m_desktops.empty ()
          ? -1
          : (synced_active >= 0 ? synced_active
                                : ws::active_for_monitor (monitor));
  if (m_desktop_active < 0)
    m_desktops.clear ();
  m_pending_desktop_sync = false;
  m_pending_desktop_target = -1;
  m_last_internal_switch_us = 0;
  m_last_internal_switch_from = -1;
  m_last_internal_switch_to = -1;
  if (m_drag_active || m_drag_slot >= 0 || m_drag_thumbs_revealed)
    reset_drag_state ();
  g_print ("[ov] desktops=%zu active=%d\n", m_desktops.size (),
           m_desktop_active);
  winome_shell_panel_set_workspace_scroll_for_monitor (
      panel_scroll_target_monitor (),
      std::max (static_cast<int> (m_desktops.size ()), 1),
      static_cast<double> (std::max (m_desktop_active, 0)));

  const std::vector<RECT> prev_ws_cur_boxes = m_ws_cur_boxes;

  // Geometry (ControlsManagerLayout + WorkspacesView workspace box).
  PlacedGeometry geo;
  const std::unordered_map<HWND, int> *dash_map =
      !local_hwnd_map.empty () ? &local_hwnd_map : nullptr;
  if (!compute_geometry (monitor, mi.rcMonitor, panel_height, &geo, dash_map))
    return;
  if (opening) {
    m_target_state = ControlsState::HIDDEN;
    m_app_grid_mode = false;
  }
  const RECT &active_bg = m_app_grid_mode ? m_bg_grid : m_bg_picker;
  const LONG switch_off_x =
      (!opening && switch_dir != 0)
          ? static_cast<LONG> (switch_dir * (active_bg.right - active_bg.left))
          : 0;
  position_widgets (geo, opening, switch_off_x);

  // The dark backdrop applies only once the workspace background actually
  // covers the work area: before that, a bare #overviewGroup would flash
  // full-screen dark in the pre-placement frames.
  gtk_widget_set_name (m_root, "overviewGroup");

  int scale = gtk_widget_get_scale_factor (m_window);
  m_grid_w = std::max (std::min (w / scale - 32, 1820), 400);
  m_grid_h = std::max (
      (int)(geo.app_grid_box.bottom - geo.app_grid_box.top) / scale, 260);
  m_grid_x = (w / scale - m_grid_w) / 2;
  m_grid_y = (int)geo.app_grid_box.top / scale;
  if (m_app_grid != nullptr) {
    app_grid_set_host_bounds (m_app_grid, mi.rcMonitor.left, mi.rcMonitor.top,
                              w, h, panel_height, panel_height + m_dash_top);
    if (m_app_grid_clip != nullptr) {
      gtk_fixed_move (GTK_FIXED (m_root), m_app_grid_clip, m_grid_x, m_grid_y);
      app_grid_clip_set_bounds (m_app_grid_clip, m_grid_w, m_grid_h);
    } else {
      gtk_fixed_move (GTK_FIXED (m_root), m_app_grid, m_grid_x, m_grid_y);
    }
    const bool need_grid_now =
        m_app_grid_mode ||
        m_pending_show_state == ControlsState::APP_GRID ||
        !m_search_query.empty ();
    if (need_grid_now)
      ensure_app_grid_synced ();
  }

  RECT workarea = {0, panel_height, w, h};
  double chrome_oversize =
      std::max ({close_button_size () / 2.0,
                 kWindowIconSize * (1.0 - kIconOverlap)}) +
      kWindowActiveSizeInc;
  double spacing = (window_picker_spacing () + chrome_oversize) * scale;

  // Preserve existing live DWM thumbnail handles across in-place relayouts
  // (e.g. after dropping a window onto another workspace or closing a window)
  // so remaining windows animate smoothly without a DWM re-register flash.
  struct SavedThumb {
    HWND target = nullptr;
    HTHUMBNAIL thumb = nullptr;
    RECT current{};
    BYTE opacity = 255;
  };
  std::vector<SavedThumb> saved_thumbs;
  if (!opening && switch_dir == 0) {
    saved_thumbs.reserve (m_slots.size ());
    for (SlotEntry &old_s : m_slots) {
      if (old_s.thumb != nullptr && old_s.target != nullptr) {
        saved_thumbs.push_back (SavedThumb{old_s.target, old_s.thumb,
                                           old_s.current,
                                           old_s.current_opacity});
        old_s.thumb = nullptr;
      }
    }
  }

  set_hover (-1, true);
  destroy_previews ();
  if (m_anim.source != 0) {
    gtk_widget_remove_tick_callback (m_window, m_anim.source);
    m_anim.source = 0;
  }

  const int ws_count = std::max (static_cast<int> (m_desktops.size ()), 1);
  const int active_ws = std::clamp (m_desktop_active, 0, ws_count - 1);
  bool need_relayout_anim = false;

  for (int d = 0; d < ws_count; ++d) {
    std::vector<OvWindowInfo> desk_windows;
    for (size_t i = 0; i < all_windows.size (); ++i) {
      int wd = (i < window_desktop.size ()) ? window_desktop[i] : -1;
      if (m_desktops.empty () || wd == d || (wd < 0 && d == active_ws))
        desk_windows.push_back (all_windows[i]);
    }
    if (desk_windows.empty ())
      continue;

    const RECT &ws_picker_d = m_ws_picker_boxes[static_cast<size_t> (d)];
    const RECT &ws_grid_d = m_ws_grid_boxes[static_cast<size_t> (d)];
    const RECT &ws_full_d = m_ws_full_boxes[static_cast<size_t> (d)];

    // Inset the picker layout box inside geo.ws_box_picker (matching upstream
    // WorkspaceLayout._adjustSpacingAndPadding) so window previews never touch
    // the 30px-rounded outer border of .workspace-background.
    RECT picker_layout_box = geo.ws_box_picker;
    const int pad_x = 20 * scale;
    const int pad_top = 18 * scale;
    const int pad_bot = 26 * scale;
    if (picker_layout_box.right - picker_layout_box.left > 2 * pad_x + 64 &&
        picker_layout_box.bottom - picker_layout_box.top >
            pad_top + pad_bot + 64) {
      picker_layout_box.left += pad_x;
      picker_layout_box.right -= pad_x;
      picker_layout_box.top += pad_top;
      picker_layout_box.bottom -= pad_bot;
    }

    std::vector<OvSlot> picker_slots;
    overview_compute_slots (desk_windows, mi.rcMonitor, workarea,
                            picker_layout_box, spacing, spacing, &picker_slots);

    RECT inset_grid = ws_grid_d;
    const int gpad = 2 * scale;
    if (inset_grid.right - inset_grid.left > 2 * gpad + 8 &&
        inset_grid.bottom - inset_grid.top > 2 * gpad + 8) {
      inset_grid.left += gpad;
      inset_grid.top += gpad;
      inset_grid.right -= gpad;
      inset_grid.bottom -= gpad;
    }

    for (const OvSlot &s : picker_slots) {
      SlotEntry e{};
      e.target = s.window.hwnd;
      e.thumb = nullptr;
      e.desktop_index = d;
      RECT client_start = {s.window.rect.left, s.window.rect.top - panel_height,
                           s.window.rect.right,
                           s.window.rect.bottom - panel_height};
      e.canonical_start = client_start;
      e.start = (d == active_ws)
                    ? client_start
                    : remap_rect_between_boxes (client_start, m_bg_full,
                                                ws_full_d);
      e.z_order = static_cast<int> (all_windows.size ());
      for (size_t wi = 0; wi < all_windows.size (); ++wi) {
        if (all_windows[wi].hwnd == e.target) {
          e.z_order = static_cast<int> (wi);
          break;
        }
      }
      e.minimized_any = (IsIconic (s.window.hwnd) != FALSE);
      e.minimized = (d == active_ws) && e.minimized_any;
      e.grid_opacity = e.minimized_any ? 0 : 255;
      int sw = (int)((s.window.rect.right - s.window.rect.left) * s.scale);
      int sh = (int)((s.window.rect.bottom - s.window.rect.top) * s.scale);
      e.canonical_picker_rect.left = (int)s.x;
      e.canonical_picker_rect.top = (int)s.y;
      e.canonical_picker_rect.right = e.canonical_picker_rect.left + sw;
      e.canonical_picker_rect.bottom = e.canonical_picker_rect.top + sh;
      if (e.canonical_picker_rect.right <= e.canonical_picker_rect.left ||
          e.canonical_picker_rect.bottom <= e.canonical_picker_rect.top)
        continue;

      e.picker_rect =
          (d == active_ws)
              ? e.canonical_picker_rect
              : remap_rect_between_boxes (e.canonical_picker_rect,
                                          geo.ws_box_picker, ws_picker_d);

      // Upstream workspacesView.js _getWorkspaceModeForOverviewState:
      // ControlsState.APP_GRID uses workspaceMode = 0 (true desktop window
      // layout scaled into each miniature workspace card, with minimized
      // windows hidden).
      RECT clipped_win;
      clipped_win.left =
          std::clamp (s.window.rect.left, workarea.left, workarea.right);
      clipped_win.top =
          std::clamp (s.window.rect.top, workarea.top, workarea.bottom);
      clipped_win.right =
          std::clamp (s.window.rect.right, clipped_win.left, workarea.right);
      clipped_win.bottom =
          std::clamp (s.window.rect.bottom, clipped_win.top, workarea.bottom);
      if (clipped_win.right > clipped_win.left &&
          clipped_win.bottom > clipped_win.top) {
        e.grid_rect =
            remap_rect_between_boxes (clipped_win, workarea, inset_grid);
      } else {
        e.grid_rect = remap_rect_between_boxes (e.canonical_picker_rect,
                                                geo.ws_box_picker, inset_grid);
        e.grid_opacity = 0;
      }

      if (e.minimized) {
        e.start.left = ws_picker_d.left;
        e.start.top = ws_picker_d.top;
        e.start.right = e.start.left + 1;
        e.start.bottom = e.start.top + 1;
      }
      e.rect = m_app_grid_mode ? e.grid_rect : e.picker_rect;
      e.anim_from = opening ? e.start : e.rect;
      e.anim_to = e.rect;
      e.current = opening ? e.start : e.rect;
      BYTE settled_op = m_app_grid_mode ? e.grid_opacity : 255;
      if (m_app_grid_mode && m_folder_dialog_open && settled_op > 0)
        settled_op = 76;
      e.current_opacity = opening ? (e.minimized ? 0 : 255)
                                  : (m_search_mode ? 0 : settled_op);

      for (SavedThumb &st : saved_thumbs) {
        if (st.thumb != nullptr && st.target == e.target) {
          e.thumb = st.thumb;
          st.thumb = nullptr;
          e.anim_from = st.current;
          e.current = st.current;
          e.anim_from_opacity = st.opacity;
          e.current_opacity = st.opacity;
          if (!EqualRect (&st.current, &e.rect))
            need_relayout_anim = true;
          break;
        }
      }

      m_slots.push_back (e);
    }
  }

  for (SavedThumb &st : saved_thumbs) {
    if (st.thumb != nullptr)
      DwmUnregisterThumbnail (st.thumb);
  }

  // When opening cloaked, defer uncloak + focus + restack until AFTER all DWM
  // thumbnails are registered at exact t=0 desktop rects so DWM never
  // composites a bare wallpaper frame above real desktop windows.
  if (!opening || !m_cloaked_for_open) {
    if (opening && m_want_focus_on_open && !defer_reveal &&
        winome_shell_panel_top_popover_hwnd_for_monitor (monitor) == nullptr)
      focus_overview_window (false);
    if (!defer_reveal)
      restack_locked ();
  }

  m_ws_hist1_boxes.clear ();
  m_ws_hist2_boxes.clear ();

  const bool opening_to_grid =
      opening && m_pending_show_state == ControlsState::APP_GRID;
  m_thumbs_cur_scale = (opening_to_grid || (!opening && m_app_grid_mode)) ? 0.5 : 1.0;
  m_thumbs_cur_ty = (opening_to_grid || (!opening && m_app_grid_mode))
                        ? (overview_thumbs_get_height (m_thumbs) * 0.5)
                        : 0.0;
  if (m_thumbs != nullptr)
    overview_thumbs_set_transform (m_thumbs, m_thumbs_cur_scale, m_thumbs_cur_ty);

  // Register mini window previews inside the workspace thumbnail pills FIRST
  // so they always sit underneath real workspace window clones in DWM's
  // composition order. When opening to WINDOW_PICKER, mini previews start at
  // opacity 255 (clipped behind m_ws_full_boxes at t=0) and unveil smoothly
  // with zero flash as the active workspace shrinks — matching GNOME Shell's
  // overviewControls.js _getThumbnailsBoxParams().
  const bool had_reused_slots = std::any_of (
      m_slots.begin (), m_slots.end (),
      [] (const SlotEntry &e) { return e.thumb != nullptr; });
  create_mini_previews (monitor, mi.rcMonitor, geo.thumbs_x, geo.thumbs_y,
                        (opening_to_grid || (!opening && m_app_grid_mode))
                            ? 0
                            : 255,
                        &all_windows, &window_desktop);

  // Register workspace window clones AFTER mini previews (in back-to-front
  // desktop Z-order): start at real desktop rects when opening, slide-in
  // start offset when switching desktops, or current/settled rects otherwise.
  create_previews (opening, switch_off_x,
                   had_reused_slots && !m_mini_slots.empty ());

  m_placed = true;
  g_print ("[ov] placed %dx%d slots=%zu ws=%ldx%ld grid=%dx%d@(%d,%d) dash=%d\n",
           w, h, m_slots.size (), (long)(m_ws_box.right - m_ws_box.left),
           (long)(m_ws_box.bottom - m_ws_box.top), m_grid_w, m_grid_h, m_grid_x,
           m_grid_y, overview_dash_get_width (m_dash));

  if (opening) {
    ControlsState desired = m_pending_show_state;
    m_bg_cur = m_bg_full;
    m_ws_cur_boxes = m_ws_full_boxes;
    m_bg_radius = 0.0;
    const bool start_thumbs_ready =
        m_thumbs_has_pills && desired != ControlsState::APP_GRID;
    m_thumbs_cur_opacity = start_thumbs_ready ? 1.0 : 0.0;
    m_thumbs_cur_scale = start_thumbs_ready ? 1.0 : 0.5;
    m_thumbs_cur_ty = start_thumbs_ready
                          ? 0.0
                          : (overview_thumbs_get_height (m_thumbs) * 0.5);
    m_grid_cur_opacity = 0.0;
    m_grid_cur_slide_y = kGridSlideOffsetPx;
    if (m_thumbs != nullptr) {
      overview_thumbs_set_transform (m_thumbs, m_thumbs_cur_scale,
                                     m_thumbs_cur_ty);
      gtk_widget_set_opacity (m_thumbs, m_thumbs_cur_opacity);
      gtk_widget_set_visible (m_thumbs, FALSE);
    }
    if (m_app_grid != nullptr) {
      app_grid_set_transition_opacity (m_app_grid, true, 1.0);
      app_grid_clip_set_transition (m_app_grid_clip,
                                    desired == ControlsState::APP_GRID, 0.0,
                                    kGridSlideOffsetPx, true);
      sync_app_grid_visible (desired == ControlsState::APP_GRID);
    }
    if (defer_reveal) {
      m_pending_reveal = true;
    } else {
      m_pending_reveal = false;
      start_state_transition (desired, kOpenMs);
      if (m_cloaked_for_open) {
        arm_uncloak_after_paint ();
      } else {
        ensure_overview_visible_on_active_desktop (m_desktop_active);
        if (m_want_focus_on_open &&
            winome_shell_panel_top_popover_hwnd_for_monitor (monitor) == nullptr) {
          focus_overview_window (false);
        }
        restack_locked ();
        if (m_want_focus_on_open && can_auto_focus_overview_search ()) {
          if (GetForegroundWindow () == m_hwnd)
            ensure_search_entry_focus ();
          else
            schedule_focus_verify ();
        }
      }
    }
  } else {
    m_ws_box = m_app_grid_mode ? m_bg_grid : m_bg_picker;
    m_bg_radius_target = m_app_grid_mode ? m_radius_grid : m_radius_picker;
    m_bg_radius = m_bg_radius_target;
    m_thumbs_cur_opacity = (!m_app_grid_mode && m_thumbs_has_pills) ? 1.0 : 0.0;
    m_thumbs_cur_scale = m_app_grid_mode ? 0.5 : 1.0;
    m_thumbs_cur_ty =
        m_app_grid_mode ? (overview_thumbs_get_height (m_thumbs) * 0.5) : 0.0;
    if (m_thumbs != nullptr) {
      overview_thumbs_set_transform (m_thumbs, m_thumbs_cur_scale,
                                     m_thumbs_cur_ty);
      gtk_widget_set_visible (m_thumbs,
                              !m_app_grid_mode && m_thumbs_has_pills);
      gtk_widget_set_opacity (m_thumbs, m_thumbs_cur_opacity);
    }
    set_mini_opacity (m_app_grid_mode ? 0 : 255);
    if (m_app_grid != nullptr) {
      app_grid_set_transition_opacity (m_app_grid, false, 1.0);
      app_grid_clip_set_transition (m_app_grid_clip, false,
                                    m_app_grid_mode ? 1.0 : 0.0,
                                    m_app_grid_mode ? 0.0 : kGridSlideOffsetPx,
                                    true);
      sync_app_grid_visible (m_app_grid_mode);
    }
    m_grid_cur_opacity = m_app_grid_mode ? 1.0 : 0.0;
    m_grid_cur_slide_y = m_app_grid_mode ? 0.0 : kGridSlideOffsetPx;

    if (switch_dir == 0 && need_relayout_anim) {
      if (!prev_ws_cur_boxes.empty ()) {
        m_ws_cur_boxes = prev_ws_cur_boxes;
        const std::vector<RECT> &tgt_boxes =
            m_app_grid_mode ? m_ws_grid_boxes : m_ws_picker_boxes;
        if (m_ws_cur_boxes.size () < tgt_boxes.size ()) {
          for (size_t di = m_ws_cur_boxes.size (); di < tgt_boxes.size (); ++di)
            m_ws_cur_boxes.push_back (tgt_boxes[di]);
        } else if (m_ws_cur_boxes.size () > tgt_boxes.size ()) {
          m_ws_cur_boxes.resize (tgt_boxes.size ());
        }
      }
      for (SlotEntry &s : m_slots)
        apply_thumb_rect (s, s.current, s.current_opacity, true);
      start_state_transition (m_target_state, kRelayoutMs);
    } else {
      const std::vector<RECT> &target_boxes =
          m_app_grid_mode ? m_ws_grid_boxes : m_ws_picker_boxes;
      if (switch_off_x == 0) {
        apply_ws_boxes (target_boxes);
      } else {
        std::vector<RECT> shifted = target_boxes;
        for (RECT &r : shifted)
          r = translate_rect_x (r, switch_off_x);
        apply_ws_boxes (shifted);
      }
      for (SlotEntry &s : m_slots) {
        s.rect = m_app_grid_mode ? s.grid_rect : s.picker_rect;
        BYTE op = m_search_mode ? 0 : (m_app_grid_mode ? s.grid_opacity : 255);
        if (m_app_grid_mode && m_folder_dialog_open && op > 0)
          op = 76;
        apply_thumb_rect (s, translate_rect_x (s.rect, switch_off_x), op);
      }
      if (!m_app_grid_mode && !m_search_mode && switch_dir == 0)
        set_hover (slot_at_cursor ());
    }
  }
  if (debug_get_flags ().zorder_trace)
    dump_zorder (this, "placed");
}

void
cancel_pending_place_idles (void)
{
  if (m_place_opening_source != 0) {
    g_source_remove (m_place_opening_source);
    m_place_opening_source = 0;
  }
  if (m_place_idle_source != 0) {
    g_source_remove (m_place_idle_source);
    m_place_idle_source = 0;
  }
}

gboolean
overview_place_idle (gpointer user_data)
{
  (void)user_data;
  m_place_idle_source = 0;
  place_overview_locked (false);
  return G_SOURCE_REMOVE;
}

gboolean
overview_place_opening_idle (gpointer user_data)
{
  (void)user_data;
  m_place_opening_source = 0;
  if (!m_placed)
    place_overview_locked (true);
  else
    restack_locked ();
  return G_SOURCE_REMOVE;
}


// --- animation ---------------------------------------------------------------------

bool
panel_uses_blocking_gl_swap () const
{
  if (m_window == nullptr || !GTK_IS_NATIVE (m_window))
    return false;
  GskRenderer *renderer = gtk_native_get_renderer (GTK_NATIVE (m_window));
  if (renderer == nullptr)
    return false;
  const char *type = G_OBJECT_TYPE_NAME (renderer);
  return type != nullptr &&
         (std::strstr (type, "GLRenderer") != nullptr ||
          std::strstr (type, "NglRenderer") != nullptr);
}

gboolean
anim_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
  (void)widget;
  (void)user_data;
  Animation &anim = m_anim;

  const gint64 tick_t0 = g_get_monotonic_time ();
  if (m_open_perf_pending) {
    if (!anim.started && m_open_perf_tick0_enter_us == 0)
      m_open_perf_tick0_enter_us = tick_t0;
    else if (anim.started && m_open_perf_tick1_enter_us == 0)
      m_open_perf_tick1_enter_us = tick_t0;
  }
  flush_pending_dwm_frame ();
  ensure_after_paint_hook (clock);

  // Frame clock keeps the 250ms transition locked to the compositor's
  // cadence (a fixed 16ms timer beats against the display refresh). Anchor
  // start_us on the first rendered frame so one-time map/GL setup time never
  // eats the opening third of the transition.
  gint64 now = gdk_frame_clock_get_frame_time (clock);
  const bool first_frame = !anim.started;
  if (first_frame) {
    anim.started = true;
    anim.start_us = now - 8000;
    m_anim_tick_cpu_max_us = 0;
    m_anim_dwm_flush_max_us = 0;
    m_anim_panel_dirty_frames = 0;
  }
  g_anim_tracker.record_frame_time_us (now);
  double t = (now - anim.start_us) / 1000.0 / (double)anim.duration_ms;
  if (t < 0.0)
    t = 0.0;   // frame timestamp can predate the anim start
  if (t > 1.0)
    t = 1.0;

  double e = (anim.to_state == ControlsState::HIDDEN ||
              anim.from_state == anim.to_state)
                 ? ease_out_quad (t)
                 : ((anim.from_state == ControlsState::HIDDEN &&
                     anim.to_state == ControlsState::WINDOW_PICKER)
                        ? ease_out_sine (t)
                        : ease_out_cubic (t));

  RECT bg = lerp_rect (anim.bg_from, anim.bg_to, e);
  m_bg_radius = anim.radius_from + (anim.radius_to - anim.radius_from) * e;
  // Drive panel backdrop (#000000 <-> #222226) and #panelActivities pill
  // alpha at full 60fps in lockstep with the overview on non-blocking
  // composition renderers (default GskCairoRenderer + DirectComposition).
  // Only coarsen intermediate updates when an explicit Win32 GL swap
  // renderer is forced (two wglSwapBuffers calls per frame serialize VSync).
  const double next_panel_p =
      std::clamp (anim.panel_from + (anim.panel_to - anim.panel_from) * e,
                  0.0, 1.0);
  if (winome::should_push_panel_overview_progress (
          next_panel_p, m_panel_cur_progress, first_frame, t,
          panel_uses_blocking_gl_swap ())) {
    m_panel_cur_progress = next_panel_p;
    ++m_anim_panel_dirty_frames;
    winome_shell_panel_set_overview_progress_for_monitor (
        m_monitor != nullptr ? m_monitor : resolve_monitor (),
        m_panel_cur_progress);
  }
  bool gtk_will_paint = false;
  if (!m_ws_anim_to_boxes.empty () &&
      m_ws_anim_from_boxes.size () == m_ws_anim_to_boxes.size ()) {
    static thread_local std::vector<RECT> cur_boxes;
    cur_boxes.resize (m_ws_anim_to_boxes.size ());
    for (size_t i = 0; i < m_ws_anim_to_boxes.size (); ++i)
      cur_boxes[i] =
          lerp_rect (m_ws_anim_from_boxes[i], m_ws_anim_to_boxes[i], e);
    gtk_will_paint = apply_ws_boxes (cur_boxes, false) || gtk_will_paint;
  } else {
    gtk_will_paint = apply_bg_rect (bg, false) || gtk_will_paint;
  }

  // Keep thumbnails fade/scale/drop ahead of the workspace split gap in
  // WINDOW_PICKER <-> APP_GRID so empty pills never peek between separated
  // 15% workspace cards.
  double p_thumbs = e;
  if (anim.to_state == ControlsState::APP_GRID &&
      anim.from_state != ControlsState::APP_GRID) {
    p_thumbs = std::clamp (e * 1.65, 0.0, 1.0);
  } else if (anim.from_state == ControlsState::APP_GRID &&
             anim.to_state == ControlsState::WINDOW_PICKER) {
    p_thumbs = std::clamp ((e - 0.35) / 0.65, 0.0, 1.0);
  }

  m_thumbs_cur_opacity =
      anim.thumbs_from + (anim.thumbs_to - anim.thumbs_from) * p_thumbs;
  m_thumbs_cur_scale =
      anim.thumbs_scale_from +
      (anim.thumbs_scale_to - anim.thumbs_scale_from) * p_thumbs;
  m_thumbs_cur_ty =
      anim.thumbs_ty_from + (anim.thumbs_ty_to - anim.thumbs_ty_from) * p_thumbs;

  bool update_mini = false;
  BYTE desired_mini_op = 0;
  if (m_thumbs != nullptr && m_thumbs_has_pills) {
    const double f = std::clamp (m_thumbs_cur_opacity, 0.0, 1.0);
    const int dpi_scale =
        m_window != nullptr ? std::max (gtk_widget_get_scale_factor (m_window), 1)
                            : 1;
    const double pivot_x =
        (static_cast<double> (m_thumbs_rect.left) + m_thumbs_rect.right) * 0.5;
    const double pivot_y =
        (static_cast<double> (m_thumbs_rect.top) + m_thumbs_rect.bottom) * 0.5;
    const RECT thumbs_live_rect = transform_rect_around_pivot (
        m_thumbs_rect, pivot_x, pivot_y, m_thumbs_cur_scale,
        m_thumbs_cur_ty * dpi_scale);
    const bool covered =
        is_rect_fully_covered_by_boxes (thumbs_live_rect, m_ws_cur_boxes);
    const bool want_vis = (!covered && f > 0.01);
    if (gtk_widget_get_visible (m_thumbs) != (want_vis ? TRUE : FALSE)) {
      gtk_widget_set_visible (m_thumbs, want_vis ? TRUE : FALSE);
      gtk_will_paint = true;
    }
    if (want_vis) {
      overview_thumbs_set_transform (m_thumbs, m_thumbs_cur_scale,
                                     m_thumbs_cur_ty);
      if (std::fabs (gtk_widget_get_opacity (m_thumbs) - f) > 0.015) {
        gtk_widget_set_opacity (m_thumbs, f);
        gtk_will_paint = true;
      }
    }
    update_mini = true;
    desired_mini_op =
        want_vis ? static_cast<BYTE> (std::lround (255.0 * f)) : 0;
  }

  m_grid_cur_opacity =
      anim.grid_opacity_from + (anim.grid_opacity_to - anim.grid_opacity_from) * e;
  m_grid_cur_slide_y =
      anim.grid_slide_from + (anim.grid_slide_to - anim.grid_slide_from) * e;
  if (m_app_grid != nullptr && m_app_grid_clip != nullptr &&
      gtk_widget_get_visible (m_app_grid_clip)) {
    app_grid_clip_set_transition (
        m_app_grid_clip, true, std::clamp (m_grid_cur_opacity, 0.0, 1.0),
        m_grid_cur_slide_y);
  }

  // Defer DWM thumbnail commits to GdkFrameClock::after-paint whenever GTK is
  // painting a new frame this tick (including the final settle frame for
  // WINDOW_PICKER / APP_GRID), so wglSwapBuffers (m_ws_bg + m_thumbs) and
  // DwmUpdateThumbnailProperties (m_slots + m_mini_slots) hit DWM's compositor
  // in the exact same VSync with zero 1-frame empty-pill flash or lag.
  const bool defer_to_after_paint =
      ((t < 1.0 || anim.to_state != ControlsState::HIDDEN) &&
       gtk_will_paint && m_overview_after_paint_id != 0);
  if (defer_to_after_paint) {
    for (SlotEntry &s : m_slots) {
      int op = (int)(s.anim_from_opacity +
                     ((double)s.anim_to_opacity - s.anim_from_opacity) * e + 0.5);
      s.pending_rect = lerp_rect (s.anim_from, s.anim_to, e);
      s.pending_opacity = static_cast<BYTE> (std::clamp (op, 0, 255));
      s.has_pending_dwm = true;
    }
    m_dwm_pending_has_mini = update_mini;
    m_dwm_pending_mini_opacity = desired_mini_op;
    m_dwm_frame_pending = true;
  } else {
    clear_pending_dwm_frame ();
    for (SlotEntry &s : m_slots) {
      int op = (int)(s.anim_from_opacity +
                     ((double)s.anim_to_opacity - s.anim_from_opacity) * e + 0.5);
      BYTE clamped_op = static_cast<BYTE> (std::clamp (op, 0, 255));
      apply_thumb_rect (s, lerp_rect (s.anim_from, s.anim_to, e), clamped_op);
    }
    if (update_mini)
      set_mini_opacity (desired_mini_op);
  }

  const gint64 tick_dt = g_get_monotonic_time () - tick_t0;
  if (tick_dt > m_anim_tick_cpu_max_us)
    m_anim_tick_cpu_max_us = tick_dt;

  if (t >= 1.0) {
    anim.source = 0;
    anim.started = false;
    auto fstats = g_anim_tracker.snapshot ();
    if (fstats.frames > 0) {
      perf_record_overview_frame_stats (fstats);
      g_print ("[perf] overview anim frames=%llu dropped=%llu avg=%.2fms "
               "p95=%.2fms max=%.2fms tick_max=%.2fms dwm_max=%.2fms "
               "panel_draws=%d\n",
               static_cast<unsigned long long> (fstats.frames),
               static_cast<unsigned long long> (fstats.dropped_frames),
               fstats.avg_frame_ms, fstats.p95_frame_ms, fstats.max_frame_ms,
               m_anim_tick_cpu_max_us / 1000.0,
               m_anim_dwm_flush_max_us / 1000.0,
               m_anim_panel_dirty_frames);
    }
    if (anim.to_state == ControlsState::HIDDEN) {
      finish_hide ();
      return G_SOURCE_REMOVE;
    }
    if (m_open_perf_pending) {
      m_open_perf_pending = false;
      const gint64 settle_us = g_get_monotonic_time ();
      const double total_ms = (settle_us - m_open_perf_show_us) / 1000.0;
      if (total_ms > 320.0) {
        const double setup_ms =
            m_open_perf_trans_us > m_open_perf_show_us
                ? (m_open_perf_trans_us - m_open_perf_show_us) / 1000.0
                : -1.0;
        const double wait0_ms =
            (m_open_perf_tick0_enter_us > 0 && m_open_perf_trans_us > 0)
                ? (m_open_perf_tick0_enter_us - m_open_perf_trans_us) / 1000.0
                : -1.0;
        const double tick0_ms =
            (m_open_perf_tick0_exit_us > 0 && m_open_perf_tick0_enter_us > 0)
                ? (m_open_perf_tick0_exit_us - m_open_perf_tick0_enter_us) / 1000.0
                : -1.0;
        const double paint0_ms =
            (m_open_perf_paint0_us > 0 && m_open_perf_tick0_exit_us > 0)
                ? (m_open_perf_paint0_us - m_open_perf_tick0_exit_us) / 1000.0
                : -1.0;
        const double wait1_ms =
            (m_open_perf_tick1_enter_us > 0 && m_open_perf_paint0_us > 0)
                ? (m_open_perf_tick1_enter_us - m_open_perf_paint0_us) / 1000.0
                : ((m_open_perf_tick1_enter_us > 0 && m_open_perf_tick0_exit_us > 0)
                       ? (m_open_perf_tick1_enter_us - m_open_perf_tick0_exit_us) / 1000.0
                       : -1.0);
        g_print ("[ov-perf] slow_open total=%.1fms setup=%.1fms wait_tick0=%.1fms "
                 "tick0=%.1fms paint0=%.1fms wait_tick1=%.1fms frames=%llu\n",
                 total_ms, setup_ms, wait0_ms, tick0_ms, paint0_ms, wait1_ms,
                 static_cast<unsigned long long> (fstats.frames));
      }
    }
    // Re-draw settled workspace boxes once anim.source == 0 to paint drop-shadows.
    bool settle_painted = false;
    if (!m_ws_anim_to_boxes.empty ())
      settle_painted = apply_ws_boxes (m_ws_anim_to_boxes, false);
    else
      settle_painted = apply_bg_rect (bg, false);
    const bool settle_defer =
        (settle_painted || defer_to_after_paint) &&
        m_overview_after_paint_id != 0;
    if (anim.to_state == ControlsState::APP_GRID) {
      m_thumbs_cur_opacity = 0.0;
      m_thumbs_cur_scale = 0.5;
      m_thumbs_cur_ty = overview_thumbs_get_height (m_thumbs) * 0.5;
      if (m_thumbs != nullptr) {
        overview_thumbs_set_transform (m_thumbs, m_thumbs_cur_scale,
                                       m_thumbs_cur_ty);
        if (gtk_widget_get_visible (m_thumbs)) {
          gtk_widget_set_opacity (m_thumbs, 0.0);
          gtk_widget_set_visible (m_thumbs, FALSE);
        }
      }
      if (settle_defer) {
        m_dwm_pending_has_mini = true;
        m_dwm_pending_mini_opacity = 0;
        m_dwm_frame_pending = true;
      } else {
        set_mini_opacity (0);
      }
      if (m_app_grid != nullptr) {
        sync_app_grid_visible (true);
        app_grid_clip_set_transition (m_app_grid_clip, false, 1.0, 0.0, true);
        app_grid_set_transition_opacity (m_app_grid, false, 1.0);
        app_grid_refresh_running (m_app_grid);
        app_grid_clip_schedule_page0_capture (m_app_grid_clip);
      }
      m_grid_cur_opacity = 1.0;
      m_grid_cur_slide_y = 0.0;
      if (g_live_wallpaper_owner == this && !m_search_mode)
        live_wallpaper_set_paused (false);
      apply_pending_desktop_sync_if_any ();
      recheck_overview_search_focus_on_settle ("app-grid");
      return G_SOURCE_REMOVE;
    }
    // Settled on WINDOW_PICKER.
    if (m_app_grid != nullptr && m_app_grid_clip != nullptr &&
        gtk_widget_get_visible (m_app_grid_clip)) {
      app_grid_clip_set_transition (m_app_grid_clip, false, 0.0,
                                    kGridSlideOffsetPx, true);
      app_grid_set_transition_opacity (m_app_grid, false, 1.0);
      sync_app_grid_visible (false);
    }
    m_grid_cur_opacity = 0.0;
    m_grid_cur_slide_y = kGridSlideOffsetPx;
    m_thumbs_cur_opacity = m_thumbs_has_pills ? 1.0 : 0.0;
    m_thumbs_cur_scale = 1.0;
    m_thumbs_cur_ty = 0.0;
    if (m_thumbs != nullptr && m_thumbs_has_pills) {
      overview_thumbs_set_transform (m_thumbs, 1.0, 0.0);
      gtk_widget_set_visible (m_thumbs, TRUE);
      gtk_widget_set_opacity (m_thumbs, 1.0);
      if (settle_defer) {
        m_dwm_pending_has_mini = true;
        m_dwm_pending_mini_opacity = 255;
        m_dwm_frame_pending = true;
      } else {
        set_mini_opacity (255);
      }
    }
    if (m_pending_desktop_sync)
      apply_pending_desktop_sync_if_any ();
    else
      schedule_settle_hover (140);
    if (m_anim.source == 0 && m_switch_source == 0 && !m_search_mode)
      if (g_live_wallpaper_owner == this) live_wallpaper_set_paused (false);
    recheck_overview_search_focus_on_settle ("picker");
    return G_SOURCE_REMOVE;
  }
  if (m_open_perf_pending && m_open_perf_tick0_exit_us == 0)
    m_open_perf_tick0_exit_us = g_get_monotonic_time ();
  return G_SOURCE_CONTINUE;
}

// --- workspace switch animation ---------------------------------------------------

// Cancel an in-flight switch, destroying the outgoing previews without
// snapping (the overview itself is closing).
void
switch_cancel (void)
{
  clear_pending_dwm_frame ();
  if (m_switch_source != 0) {
    gtk_widget_remove_tick_callback (m_window, m_switch_source);
    m_switch_source = 0;
  }
  m_switch_started = false;
  for (SlotEntry &l : m_leaving)
    if (l.thumb != nullptr)
      DwmUnregisterThumbnail (l.thumb);
  m_leaving.clear ();
  m_ws_hist1_boxes.clear ();
  m_ws_hist2_boxes.clear ();
}

// Snap-finish an in-flight switch with the overview still on screen.
void
switch_finish (bool restore_hover)
{
  bool was_running = m_switch_source != 0 || !m_leaving.empty ();
  switch_cancel ();
  if (!was_running || m_window == nullptr || !gtk_widget_get_visible (m_window))
    return;
  const RECT &target_bg = m_app_grid_mode ? m_bg_grid : m_bg_picker;
  m_bg_final = target_bg;
  apply_ws_boxes (m_app_grid_mode ? m_ws_grid_boxes : m_ws_picker_boxes);
  overview_thumbs_set_indicator_value (m_thumbs, -1.0);
  winome_shell_panel_set_workspace_scroll_for_monitor (
      panel_scroll_target_monitor (),
      std::max (static_cast<int> (m_desktops.size ()), 1),
      static_cast<double> (std::max (m_desktop_active, 0)));
  for (SlotEntry &s : m_slots) {
    BYTE op = m_search_mode ? 0 : (m_app_grid_mode ? s.grid_opacity : 255);
    if (m_app_grid_mode && m_folder_dialog_open && op > 0)
      op = 76;
    apply_thumb_rect (s, s.rect, op);
  }
  if (winome::get_config ().dash_isolate_workspaces)
    refresh_dash_in_place ();
  if (restore_hover && !m_app_grid_mode && !m_search_mode &&
      m_target_state == ControlsState::WINDOW_PICKER) {
    set_hover (slot_at_cursor ());
  }
  if (m_anim.source == 0)
    if (g_live_wallpaper_owner == this) live_wallpaper_set_paused (false);
}

gboolean
switch_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
  (void)widget;
  (void)user_data;
  flush_pending_dwm_frame ();
  ensure_after_paint_hook (clock);
  const gint64 now = gdk_frame_clock_get_frame_time (clock);
  if (!m_switch_started) {
    m_switch_started = true;
    m_switch_start_us = now - 8000;
  }
  double t = (now - m_switch_start_us) / 1000.0 / (double)kSwitchMs;
  if (t < 0.0)
    t = 0.0;
  if (t > 1.0)
    t = 1.0;
  double e = ease_out_cubic (t);

  // WorkspacesView._scrollToActive + _updateWorkspacesState:
  // Continuous scroll adjustment position slides the entire horizontal
  // workspace strip while smoothly interpolating each workspace's scale
  // between WORKSPACE_INACTIVE_SCALE (0.94) and 1.0.
  const double scroll_pos = m_switch_from + (m_switch_to - m_switch_from) * e;
  m_switch_cur_pos = scroll_pos;
  const int ws_count = std::max (static_cast<int> (m_desktops.size ()), 1);
  const int scale =
      m_window != nullptr ? std::max (gtk_widget_get_scale_factor (m_window), 1)
                          : 1;

  if (!m_app_grid_mode) {
    std::vector<RECT> cur_boxes (static_cast<size_t> (ws_count));
    for (int d = 0; d < ws_count; ++d) {
      cur_boxes[static_cast<size_t> (d)] = picker_box_for_desktop (
          d, scroll_pos, m_bg_picker, m_monitor_w, scale);
    }
    const bool gtk_will_paint = apply_ws_boxes (cur_boxes, false);
    const bool defer =
        (t < 1.0 && gtk_will_paint && m_overview_after_paint_id != 0);
    for (SlotEntry &s : m_slots) {
      int d = std::clamp (s.desktop_index, 0, ws_count - 1);
      RECT r = remap_rect_between_boxes (
          s.canonical_picker_rect, m_bg_picker,
          cur_boxes[static_cast<size_t> (d)]);
      BYTE op = m_search_mode ? 0 : 255;
      if (defer) {
        s.pending_rect = r;
        s.pending_opacity = op;
        s.has_pending_dwm = true;
      } else {
        apply_thumb_rect (s, r, op);
      }
    }
    if (defer) {
      m_dwm_pending_has_mini = false;
      m_dwm_frame_pending = true;
    }
  } else {
    apply_ws_boxes (m_ws_grid_boxes);
    for (SlotEntry &s : m_slots) {
      BYTE op = m_search_mode ? 0 : s.grid_opacity;
      if (m_folder_dialog_open && op > 0)
        op = 76;
      apply_thumb_rect (s, s.grid_rect, op);
    }
  }
  overview_thumbs_set_indicator_value (m_thumbs, scroll_pos);
  winome_shell_panel_set_workspace_scroll_for_monitor (
      panel_scroll_target_monitor (),
      ws_count, scroll_pos);

  if (t >= 1.0) {
    clear_pending_dwm_frame ();
    switch_finish (true);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

void
refresh_dash_in_place (void)
{
  if (m_dash == nullptr || m_window == nullptr || m_root == nullptr ||
      g_panel_hwnd == nullptr || !gtk_widget_get_visible (m_window) ||
      !m_placed || m_target_state == ControlsState::HIDDEN) {
    return;
  }
  HMONITOR monitor = resolve_monitor ();
  const OverviewMonitorRole role = resolve_monitor_overview_role (
      ws::effective_mode (), monitor_manager_is_shell_primary (monitor));
  if (role.show_dash != m_show_dash) {
    rebuild_now ();
    return;
  }
  if (!m_show_dash) {
    gtk_widget_set_visible (m_dash, FALSE);
    return;
  }
  MONITORINFO mi{};
  mi.cbSize = sizeof (mi);
  if (!GetMonitorInfoW (monitor, &mi))
    return;
  int scale = std::max (gtk_widget_get_scale_factor (m_window), 1);
  int mw = mi.rcMonitor.right - mi.rcMonitor.left;
  int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
  int wa_h = std::max (mh - m_panel_height, 1);
  int dash_max = (int)(wa_h * kDashMaxHeightRatio + 0.5);
  overview_dash_repopulate (m_dash, monitor, dash_max / scale,
                            m_desktop_active);
  int dash_w = overview_dash_get_width (m_dash) * scale;
  int dash_h = std::min (overview_dash_get_height (m_dash) * scale, dash_max);
  m_dash_top = wa_h - dash_h;
  gtk_fixed_move (GTK_FIXED (m_root), m_dash, (mw - dash_w) / 2 / scale,
                  m_dash_top / scale);
  gtk_widget_set_visible (m_dash, TRUE);
}


HMONITOR
panel_scroll_target_monitor (void) const
{
  if (ws::effective_mode () == ws::MultiMonitorWorkspaceMode::Span &&
      !has_multiple_open_views ()) {
    return nullptr;
  }
  return resolve_monitor ();
}

bool
switch_workspace_from_user (int target)
{
  if (!ws::switch_to (resolve_monitor (), target))
    return false;
  ensure_overview_visible_on_active_desktop (target);
  schedule_vd_cloak_guard (60);
  switch_workspace_animated (target, true);
  if (ws::effective_mode () == ws::MultiMonitorWorkspaceMode::Span) {
    sync_sibling_views_workspace_switch (
        this, target, static_cast<int> (m_desktops.size ()));
    for (auto &up : g_views) {
      if (up && up.get () != this && up->is_open ()) {
        up->ensure_overview_visible_on_active_desktop (target);
        up->schedule_vd_cloak_guard (60);
      }
    }
  }
  return true;
}

// Animated workspace switch (WorkspacesView._scrollToActive: 250ms
// EASE_OUT_CUBIC on the scroll adjustment; zero DWM thumbnail teardown or
// EnumWindows re-enumeration when desktop count is unchanged).
void
switch_workspace_animated (int to, bool is_internal = false,
                           int known_desktop_count = -1)
{
  if (m_window == nullptr || !gtk_widget_get_visible (m_window) ||
      !m_placed || m_target_state == ControlsState::HIDDEN) {
    return;
  }
  if (to < 0 || to == m_desktop_active)
    return;
  if (m_anim.source != 0) {
    // Open or picker<->grid transition is in flight; defer desktop sync until
    // anim_tick completes so the workspace switch is never silently dropped.
    m_pending_desktop_sync = true;
    m_pending_desktop_target = to;
    return;
  }

  // Validate against live virtual desktop count (reusing known_desktop_count
  // from a Span sibling broadcast or external callback to avoid redundant
  // synchronous COM roundtrips on the GTK main thread).
  int fresh_count = known_desktop_count;
  if (fresh_count <= 0) {
    HMONITOR mon = resolve_monitor ();
    fresh_count = static_cast<int> (ws::desktops_for_monitor (mon).size ());
  }
  if (fresh_count <= 0 || to >= fresh_count)
    return;

  const bool was_switching = (m_switch_source != 0);
  const int prev_active = m_desktop_active;
  const double from_pos =
      was_switching ? m_switch_cur_pos
                    : static_cast<double> (std::max (m_desktop_active, 0));
  switch_cancel ();

  set_hover (-1, true);
  reset_drag_state ();

  if (fresh_count != static_cast<int> (m_desktops.size ())) {
    place_overview_locked (false, 0);
    return;
  }

  m_desktop_active = to;
  const int ws_count = std::max (static_cast<int> (m_desktops.size ()), 1);
  const int scale = std::max (gtk_widget_get_scale_factor (m_window), 1);
  m_ws_picker_boxes.resize (static_cast<size_t> (ws_count));
  m_ws_full_boxes.resize (static_cast<size_t> (ws_count));
  for (int d = 0; d < ws_count; ++d) {
    m_ws_picker_boxes[static_cast<size_t> (d)] =
        picker_box_for_desktop (d, static_cast<double> (to), m_bg_picker,
                                m_monitor_w, scale);
    m_ws_full_boxes[static_cast<size_t> (d)] =
        full_box_for_desktop (d, to, m_bg_full, m_bg_picker, m_monitor_w,
                              scale);
  }
  for (SlotEntry &s : m_slots) {
    int d = std::clamp (s.desktop_index, 0, ws_count - 1);
    const RECT &ws_picker_d = m_ws_picker_boxes[static_cast<size_t> (d)];
    const RECT &ws_full_d = m_ws_full_boxes[static_cast<size_t> (d)];
    s.picker_rect = (d == to)
                        ? s.canonical_picker_rect
                        : remap_rect_between_boxes (
                              s.canonical_picker_rect, m_bg_picker, ws_picker_d);
    s.rect = m_app_grid_mode ? s.grid_rect : s.picker_rect;
    // Keep HIDDEN close-animation endpoints aligned with the newly active
    // workspace so exiting Overview zooms into workspace `to` while sliding
    // neighboring workspaces' windows off-screen with their workspace boxes.
    s.minimized = (d == to) && s.minimized_any;
    if (s.minimized) {
      s.start.left = ws_picker_d.left;
      s.start.top = ws_picker_d.top;
      s.start.right = s.start.left + 1;
      s.start.bottom = s.start.top + 1;
    } else {
      s.start = (d == to)
                    ? s.canonical_start
                    : remap_rect_between_boxes (s.canonical_start, m_bg_full,
                                                ws_full_d);
    }
  }

  if (m_thumbs != nullptr && m_thumbs_has_pills) {
    int wa_h = std::max (m_monitor_h - m_panel_height, 1);
    int box_h = (int)(wa_h * kMaxThumbnailScale) / scale;
    bool can_manage = ws::can_manage_workspaces ();
    const double panel_ratio =
        (m_panel_height > 0 && wa_h > 0)
            ? (static_cast<double> (m_panel_height) / static_cast<double> (wa_h))
            : 0.0;
    overview_thumbs_repopulate (
        m_thumbs, (int)m_desktops.size (), m_desktop_active, box_h,
        (double)m_monitor_w / scale, (double)m_monitor_w, (double)wa_h,
        active_wallpaper_thumb_texture (),
        can_manage, false, panel_ratio);
  }

  if (g_live_wallpaper_owner == this) live_wallpaper_set_paused (true);
  m_switch_from = from_pos;
  m_switch_to = static_cast<double> (to);
  m_switch_cur_pos = from_pos;
  m_switch_width = m_ws_box.right - m_ws_box.left;
  m_switch_started = false;
  m_switch_start_us = g_get_monotonic_time ();
  if (is_internal) {
    m_last_internal_switch_us = m_switch_start_us;
    m_last_internal_switch_from = prev_active;
    m_last_internal_switch_to = to;
  } else {
    m_last_internal_switch_us = 0;
    m_last_internal_switch_from = -1;
    m_last_internal_switch_to = -1;
  }
  overview_thumbs_set_indicator_value (m_thumbs, from_pos);
  winome_shell_panel_set_workspace_scroll_for_monitor (
      panel_scroll_target_monitor (),
      std::max (static_cast<int> (m_desktops.size ()), 1), from_pos);
  m_switch_source =
      gtk_widget_add_tick_callback (m_window, switch_tick_cb, this, nullptr);
  g_print ("[ov] mon=%p workspace switch %.2f -> %d (animated%s)\n",
           static_cast<void *> (m_monitor), from_pos, to,
           is_internal ? "" : ", external");
}

void
start_state_transition (ControlsState to_state, guint duration_ms)
{
  if (m_window == nullptr)
    return;
  if (g_live_wallpaper_owner == this) live_wallpaper_set_paused (true);
  flush_pending_dwm_frame ();
  if (m_switch_source != 0)
    switch_finish (false);
  if (m_anim.source != 0) {
    gtk_widget_remove_tick_callback (m_window, m_anim.source);
    m_anim.source = 0;
  }
  cancel_search_fade ();
  if (to_state == ControlsState::HIDDEN)
    cancel_focus_verify ();
  cancel_settle_hover ();
  cancel_post_hide_workarea ();
  set_hover (-1, true);
  reset_drag_state ();

  ControlsState prev_target = m_target_state;
  m_target_state = to_state;
  m_app_grid_mode = (to_state == ControlsState::APP_GRID);
  if (g_focused_view == this)
    g_atomic_can_shift_app_grid.store (to_state == ControlsState::WINDOW_PICKER,
                                       std::memory_order_release);

  if (to_state != ControlsState::APP_GRID && m_app_grid != nullptr) {
    app_grid_close_open_folder (m_app_grid);
    app_grid_close_menu (m_app_grid);
  }
  if (m_dash != nullptr)
    overview_dash_set_apps_active (m_dash, to_state == ControlsState::APP_GRID);

  m_anim.from_state = prev_target;
  m_anim.to_state = to_state;
  m_anim.duration_ms = std::max (duration_ms, 1u);
  m_anim.started = false;
  m_grid_last_moved_y = INT_MIN;

  // Snapshot live visual state so mid-flight interruptions are completely
  // seamless with zero jump.
  m_anim.bg_from = m_bg_cur;
  m_ws_anim_from_boxes = m_ws_cur_boxes;
  m_anim.radius_from = m_bg_radius;
  m_anim.thumbs_from = m_thumbs_cur_opacity;
  m_anim.thumbs_scale_from = m_thumbs_cur_scale;
  m_anim.thumbs_ty_from = m_thumbs_cur_ty;
  m_anim.grid_opacity_from = m_grid_cur_opacity;
  m_anim.grid_slide_from = m_grid_cur_slide_y;
  m_anim.panel_from = m_panel_cur_progress;
  m_anim.panel_to = (to_state == ControlsState::HIDDEN) ? 0.0 : 1.0;

  const double thumbs_grid_ty =
      m_thumbs != nullptr ? (overview_thumbs_get_height (m_thumbs) * 0.5) : 0.0;

  if (to_state == ControlsState::WINDOW_PICKER) {
    m_anim.bg_to = m_bg_picker;
    m_ws_anim_to_boxes = m_ws_picker_boxes;
    m_bg_final = m_bg_picker;
    m_anim.radius_to = m_radius_picker;
    m_bg_radius_target = m_radius_picker;
    m_ws_box = m_bg_picker;
    m_anim.thumbs_to = m_thumbs_has_pills ? 1.0 : 0.0;
    m_anim.thumbs_scale_to = 1.0;
    m_anim.thumbs_ty_to = 0.0;
    m_anim.grid_opacity_to = 0.0;
    m_anim.grid_slide_to = kGridSlideOffsetPx;
    if (m_app_grid != nullptr && m_app_grid_clip != nullptr &&
        gtk_widget_get_visible (m_app_grid_clip)) {
      app_grid_set_transition_opacity (m_app_grid, true, 1.0);
      const bool using_cache = app_grid_clip_begin_transition (
          m_app_grid_clip, std::clamp (m_grid_cur_opacity, 0.0, 1.0),
          m_grid_cur_slide_y);
      if (using_cache)
        gtk_widget_set_visible (m_app_grid, FALSE);
    }
    for (SlotEntry &s : m_slots) {
      s.anim_from = s.current;
      s.anim_from_opacity = s.current_opacity;
      s.anim_to = s.picker_rect;
      s.anim_to_opacity = 255;
      s.rect = s.picker_rect;
    }
  } else if (to_state == ControlsState::APP_GRID) {
    m_anim.bg_to = m_bg_grid;
    m_ws_anim_to_boxes = m_ws_grid_boxes;
    m_bg_final = m_bg_grid;
    m_anim.radius_to = m_radius_grid;
    m_bg_radius_target = m_radius_grid;
    m_ws_box = m_bg_grid;
    // Match GNOME Shell overviewControls.js _getThumbnailsBoxParams():
    // fade opacity 1->0, scale 1->0.5 around (0.5, 0.5), translate_y 0->h/2.
    m_anim.thumbs_to = 0.0;
    m_anim.thumbs_scale_to = 0.5;
    m_anim.thumbs_ty_to = thumbs_grid_ty;
    m_anim.grid_opacity_to = 1.0;
    m_anim.grid_slide_to = 0.0;
    if (m_app_grid != nullptr) {
      ensure_app_grid_synced (false);
      const bool was_hidden =
          (prev_target != ControlsState::APP_GRID &&
           (m_app_grid_clip == nullptr ||
            !gtk_widget_get_visible (m_app_grid_clip)));
      if (was_hidden) {
        app_grid_reset_page (m_app_grid);
        m_grid_cur_opacity = 0.0;
        m_grid_cur_slide_y = kGridSlideOffsetPx;
        m_anim.grid_opacity_from = 0.0;
        m_anim.grid_slide_from = kGridSlideOffsetPx;
      }
      app_grid_set_transition_opacity (m_app_grid, true, 1.0);
      const bool using_cache = app_grid_clip_begin_transition (
          m_app_grid_clip, std::clamp (m_grid_cur_opacity, 0.0, 1.0),
          m_grid_cur_slide_y);
      if (using_cache && m_app_grid_clip != nullptr) {
        gtk_widget_set_visible (m_app_grid_clip, TRUE);
        gtk_widget_set_visible (m_app_grid, FALSE);
      } else {
        sync_app_grid_visible (true);
      }
    }
    for (SlotEntry &s : m_slots) {
      s.anim_from = s.current;
      s.anim_from_opacity = s.current_opacity;
      s.anim_to = s.grid_rect;
      s.anim_to_opacity = s.grid_opacity;
      s.rect = s.grid_rect;
    }
  } else {
    m_pending_desktop_sync = false;
    m_pending_desktop_target = -1;
    m_anim.bg_to = m_bg_full;
    m_ws_anim_to_boxes = m_ws_full_boxes;
    m_anim.radius_to = 0.0;
    m_bg_radius_target = 0.0;
    // Match GNOME Shell overviewControls.js: when closing from WINDOW_PICKER,
    // keep the thumbnails bar (and its mini window previews) stationary at
    // opacity 1.0 behind the expanding workspace so it is smoothly covered
    // without flashing; when closing directly from APP_GRID, keep it hidden.
    const bool keep_hidden_from_grid =
        (prev_target == ControlsState::APP_GRID && m_thumbs_cur_opacity <= 0.01);
    if (keep_hidden_from_grid) {
      m_anim.thumbs_to = 0.0;
      m_anim.thumbs_scale_to = 0.5;
      m_anim.thumbs_ty_to = thumbs_grid_ty;
    } else {
      m_anim.thumbs_to = m_thumbs_has_pills ? 1.0 : 0.0;
      m_anim.thumbs_scale_to = 1.0;
      m_anim.thumbs_ty_to = 0.0;
    }
    m_anim.grid_opacity_to = 0.0;
    m_anim.grid_slide_to = kGridSlideOffsetPx;
    if (m_app_grid != nullptr && m_app_grid_clip != nullptr &&
        gtk_widget_get_visible (m_app_grid_clip)) {
      app_grid_set_transition_opacity (m_app_grid, true, 1.0);
      const bool using_cache = app_grid_clip_begin_transition (
          m_app_grid_clip, std::clamp (m_grid_cur_opacity, 0.0, 1.0),
          m_grid_cur_slide_y);
      if (using_cache)
        gtk_widget_set_visible (m_app_grid, FALSE);
    }
    for (SlotEntry &s : m_slots) {
      s.anim_from = s.current;
      s.anim_from_opacity = s.current_opacity;
      s.anim_to = s.start;
      s.anim_to_opacity = s.minimized_any ? 0 : 255;
    }
  }

  if (m_ws_anim_from_boxes.size () != m_ws_anim_to_boxes.size ())
    m_ws_anim_from_boxes = m_ws_anim_to_boxes;

  m_ws_hist1_boxes.clear ();
  m_ws_hist2_boxes.clear ();

  restack_locked ();
  if (to_state == ControlsState::HIDDEN ||
      prev_target != ControlsState::HIDDEN) {
    m_open_perf_pending = false;
  } else if (m_open_perf_pending && prev_target == ControlsState::HIDDEN &&
             m_open_perf_trans_us == 0) {
    m_open_perf_trans_us = g_get_monotonic_time ();
  }
  m_anim.start_us = g_get_monotonic_time ();
  g_anim_tracker.reset ();
  m_anim.source =
      gtk_widget_add_tick_callback (m_window, anim_tick_cb, this, nullptr);
}


void
finish_hide (void)
{
  m_pending_reveal = false;
  m_uncloak_on_after_paint = false;
  cancel_uncloak_fallback ();
  cancel_vd_cloak_guard ();
  m_panel_cur_progress = 0.0;
  winome_shell_panel_set_workspace_scroll_for_monitor (resolve_monitor (), -1, -1.0);
  if (g_live_wallpaper_owner == this) {
    live_wallpaper_stop ();
    g_using_live_wallpaper = false;
    g_live_wallpaper_owner = nullptr;
    g_clear_object (&g_live_wallpaper);
    g_clear_object (&g_live_wallpaper_thumb);
  }
  clear_pending_dwm_frame ();
  cancel_focus_verify ();
  cancel_settle_hover ();
  set_hover (-1, true);
  cancel_pending_place_idles ();
  cancel_delayed_rebuild ();
  if (m_anim.source != 0) {
    gtk_widget_remove_tick_callback (m_window, m_anim.source);
    m_anim.source = 0;
  }
  bool was_overview_fg =
      (m_hwnd != nullptr && GetForegroundWindow () == m_hwnd);
  if (m_window != nullptr && gtk_widget_get_visible (m_window))
    gtk_widget_set_visible (m_window, FALSE);
  uncloak_if_needed ();
  winome::sanitize_gdk_gl_backing_windows ();
  winome_shell_panel_set_overview_progress_for_monitor (
      resolve_monitor (), 0.0);
  destroy_previews ();
  switch_cancel ();
  destroy_mini_previews ();
  m_slots.shrink_to_fit ();
  m_mini_slots.shrink_to_fit ();
  m_placed = false;
  m_target_state = ControlsState::HIDDEN;
  m_pending_show_state = ControlsState::WINDOW_PICKER;
  m_pending_desktop_sync = false;
  m_pending_desktop_target = -1;
  m_last_internal_switch_us = 0;
  m_last_internal_switch_from = -1;
  m_last_internal_switch_to = -1;
  if (g_focused_view == this) {
    g_focused_view = find_any_open_view (this);
    g_atomic_can_shift_app_grid.store (
        g_focused_view != nullptr && g_focused_view->m_target_state == ControlsState::WINDOW_PICKER,
        std::memory_order_release);
    if (g_focused_view == nullptr)
      g_pending_shift_app_grid.store (false, std::memory_order_release);
  }
  reset_drag_state ();
  m_drag_just_ended = false;
  if (m_dash != nullptr) {
    overview_dash_close_menu (m_dash);
    overview_dash_set_apps_active (m_dash, false);
  }
  if (m_app_grid != nullptr) {
    app_grid_close_open_folder (m_app_grid);
    app_grid_close_menu (m_app_grid);
    app_grid_reset_page (m_app_grid);
    app_grid_clip_set_transition (m_app_grid_clip, false, 1.0, 0.0, true);
    app_grid_set_transition_opacity (m_app_grid, false, 1.0);
    sync_app_grid_visible (false);
  }
  m_folder_dialog_open = false;
  if (m_search_fade_source != 0 && m_window != nullptr) {
    gtk_widget_remove_tick_callback (m_window, m_search_fade_source);
    m_search_fade_source = 0;
  }
  m_search_fade_cur = 0.0;
  m_search_fade_from = 0.0;
  m_search_fade_to = 0.0;
  m_search_mode = false;
  m_search_preedit_active = false;
  if (m_search != nullptr) {
    if (gtk_widget_get_realized (m_search))
      gtk_entry_reset_im_context (GTK_ENTRY (m_search));
    gtk_widget_remove_css_class (m_search, "search-active");
    set_search_dormant (true);
    gtk_widget_set_opacity (m_search, 1.0);
    gtk_widget_set_can_target (m_search, TRUE);
  }
  if (m_dash != nullptr) {
    gtk_widget_set_opacity (m_dash, 1.0);
    gtk_widget_set_can_target (m_dash, TRUE);
  }
  if (m_ws_bg != nullptr)
    gtk_widget_set_opacity (m_ws_bg, 1.0);
  if (m_app_grid_clip != nullptr)
    gtk_widget_set_opacity (m_app_grid_clip, 1.0);
  if (m_search_view != nullptr) {
    overview_search_view_reset (m_search_view);
    gtk_widget_set_opacity (m_search_view, 0.0);
    gtk_widget_set_visible (m_search_view, FALSE);
  }
  m_grid_cur_opacity = 0.0;
  m_grid_cur_slide_y = kGridSlideOffsetPx;
  m_grid_last_moved_y = INT_MIN;
  m_app_grid_mode = false;
  if (!overview_any_open ()) {
    if (g_idle_trim_timer != 0) {
      g_source_remove (g_idle_trim_timer);
      g_idle_trim_timer = 0;
    }
    g_idle_trim_timer =
        g_timeout_add_seconds (15, overview_idle_trim_cb, nullptr);
  }
  // The side controls stay VISIBLE at their end-of-close geometry (the
  // background rests on the full work area, radius 0): the window keeps its
  // fullscreen size while hidden, so the first frame of the next open shows
  // the wallpaper-covered work area — GNOME's t=0 — instead of a bare
  // #222226 flash while the placement idle is still pending.
  m_bg_cur = m_bg_full;
  m_ws_cur_boxes.assign (1, m_bg_full);
  m_ws_hist1_boxes.clear ();
  m_ws_hist2_boxes.clear ();
  m_bg_radius = 0.0;
  m_ws_bg_last_radius = -1.0;
  m_ws_bg_settled_shadow_drawn = false;
  m_thumbs_cur_opacity = 0.0;
  m_thumbs_cur_scale = 1.0;
  m_thumbs_cur_ty = 0.0;
  if (m_thumbs != nullptr) {
    overview_thumbs_set_transform (m_thumbs, 1.0, 0.0);
    gtk_widget_set_opacity (m_thumbs, 0.0);
    gtk_widget_set_visible (m_thumbs, FALSE);
  }
  if (m_hwnd != nullptr && IsWindow (m_hwnd))
    apply_ex_styles (m_hwnd);
  const bool still_any_open = overview_any_open ();
  if (!still_any_open) {
    bool was_discarded =
        g_restore_focus_discarded.exchange (false, std::memory_order_acq_rel);
    HWND restore_hwnd = g_prev_foreground_hwnd;
    g_prev_foreground_hwnd = nullptr;
    AllowSetForegroundWindow (ASFW_ANY);
    if (!was_discarded && was_overview_fg &&
        is_external_foreground_target (restore_hwnd) &&
        IsWindowVisible (restore_hwnd) && !IsIconic (restore_hwnd) &&
        !is_cloaked (restore_hwnd) &&
        (m_desktops.empty () || ws::window_on_active_workspace (restore_hwnd))) {
      SetForegroundWindow (restore_hwnd);
    }
    schedule_post_hide_workarea (180);
  } else if (was_overview_fg && g_focused_view != nullptr && g_focused_view->is_open ()) {
    g_focused_view->focus_overview_window (false);
  }
  winome::evaluate_fullscreen_state ();
  sync_overview_active_state (overview_any_open ());
}

void
cancel_delayed_rebuild (void)
{
  if (m_close_rebuild_timer != 0) {
    g_source_remove (m_close_rebuild_timer);
    m_close_rebuild_timer = 0;
  }
}

gboolean
delayed_rebuild_cb (gpointer user_data G_GNUC_UNUSED)
{
  if (m_drag_slot >= 0 || m_drag_active)
    return G_SOURCE_CONTINUE;
  m_close_rebuild_timer = 0;
  if (m_window != nullptr && gtk_widget_get_visible (m_window) && m_placed &&
      m_target_state != ControlsState::HIDDEN && overview_active ()) {
    rebuild_now ();
  }
  return G_SOURCE_REMOVE;
}

void
schedule_delayed_rebuild (guint delay_ms)
{
  cancel_delayed_rebuild ();
  m_close_rebuild_timer =
      g_timeout_add (delay_ms, delayed_rebuild_static_cb, this);
}

void
reset_drag_state (void)
{
  if (m_drag_tick_source != 0 && m_window != nullptr) {
    gtk_widget_remove_tick_callback (m_window, m_drag_tick_source);
    m_drag_tick_source = 0;
  }
  if (m_drag_snapback_source != 0 && m_window != nullptr) {
    gtk_widget_remove_tick_callback (m_window, m_drag_snapback_source);
    m_drag_snapback_source = 0;
  }
  if (m_drag_snapback_slot >= 0 &&
      m_drag_snapback_slot < static_cast<int> (m_slots.size ())) {
    SlotEntry &s = m_slots[static_cast<size_t> (m_drag_snapback_slot)];
    apply_thumb_rect (s, s.rect, 255);
  }
  m_drag_snapback_slot = -1;
  m_drag_slot = -1;
  m_drag_hwnd = nullptr;
  m_drag_active = false;
  m_drag_thumbs_revealed = false;
  if (m_drag_drop_ws_peek != -1) {
    m_drag_drop_ws_peek = -1;
    if (m_ws_bg != nullptr)
      gtk_widget_queue_draw (m_ws_bg);
  }
  if (m_thumbs != nullptr)
    overview_thumbs_set_drop_target (m_thumbs, -1);
}

bool
update_dragged_slot_transform (gint64 now_us)
{
  if (!m_drag_active || m_drag_slot < 0 ||
      m_drag_slot >= static_cast<int> (m_slots.size ()) ||
      m_window == nullptr) {
    return false;
  }
  const int scale = std::max (gtk_widget_get_scale_factor (m_window), 1);
  double t = (now_us - m_drag_scale_start_us) / 1000.0 /
             static_cast<double> (kDragScaleMs);
  t = std::clamp (t, 0.0, 1.0);
  const double e = ease_out_quad (t);
  const double cur_scale = 1.0 + (m_drag_target_scale - 1.0) * e;
  const BYTE cur_op = static_cast<BYTE> (std::clamp (
      static_cast<int> (
          std::lround (255.0 + (kDraggingWindowOpacity - 255.0) * e)),
      64, 255));

  const int orig_w = m_drag_orig_rect.right - m_drag_orig_rect.left;
  const int orig_h = m_drag_orig_rect.bottom - m_drag_orig_rect.top;
  const int dw =
      std::max (static_cast<int> (std::lround (orig_w * cur_scale)), 48 * scale);
  const int dh =
      std::max (static_cast<int> (std::lround (orig_h * cur_scale)), 36 * scale);
  // dnd.js _updateActorPosition: scale around the grab point so the preview
  // never teleports to the cursor center at t=0.
  const int left =
      m_drag_cur_px + static_cast<int> (std::lround (m_drag_offset_x * cur_scale));
  const int top =
      m_drag_cur_py + static_cast<int> (std::lround (m_drag_offset_y * cur_scale));
  RECT moved{left, top, left + dw, top + dh};
  apply_thumb_rect (m_slots[static_cast<size_t> (m_drag_slot)], moved, cur_op);
  return t < 1.0;
}

gboolean
drag_scale_tick (GtkWidget *widget G_GNUC_UNUSED, GdkFrameClock *clock,
                 gpointer user_data G_GNUC_UNUSED)
{
  if (!m_drag_active || m_drag_slot < 0) {
    m_drag_tick_source = 0;
    return G_SOURCE_REMOVE;
  }
  const gint64 now = gdk_frame_clock_get_frame_time (clock);
  update_dragged_slot_transform (now);
  return G_SOURCE_CONTINUE;
}

gboolean
drag_snapback_tick (GtkWidget *widget G_GNUC_UNUSED, GdkFrameClock *clock,
                    gpointer user_data G_GNUC_UNUSED)
{
  if (m_drag_snapback_slot < 0 ||
      m_drag_snapback_slot >= static_cast<int> (m_slots.size ())) {
    m_drag_snapback_slot = -1;
    m_drag_snapback_source = 0;
    return G_SOURCE_REMOVE;
  }
  const gint64 now = gdk_frame_clock_get_frame_time (clock);
  double t = (now - m_drag_snapback_start_us) / 1000.0 /
             static_cast<double> (kDragSnapBackMs);
  t = std::clamp (t, 0.0, 1.0);
  const double e = ease_out_quad (t);
  SlotEntry &s = m_slots[static_cast<size_t> (m_drag_snapback_slot)];
  RECT cur = lerp_rect (m_drag_snapback_from, s.rect, e);
  BYTE op = static_cast<BYTE> (std::clamp (
      static_cast<int> (std::lround (
          m_drag_snapback_from_op + (255.0 - m_drag_snapback_from_op) * e)),
      0, 255));
  apply_thumb_rect (s, cur, op);
  if (t >= 1.0) {
    apply_thumb_rect (s, s.rect, 255);
    m_drag_snapback_slot = -1;
    m_drag_snapback_source = 0;
    if (!m_app_grid_mode && m_target_state == ControlsState::WINDOW_PICKER)
      set_hover (slot_at_cursor ());
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

int
resolve_drag_drop_workspace (int px, int py, int scale, int *out_thumb_pill,
                             int *out_peek_ws)
{
  if (out_thumb_pill != nullptr)
    *out_thumb_pill = -1;
  if (out_peek_ws != nullptr)
    *out_peek_ws = -1;

  // 1. ThumbnailsBox pills at the top (workspaceThumbnail.js handleDragOver).
  if (m_thumbs != nullptr && m_thumbs_rect.right > m_thumbs_rect.left) {
    const int pad = 8 * scale;
    if (px >= m_thumbs_rect.left - pad && px <= m_thumbs_rect.right + pad &&
        py >= m_thumbs_rect.top - pad && py <= m_thumbs_rect.bottom + pad) {
      double lx = static_cast<double> (px - m_thumbs_rect.left) / scale;
      double ly = static_cast<double> (py - m_thumbs_rect.top) / scale;
      int pill = overview_thumbs_drop_target_at (m_thumbs, lx, ly);
      if (pill >= 0) {
        if (out_thumb_pill != nullptr)
          *out_thumb_pill = pill;
        return pill;
      }
    }
  }

  // 2. Peeking adjacent workspaces on the left/right edges (or miniature
  //    workspaces in APP_GRID) (workspace.js Workspace.handleDragOver /
  //    acceptDrop). Match either when the pointer is inside the adjacent
  //    workspace box / side margin, or when the dragged window preview
  //    significantly overlaps the adjacent workspace box.
  const int active_ws = std::max (m_desktop_active, 0);
  const RECT dragged_rect =
      (m_drag_slot >= 0 && m_drag_slot < static_cast<int> (m_slots.size ()))
          ? m_slots[static_cast<size_t> (m_drag_slot)].current
          : RECT{px, py, px + 1, py + 1};
  const int dragged_w = std::max<int> (dragged_rect.right - dragged_rect.left, 1);

  for (int d = 0; d < static_cast<int> (m_ws_cur_boxes.size ()); ++d) {
    if (d == active_ws)
      continue;
    const RECT &b = m_ws_cur_boxes[static_cast<size_t> (d)];
    if (b.right <= 0 || b.left >= m_monitor_w)
      continue;
    bool hit = (px >= b.left && px < b.right && py >= b.top && py < b.bottom);
    if (!hit && !m_app_grid_mode &&
        py >= m_bg_picker.top && py < m_bg_picker.bottom) {
      if (d == active_ws - 1 &&
          (px < m_bg_picker.left ||
           dragged_rect.left < b.right - dragged_w / 4)) {
        hit = true;
      } else if (d == active_ws + 1 &&
                 (px >= m_bg_picker.right ||
                  dragged_rect.right > b.left + dragged_w / 4)) {
        hit = true;
      }
    }
    if (hit) {
      if (out_peek_ws != nullptr)
        *out_peek_ws = d;
      if (out_thumb_pill != nullptr)
        *out_thumb_pill = d;
      return d;
    }
  }
  return -1;
}

void
on_drag_begin (GtkGestureDrag *drag, double x, double y,
               gpointer user_data G_GNUC_UNUSED)
{
  reset_drag_state ();

  if (m_app_grid_mode || m_search_mode || m_search_fade_cur > 0.005 ||
      m_folder_dialog_open || m_desktops.empty () || m_window == nullptr ||
      m_target_state == ControlsState::HIDDEN || !overview_active ()) {
    gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_DENIED);
    return;
  }

  if (m_switch_source != 0)
    switch_finish (false);

  int scale = std::max (gtk_widget_get_scale_factor (m_window), 1);
  int px = (int)(x * scale);
  int py = (int)(y * scale);
  if (py < 0 || py >= m_dash_top ||
      (px >= m_search_rect.left && px < m_search_rect.right &&
       py >= m_search_rect.top && py < m_search_rect.bottom) ||
      (m_thumbs_rect.right > m_thumbs_rect.left &&
       px >= m_thumbs_rect.left && px < m_thumbs_rect.right &&
       py >= m_thumbs_rect.top && py < m_thumbs_rect.bottom)) {
    gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_DENIED);
    return;
  }

  const int active_ws = std::max (m_desktop_active, 0);
  for (int i = 0; i < (int)m_slots.size (); ++i) {
    if (m_slots[i].desktop_index != active_ws)
      continue;
    const RECT r = slot_display_rect (i);
    if (px >= r.left && px < r.right && py >= r.top && py < r.bottom) {
      set_hover (-1, true);
      m_drag_slot = i;
      m_drag_hwnd = m_slots[i].target;
      m_drag_orig_rect = m_slots[i].rect;
      m_drag_start_px = px;
      m_drag_start_py = py;
      m_drag_cur_px = px;
      m_drag_cur_py = py;
      m_drag_offset_x = m_drag_orig_rect.left - px;
      m_drag_offset_y = m_drag_orig_rect.top - py;
      const int orig_w = m_drag_orig_rect.right - m_drag_orig_rect.left;
      const int orig_h = m_drag_orig_rect.bottom - m_drag_orig_rect.top;
      const int max_dim = std::max (orig_w, orig_h);
      m_drag_target_scale =
          max_dim > 0
              ? std::clamp (static_cast<double> (kWindowDndSize * scale) /
                                max_dim,
                            0.38, 0.82)
              : 0.65;
      return;
    }
  }
  gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_DENIED);
}

void
on_drag_update (GtkGestureDrag *drag G_GNUC_UNUSED, double dx, double dy,
                gpointer user_data G_GNUC_UNUSED)
{
  if (m_drag_slot < 0 || m_drag_slot >= (int)m_slots.size () ||
      m_window == nullptr)
    return;

  int scale = std::max (gtk_widget_get_scale_factor (m_window), 1);
  m_drag_cur_px = m_drag_start_px + (int)(dx * scale);
  m_drag_cur_py = m_drag_start_py + (int)(dy * scale);

  if (!m_drag_active) {
    if (dx * dx + dy * dy < 36.0)
      return;
    m_drag_active = true;
    // Claim the gesture sequence so GtkGestureClick (on_click_released, which
    // runs before on_drag_end on button release) does not misinterpret the
    // drop release over a peeking workspace as a workspace-switch click.
    gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED);
    set_hover (-1, true);
    promote_slot_thumb_to_top (m_slots[m_drag_slot]);
    m_drag_scale_start_us = g_get_monotonic_time ();
    if (m_drag_tick_source == 0) {
      m_drag_tick_source = gtk_widget_add_tick_callback (
          m_window, drag_scale_tick_cb, this, nullptr);
    }
  }

  if (m_drag_tick_source == 0)
    update_dragged_slot_transform (g_get_monotonic_time ());

  POINT screen_pt{};
  HMONITOR cur_mon = nullptr;
  if (GetCursorPos (&screen_pt))
    cur_mon = MonitorFromPoint (screen_pt, MONITOR_DEFAULTTONULL);

  if (cur_mon != nullptr && cur_mon != resolve_monitor ()) {
    if (m_thumbs != nullptr)
      overview_thumbs_set_drop_target (m_thumbs, -1);
    if (m_drag_drop_ws_peek != -1) {
      m_drag_drop_ws_peek = -1;
      if (m_ws_bg != nullptr)
        gtk_widget_queue_draw (m_ws_bg);
    }
    set_external_drop_highlight_all (screen_pt, this);
    return;
  }

  clear_external_drop_highlight_all (this);

  int thumb_pill = -1;
  int peek_ws = -1;
  resolve_drag_drop_workspace (m_drag_cur_px, m_drag_cur_py, scale, &thumb_pill,
                               &peek_ws);
  if (m_thumbs != nullptr)
    overview_thumbs_set_drop_target (m_thumbs, thumb_pill);
  if (m_drag_drop_ws_peek != peek_ws) {
    m_drag_drop_ws_peek = peek_ws;
    if (m_ws_bg != nullptr)
      gtk_widget_queue_draw (m_ws_bg);
  }
}

void
on_drag_end (GtkGestureDrag *drag G_GNUC_UNUSED, double dx, double dy,
             gpointer user_data G_GNUC_UNUSED)
{
  if (m_drag_slot < 0)
    return;

  if (m_drag_tick_source != 0 && m_window != nullptr) {
    gtk_widget_remove_tick_callback (m_window, m_drag_tick_source);
    m_drag_tick_source = 0;
  }

  const int dragged_slot = m_drag_slot;
  const bool was_drag = m_drag_active;
  const HWND target_hwnd = m_drag_hwnd;
  const int scale =
      m_window != nullptr ? std::max (gtk_widget_get_scale_factor (m_window), 1)
                          : 1;
  const int end_px = m_drag_start_px + (int)(dx * scale);
  const int end_py = m_drag_start_py + (int)(dy * scale);

  m_drag_active = false;
  m_drag_slot = -1;
  m_drag_hwnd = nullptr;

  if (m_drag_drop_ws_peek != -1) {
    m_drag_drop_ws_peek = -1;
    if (m_ws_bg != nullptr)
      gtk_widget_queue_draw (m_ws_bg);
  }
  if (m_thumbs != nullptr)
    overview_thumbs_set_drop_target (m_thumbs, -1);
  clear_external_drop_highlight_all (this);

  if (!was_drag)
    return;

  m_drag_just_ended = true;

  bool moved = false;
  POINT screen_pt{};
  const bool have_cursor = GetCursorPos (&screen_pt) != FALSE;
  const HMONITOR src_mon = resolve_monitor ();
  const HMONITOR drop_mon =
      have_cursor ? MonitorFromPoint (screen_pt, MONITOR_DEFAULTTONULL)
                  : nullptr;

  if (drop_mon != nullptr && drop_mon != src_mon &&
      target_hwnd != nullptr && IsWindow (target_hwnd)) {
    g_suppress_ws_change_rebuild = true;
    HMONITOR dst_mon = nullptr;
    int dst_ws = -1;
    bool dst_create = false;
    if (hit_test_any_open_view (screen_pt, this, &dst_mon, &dst_ws, &dst_create)) {
      if (dst_create && ws::create_workspace (dst_mon)) {
        const int n = ws::count_for_monitor (dst_mon);
        if (n > 0) dst_ws = n - 1;
      }
      moved = ws::transfer_window_to_monitor_workspace (
          target_hwnd, dst_mon, dst_ws, screen_pt, false);
    } else {
      // Dropped onto a monitor whose Overview is CLOSED -> transfer directly
      // onto that monitor's live active workspace at the drop cursor point.
      moved = ws::transfer_window_to_monitor_workspace (
          target_hwnd, drop_mon, ws::active_for_monitor (drop_mon),
          screen_pt, true);
    }
    g_suppress_ws_change_rebuild = false;
  } else {
    int thumb_pill = -1;
    int peek_ws = -1;
    const int tgt = resolve_drag_drop_workspace (end_px, end_py, scale,
                                                 &thumb_pill, &peek_ws);
    if (tgt >= 0 && target_hwnd != nullptr && IsWindow (target_hwnd)) {
      if (tgt < (int)m_desktops.size () && tgt != m_desktop_active) {
        moved = ws::move_window_to_workspace (target_hwnd, src_mon, tgt);
      } else if (tgt == (int)m_desktops.size () && ws::create_workspace (src_mon)) {
        int n = ws::count_for_monitor (src_mon);
        if (n > 0)
          moved = ws::move_window_to_workspace (target_hwnd, src_mon, n - 1);
      }
    }
  }

  if (moved) {
    reset_drag_state ();
    overview_refresh_all_open ();
    // Explorer may finish committing the moved window's desktop GUID a few
    // dozen ms asynchronously; run a lightweight follow-up dash refresh so
    // workspace-isolated mode never leaves stale icons behind.
    schedule_dash_resync (120);
    return;
  }

  // dnd.js _cancelDrag: smoothly animate the dragged preview back to its slot
  // over SNAP_BACK_ANIMATION_TIME (250ms EASE_OUT_QUAD) instead of rebuilding.
  if (dragged_slot >= 0 &&
      dragged_slot < static_cast<int> (m_slots.size ()) &&
      m_window != nullptr) {
    m_drag_snapback_slot = dragged_slot;
    m_drag_snapback_from = m_slots[static_cast<size_t> (dragged_slot)].current;
    m_drag_snapback_from_op =
        m_slots[static_cast<size_t> (dragged_slot)].current_opacity;
    m_drag_snapback_start_us = g_get_monotonic_time ();
    m_drag_snapback_source = gtk_widget_add_tick_callback (
        m_window, drag_snapback_tick_cb, this, nullptr);
  }
}


void
acquire_overview_foreground (HWND hwnd)
{
  if (hwnd == nullptr || !IsWindow (hwnd))
    return;
  apply_ex_styles (hwnd);
  if (GetForegroundWindow () != hwnd && !SetForegroundWindow (hwnd)) {
    // Unlock Windows foreground activation across AppContainer/UWP threads
    // (such as StartMenuExperienceHost.exe) where AttachThreadInput fails,
    // which also automatically light-dismisses an open native Start menu.
    INPUT unlock_input = {};
    unlock_input.type = INPUT_KEYBOARD;
    unlock_input.ki.wVk = 0xFF;
    unlock_input.ki.dwExtraInfo = 0x574E4F31;  // kSyntheticWinExtra
    unlock_input.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput (1, &unlock_input, sizeof (INPUT));
    if (!SetForegroundWindow (hwnd)) {
      HWND fg = GetForegroundWindow ();
      if (fg != nullptr && IsWindow (fg)) {
        DWORD fg_tid = GetWindowThreadProcessId (fg, nullptr);
        DWORD cur_tid = GetCurrentThreadId ();
        if (fg_tid != 0 && fg_tid != cur_tid &&
            AttachThreadInput (cur_tid, fg_tid, TRUE)) {
          BringWindowToTop (hwnd);
          SetForegroundWindow (hwnd);
          SetActiveWindow (hwnd);
          AttachThreadInput (cur_tid, fg_tid, FALSE);
        }
      }
      if (GetForegroundWindow () != hwnd) {
        INPUT pair[2] = {};
        pair[0].type = INPUT_KEYBOARD;
        pair[0].ki.wVk = 0xFF;
        pair[0].ki.dwExtraInfo = 0x574E4F31;
        pair[1].type = INPUT_KEYBOARD;
        pair[1].ki.wVk = 0xFF;
        pair[1].ki.dwExtraInfo = 0x574E4F31;
        pair[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput (2, pair, sizeof (INPUT));
        AllowSetForegroundWindow (ASFW_ANY);
        BringWindowToTop (hwnd);
        SetForegroundWindow (hwnd);
      }
      if (GetForegroundWindow () != hwnd) {
        SwitchToThisWindow (hwnd, TRUE);
        if (GetForegroundWindow () != hwnd &&
            is_shell_core_window (GetForegroundWindow ())) {
          // Win11 AppContainer CoreWindow (Start/Search/QuickSettings/Input)
          // sometimes ignores VK=0xFF foreground unlock; a synthetic VK_MENU
          // pulse releases the foreground lock without triggering any Win32
          // menu bar on a CoreWindow.
          INPUT alt_pair[2] = {};
          alt_pair[0].type = INPUT_KEYBOARD;
          alt_pair[0].ki.wVk = VK_MENU;
          alt_pair[0].ki.dwExtraInfo = 0x574E4F31;
          alt_pair[1].type = INPUT_KEYBOARD;
          alt_pair[1].ki.wVk = VK_MENU;
          alt_pair[1].ki.dwExtraInfo = 0x574E4F31;
          alt_pair[1].ki.dwFlags = KEYEVENTF_KEYUP;
          SendInput (2, alt_pair, sizeof (INPUT));
          AllowSetForegroundWindow (ASFW_ANY);
          BringWindowToTop (hwnd);
          SetForegroundWindow (hwnd);
        }
      }
    }
  }
  if (GetForegroundWindow () == hwnd && GetFocus () != hwnd)
    SetFocus (hwnd);
  if (GetForegroundWindow () != hwnd) {
    HWND cur_fg = GetForegroundWindow ();
    g_print ("[ov-focus] acquire_foreground failed target=%p fg=%p(%s)\n",
             static_cast<void *> (hwnd), static_cast<void *> (cur_fg),
             hwnd_class_utf8_diag (cur_fg).c_str ());
  }
  restack_locked ();
}

GtkWidget *
search_focus_target (void)
{
  if (m_search == nullptr)
    return nullptr;
  GtkWidget *delegate =
      GTK_WIDGET (gtk_editable_get_delegate (GTK_EDITABLE (m_search)));
  return delegate != nullptr ? delegate : m_search;
}

bool
is_search_focused (void)
{
  if (m_window == nullptr || m_search == nullptr)
    return false;
  GtkWidget *focus = gtk_window_get_focus (GTK_WINDOW (m_window));
  GtkWidget *target = search_focus_target ();
  return focus != nullptr &&
         (focus == target || focus == m_search ||
          gtk_widget_is_ancestor (focus, m_search));
}

void
set_search_dormant (bool dormant)
{
  if (m_search == nullptr)
    return;
  const bool keep_dormant =
      dormant && m_search_raw_text.empty () && !m_search_preedit_active;
  if (keep_dormant)
    gtk_widget_add_css_class (m_search, "search-dormant");
  else
    gtk_widget_remove_css_class (m_search, "search-dormant");
}

void
ensure_search_entry_focus (void)
{
  if (m_window == nullptr || m_search == nullptr)
    return;
  if (!is_search_focused ()) {
    gboolean ok = gtk_entry_grab_focus_without_selecting (GTK_ENTRY (m_search));
    if (!is_search_focused ()) {
      GtkWidget *cur = gtk_window_get_focus (GTK_WINDOW (m_window));
      g_print ("[ov-focus] ensure_search_entry_focus failed ok=%d focus=%p\n",
               ok ? 1 : 0, static_cast<void *> (cur));
    }
  }
}

bool
has_visible_overview_popover (void)
{
  if (m_app_grid != nullptr && app_grid_has_open_menu (m_app_grid))
    return true;
  if (m_dash != nullptr && overview_dash_has_open_menu (m_dash))
    return true;
  return false;
}

bool
can_auto_focus_overview_search (void)
{
  return m_window != nullptr && m_search != nullptr && m_placed &&
         m_target_state != ControlsState::HIDDEN && !m_folder_dialog_open &&
         winome_shell_panel_top_popover_hwnd_for_monitor (resolve_monitor ()) == nullptr &&
         !has_visible_overview_popover ();
}

void
focus_overview_window (bool focus_search)
{
  acquire_overview_foreground (m_hwnd);
  if (m_window == nullptr)
    return;
  // Keep GTK/IME focus armed on m_search's GtkText at all times while overview
  // is open so Windows IME (VK_PROCESSKEY / WM_IME_*) binds before key 1;
  // `.search-dormant` hides the blue focus ring & caret when not active.
  if (m_search != nullptr) {
    set_search_dormant (!focus_search);
    ensure_search_entry_focus ();
  } else if (m_focus_sink != nullptr &&
             gtk_widget_get_focusable (m_focus_sink)) {
    gtk_window_set_focus (GTK_WINDOW (m_window), m_focus_sink);
  } else if (m_root != nullptr) {
    gtk_widget_grab_focus (m_root);
  }
  HWND cur_fg = GetForegroundWindow ();
  g_print ("[ov-focus] focus_overview search=%d fg_ok=%d(%s) active=%d search_focus=%d\n",
           focus_search ? 1 : 0, cur_fg == m_hwnd ? 1 : 0,
           hwnd_class_utf8_diag (cur_fg).c_str (),
           gtk_window_is_active (GTK_WINDOW (m_window)) ? 1 : 0,
           is_search_focused () ? 1 : 0);
}

void
cancel_focus_verify (void)
{
  if (m_focus_verify_timer != 0) {
    g_source_remove (m_focus_verify_timer);
    m_focus_verify_timer = 0;
  }
}

gboolean
focus_verify_cb (gpointer user_data G_GNUC_UNUSED)
{
  m_focus_verify_timer = 0;
  if (!can_auto_focus_overview_search ())
    return G_SOURCE_REMOVE;
  HWND fg = GetForegroundWindow ();
  if (fg != m_hwnd &&
      (is_overview_chrome_or_panel_hwnd (fg) || fg == nullptr ||
       fg == g_prev_foreground_hwnd || is_shell_core_window (fg))) {
    acquire_overview_foreground (m_hwnd);
  }
  if (GetForegroundWindow () == m_hwnd)
    ensure_search_entry_focus ();
  HWND after_fg = GetForegroundWindow ();
  g_print ("[ov-focus] verify fg_ok=%d(%s) active=%d search_focus=%d dormant=%d\n",
           after_fg == m_hwnd ? 1 : 0,
           hwnd_class_utf8_diag (after_fg).c_str (),
           gtk_window_is_active (GTK_WINDOW (m_window)) ? 1 : 0,
           is_search_focused () ? 1 : 0,
           (m_search != nullptr &&
            gtk_widget_has_css_class (m_search, "search-dormant"))
               ? 1
               : 0);
  return G_SOURCE_REMOVE;
}

void
schedule_focus_verify (void)
{
  if (m_focus_verify_timer == 0) {
    m_focus_verify_timer = g_idle_add_full (G_PRIORITY_DEFAULT_IDLE,
                                            focus_verify_static_cb, this, nullptr);
  }
}

void
recheck_overview_search_focus_on_settle (const char *tag)
{
  if (!can_auto_focus_overview_search ())
    return;
  HWND fg = GetForegroundWindow ();
  if (is_overview_chrome_or_panel_hwnd (fg))
    acquire_overview_foreground (m_hwnd);
  if (GetForegroundWindow () == m_hwnd)
    ensure_search_entry_focus ();
  HWND after_fg = GetForegroundWindow ();
  g_print ("[ov-focus] settle(%s) fg_ok=%d(%s) active=%d search_focus=%d dormant=%d\n",
           tag != nullptr ? tag : "?", after_fg == m_hwnd ? 1 : 0,
           hwnd_class_utf8_diag (after_fg).c_str (),
           gtk_window_is_active (GTK_WINDOW (m_window)) ? 1 : 0,
           is_search_focused () ? 1 : 0,
           (m_search != nullptr &&
            gtk_widget_has_css_class (m_search, "search-dormant"))
               ? 1
               : 0);
}

void
focus_overview_for_search (void)
{
  focus_overview_window (true);
}

void
on_click_released (GtkGestureClick *gesture, int n_press, double x, double y,
                   gpointer user_data)
{
  (void)n_press;
  (void)user_data;
  guint button =
      gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));

  if (m_drag_active) {
    return;
  }
  if (m_drag_just_ended) {
    m_drag_just_ended = false;
    return;
  }
  if (button != GDK_BUTTON_PRIMARY && button != GDK_BUTTON_MIDDLE)
    return;

  int scale = m_window != nullptr ? gtk_widget_get_scale_factor (m_window) : 1;
  int px = (int)(x * scale);
  int py = (int)(y * scale);

  // A click that just dismissed a panel popover floating above the overview
  // is consumed by GNOME's menu grab: close the menu only.
  if (m_target_state == ControlsState::HIDDEN || !overview_active ())
    return;

  if (winome_shell_panel_popover_just_closed ()) {
    g_print ("[ov] click (%d,%d) swallowed (popover dismiss)\n", px, py);
    return;
  }

  if (py < 0) {
    g_print ("[ov] click (%d,%d) ignored (panel strip)\n", px, py);
    restack_locked ();
    return;
  }

  // Clicking outside the search entry when no search query is active unfocuses
  // the search box so its focus ring clears cleanly.
  const bool in_search_rect =
      (px >= m_search_rect.left && px < m_search_rect.right &&
       py >= m_search_rect.top && py < m_search_rect.bottom);
  if (!in_search_rect && m_search_query.empty () && !m_search_preedit_active) {
    set_search_dormant (true);
    ensure_search_entry_focus ();
  }

  if (m_search_mode) {
    if (in_search_rect)
      focus_overview_for_search ();
    restack_locked ();
    return;
  }

  const int active_ws = std::max (m_desktop_active, 0);

  if (m_app_grid_mode) {
    if (m_folder_dialog_open && m_app_grid != nullptr) {
      app_grid_close_open_folder (m_app_grid);
      restack_locked ();
      return;
    }
    POINT pt = {px, py};
    for (int d = 0; d < static_cast<int> (m_ws_cur_boxes.size ()); ++d) {
      const RECT &b = m_ws_cur_boxes[static_cast<size_t> (d)];
      if (pt.x >= b.left && pt.x < b.right && pt.y >= b.top && pt.y < b.bottom) {
        if (d != active_ws)
          switch_workspace_from_user (d);
        g_print ("[ov] click (%d,%d) small workspace %d -> window picker\n", px,
                 py, d);
        toggle_app_grid ();
        return;
      }
    }
    if (pt.x >= m_ws_box.left && pt.x < m_ws_box.right &&
        pt.y >= m_ws_box.top && pt.y < m_ws_box.bottom) {
      g_print ("[ov] click (%d,%d) small workspace -> window picker\n", px,
               py);
      toggle_app_grid ();
      return;
    }
    restack_locked ();
    return;
  }

  POINT pt = {px, py};

  // Clicking on an inactive peeking workspace (or a window preview inside an
  // inactive peeking workspace) switches to that workspace while keeping the
  // overview open (workspace.js _init clickGesture + _onCloneSelected).
  for (int d = 0; d < static_cast<int> (m_ws_cur_boxes.size ()); ++d) {
    if (d == active_ws)
      continue;
    const RECT &b = m_ws_cur_boxes[static_cast<size_t> (d)];
    if (b.right <= 0 || b.left >= m_monitor_w)
      continue;
    if (pt.x >= b.left && pt.x < b.right && pt.y >= b.top && pt.y < b.bottom) {
      if (button == GDK_BUTTON_PRIMARY) {
        g_print ("[ov] click (%d,%d) peeking workspace %d -> switch\n", px, py,
                 d);
        switch_workspace_from_user (d);
      } else if (button == GDK_BUTTON_MIDDLE) {
        for (const SlotEntry &e : m_slots) {
          if (e.desktop_index != d)
            continue;
          if (pt.x >= e.rect.left && pt.x < e.rect.right &&
              pt.y >= e.rect.top && pt.y < e.rect.bottom) {
            if (IsWindow (e.target)) {
              PostMessageW (e.target, WM_CLOSE, 0, 0);
              schedule_delayed_rebuild (140);
            }
            break;
          }
        }
      }
      return;
    }
  }

  if (button == GDK_BUTTON_MIDDLE) {
    for (const SlotEntry &e : m_slots) {
      if (e.desktop_index != active_ws)
        continue;
      if (pt.x >= e.rect.left && pt.x < e.rect.right && pt.y >= e.rect.top &&
          pt.y < e.rect.bottom) {
        if (IsWindow (e.target)) {
          PostMessageW (e.target, WM_CLOSE, 0, 0);
          schedule_delayed_rebuild (140);
        }
        return;
      }
    }
    return;
  }

  // Inside a preview slot on the active workspace: restore/focus that window
  // and close.
  for (int i = 0; i < (int)m_slots.size (); ++i) {
    if (m_slots[i].desktop_index != active_ws)
      continue;
    const RECT r = slot_display_rect (i);
    if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top &&
        pt.y < r.bottom) {
      g_print ("[ov] click (%d,%d) hit slot\n", px, py);
      activate_slot_and_close (i, "ov-click-slot");
      return;
    }
  }

  // Dash band and search row handle their own input.
  if (py >= m_dash_top) {
    g_print ("[ov] click (%d,%d) ignored (dash band)\n", px, py);
    restack_locked ();
    return;
  }
  if (pt.x >= m_search_rect.left && pt.x < m_search_rect.right &&
      pt.y >= m_search_rect.top && pt.y < m_search_rect.bottom) {
    g_print ("[ov] click (%d,%d) focus search entry\n", px, py);
    focus_overview_for_search ();
    return;
  }
  // The workspace thumbnails row handles its own input (clicking a pill
  // switches desktop; the overview stays open, exactly like upstream).
  if (m_thumbs_rect.right > m_thumbs_rect.left &&
      pt.x >= m_thumbs_rect.left && pt.x < m_thumbs_rect.right &&
      pt.y >= m_thumbs_rect.top && pt.y < m_thumbs_rect.bottom) {
    g_print ("[ov] click (%d,%d) ignored (thumbs row)\n", px, py);
    restack_locked ();
    return;
  }

  // Every other region is excluded: only clicks INSIDE the workspace box
  // (the desktop frame) count as a "blank" click that dismisses the
  // overview. The area outside it — the sides, the band between the search
  // entry and the workspace box, above the dash — does nothing, so stray
  // clicks can never close the overview by accident (GNOME keeps those
  // regions inert; the cover pane handles the dismissal semantics).
  if (pt.x >= m_ws_box.left && pt.x < m_ws_box.right &&
      pt.y >= m_ws_box.top && pt.y < m_ws_box.bottom) {
    g_print ("[ov] click (%d,%d) blank inside workspace -> close\n", px, py);
    dismiss ("ov-click-blank");
    return;
  }

  g_print ("[ov] click (%d,%d) ignored (outside workspace)\n", px, py);
  restack_locked ();
}

void
on_overview_surface_layout (GdkSurface *surface G_GNUC_UNUSED,
                            int width,
                            int height,
                            gpointer user_data G_GNUC_UNUSED)
{
  if (m_window == nullptr || !gtk_widget_get_visible (m_window))
    return;
  if (m_anim.source != 0 || m_switch_source != 0)
    return;
  const bool size_changed =
      (width != m_last_layout_w || height != m_last_layout_h);
  m_last_layout_w = width;
  m_last_layout_h = height;
  if (!m_placed || size_changed)
    restack_locked ();
  if (m_placed && m_target_state != ControlsState::HIDDEN) {
    int scale = std::max (gtk_widget_get_scale_factor (m_window), 1);
    int cur_search_h = m_placed_search_h;
    if (m_search != nullptr && gtk_widget_get_visible (m_search)) {
      GtkRequisition nat{};
      gtk_widget_get_preferred_size (m_search, nullptr, &nat);
      if (nat.height > 0)
        cur_search_h = nat.height;
    }
    if (scale != m_placed_scale || cur_search_h != m_placed_search_h)
      reposition_view ();
  }
}

void
on_realize (GtkWidget *widget, gpointer user_data)
{
  (void)user_data;
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (widget));
  if (surface != nullptr) {
    HWND hwnd = static_cast<HWND> (gdk_win32_surface_get_handle (surface));
    m_hwnd = hwnd;
    apply_ex_styles (hwnd);
    winome::vd::pin_window_async (hwnd);
    if (!m_overview_surface_hooked) {
      m_overview_surface_hooked = true;
      g_signal_connect (surface, "layout",
                        G_CALLBACK (on_overview_surface_layout_cb), this);
    }
    ensure_after_paint_hook (gdk_surface_get_frame_clock (surface));
  }
}

void
on_overview_map (GtkWidget *widget, gpointer user_data)
{
  (void)widget;
  (void)user_data;
  if (g_idle_trim_timer != 0) {
    g_source_remove (g_idle_trim_timer);
    g_idle_trim_timer = 0;
  }
  // G_PRIORITY_HIGH_IDLE (100) sorts BEFORE the frame clock's repaint source
  // (G_PRIORITY_REDRAW 120): the placement runs before the first post-map
  // paint, so the first frame is the placed overview, not a dark flash.
  cancel_pending_place_idles ();
  m_place_opening_source = g_idle_add_full (
      G_PRIORITY_HIGH_IDLE, overview_place_opening_idle_cb, this, nullptr);
}

// ThumbnailsBox click action -> Workspace.activate: switch the desktop and
// relayout (the overview stays open, exactly like upstream).
void
on_thumb_clicked (int index, void *user_data)
{
  (void)user_data;
  g_print ("[ov] thumb click -> desktop %d (current %d)\n", index,
           m_desktop_active);
  if (index < 0 || index == m_desktop_active)
    return;
  m_last_ws_scroll_us = g_get_monotonic_time ();
  if (!switch_workspace_from_user (index)) {
    g_print ("[ov] desktop switch failed\n");
  }
}

// WorkspacesDisplay._onScrollEvent + WindowManager.handleWorkspaceScroll:
// Scroll wheel anywhere on the Overview workspace area switches to the
// previous/next workspace (throttled by SCROLL_TIMEOUT_TIME = 150ms).
gboolean
on_overview_scroll (GtkEventControllerScroll *ctrl G_GNUC_UNUSED, double dx,
                    double dy, gpointer user_data G_GNUC_UNUSED)
{
  if (m_window == nullptr || !gtk_widget_get_visible (m_window) ||
      !m_placed || m_target_state == ControlsState::HIDDEN ||
      !overview_active ()) {
    return GDK_EVENT_PROPAGATE;
  }
  if (m_drag_active || m_drag_slot >= 0 || m_drag_snapback_source != 0 ||
      m_search_mode || m_folder_dialog_open)
    return GDK_EVENT_PROPAGATE;

  // In APP_GRID mode, let the app grid's own scroll controller handle wheel
  // scrolling when the pointer is over the app grid area; scrolling above the
  // app grid (e.g. over the top miniature workspaces row) switches workspaces.
  if (m_app_grid_mode || m_target_state == ControlsState::APP_GRID) {
    if (m_hwnd != nullptr) {
      int scale = std::max (gtk_widget_get_scale_factor (m_window), 1);
      POINT pt{};
      if (GetCursorPos (&pt) && ScreenToClient (m_hwnd, &pt)) {
        if (pt.y >= m_grid_y * scale)
          return GDK_EVENT_PROPAGATE;
      }
    }
  }

  if (m_desktops.size () <= 1 || m_desktop_active < 0)
    return GDK_EVENT_PROPAGATE;

  double delta = (std::fabs (dy) >= std::fabs (dx)) ? dy : dx;
  if (std::fabs (delta) < 1e-4)
    return GDK_EVENT_PROPAGATE;

  const gint64 now_us = g_get_monotonic_time ();
  if (now_us - m_last_ws_scroll_us <
      static_cast<gint64> (kWorkspaceScrollTimeoutMs) * 1000) {
    return GDK_EVENT_STOP;
  }

  const int step = (delta > 0.0) ? 1 : -1;
  const int target = m_desktop_active + step;
  if (target < 0 || target >= static_cast<int> (m_desktops.size ()))
    return GDK_EVENT_STOP;

  m_last_ws_scroll_us = now_us;
  switch_workspace_from_user (target);
  return GDK_EVENT_STOP;
}

void
on_thumb_add_desktop (void *user_data G_GNUC_UNUSED)
{
  HMONITOR mon = resolve_monitor ();
  const OverviewMonitorRole role = resolve_monitor_overview_role (
      ws::effective_mode (), monitor_manager_is_shell_primary (mon));
  if (!role.multi_workspace)
    return;
  if (ws::create_workspace (mon)) {
    int n = ws::count_for_monitor (mon);
    if (n > 0)
      ws::switch_to (mon, n - 1);
    if (ws::effective_mode () == ws::MultiMonitorWorkspaceMode::Span)
      overview_refresh_all_open ();
    else
      rebuild_now ();
  }
}

void
on_thumb_remove_desktop (int index, void *user_data G_GNUC_UNUSED)
{
  HMONITOR mon = resolve_monitor ();
  const OverviewMonitorRole role = resolve_monitor_overview_role (
      ws::effective_mode (), monitor_manager_is_shell_primary (mon));
  if (!role.multi_workspace)
    return;
  if (ws::remove_workspace (mon, index)) {
    if (ws::effective_mode () == ws::MultiMonitorWorkspaceMode::Span)
      overview_refresh_all_open ();
    else
      rebuild_now ();
  }
}

void
on_dash_show_apps_clicked (void *user_data G_GNUC_UNUSED)
{
  toggle_app_grid ();
}

void
on_dash_changed (void *user_data G_GNUC_UNUSED)
{
  rebuild_now ();
  refresh_other_views_dash (this);
}

void
on_app_grid_launch_done (void *user_data G_GNUC_UNUSED)
{
  overview_discard_restore_focus ();
  dismiss ("app-grid-launch");
}

void
on_app_grid_pins_changed (void *user_data G_GNUC_UNUSED)
{
  if (m_dash != nullptr && m_window != nullptr &&
      gtk_widget_get_visible (m_window)) {
    if (m_target_state == ControlsState::APP_GRID && g_panel_hwnd != nullptr) {
      refresh_dash_in_place ();
      refresh_other_views_dash (this);
      if (m_app_grid != nullptr)
        app_grid_refresh_running (m_app_grid);
      return;
    }
    rebuild_now ();
  }
}


// Relayout without animation (window closed via the chrome button etc.).
void
rebuild_now (const SharedOpenSnapshot *shared_snap = nullptr)
{
  if (m_window == nullptr || !gtk_widget_get_visible (m_window))
    return;
  if (m_target_state == ControlsState::HIDDEN)
    return;
  place_overview_locked (false, 0, shared_snap);
}

void
apply_pending_desktop_sync_if_any (void)
{
  if (!m_pending_desktop_sync)
    return;
  int target_ws = m_pending_desktop_target;
  m_pending_desktop_sync = false;
  m_pending_desktop_target = -1;
  if (m_window == nullptr || !gtk_widget_get_visible (m_window) ||
      !m_placed || m_target_state == ControlsState::HIDDEN ||
      !overview_active ()) {
    return;
  }
  HMONITOR mon = resolve_monitor ();
  int cur_idx = ws::active_for_monitor (mon);
  int cur_count = ws::count_for_monitor (mon);
  int desired = (cur_idx >= 0) ? cur_idx : target_ws;
  ensure_overview_visible_on_active_desktop (desired);
  schedule_vd_cloak_guard (60);
  if (desired >= 0 && desired != m_desktop_active)
    switch_workspace_animated (desired);
  else if (cur_count > 0 && cur_count != (int)m_desktops.size ())
    rebuild_now ();
}


// --- Dedicated GNOME Search Results View (#searchResults) --------------------
// Backed by overview-search.cpp (ported from GNOME Shell search.js,
// searchController.js, appDisplay.js, misc/systemActions.js, misc/util.js)
// with smooth 200ms cross-fade (overviewControls.js:623-643 _onSearchChanged).

void
on_search_view_close_overview (const char *reason, bool discard_focus,
                               void *user_data G_GNUC_UNUSED)
{
  if (discard_focus)
    overview_discard_restore_focus ();
  dismiss (reason != nullptr ? reason : "search-activate");
}

void
on_search_view_activate_window (HWND hwnd, int desktop_index,
                                void *user_data G_GNUC_UNUSED)
{
  overview_discard_restore_focus ();
  int target_ws = desktop_index;
  if (target_ws < 0 && hwnd != nullptr) {
    for (const SlotEntry &s : m_slots) {
      if (s.target == hwnd) {
        target_ws = s.desktop_index;
        break;
      }
    }
  }
  if (target_ws >= 0 && target_ws != m_desktop_active)
    ws::switch_to (resolve_monitor (), target_ws);
  if (hwnd != nullptr && IsWindow (hwnd)) {
    if (IsIconic (hwnd))
      ShowWindow (hwnd, SW_RESTORE);
    SetForegroundWindow (hwnd);
  }
  dismiss ("search-result-window");
}

std::vector<SearchWindowCandidate>
on_search_view_get_open_windows (void *user_data G_GNUC_UNUSED)
{
  std::vector<SearchWindowCandidate> out;
  out.reserve (m_slots.size ());
  for (const SlotEntry &s : m_slots) {
    if (s.target == nullptr || !IsWindow (s.target))
      continue;
    SearchWindowCandidate wc;
    wc.hwnd = s.target;
    wc.desktop_index = s.desktop_index;
    wc.title = hwnd_title_utf8 (s.target);
    wc.process_name = hwnd_process_name_utf8 (s.target);
    out.push_back (std::move (wc));
  }
  return out;
}

void
on_search_view_open_settings (const char *page, void *user_data G_GNUC_UNUSED)
{
  if (page != nullptr && page[0] != '\0')
    open_settings_window_page (page);
  else
    open_settings_window ();
}

void
cancel_search_fade (void)
{
  if (m_search_fade_source != 0 && m_window != nullptr) {
    gtk_widget_remove_tick_callback (m_window, m_search_fade_source);
    m_search_fade_source = 0;
  }
}

void
apply_search_fade_progress (double p)
{
  p = std::clamp (p, 0.0, 1.0);
  m_search_fade_cur = p;
  const double inv = 1.0 - p;

  if (m_search_view != nullptr) {
    gtk_widget_set_opacity (m_search_view, p);
    gtk_widget_set_visible (m_search_view, p > 0.005 ? TRUE : FALSE);
  }
  if (m_ws_bg != nullptr) {
    double bg_base = m_folder_dialog_open ? 0.30 : 1.0;
    gtk_widget_set_opacity (m_ws_bg, inv * bg_base);
    gtk_widget_set_visible (m_ws_bg, inv > 0.005 ? TRUE : FALSE);
  }
  const bool want_thumbs = !m_app_grid_mode && m_thumbs_has_pills;
  if (m_thumbs != nullptr) {
    gtk_widget_set_opacity (m_thumbs, want_thumbs ? inv : 0.0);
    gtk_widget_set_visible (m_thumbs,
                            (want_thumbs && inv > 0.005) ? TRUE : FALSE);
  }
  set_mini_opacity (
      want_thumbs ? static_cast<BYTE> (std::lround (inv * 255.0)) : 0);

  if (m_app_grid_clip != nullptr) {
    if (m_app_grid_mode) {
      gtk_widget_set_opacity (m_app_grid_clip, inv);
      sync_app_grid_visible (inv > 0.005);
    } else {
      gtk_widget_set_opacity (m_app_grid_clip, 1.0);
      sync_app_grid_visible (false);
    }
  }

  for (SlotEntry &s : m_slots) {
    s.rect = m_app_grid_mode ? s.grid_rect : s.picker_rect;
    int base_op = m_app_grid_mode ? static_cast<int> (s.grid_opacity) : 255;
    if (m_app_grid_mode && m_folder_dialog_open && base_op > 0)
      base_op = 76;
    BYTE op = static_cast<BYTE> (
        std::clamp<long> (std::lround (base_op * inv), 0, 255));
    apply_thumb_rect (s, s.rect, op, false);
  }
}

gboolean
search_fade_tick (GtkWidget *widget G_GNUC_UNUSED,
                  GdkFrameClock *clock,
                  gpointer user_data G_GNUC_UNUSED)
{
  const gint64 now = clock != nullptr ? gdk_frame_clock_get_frame_time (clock)
                                      : g_get_monotonic_time ();
  const double elapsed_ms =
      static_cast<double> (now - m_search_fade_start_us) / 1000.0;
  const double raw_t =
      std::clamp (elapsed_ms / static_cast<double> (kSearchFadeMs), 0.0, 1.0);
  const double eased = ease_out_quad (raw_t);
  const double cur =
      m_search_fade_from + (m_search_fade_to - m_search_fade_from) * eased;
  apply_search_fade_progress (cur);

  if (raw_t >= 1.0) {
    m_search_fade_source = 0;
    if (m_search_fade_to <= 0.001) {
      if (m_search_view != nullptr)
        overview_search_view_reset (m_search_view);
      if (m_app_grid_clip != nullptr)
        gtk_widget_set_opacity (m_app_grid_clip, 1.0);
      if (!m_app_grid_mode && !m_search_mode)
        set_hover (slot_at_cursor ());
    }
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

void
start_search_fade (bool to_search)
{
  const double target = to_search ? 1.0 : 0.0;
  if (m_window == nullptr) {
    apply_search_fade_progress (target);
    return;
  }
  // Never run search cross-fade concurrently with workspace switch or
  // open/app-grid transitions (both drive DWM thumbnail rects & opacities).
  if (m_switch_source != 0)
    switch_finish (false);
  if (m_anim.source != 0 && m_target_state != ControlsState::HIDDEN) {
    gtk_widget_remove_tick_callback (m_window, m_anim.source);
    m_anim.source = 0;
    m_anim.started = false;
    if (!m_ws_anim_to_boxes.empty ())
      apply_ws_boxes (m_ws_anim_to_boxes);
    if (m_target_state == ControlsState::APP_GRID && m_app_grid != nullptr) {
      sync_app_grid_visible (true);
      app_grid_clip_set_transition (m_app_grid_clip, false, 1.0, 0.0, true);
      app_grid_set_transition_opacity (m_app_grid, false, 1.0);
      m_grid_cur_opacity = 1.0;
      m_grid_cur_slide_y = 0.0;
    } else if (m_target_state == ControlsState::WINDOW_PICKER) {
      if (m_app_grid_clip != nullptr) {
        app_grid_clip_set_transition (m_app_grid_clip, false, 0.0,
                                      kGridSlideOffsetPx, true);
        sync_app_grid_visible (false);
      }
      m_grid_cur_opacity = 0.0;
      m_grid_cur_slide_y = kGridSlideOffsetPx;
    }
  }
  if (std::fabs (m_search_fade_cur - target) < 0.005 &&
      m_search_fade_source == 0) {
    apply_search_fade_progress (target);
    return;
  }
  m_search_fade_from = m_search_fade_cur;
  m_search_fade_to = target;
  m_search_fade_start_us = g_get_monotonic_time ();
  if (m_search_fade_source == 0) {
    m_search_fade_source =
        gtk_widget_add_tick_callback (m_window, search_fade_tick_cb, this,
                                      nullptr);
  }
}

bool
is_search_entry_caret_at_end (void)
{
  if (m_search == nullptr)
    return true;
  const char *txt = gtk_editable_get_text (GTK_EDITABLE (m_search));
  if (txt == nullptr)
    return true;
  const int pos = gtk_editable_get_position (GTK_EDITABLE (m_search));
  const int len = static_cast<int> (g_utf8_strlen (txt, -1));
  return pos >= len;
}

void
sync_search_mode_ui (void)
{
  const bool want_search = !m_search_query.empty ();
  const bool prev_mode = m_search_mode;
  m_search_mode = want_search;

  if (m_search != nullptr) {
    if (want_search)
      gtk_widget_add_css_class (m_search, "search-active");
    else
      gtk_widget_remove_css_class (m_search, "search-active");
  }

  if (m_window == nullptr || !gtk_widget_get_visible (m_window) || !m_placed ||
      m_target_state == ControlsState::HIDDEN) {
    cancel_search_fade ();
    m_search_fade_cur = 0.0;
    if (m_search_view != nullptr) {
      overview_search_view_reset (m_search_view);
      gtk_widget_set_visible (m_search_view, FALSE);
    }
    return;
  }

  if (want_search) {
    set_hover (-1, true);
    if (g_live_wallpaper_owner == this)
      live_wallpaper_set_paused (true);
    if (!prev_mode && m_app_grid != nullptr)
      app_grid_close_open_folder (m_app_grid);
    if (m_search_view != nullptr)
      overview_search_view_set_query (m_search_view, m_search_raw_text);
    start_search_fade (true);
  } else {
    if (g_live_wallpaper_owner == this && m_anim.source == 0 &&
        m_switch_source == 0) {
      live_wallpaper_set_paused (false);
    }
    start_search_fade (false);
  }
}

void
on_app_grid_folder_toggled (bool open, void *user_data G_GNUC_UNUSED)
{
  m_folder_dialog_open = open;
  if (m_search != nullptr) {
    gtk_widget_set_opacity (m_search, open ? 0.30 : 1.0);
    gtk_widget_set_can_target (m_search, open ? FALSE : TRUE);
  }
  if (m_dash != nullptr) {
    gtk_widget_set_opacity (m_dash, open ? 0.30 : 1.0);
    gtk_widget_set_can_target (m_dash, open ? FALSE : TRUE);
  }
  if (m_ws_bg != nullptr && !m_search_mode)
    gtk_widget_set_opacity (m_ws_bg, open ? 0.30 : 1.0);
  if (m_app_grid_mode && !m_search_mode) {
    for (SlotEntry &s : m_slots) {
      BYTE op = (open && s.grid_opacity > 0) ? 76 : s.grid_opacity;
      apply_thumb_rect (s, s.grid_rect, op, true);
    }
  }
  if (!open && overview_active () && m_window != nullptr &&
      gtk_widget_get_visible (m_window) &&
      m_target_state != ControlsState::HIDDEN) {
    focus_overview_window (!m_search_raw_text.empty () || m_search_preedit_active);
    return;
  }
  restack_locked ();
}

void
on_search_changed (GtkEditable *editable, gpointer user_data G_GNUC_UNUSED)
{
  const char *text = gtk_editable_get_text (editable);
  std::string raw = text != nullptr ? text : "";
  if (!raw.empty ())
    set_search_dormant (false);
  std::vector<std::string> terms = split_search_terms (raw);
  std::string norm;
  for (size_t i = 0; i < terms.size (); ++i) {
    if (i > 0)
      norm.push_back (' ');
    norm += terms[i];
  }

  if (raw.empty () && m_search != nullptr) {
    gtk_entry_set_icon_from_icon_name (GTK_ENTRY (m_search),
                                       GTK_ENTRY_ICON_SECONDARY, nullptr);
  } else if (!raw.empty () && m_search != nullptr) {
    gtk_entry_set_icon_from_icon_name (GTK_ENTRY (m_search),
                                       GTK_ENTRY_ICON_SECONDARY,
                                       "edit-clear-symbolic");
  }

  if (raw == m_search_raw_text && norm == m_search_query)
    return;
  m_search_raw_text = std::move (raw);
  m_search_query = std::move (norm);
  sync_search_mode_ui ();
}

void
on_search_icon_press (GtkEntry *entry, GtkEntryIconPosition icon_pos,
                      gpointer user_data G_GNUC_UNUSED)
{
  if (icon_pos == GTK_ENTRY_ICON_SECONDARY)
    gtk_editable_set_text (GTK_EDITABLE (entry), "");
}

void
on_search_activate (GtkEntry *entry G_GNUC_UNUSED,
                    gpointer user_data G_GNUC_UNUSED)
{
  if (m_search_preedit_active)
    return;
  if (m_search_mode) {
    if (m_search_view != nullptr)
      overview_search_view_activate_selected (m_search_view, false);
    return;
  }
  if (m_app_grid_mode && m_app_grid != nullptr) {
    AllowSetForegroundWindow (ASFW_ANY);
    if (app_grid_launch_first (m_app_grid)) {
      overview_discard_restore_focus ();
      dismiss ("app-grid-enter");
    }
    return;
  }
  if (!m_slots.empty ()) {
    overview_discard_restore_focus ();
    HWND target = m_slots[0].target;
    if (IsWindow (target)) {
      if (IsIconic (target))
        ShowWindow (target, SW_RESTORE);
      SetForegroundWindow (target);
    }
    dismiss ("search-enter-window");
    return;
  }
}

bool
handle_overview_nav_key (guint keyval, GdkModifierType state)
{
  if (m_search_mode && m_search_view != nullptr) {
    // Ctrl+Enter launches a new instance of the selected app
    // (upstream search.js SearchEntry activate-new-instance).
    if ((state & GDK_CONTROL_MASK) &&
        (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)) {
      if (overview_search_view_activate_selected (m_search_view, true))
        return true;
    }
    if (overview_search_view_navigate (m_search_view, keyval,
                                       is_search_entry_caret_at_end ())) {
      return true;
    }
  }
  if (m_app_grid_mode && m_app_grid != nullptr && m_search_raw_text.empty () &&
      !m_search_preedit_active &&
      (keyval == GDK_KEY_Home || keyval == GDK_KEY_End)) {
    if (keyval == GDK_KEY_Home)
      app_grid_first_page (m_app_grid);
    else
      app_grid_last_page (m_app_grid);
    return true;
  }
  return false;
}

gboolean
on_overview_key_pressed (GtkEventControllerKey *controller,
                         guint keyval,
                         guint keycode G_GNUC_UNUSED,
                         GdkModifierType state,
                         gpointer user_data G_GNUC_UNUSED)
{
  if (has_visible_overview_popover ())
    return GDK_EVENT_PROPAGATE;
  if (keyval == GDK_KEY_Escape) {
    if (m_search_preedit_active)
      return GDK_EVENT_PROPAGATE;
    if (!m_search_raw_text.empty () || !m_search_query.empty ()) {
      gtk_editable_set_text (GTK_EDITABLE (m_search), "");
      focus_overview_window (false);
      return GDK_EVENT_STOP;
    }
    if (m_app_grid_mode) {
      if (m_app_grid != nullptr && app_grid_close_open_folder (m_app_grid))
        return GDK_EVENT_STOP;
      toggle_app_grid ();
      return GDK_EVENT_STOP;
    }
    dismiss ("escape");
    return GDK_EVENT_STOP;
  }
  if (handle_overview_nav_key (keyval, state))
    return GDK_EVENT_STOP;
  if ((state & GDK_CONTROL_MASK) && (state & GDK_SHIFT_MASK) &&
      (keyval == GDK_KEY_n || keyval == GDK_KEY_N)) {
    on_thumb_add_desktop (nullptr);
    return GDK_EVENT_STOP;
  }
  if (keyval == GDK_KEY_Page_Up || keyval == GDK_KEY_Page_Down) {
    if (m_search_mode || m_search_preedit_active)
      return GDK_EVENT_PROPAGATE;
    if (m_app_grid_mode && m_app_grid != nullptr) {
      if (keyval == GDK_KEY_Page_Up)
        app_grid_prev_page (m_app_grid);
      else
        app_grid_next_page (m_app_grid);
      return GDK_EVENT_STOP;
    }
    if (m_desktops.size () > 1 && m_desktop_active >= 0) {
      int target = m_desktop_active + (keyval == GDK_KEY_Page_Up ? -1 : 1);
      if (target >= 0 && target < (int)m_desktops.size ())
        switch_workspace_from_user (target);
      return GDK_EVENT_STOP;
    }
  }
  if ((state & GDK_CONTROL_MASK) &&
      (keyval == GDK_KEY_f || keyval == GDK_KEY_F)) {
    focus_overview_for_search ();
    return GDK_EVENT_STOP;
  }
  if (m_placed && m_target_state != ControlsState::HIDDEN &&
      m_search != nullptr) {
    const bool has_text = !m_search_raw_text.empty ();
    if (!is_search_focused ()) {
      if (should_overview_capture_search_key (keyval, state, has_text,
                                              m_search_preedit_active)) {
        g_print ("[ov-key] capture_refocus key=0x%x state=%d fg_ok=%d\n",
                 keyval, static_cast<int> (m_target_state),
                 GetForegroundWindow () == m_hwnd ? 1 : 0);
        focus_overview_for_search ();
        GtkWidget *target = search_focus_target ();
        if (controller != nullptr && target != nullptr)
          gtk_event_controller_key_forward (controller, target);
        return GDK_EVENT_STOP;
      }
    } else {
      const bool dormant =
          gtk_widget_has_css_class (m_search, "search-dormant");
      if (should_overview_consume_dormant_key (keyval, state, dormant, has_text,
                                               m_search_preedit_active)) {
        return GDK_EVENT_STOP;
      }
      if (dormant &&
          should_overview_capture_search_key (keyval, state, has_text,
                                              m_search_preedit_active)) {
        g_print ("[ov-key] dormant_unlock key=0x%x state=%d\n", keyval,
                 static_cast<int> (m_target_state));
        set_search_dormant (false);
      }
    }
  }
  return GDK_EVENT_PROPAGATE;
}



  void init_for_monitor (HMONITOR monitor)
  {
    m_monitor = monitor;

    m_window = gtk_window_new ();
    gtk_window_set_title (GTK_WINDOW (m_window), "WINOME Overview");
    gtk_window_set_decorated (GTK_WINDOW (m_window), FALSE);
    gtk_window_set_hide_on_close (GTK_WINDOW (m_window), TRUE);

    MONITORINFO mi{};
    mi.cbSize = sizeof (mi);
    if (monitor != nullptr && GetMonitorInfoW (monitor, &mi)) {
      RECT ov_rc = compute_overview_hwnd_rect (mi.rcMonitor,
                                               resolve_panel_inset ());
      gtk_window_set_default_size (
          GTK_WINDOW (m_window),
          std::max<int> (ov_rc.right - ov_rc.left, 1),
          std::max<int> (ov_rc.bottom - ov_rc.top, 1));
    }

    gtk_widget_add_css_class (m_window, "winome-overview");
    install_bundled_fonts (m_window);

    m_root = gtk_fixed_new ();
    gtk_widget_set_focusable (m_root, TRUE);
    gtk_widget_set_focus_on_click (m_root, FALSE);
    gtk_window_set_child (GTK_WINDOW (m_window), m_root);

    m_focus_sink = gtk_drawing_area_new ();
    gtk_widget_set_size_request (m_focus_sink, 0, 0);
    gtk_widget_set_focusable (m_focus_sink, FALSE);
    gtk_widget_set_can_target (m_focus_sink, FALSE);
    gtk_fixed_put (GTK_FIXED (m_root), m_focus_sink, 0, 0);

    m_search = gtk_entry_new ();
    gtk_widget_add_css_class (m_search, "search-entry");
    gtk_widget_add_css_class (m_search, "search-dormant");
    gtk_entry_set_icon_from_icon_name (GTK_ENTRY (m_search),
                                       GTK_ENTRY_ICON_PRIMARY,
                                       "edit-find-symbolic");
    gtk_entry_set_placeholder_text (
        GTK_ENTRY (m_search),
        winome::tr ("Type to search", "输入以搜索"));
    GtkGesture *search_press = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (search_press), 0);
    gtk_event_controller_set_propagation_phase (
        GTK_EVENT_CONTROLLER (search_press), GTK_PHASE_CAPTURE);
    g_signal_connect (
        search_press, "pressed",
        G_CALLBACK (+[] (GtkGestureClick *g G_GNUC_UNUSED, int n_press G_GNUC_UNUSED,
                         double x G_GNUC_UNUSED, double y G_GNUC_UNUSED,
                         gpointer ud) {
          static_cast<OverviewView *> (ud)->set_search_dormant (false);
        }),
        this);
    gtk_widget_add_controller (m_search, GTK_EVENT_CONTROLLER (search_press));
    if (GtkEditable *delegate = gtk_editable_get_delegate (GTK_EDITABLE (m_search))) {
      g_signal_connect (
          delegate, "preedit-changed",
          G_CALLBACK (+[] (GtkText *t G_GNUC_UNUSED, const char *preedit,
                           gpointer ud) {
            auto *self = static_cast<OverviewView *> (ud);
            const bool active = (preedit != nullptr && preedit[0] != '\0');
            if (active == self->m_search_preedit_active)
              return;
            self->m_search_preedit_active = active;
            if (active) {
              self->set_search_dormant (false);
              if (self->m_folder_dialog_open && self->m_app_grid != nullptr)
                app_grid_close_open_folder (self->m_app_grid);
            }
          }),
          this);
    }
    g_signal_connect (m_search, "changed", G_CALLBACK (on_search_changed_cb), this);
    g_signal_connect (m_search, "icon-press", G_CALLBACK (on_search_icon_press_cb), this);
    g_signal_connect (m_search, "activate", G_CALLBACK (on_search_activate_cb), this);
    gtk_fixed_put (GTK_FIXED (m_root), m_search, 0, 0);

    OverviewSearchCallbacks search_cbs{};
    search_cbs.on_close_overview = on_search_view_close_cb;
    search_cbs.on_activate_window = on_search_view_activate_cb;
    search_cbs.get_open_windows = on_search_view_get_windows_cb;
    search_cbs.on_open_settings = on_search_view_open_settings_cb;
    search_cbs.user_data = this;
    m_search_view = overview_search_view_new (search_cbs);
    gtk_widget_set_opacity (m_search_view, 0.0);
    gtk_fixed_put (GTK_FIXED (m_root), m_search_view, 0, 0);
    gtk_widget_set_visible (m_search_view, FALSE);

    m_app_grid = app_grid_new (on_app_grid_launch_done_cb,
                               on_app_grid_pins_changed_cb, this);
    app_grid_set_folder_toggled_cb (m_app_grid, on_app_grid_folder_toggled_cb, this);
    app_grid_set_menu_closed_cb (
        m_app_grid,
        +[] (void *ud) {
          auto *self = static_cast<OverviewView *> (ud);
          if (self != nullptr && self->is_open ())
            self->restore_focus_if_active ();
        },
        this);
    m_app_grid_clip = app_grid_clip_new (m_app_grid, this);
    gtk_fixed_put (GTK_FIXED (m_root), m_app_grid_clip, 0, 0);
    sync_app_grid_visible (false);

    m_dash = overview_dash_new (m_root);
    overview_dash_set_show_apps_cb (m_dash, on_dash_show_apps_clicked_cb, this);
    overview_dash_set_changed_cb (m_dash, on_dash_changed_cb, this);
    gtk_fixed_put (GTK_FIXED (m_root), m_dash, 0, 0);

    m_thumbs = overview_thumbs_new ();
    overview_thumbs_set_click_cb (m_thumbs, on_thumb_clicked_cb, this);
    overview_thumbs_set_manage_cbs (m_thumbs, on_thumb_add_desktop_cb,
                                    on_thumb_remove_desktop_cb, this);
    gtk_fixed_put (GTK_FIXED (m_root), m_thumbs, 0, 0);
    gtk_widget_set_visible (m_thumbs, FALSE);

    // Z-order matches upstream ControlsManager.add_child():
    // [search, search_view, app_grid_clip, dash, thumbs, ws_bg] so ws_bg
    // always paints ABOVE thumbs during open/close transitions!
    m_ws_bg = (GtkWidget *)g_object_new (winome_workspace_bg_get_type (), nullptr);
    WINOME_WORKSPACE_BG (m_ws_bg)->owner = this;
    gtk_widget_set_can_target (m_ws_bg, FALSE);
    gtk_fixed_put (GTK_FIXED (m_root), m_ws_bg, 0, 0);

    g_signal_connect (m_window, "realize", G_CALLBACK (on_realize_cb), this);
    g_signal_connect (m_window, "map", G_CALLBACK (on_overview_map_cb), this);

    GtkEventController *motion = gtk_event_controller_motion_new ();
    g_signal_connect (motion, "enter", G_CALLBACK (on_root_motion_cb), this);
    g_signal_connect (motion, "motion", G_CALLBACK (on_root_motion_cb), this);
    g_signal_connect (motion, "leave", G_CALLBACK (on_root_motion_leave_cb), this);
    gtk_widget_add_controller (m_root, GTK_EVENT_CONTROLLER (motion));

    g_signal_connect (
        m_window, "notify::is-active",
        G_CALLBACK (+[] (GtkWindow *win, GParamSpec *pspec G_GNUC_UNUSED,
                         gpointer ud) {
          auto *self = static_cast<OverviewView *> (ud);
          if (gtk_window_is_active (win)) {
            g_focused_view = self;
            g_atomic_can_shift_app_grid.store (
                self->m_target_state == ControlsState::WINDOW_PICKER,
                std::memory_order_release);
          }
          restack_locked ();
          if (gtk_window_is_active (win) && self->can_auto_focus_overview_search ())
            self->ensure_search_entry_focus ();
        }),
        this);

    GtkGesture *drag = gtk_gesture_drag_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (drag), GDK_BUTTON_PRIMARY);
    g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin_cb), this);
    g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update_cb), this);
    g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end_cb), this);
    gtk_widget_add_controller (m_window, GTK_EVENT_CONTROLLER (drag));

    GtkGesture *click = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);
    g_signal_connect (click, "released", G_CALLBACK (on_click_released_cb), this);
    gtk_widget_add_controller (m_window, GTK_EVENT_CONTROLLER (click));

    GtkEventController *keys = gtk_event_controller_key_new ();
    gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
    g_signal_connect (keys, "key-pressed", G_CALLBACK (on_overview_key_pressed_cb), this);
    gtk_widget_add_controller (m_window, keys);

    GtkEventController *ws_scroll = gtk_event_controller_scroll_new (
        GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect (ws_scroll, "scroll", G_CALLBACK (on_overview_scroll_cb), this);
    gtk_widget_add_controller (m_window, ws_scroll);

    g_signal_connect (
        m_window, "close-request",
        G_CALLBACK (+[] (GtkWindow *w G_GNUC_UNUSED, gpointer ud) -> gboolean {
          static_cast<OverviewView *> (ud)->dismiss ("close-request");
          return TRUE;
        }),
        this);
    g_signal_connect (
        m_window, "unrealize",
        G_CALLBACK (+[] (GtkWidget *w G_GNUC_UNUSED, gpointer ud) {
          auto *self = static_cast<OverviewView *> (ud);
          self->clear_pending_dwm_frame ();
          self->disconnect_after_paint_hook ();
          unsubclass_overview_hwnd (self);
          self->m_overview_surface_hooked = false;
        }),
        this);
    g_signal_connect (
        m_window, "destroy",
        G_CALLBACK (+[] (GtkWidget *w, gpointer ud) {
          static_cast<OverviewView *> (ud)->cleanup_on_destroy (w);
        }),
        this);
  }

  void start_deferred_open_animation ()
  {
    if (!m_pending_reveal) {
      if (m_cloaked_for_open && is_open ())
        arm_uncloak_after_paint ();
      return;
    }
    m_pending_reveal = false;
    if (m_window == nullptr || !gtk_widget_get_visible (m_window) || !m_placed)
      return;
    start_state_transition (m_pending_show_state, kOpenMs);
    if (m_cloaked_for_open) {
      arm_uncloak_after_paint ();
    } else {
      ensure_overview_visible_on_active_desktop (m_desktop_active);
      HMONITOR monitor = resolve_monitor ();
      if (m_want_focus_on_open &&
          winome_shell_panel_top_popover_hwnd_for_monitor (monitor) == nullptr) {
        focus_overview_window (false);
      }
      restack_locked ();
      if (m_want_focus_on_open && can_auto_focus_overview_search ()) {
        if (GetForegroundWindow () == m_hwnd)
          ensure_search_entry_focus ();
        else
          schedule_focus_verify ();
      }
    }
  }

  void show_view (bool shift_to_grid, bool take_focus, bool defer_reveal = false,
                  const SharedOpenSnapshot *shared_snap = nullptr)
  {
    if (m_window == nullptr)
      return;

    m_gl_backing_sanitized = false;
    const bool opening_from_hidden =
        !gtk_widget_get_visible (m_window) ||
        m_target_state == ControlsState::HIDDEN;
    m_open_perf_pending = opening_from_hidden;
    m_open_perf_show_us = opening_from_hidden ? g_get_monotonic_time () : 0;
    m_open_perf_trans_us = 0;
    m_open_perf_tick0_enter_us = 0;
    m_open_perf_tick0_exit_us = 0;
    m_open_perf_paint0_us = 0;
    m_open_perf_tick1_enter_us = 0;

    m_want_focus_on_open = take_focus;
    if (take_focus) {
      g_focused_view = this;
      g_requested_monitor.store (m_monitor, std::memory_order_release);
    }

    if (g_prewarm_timer != 0) {
      g_source_remove (g_prewarm_timer);
      g_prewarm_timer = 0;
    }
    cancel_focus_verify ();
    cancel_settle_hover ();
    cancel_post_hide_workarea ();
    cancel_delayed_rebuild ();

    cancel_search_fade ();
    m_search_fade_cur = 0.0;
    m_search_fade_from = 0.0;
    m_search_fade_to = 0.0;
    m_search_preedit_active = false;
    if (m_search != nullptr) {
      if (gtk_widget_get_realized (m_search))
        gtk_entry_reset_im_context (GTK_ENTRY (m_search));
      const char *cur_text = gtk_editable_get_text (GTK_EDITABLE (m_search));
      if (!m_search_query.empty () || !m_search_raw_text.empty () ||
          (cur_text != nullptr && cur_text[0] != '\0')) {
        gtk_editable_set_text (GTK_EDITABLE (m_search), "");
        m_search_query.clear ();
        m_search_raw_text.clear ();
        gtk_entry_set_icon_from_icon_name (GTK_ENTRY (m_search),
                                           GTK_ENTRY_ICON_SECONDARY, nullptr);
      }
      gtk_widget_remove_css_class (m_search, "search-active");
      set_search_dormant (true);
      gtk_widget_set_opacity (m_search, 1.0);
      gtk_widget_set_can_target (m_search, TRUE);
    }
    if (m_dash != nullptr) {
      gtk_widget_set_opacity (m_dash, 1.0);
      gtk_widget_set_can_target (m_dash, TRUE);
    }
    if (m_app_grid != nullptr)
      app_grid_close_open_folder (m_app_grid);
    m_search_mode = false;
    m_folder_dialog_open = false;
    if (m_search_view != nullptr) {
      overview_search_view_reset (m_search_view);
      gtk_widget_set_opacity (m_search_view, 0.0);
      gtk_widget_set_visible (m_search_view, FALSE);
    }
    if (m_ws_bg != nullptr)
      gtk_widget_set_opacity (m_ws_bg, 1.0);
    if (m_app_grid_clip != nullptr)
      gtk_widget_set_opacity (m_app_grid_clip, 1.0);

    m_pending_show_state = shift_to_grid ? ControlsState::APP_GRID
                                         : ControlsState::WINDOW_PICKER;
    if (take_focus) {
      g_atomic_can_shift_app_grid.store (!shift_to_grid,
                                         std::memory_order_release);
    }

    if (!gtk_widget_get_visible (m_window)) {
      // Pre-realize, position onto target monitor rect, and DWM-cloak BEFORE
      // mapping so DWM never displays a bare wallpaper frame above real desktop
      // windows before DWM thumbnails are registered at their exact t=0 rects.
      MONITORINFO mi{};
      mi.cbSize = sizeof (mi);
      if (m_monitor != nullptr && GetMonitorInfoW (m_monitor, &mi)) {
        if (!gtk_widget_get_realized (m_window))
          gtk_widget_realize (m_window);
        GdkSurface *surf = gtk_native_get_surface (GTK_NATIVE (m_window));
        HWND h = surf != nullptr
                     ? static_cast<HWND> (gdk_win32_surface_get_handle (surf))
                     : nullptr;
        if (h != nullptr) {
          m_hwnd = h;
          set_hwnd_cloaked (h, true);
          m_cloaked_for_open = true;
          const int panel_h = resolve_panel_inset ();
          m_panel_height = panel_h;
          const RECT ov_rc = compute_overview_hwnd_rect (mi.rcMonitor, panel_h);
          m_want_hwnd_rect = ov_rc;
          SetWindowPos (h, HWND_TOPMOST, ov_rc.left, ov_rc.top,
                        ov_rc.right - ov_rc.left, ov_rc.bottom - ov_rc.top,
                        SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
      }
      cancel_pending_place_idles ();
      m_placed = false;
      m_pending_reveal = false;
      m_target_state = ControlsState::HIDDEN;
      m_app_grid_mode = false;
      g_print ("[ov] show mon=%p focus=%d\n", static_cast<void *> (m_monitor),
               take_focus ? 1 : 0);
      gtk_widget_set_visible (m_window, TRUE);
      if (!defer_reveal)
        winome::evaluate_fullscreen_state ();
      if (!m_placed)
        place_overview_locked (true, 0, shared_snap, defer_reveal);
      if (m_placed) {
        cancel_pending_place_idles ();
      } else {
        uncloak_if_needed ();
        if (m_place_opening_source == 0) {
          m_place_opening_source = g_idle_add_full (
              G_PRIORITY_HIGH_IDLE, overview_place_opening_idle_cb, this, nullptr);
        }
      }
    } else if (m_placed && m_target_state == ControlsState::HIDDEN) {
      uncloak_if_needed ();
      const int cur_ws = ws::active_for_monitor (resolve_monitor ());
      if (!defer_reveal) {
        winome::evaluate_fullscreen_state ();
        start_state_transition (m_pending_show_state, kOpenMs);
        if (cur_ws >= 0 && cur_ws != m_desktop_active) {
          m_pending_desktop_sync = true;
          m_pending_desktop_target = cur_ws;
        }
        ensure_overview_visible_on_active_desktop (cur_ws);
        schedule_vd_cloak_guard (60);
        if (take_focus &&
            winome_shell_panel_top_popover_hwnd_for_monitor (m_monitor) == nullptr) {
          focus_overview_window (false);
        }
        restack_locked ();
      } else {
        m_pending_reveal = true;
        if (cur_ws >= 0 && cur_ws != m_desktop_active) {
          m_pending_desktop_sync = true;
          m_pending_desktop_target = cur_ws;
        }
      }
    } else if (m_placed) {
      uncloak_if_needed ();
      ensure_overview_visible_on_active_desktop (-1);
      schedule_vd_cloak_guard (60);
      if (m_place_idle_source == 0) {
        m_place_idle_source = g_idle_add_full (
            G_PRIORITY_HIGH_IDLE, overview_place_idle_cb, this, nullptr);
      }
      if (!defer_reveal && take_focus &&
          winome_shell_panel_top_popover_hwnd_for_monitor (m_monitor) == nullptr) {
        focus_overview_window (false);
      }
    }
  }

  void hide_view ()
  {
    g_print ("[ov] hide mon=%p\n", static_cast<void *> (m_monitor));
    if (g_focused_view == this) {
      g_atomic_can_shift_app_grid.store (false, std::memory_order_release);
      g_pending_shift_app_grid.store (false, std::memory_order_release);
    }
    set_hover (-1, true);
    if (m_app_grid != nullptr)
      app_grid_close_open_folder (m_app_grid);
    if (m_search_mode || m_search_fade_source != 0) {
      cancel_search_fade ();
      m_search_fade_cur = 0.0;
      m_search_mode = false;
      m_thumbs_cur_opacity = 0.0;
      if (m_thumbs != nullptr) {
        gtk_widget_set_opacity (m_thumbs, 0.0);
        gtk_widget_set_visible (m_thumbs, FALSE);
      }
      set_mini_opacity (0);
      if (m_search_view != nullptr) {
        overview_search_view_reset (m_search_view);
        gtk_widget_set_opacity (m_search_view, 0.0);
        gtk_widget_set_visible (m_search_view, FALSE);
      }
      if (m_ws_bg != nullptr) {
        gtk_widget_set_opacity (m_ws_bg, 1.0);
        gtk_widget_set_visible (m_ws_bg, TRUE);
      }
      if (m_app_grid_clip != nullptr)
        gtk_widget_set_opacity (m_app_grid_clip, 1.0);
    }
    if (m_window == nullptr || !gtk_widget_get_visible (m_window) || !m_placed) {
      finish_hide ();
      return;
    }
    start_state_transition (ControlsState::HIDDEN, kCloseMs);
  }

  void toggle_app_grid ()
  {
    if (m_window == nullptr || !gtk_widget_get_visible (m_window) ||
        !overview_active ()) {
      return;
    }
    if (!m_placed) {
      m_pending_show_state =
          (m_pending_show_state == ControlsState::APP_GRID)
              ? ControlsState::WINDOW_PICKER
              : ControlsState::APP_GRID;
      if (g_focused_view == this) {
        g_atomic_can_shift_app_grid.store (
            m_pending_show_state == ControlsState::WINDOW_PICKER,
            std::memory_order_release);
      }
      return;
    }
    if (m_target_state == ControlsState::HIDDEN)
      return;
    if (m_search_mode && m_search != nullptr) {
      gtk_editable_set_text (GTK_EDITABLE (m_search), "");
      focus_overview_window (false);
      if (m_target_state == ControlsState::APP_GRID) {
        restack_locked ();
        return;
      }
      cancel_search_fade ();
      apply_search_fade_progress (0.0);
      if (m_search_view != nullptr)
        overview_search_view_reset (m_search_view);
    }
    if (m_app_grid != nullptr)
      app_grid_close_open_folder (m_app_grid);
    ControlsState next = (m_target_state == ControlsState::APP_GRID)
                             ? ControlsState::WINDOW_PICKER
                             : ControlsState::APP_GRID;
    g_focused_view = this;
    start_state_transition (next, kOpenMs);
    if (winome_shell_panel_top_popover_hwnd_for_monitor (m_monitor) == nullptr)
      focus_overview_window (false);
    restack_locked ();
  }

  void restore_focus_if_active ()
  {
    if (!overview_active () || !is_open () || m_folder_dialog_open ||
        winome_shell_panel_top_popover_hwnd_for_monitor (m_monitor) != nullptr) {
      return;
    }
    focus_overview_window (!m_search_raw_text.empty () || m_search_preedit_active);
  }

  bool handle_escape ()
  {
    if (!is_open ())
      return false;
    if (m_dash != nullptr && overview_dash_has_open_menu (m_dash)) {
      overview_dash_close_menu (m_dash);
      return true;
    }
    if (m_app_grid != nullptr && app_grid_has_open_menu (m_app_grid)) {
      app_grid_close_menu (m_app_grid);
      return true;
    }
    if (m_search != nullptr) {
      const char *txt = gtk_editable_get_text (GTK_EDITABLE (m_search));
      if (txt != nullptr && txt[0] != '\0') {
        gtk_editable_set_text (GTK_EDITABLE (m_search), "");
        restore_focus_if_active ();
        return true;
      }
    }
    if (m_app_grid_mode || m_target_state == ControlsState::APP_GRID ||
        (!m_placed && m_pending_show_state == ControlsState::APP_GRID)) {
      if (m_app_grid != nullptr && app_grid_close_open_folder (m_app_grid))
        return true;
      toggle_app_grid ();
      return true;
    }
    dismiss ("esc");
    return true;
  }

  bool hit_test_screen_point (POINT screen_pt, int *out_ws, bool *out_create_new)
  {
    if (!is_open () || m_search_mode || m_folder_dialog_open)
      return false;
    HMONITOR mon = resolve_monitor ();
    MONITORINFO mi{};
    mi.cbSize = sizeof (mi);
    if (mon == nullptr || !GetMonitorInfoW (mon, &mi))
      return false;
    if (screen_pt.x < mi.rcMonitor.left || screen_pt.x >= mi.rcMonitor.right ||
        screen_pt.y < mi.rcMonitor.top + m_panel_height ||
        screen_pt.y >= mi.rcMonitor.bottom) {
      return false;
    }
    const int px = screen_pt.x - mi.rcMonitor.left;
    const int py = screen_pt.y - (mi.rcMonitor.top + m_panel_height);
    const int scale = std::max (gtk_widget_get_scale_factor (m_window), 1);
    int thumb_pill = -1;
    int peek_ws = -1;
    const int tgt = resolve_drag_drop_workspace (px, py, scale, &thumb_pill, &peek_ws);
    if (tgt >= 0) {
      if (out_ws != nullptr)
        *out_ws = tgt;
      if (out_create_new != nullptr)
        *out_create_new = (tgt == static_cast<int> (m_desktops.size ()));
      return true;
    }
    if (out_ws != nullptr)
      *out_ws = std::max (m_desktop_active, 0);
    if (out_create_new != nullptr)
      *out_create_new = false;
    return true;
  }

  void set_external_drop_highlight (POINT screen_pt)
  {
    if (!is_open () || m_search_mode || m_folder_dialog_open) {
      clear_external_drop_highlight ();
      return;
    }
    HMONITOR mon = resolve_monitor ();
    MONITORINFO mi{};
    mi.cbSize = sizeof (mi);
    if (mon == nullptr || !GetMonitorInfoW (mon, &mi) ||
        screen_pt.x < mi.rcMonitor.left || screen_pt.x >= mi.rcMonitor.right ||
        screen_pt.y < mi.rcMonitor.top + m_panel_height ||
        screen_pt.y >= mi.rcMonitor.bottom) {
      clear_external_drop_highlight ();
      return;
    }
    const int px = screen_pt.x - mi.rcMonitor.left;
    const int py = screen_pt.y - (mi.rcMonitor.top + m_panel_height);
    const int scale = std::max (gtk_widget_get_scale_factor (m_window), 1);
    int thumb_pill = -1;
    int peek_ws = -1;
    const int tgt = resolve_drag_drop_workspace (px, py, scale, &thumb_pill, &peek_ws);
    int want_ws_ring = -1;
    int want_pill = -1;
    if (tgt >= 0) {
      want_pill = thumb_pill;
      want_ws_ring = peek_ws;
    } else {
      want_ws_ring = std::max (m_desktop_active, 0);
      want_pill = want_ws_ring;
    }
    if (m_thumbs != nullptr)
      overview_thumbs_set_drop_target (m_thumbs, want_pill);
    if (m_ext_drop_highlight_ws != want_ws_ring) {
      m_ext_drop_highlight_ws = want_ws_ring;
      if (m_ws_bg != nullptr)
        gtk_widget_queue_draw (m_ws_bg);
    }
  }

  void clear_external_drop_highlight ()
  {
    if (m_ext_drop_highlight_ws != -1) {
      m_ext_drop_highlight_ws = -1;
      if (m_ws_bg != nullptr)
        gtk_widget_queue_draw (m_ws_bg);
    }
    if (!m_drag_active && m_thumbs != nullptr)
      overview_thumbs_set_drop_target (m_thumbs, -1);
  }

  void on_desktop_changed (HMONITOR monitor, int count, int active_index)
  {
    if (!is_open () || !overview_active ())
      return;
    if (monitor != nullptr && monitor != resolve_monitor ())
      return;
    ensure_overview_visible_on_active_desktop (active_index);
    schedule_vd_cloak_guard (60);
    if (!m_placed)
      return;
    const bool count_changed = (count != (int)m_desktops.size ());
    const bool active_changed =
        (active_index >= 0 && active_index != m_desktop_active);
    if (!count_changed && !active_changed)
      return;
    if (!count_changed && active_changed &&
        should_ignore_stale_desktop_echo (
            active_index, m_last_internal_switch_from,
            m_last_internal_switch_to,
            g_get_monotonic_time () - m_last_internal_switch_us)) {
      return;
    }
    if (m_anim.source != 0) {
      m_pending_desktop_sync = true;
      m_pending_desktop_target = active_changed ? active_index : -1;
      return;
    }
    if (active_changed) {
      g_print ("[ov] mon=%p desktop changed %d -> %d (count=%d, external)\n",
               static_cast<void *> (m_monitor), m_desktop_active, active_index, count);
      switch_workspace_animated (active_index, false, count);
    } else {
      g_print ("[ov] mon=%p desktop count changed %zu -> %d (external)\n",
               static_cast<void *> (m_monitor), m_desktops.size (), count);
      rebuild_now ();
    }
  }

  void reposition_view ()
  {
    if (m_window == nullptr || !gtk_widget_get_visible (m_window))
      return;
    if (!m_placed) {
      if (!overview_active ())
        finish_hide ();
      return;
    }
    if (m_target_state == ControlsState::HIDDEN) {
      finish_hide ();
      return;
    }
    place_overview_locked (false);
  }

  void cleanup_on_destroy (GtkWidget *w)
  {
    m_pending_reveal = false;
    m_uncloak_on_after_paint = false;
    cancel_uncloak_fallback ();
    cancel_vd_cloak_guard ();
    uncloak_if_needed ();
    clear_pending_dwm_frame ();
    disconnect_after_paint_hook ();
    unsubclass_overview_hwnd (this);
    m_overview_surface_hooked = false;
    cancel_pending_place_idles ();
    cancel_delayed_rebuild ();
    cancel_hover_idle ();
    cancel_focus_verify ();
    cancel_settle_hover ();

    if (m_drag_tick_source != 0 && w != nullptr) {
      gtk_widget_remove_tick_callback (w, m_drag_tick_source);
      m_drag_tick_source = 0;
    }
    if (m_drag_snapback_source != 0 && w != nullptr) {
      gtk_widget_remove_tick_callback (w, m_drag_snapback_source);
      m_drag_snapback_source = 0;
    }
    if (m_scale_anim_source != 0 && w != nullptr) {
      gtk_widget_remove_tick_callback (w, m_scale_anim_source);
      m_scale_anim_source = 0;
    }
    m_scale_anims.clear ();
    if (m_search_fade_source != 0 && w != nullptr) {
      gtk_widget_remove_tick_callback (w, m_search_fade_source);
      m_search_fade_source = 0;
    }
    if (m_anim.source != 0 && w != nullptr) {
      gtk_widget_remove_tick_callback (w, m_anim.source);
      m_anim.source = 0;
    }
    switch_cancel ();
    destroy_previews ();
    destroy_mini_previews ();
    if (m_ws_bg != nullptr)
      WINOME_WORKSPACE_BG (m_ws_bg)->owner = nullptr;
    if (m_app_grid_clip != nullptr)
      WINOME_APP_GRID_CLIP (m_app_grid_clip)->owner = nullptr;
    if (g_chrome_owner_view == this) {
      overlay_hide_all ();
      g_chrome_owner_view = nullptr;
    }
    if (g_live_wallpaper_owner == this) {
      live_wallpaper_stop ();
      g_using_live_wallpaper = false;
      g_live_wallpaper_owner = nullptr;
    }
    if (g_focused_view == this)
      g_focused_view = find_any_open_view (this);
    winome_shell_panel_set_overview_progress_for_monitor (m_monitor, 0.0);
    winome_shell_panel_set_workspace_scroll_for_monitor (m_monitor, -1, -1.0);

    m_window = nullptr;
    m_hwnd = nullptr;
    m_root = nullptr;
    m_focus_sink = nullptr;
    m_search = nullptr;
    m_search_view = nullptr;
    m_ws_bg = nullptr;
    m_app_grid_clip = nullptr;
    m_app_grid = nullptr;
    m_dash = nullptr;
    m_thumbs = nullptr;
    m_placed = false;
    m_target_state = ControlsState::HIDDEN;
  }

  void destroy_view ()
  {
    GtkWidget *win = m_window;
    if (win != nullptr) {
      gtk_window_destroy (GTK_WINDOW (win));
    } else {
      cleanup_on_destroy (nullptr);
    }
  }
};


// --- Custom widget implementations bound to OverviewView::owner --------------

static void
winome_workspace_bg_snapshot_impl (WinomeWorkspaceBg *self,
                                   GtkWidget *widget,
                                   GtkSnapshot *snapshot)
{
  OverviewView *v = self != nullptr ? self->owner : nullptr;
  if (v == nullptr)
    return;
  int w = gtk_widget_get_width (widget);
  int h = gtk_widget_get_height (widget);
  if (w <= 0 || h <= 0)
    return;

  int scale = std::max (gtk_widget_get_scale_factor (widget), 1);
  const bool settled = (v->m_anim.source == 0 && v->m_switch_source == 0);
  v->m_ws_bg_settled_shadow_drawn = settled;

  const RECT single_fallback[1] = {v->m_bg_cur};
  const RECT *boxes =
      !v->m_ws_cur_boxes.empty () ? v->m_ws_cur_boxes.data () : single_fallback;
  const int box_count =
      !v->m_ws_cur_boxes.empty () ? static_cast<int> (v->m_ws_cur_boxes.size ()) : 1;
  const int active_ws =
      (box_count > 1) ? std::clamp (v->m_desktop_active, 0, box_count - 1) : 0;

  auto draw_ws_box = [&] (int d) {
    const RECT &b = boxes[d];
    if (b.right <= v->m_bg_full.left || b.left >= v->m_bg_full.right ||
        b.bottom <= v->m_bg_full.top || b.top >= v->m_bg_full.bottom) {
      return;
    }
    float rx = static_cast<float> (b.left - v->m_bg_full.left) / scale;
    float ry = static_cast<float> (b.top - v->m_bg_full.top) / scale;
    float rw = static_cast<float> (b.right - b.left) / scale;
    float rh = static_cast<float> (b.bottom - b.top) / scale;
    if (rw <= 0.0f || rh <= 0.0f)
      return;

    graphene_rect_t rect;
    graphene_rect_init (&rect, rx, ry, rw, rh);
    const bool use_rounded = (v->m_bg_radius > 0.5);
    GskRoundedRect clip;
    if (use_rounded)
      gsk_rounded_rect_init_from_rect (&clip, &rect, (float)v->m_bg_radius);

    if (settled && use_rounded) {
      const GdkRGBA shadow_rgba = {0.0f, 0.0f, 0.0f, 0.2f};
      gtk_snapshot_append_outset_shadow (snapshot, &clip, &shadow_rgba,
                                         0.0f, 4.0f, 4.0f, 16.0f);
    }

    if (use_rounded)
      gtk_snapshot_push_rounded_clip (snapshot, &clip);
    else
      gtk_snapshot_push_clip (snapshot, &rect);

    bool painted_tex = false;
    GdkTexture *wp_tex = v->active_wallpaper_texture ();
    if (!g_using_live_wallpaper && wp_tex != nullptr &&
        wp_tex == g_wallpaper && g_wallpaper_cairo != nullptr) {
      const double tw = static_cast<double> (
          cairo_image_surface_get_width (g_wallpaper_cairo));
      const double th = static_cast<double> (
          cairo_image_surface_get_height (g_wallpaper_cairo));
      if (tw > 0.0 && th > 0.0) {
        const double wa_h_px =
            std::max<double> (v->m_bg_full.bottom - v->m_bg_full.top, 1.0);
        const double panel_ratio =
            (v->m_panel_height > 0)
                ? (static_cast<double> (v->m_panel_height) / wa_h_px)
                : 0.0;
        WallpaperDrawRect wr = compute_workarea_wallpaper_draw_rect (
            rx, ry, rw, rh, tw, th, panel_ratio);
        if (wr.w > 0.0 && wr.h > 0.0) {
          const bool animating =
              v->m_anim.source != 0 || v->m_switch_source != 0 ||
              v->m_drag_tick_source != 0 || v->m_drag_snapback_source != 0 ||
              v->m_scale_anim_source != 0;
          cairo_t *cr = gtk_snapshot_append_cairo (snapshot, &rect);
          cairo_translate (cr, wr.x, wr.y);
          cairo_scale (cr, wr.w / tw, wr.h / th);
          cairo_set_source_surface (cr, g_wallpaper_cairo, 0.0, 0.0);
          cairo_pattern_set_filter (
              cairo_get_source (cr),
              animating ? CAIRO_FILTER_FAST : CAIRO_FILTER_BILINEAR);
          cairo_paint (cr);
          cairo_destroy (cr);
          painted_tex = true;
        }
      }
    }
    if (!painted_tex && wp_tex != nullptr) {
      double tw = gdk_texture_get_width (wp_tex);
      double th = gdk_texture_get_height (wp_tex);
      if (tw > 0 && th > 0) {
        const double wa_h_px =
            std::max<double> (v->m_bg_full.bottom - v->m_bg_full.top, 1.0);
        const double panel_ratio =
            (v->m_panel_height > 0)
                ? (static_cast<double> (v->m_panel_height) / wa_h_px)
                : 0.0;
        WallpaperDrawRect wr = compute_workarea_wallpaper_draw_rect (
            rx, ry, rw, rh, tw, th, panel_ratio);
        graphene_rect_t tex_rect;
        graphene_rect_init (&tex_rect, static_cast<float> (wr.x),
                            static_cast<float> (wr.y),
                            static_cast<float> (wr.w),
                            static_cast<float> (wr.h));
        gtk_snapshot_append_scaled_texture (
            snapshot, wp_tex, GSK_SCALING_FILTER_LINEAR, &tex_rect);
        painted_tex = true;
      }
    }
    if (!painted_tex) {
      DWORD c = GetSysColor (COLOR_DESKTOP);
      GdkRGBA desktop_color = {((c >> 0) & 0xff) / 255.0f,
                               ((c >> 8) & 0xff) / 255.0f,
                               ((c >> 16) & 0xff) / 255.0f, 1.0f};
      gtk_snapshot_append_color (snapshot, &desktop_color, &rect);
    }
    gtk_snapshot_pop (snapshot);

    if ((v->m_drag_active && d == v->m_drag_drop_ws_peek &&
         d != v->m_desktop_active) ||
        (v->m_ext_drop_highlight_ws >= 0 && d == v->m_ext_drop_highlight_ws)) {
      float widths[4] = {3.0f, 3.0f, 3.0f, 3.0f};
      GdkRGBA hi = {0.208f, 0.518f, 0.894f, 0.95f};
      GdkRGBA colors[4] = {hi, hi, hi, hi};
      if (!use_rounded)
        gsk_rounded_rect_init_from_rect (&clip, &rect, 0.0f);
      gtk_snapshot_append_border (snapshot, &clip, widths, colors);
    }
  };

  for (int d = 0; d < box_count; ++d) {
    if (d != active_ws)
      draw_ws_box (d);
  }
  if (active_ws >= 0 && active_ws < box_count)
    draw_ws_box (active_ws);
}

static bool
app_grid_clip_can_capture (OverviewView *v)
{
  if (v == nullptr)
    return false;
  return v->m_anim.source == 0 && v->m_switch_source == 0 &&
         v->m_target_state == ControlsState::APP_GRID &&
         !v->m_search_mode && !v->m_folder_dialog_open;
}

// --- Shared view lookup & cross-view helpers ---------------------------------

static OverviewView *
find_view (HMONITOR mon)
{
  if (mon == nullptr)
    return nullptr;
  for (auto &up : g_views) {
    if (up && up->m_monitor == mon)
      return up.get ();
  }
  return nullptr;
}

static OverviewView *
find_open_view (HMONITOR mon)
{
  OverviewView *v = find_view (mon);
  return (v != nullptr && v->is_open ()) ? v : nullptr;
}

static OverviewView *
find_any_open_view (OverviewView *exclude)
{
  for (auto &up : g_views) {
    if (up && up.get () != exclude && up->is_open ())
      return up.get ();
  }
  return nullptr;
}

static OverviewView *
get_or_create_view (HMONITOR mon)
{
  if (!is_valid_monitor (mon))
    mon = monitor_manager_active_monitor ();
  if (!is_valid_monitor (mon))
    mon = monitor_manager_os_primary_monitor ();
  if (mon == nullptr)
    return nullptr;
  if (OverviewView *existing = find_view (mon))
    return existing;
  auto view = std::make_unique<OverviewView> ();
  view->init_for_monitor (mon);
  OverviewView *raw = view.get ();
  g_views.push_back (std::move (view));
  return raw;
}

static void
prune_disconnected_views (void)
{
  const auto monitors = enumerate_monitors ();
  for (size_t i = 0; i < g_views.size ();) {
    bool present = false;
    if (g_views[i]) {
      for (const auto &m : monitors) {
        if (m.hmonitor == g_views[i]->m_monitor) {
          present = true;
          break;
        }
      }
    }
    if (!present) {
      if (g_views[i])
        g_views[i]->destroy_view ();
      g_views.erase (g_views.begin () + static_cast<ptrdiff_t> (i));
    } else {
      ++i;
    }
  }
}

static bool
is_any_overview_view_hwnd (HWND h)
{
  if (h == nullptr)
    return false;
  for (const auto &up : g_views) {
    if (up && up->m_hwnd == h)
      return true;
  }
  return false;
}

static OverviewView *
active_chrome_or_focused_view (void)
{
  if (g_chrome_owner_view != nullptr && g_chrome_owner_view->is_open ())
    return g_chrome_owner_view;
  if (g_focused_view != nullptr && g_focused_view->is_open ())
    return g_focused_view;
  return find_any_open_view ();
}

static bool
view_can_auto_focus_search (OverviewView *v)
{
  return v != nullptr && v->can_auto_focus_overview_search ();
}

static HWND
view_hwnd (OverviewView *v)
{
  return v != nullptr ? v->m_hwnd : nullptr;
}

static HWND
view_panel_hwnd (OverviewView *v)
{
  return v != nullptr ? v->panel_hwnd_for_view () : g_panel_hwnd;
}

static HWND
active_folder_hwnd (void)
{
  for (const auto &up : g_views) {
    if (up && up->m_app_grid != nullptr) {
      HWND fh = static_cast<HWND> (app_grid_get_folder_hwnd (up->m_app_grid));
      if (is_visible_hwnd (fh))
        return fh;
    }
  }
  return nullptr;
}

static bool
is_external_foreground_target (HWND fg)
{
  if (fg == nullptr || !IsWindow (fg))
    return false;
  if (is_any_overview_view_hwnd (fg) || is_any_panel_hwnd (fg) ||
      fg == g_chrome_hwnd || fg == g_icon_hwnd || fg == g_caption_hwnd) {
    return false;
  }
  DWORD fg_pid = 0;
  GetWindowThreadProcessId (fg, &fg_pid);
  if (fg_pid == 0 || fg_pid == GetCurrentProcessId ())
    return false;
  wchar_t cls[128] = {};
  if (GetClassNameW (fg, cls, 128) > 0) {
    if (std::wcscmp (cls, L"Windows.UI.Core.CoreWindow") == 0 ||
        std::wcscmp (cls, L"XamlExplorerHostIslandWindow") == 0 ||
        std::wcscmp (cls, L"Shell_TrayWnd") == 0 ||
        std::wcscmp (cls, L"Shell_SecondaryTrayWnd") == 0 ||
        std::wcscmp (cls, L"Progman") == 0 ||
        std::wcscmp (cls, L"WorkerW") == 0 ||
        std::wcscmp (cls, L"SHELLDLL_DefView") == 0 ||
        std::wcscmp (cls, L"SysListView32") == 0) {
      return false;
    }
  }
  if (is_overview_default_blacklisted_hwnd (fg, true))
    return false;
  return true;
}

static void
on_live_wallpaper_frame (GdkTexture *full_tex, GdkTexture *thumb_tex,
                         void *user_data G_GNUC_UNUSED)
{
  OverviewView *owner = g_live_wallpaper_owner;
  if (full_tex == nullptr || owner == nullptr || owner->m_window == nullptr ||
      !gtk_widget_get_visible (owner->m_window) || !g_using_live_wallpaper) {
    return;
  }
  g_set_object (&g_live_wallpaper, full_tex);
  if (thumb_tex != nullptr)
    g_set_object (&g_live_wallpaper_thumb, thumb_tex);
  if (owner->m_ws_bg != nullptr && gtk_widget_get_visible (owner->m_ws_bg) &&
      !owner->m_search_mode) {
    gtk_widget_queue_draw (owner->m_ws_bg);
  }
  if (owner->m_thumbs != nullptr && owner->m_thumbs_has_pills &&
      thumb_tex != nullptr) {
    overview_thumbs_set_wallpaper (owner->m_thumbs, thumb_tex);
  }
}

// --- Shared hover-chrome windows (bound dynamically to g_chrome_owner_view) --

static void
chrome_apply_want (void)
{
  HWND panel = view_panel_hwnd (g_chrome_owner_view);
  chrome_window_apply_want (g_chrome_window, g_chrome_hwnd, g_chrome_want,
                            panel != nullptr ? panel : HWND_TOPMOST);
}

ChromeSurfaceHooks g_chrome_hooks{&g_chrome_window, &g_chrome_hwnd, 0, 0, 0, false,
                                  nullptr, 0, chrome_apply_want, &g_chrome_want};

static void
chrome_hide (void)
{
  chrome_cancel_apply (&g_chrome_hooks);
  g_chrome_target = nullptr;
  if (g_chrome_window != nullptr && gtk_widget_get_visible (g_chrome_window))
    gtk_widget_set_visible (g_chrome_window, FALSE);
}

static void
on_chrome_clicked (GtkButton *btn G_GNUC_UNUSED,
                   gpointer user_data G_GNUC_UNUSED)
{
  if (g_chrome_target != nullptr && IsWindow (g_chrome_target)) {
    PostMessageW (g_chrome_target, WM_CLOSE, 0, 0);
    g_print ("[ov] close requested for slot window\n");
  }
  OverviewView *owner = g_chrome_owner_view;
  if (owner != nullptr) {
    owner->set_hover (-1, true);
    owner->schedule_delayed_rebuild (250);
  } else {
    overlay_hide_all ();
  }
}

static void
chrome_init (void)
{
  g_chrome_window = make_chrome_toplevel (&g_chrome_hooks);

  constexpr int kCloseShadowPad = 4;
  constexpr int kCloseShadowPadBottom = 6;
  const char *close_icon_name = "window-close-symbolic";
  GdkDisplay *display = gdk_display_get_default ();
  if (display != nullptr) {
    GtkIconTheme *theme = gtk_icon_theme_get_for_display (display);
    if (theme != nullptr &&
        gtk_icon_theme_has_icon (theme, "preview-close-symbolic")) {
      close_icon_name = "preview-close-symbolic";
    }
  }

  g_chrome_button = gtk_button_new ();
  gtk_widget_add_css_class (g_chrome_button, "window-close");
  gtk_widget_set_focus_on_click (g_chrome_button, FALSE);
  gtk_widget_set_halign (g_chrome_button, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (g_chrome_button, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (g_chrome_button, kCloseShadowPad);
  gtk_widget_set_margin_end (g_chrome_button, kCloseShadowPad);
  gtk_widget_set_margin_top (g_chrome_button, kCloseShadowPad);
  gtk_widget_set_margin_bottom (g_chrome_button, kCloseShadowPadBottom);

  GtkWidget *icon = gtk_image_new_from_icon_name (close_icon_name);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), close_icon_size ());
  gtk_widget_set_halign (icon, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_button_set_child (GTK_BUTTON (g_chrome_button), icon);

  GtkWidget *wrap = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append (GTK_BOX (wrap), g_chrome_button);
  gtk_window_set_child (GTK_WINDOW (g_chrome_window), wrap);

  g_signal_connect (g_chrome_button, "clicked",
                    G_CALLBACK (on_chrome_clicked), nullptr);

  attach_chrome_hover_motion (g_chrome_button);
}

static void
chrome_show_at (OverviewView *v, const RECT &preview_rect)
{
  if (g_chrome_window == nullptr || g_chrome_button == nullptr ||
      v == nullptr || v->m_window == nullptr)
    return;

  GtkWidget *wrap = gtk_window_get_child (GTK_WINDOW (g_chrome_window));
  GtkRequisition wrap_nat{}, btn_nat{};
  gtk_widget_get_preferred_size (wrap, nullptr, &wrap_nat);
  gtk_widget_get_preferred_size (g_chrome_button, nullptr, &btn_nat);
  int fallback = close_button_size ();
  int want_def_w = std::max (wrap_nat.width, fallback);
  int want_def_h = std::max (wrap_nat.height, fallback);
  int cur_def_w = 0, cur_def_h = 0;
  gtk_window_get_default_size (GTK_WINDOW (g_chrome_window), &cur_def_w,
                               &cur_def_h);
  if (cur_def_w != want_def_w || cur_def_h != want_def_h)
    gtk_window_set_default_size (GTK_WINDOW (g_chrome_window), want_def_w,
                                 want_def_h);

  int scale = std::max (gtk_widget_get_scale_factor (v->m_window), 1);
  HMONITOR monitor = v->resolve_monitor ();
  MONITORINFO mi {};
  mi.cbSize = sizeof (mi);
  GetMonitorInfoW (monitor, &mi);

  int w = want_def_w * scale;
  int h = want_def_h * scale;
  int pad = gtk_widget_get_margin_start (g_chrome_button);
  int btn_cx = (pad + std::max (btn_nat.width, fallback) / 2) * scale;
  int btn_cy = (pad + std::max (btn_nat.height, fallback) / 2) * scale;
  g_chrome_want.left = mi.rcMonitor.left + preview_rect.right - btn_cx;
  g_chrome_want.top =
      mi.rcMonitor.top + v->m_panel_height + preview_rect.top - btn_cy;
  g_chrome_want.right = g_chrome_want.left + w;
  g_chrome_want.bottom = g_chrome_want.top + h;

  gtk_widget_set_opacity (g_chrome_button, v->m_overlay_opacity);
  chrome_present_window (&g_chrome_hooks);
}

static void
icon_chrome_apply_want (void)
{
  HWND panel = view_panel_hwnd (g_chrome_owner_view);
  HWND after = is_visible_hwnd (g_chrome_hwnd)
                   ? g_chrome_hwnd
                   : (panel != nullptr ? panel : HWND_TOPMOST);
  chrome_window_apply_want (g_icon_window, g_icon_hwnd, g_icon_want, after);
}

ChromeSurfaceHooks g_icon_hooks{&g_icon_window, &g_icon_hwnd, 0, 0, 0, false,
                                nullptr, 0, icon_chrome_apply_want, &g_icon_want};

static void
icon_hide (void)
{
  chrome_cancel_apply (&g_icon_hooks);
  g_icon_target = nullptr;
  if (g_icon_window != nullptr && gtk_widget_get_visible (g_icon_window))
    gtk_widget_set_visible (g_icon_window, FALSE);
}

static void
on_icon_click (GtkGestureClick *gesture G_GNUC_UNUSED,
               int n_press G_GNUC_UNUSED, double x G_GNUC_UNUSED,
               double y G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
  OverviewView *owner = g_chrome_owner_view;
  if (owner == nullptr)
    return;
  int index = owner->m_hover_index >= 0 ? owner->m_hover_index
                                        : owner->m_last_hovered_slot;
  owner->activate_slot_and_close (index, "ov-click-icon");
}

constexpr int kIconShadowPad = 6;

static void
icon_chrome_init (void)
{
  g_icon_window = make_chrome_toplevel (&g_icon_hooks);

  g_icon_picture = gtk_picture_new ();
  gtk_widget_add_css_class (g_icon_picture, "window-icon");
  gtk_widget_add_css_class (g_icon_picture, "icon-dropshadow");
  gtk_picture_set_content_fit (GTK_PICTURE (g_icon_picture),
                               GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_size_request (g_icon_picture, kWindowIconSize,
                               kWindowIconSize);
  gtk_widget_set_margin_start (g_icon_picture, kIconShadowPad);
  gtk_widget_set_margin_end (g_icon_picture, kIconShadowPad);
  gtk_widget_set_margin_top (g_icon_picture, kIconShadowPad);
  gtk_widget_set_margin_bottom (g_icon_picture, kIconShadowPad);

  GtkWidget *wrap = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append (GTK_BOX (wrap), g_icon_picture);
  gtk_window_set_child (GTK_WINDOW (g_icon_window), wrap);
  gtk_window_set_default_size (GTK_WINDOW (g_icon_window),
                               kWindowIconSize + 2 * kIconShadowPad,
                               kWindowIconSize + 2 * kIconShadowPad);

  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "released", G_CALLBACK (on_icon_click), nullptr);
  gtk_widget_add_controller (g_icon_picture, GTK_EVENT_CONTROLLER (click));

  attach_chrome_hover_motion (g_icon_picture);
}

static void
icon_show_at (OverviewView *v, const RECT &preview_rect, HWND target)
{
  if (g_icon_window == nullptr || target == nullptr || v == nullptr ||
      v->m_window == nullptr)
    return;

  int scale = std::max (gtk_widget_get_scale_factor (v->m_window), 1);
  HMONITOR monitor = v->resolve_monitor ();
  MONITORINFO mi {};
  mi.cbSize = sizeof (mi);
  GetMonitorInfoW (monitor, &mi);

  int size = kWindowIconSize * scale;
  int pad = kIconShadowPad * scale;
  double cx = (preview_rect.left + preview_rect.right) / 2.0;
  double top = preview_rect.bottom - kIconOverlap * size;

  g_icon_want.left = mi.rcMonitor.left + (LONG)(cx - size / 2.0) - pad;
  g_icon_want.top =
      mi.rcMonitor.top + v->m_panel_height + (LONG)top - pad;
  g_icon_want.right = g_icon_want.left + size + 2 * pad;
  g_icon_want.bottom = g_icon_want.top + size + 2 * pad;

  if (g_icon_target != target) {
    GdkTexture *texture = winome_window_app_icon (target, size);
    gtk_picture_set_paintable (GTK_PICTURE (g_icon_picture),
                               GDK_PAINTABLE (texture));
    g_clear_object (&texture);
    g_icon_target = target;
  }

  gtk_widget_set_opacity (g_icon_picture, v->m_overlay_opacity);
  chrome_present_window (&g_icon_hooks);
}

static void
caption_chrome_apply_want (void)
{
  HWND panel = view_panel_hwnd (g_chrome_owner_view);
  HWND after = is_visible_hwnd (g_icon_hwnd)
                   ? g_icon_hwnd
                   : (is_visible_hwnd (g_chrome_hwnd)
                          ? g_chrome_hwnd
                          : (panel != nullptr ? panel : HWND_TOPMOST));
  chrome_window_apply_want (g_caption_window, g_caption_hwnd, g_caption_want,
                            after);
}

ChromeSurfaceHooks g_caption_hooks{&g_caption_window, &g_caption_hwnd, 0, 0, 0, false,
                                   nullptr, 0, caption_chrome_apply_want, &g_caption_want};

static void
caption_hide (void)
{
  chrome_cancel_apply (&g_caption_hooks);
  if (g_caption_window != nullptr &&
      gtk_widget_get_visible (g_caption_window))
    gtk_widget_set_visible (g_caption_window, FALSE);
}

static void
on_caption_click (GtkGestureClick *gesture G_GNUC_UNUSED,
                  int n_press G_GNUC_UNUSED, double x G_GNUC_UNUSED,
                  double y G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
  OverviewView *owner = g_chrome_owner_view;
  if (owner == nullptr)
    return;
  int index = owner->m_hover_index >= 0 ? owner->m_hover_index
                                        : owner->m_last_hovered_slot;
  owner->activate_slot_and_close (index, "ov-click-caption");
}

static void
caption_chrome_init (void)
{
  g_caption_window = make_chrome_toplevel (&g_caption_hooks);
  gtk_window_set_resizable (GTK_WINDOW (g_caption_window), TRUE);

  g_caption = gtk_label_new ("");
  gtk_widget_add_css_class (g_caption, "window-caption");
  gtk_label_set_single_line_mode (GTK_LABEL (g_caption), TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (g_caption), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (g_caption), 40);
  gtk_widget_set_halign (g_caption, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (g_caption, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (g_caption, TRUE);

  g_caption_wrap = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (g_caption_wrap, TRUE);
  gtk_widget_set_halign (g_caption_wrap, GTK_ALIGN_FILL);
  gtk_widget_set_valign (g_caption_wrap, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (g_caption_wrap), g_caption);
  gtk_window_set_child (GTK_WINDOW (g_caption_window), g_caption_wrap);

  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "released", G_CALLBACK (on_caption_click), nullptr);
  gtk_widget_add_controller (g_caption, GTK_EVENT_CONTROLLER (click));

  attach_chrome_hover_motion (g_caption);
}

static void
caption_show_at (OverviewView *v, const RECT &preview_rect)
{
  if (g_caption_window == nullptr || g_caption == nullptr ||
      g_caption_wrap == nullptr || v == nullptr || v->m_window == nullptr)
    return;

  const char *txt = gtk_label_get_text (GTK_LABEL (g_caption));
  if (txt == nullptr || txt[0] == '\0') {
    caption_hide ();
    return;
  }

  int scale = std::max (gtk_widget_get_scale_factor (v->m_window), 1);
  HMONITOR monitor = v->resolve_monitor ();
  MONITORINFO mi{};
  mi.cbSize = sizeof (mi);
  GetMonitorInfoW (monitor, &mi);

  int mon_w_logical =
      std::max ((int)(mi.rcMonitor.right - mi.rcMonitor.left) / scale, 120);
  int preview_w_logical =
      std::max ((int)(preview_rect.right - preview_rect.left) / scale, 120);
  // Quantize max_cap_w to 16px buckets so hover scale-up animation doesn't
  // invalidate caption size/measure on every single VSync frame.
  int raw_max_cap_w = std::min (mon_w_logical - 24,
                                std::max (preview_w_logical + 96, 280));
  int max_cap_w = std::min (mon_w_logical - 24, ((raw_max_cap_w + 15) / 16) * 16);

  static std::string s_cap_txt;
  static int s_cap_scale = 0;
  static int s_cap_max_w = 0;
  static int s_cap_w = 0;
  static int s_cap_h = 0;

  int want_def_w = s_cap_w;
  int want_def_h = s_cap_h;
  if (s_cap_w <= 0 || s_cap_h <= 0 || s_cap_scale != scale ||
      s_cap_max_w != max_cap_w || s_cap_txt != txt) {
    gtk_widget_set_size_request (g_caption, -1, -1);
    gtk_widget_set_size_request (g_caption_wrap, -1, -1);
    int min_w = 0, nat_w = 0;
    gtk_widget_measure (g_caption, GTK_ORIENTATION_HORIZONTAL, -1, &min_w,
                        &nat_w, nullptr, nullptr);
    if (nat_w <= 0) {
      caption_hide ();
      return;
    }
    want_def_w = std::clamp (nat_w + 4, std::min (min_w + 4, max_cap_w),
                             std::max (max_cap_w, 32));
    int min_h = 0, nat_h = 0;
    gtk_widget_measure (g_caption, GTK_ORIENTATION_VERTICAL, want_def_w, &min_h,
                        &nat_h, nullptr, nullptr);
    want_def_h = std::max (nat_h, 1);

    s_cap_txt = txt;
    s_cap_scale = scale;
    s_cap_max_w = max_cap_w;
    s_cap_w = want_def_w;
    s_cap_h = want_def_h;

    gtk_widget_set_size_request (g_caption_wrap, want_def_w, want_def_h);
    int cur_def_w = 0, cur_def_h = 0;
    gtk_window_get_default_size (GTK_WINDOW (g_caption_window), &cur_def_w,
                                 &cur_def_h);
    if (cur_def_w != want_def_w || cur_def_h != want_def_h) {
      gtk_window_set_default_size (GTK_WINDOW (g_caption_window), want_def_w,
                                   want_def_h);
      gtk_widget_queue_resize (g_caption_window);
    }
  }

  int w = want_def_w * scale;
  int h = want_def_h * scale;
  double cx = (preview_rect.left + preview_rect.right) / 2.0;
  double top = preview_rect.bottom +
               (kWindowIconSize * (1.0 - kIconOverlap) + kIconTitleSpacing) *
                   scale;

  LONG left = mi.rcMonitor.left + static_cast<LONG> (std::lround (cx - w / 2.0));
  LONG top_px =
      mi.rcMonitor.top + v->m_panel_height + static_cast<LONG> (std::lround (top));
  LONG min_left = mi.rcMonitor.left + 8 * scale;
  LONG max_left = std::max (min_left, mi.rcMonitor.right - 8 * scale - w);
  left = std::clamp (left, min_left, max_left);
  LONG min_top = mi.rcMonitor.top + v->m_panel_height;
  LONG max_top = std::max (min_top, mi.rcMonitor.bottom - 8 * scale - h);
  top_px = std::min (top_px, max_top);

  g_caption_want.left = left;
  g_caption_want.top = top_px;
  g_caption_want.right = left + w;
  g_caption_want.bottom = top_px + h;

  gtk_widget_set_opacity (g_caption, v->m_overlay_opacity);
  chrome_present_window (&g_caption_hooks);
}

static void
overlay_hide_all (void)
{
  chrome_hide ();
  icon_hide ();
  caption_hide ();
}

static void
chrome_keep_hover_alive (void)
{
  if (g_chrome_owner_view != nullptr)
    g_chrome_owner_view->keep_hover_alive_from_chrome ();
}

static void
chrome_schedule_hover_idle_hide (void)
{
  if (g_chrome_owner_view != nullptr)
    g_chrome_owner_view->schedule_hover_idle_hide ();
}

static gboolean
on_chrome_scroll_cb (GtkEventControllerScroll *ctrl, double dx, double dy,
                     gpointer user_data G_GNUC_UNUSED)
{
  OverviewView *owner = active_chrome_or_focused_view ();
  if (owner != nullptr)
    return owner->on_overview_scroll (ctrl, dx, dy, nullptr);
  return GDK_EVENT_PROPAGATE;
}

// --- Unified WndProc subclass (per-HWND OverviewView* property) --------------

constexpr const wchar_t *kOverviewOrigProcProp = L"WINOME_OverviewOrigProc";
constexpr const wchar_t *kOverviewViewProp = L"WINOME_OverviewViewPtr";

static LRESULT CALLBACK
overview_subclass_wndproc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  if (msg == WM_ERASEBKGND)
    return 1;
  if (msg == WM_STYLECHANGING && wParam == static_cast<WPARAM> (GWL_EXSTYLE) &&
      lParam != 0) {
    auto *ss = reinterpret_cast<STYLESTRUCT *> (lParam);
    ss->styleNew = (static_cast<LONG_PTR> (ss->styleNew) | WS_EX_TOOLWINDOW) &
                   ~(WS_EX_NOACTIVATE | WS_EX_APPWINDOW);
  }

  OverviewView *v = reinterpret_cast<OverviewView *> (
      GetPropW (hwnd, kOverviewViewProp));

  if (msg == WM_MOUSEACTIVATE && v != nullptr && overview_active () &&
      v->m_target_state != ControlsState::HIDDEN &&
      winome_shell_panel_top_popover_hwnd () != nullptr) {
    return MA_NOACTIVATEANDEAT;
  }

  if (msg == WM_WINDOWPOSCHANGING && lParam != 0) {
    auto *wp = reinterpret_cast<WINDOWPOS *> (lParam);
    HWND active_panel = v != nullptr ? v->panel_hwnd_for_view () : g_panel_hwnd;
    if ((GetWindowLongPtrW (hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0 &&
        active_panel != nullptr && IsWindow (active_panel) &&
        IsWindowVisible (active_panel) &&
        (GetWindowLongPtrW (active_panel, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
      if ((wp->flags & SWP_NOZORDER) == 0) {
        HWND folder_hwnd = active_folder_hwnd ();
        HWND allowed_anchor = active_panel;
        HWND chrome_layers[] = {folder_hwnd, g_chrome_hwnd, g_icon_hwnd,
                                g_caption_hwnd};
        for (HWND h : chrome_layers) {
          if (is_visible_hwnd (h) &&
              (GetWindowLongPtrW (h, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
            allowed_anchor = h;
          }
        }
        if (wp->hwndInsertAfter != active_panel &&
            wp->hwndInsertAfter != folder_hwnd &&
            wp->hwndInsertAfter != g_chrome_hwnd &&
            wp->hwndInsertAfter != g_icon_hwnd &&
            wp->hwndInsertAfter != g_caption_hwnd &&
            !is_any_panel_hwnd (wp->hwndInsertAfter) &&
            !is_any_overview_view_hwnd (wp->hwndInsertAfter)) {
          wp->hwndInsertAfter = allowed_anchor;
          wp->flags |= SWP_NOOWNERZORDER;
        }
      }
    }
    if (v != nullptr && (wp->flags & SWP_HIDEWINDOW) == 0 &&
        (IsWindowVisible (hwnd) || (wp->flags & SWP_SHOWWINDOW) != 0) &&
        v->m_want_hwnd_rect.right > v->m_want_hwnd_rect.left &&
        v->m_want_hwnd_rect.bottom > v->m_want_hwnd_rect.top) {
      RECT cur_rect{};
      const bool has_cur_rect = GetWindowRect (hwnd, &cur_rect) != FALSE;
      RECT eff_rect{};
      const bool has_eff_rect = winome::resolve_panel_windowpos_rect (
          cur_rect, has_cur_rect, wp->x, wp->y, wp->cx, wp->cy, wp->flags,
          &eff_rect);
      if (!has_eff_rect || !EqualRect (&eff_rect, &v->m_want_hwnd_rect)) {
        const RECT &want = v->m_want_hwnd_rect;
        const int want_w = want.right - want.left;
        const int want_h = want.bottom - want.top;
        const bool need_move =
            !has_cur_rect || (wp->flags & SWP_NOMOVE) == 0 ||
            cur_rect.left != want.left || cur_rect.top != want.top;
        const bool need_size =
            !has_cur_rect || (wp->flags & SWP_NOSIZE) == 0 ||
            (cur_rect.right - cur_rect.left) != want_w ||
            (cur_rect.bottom - cur_rect.top) != want_h;
        if (need_move) {
          wp->x = want.left;
          wp->y = want.top;
          wp->flags &= ~SWP_NOMOVE;
        }
        if (need_size) {
          wp->cx = want_w;
          wp->cy = want_h;
          wp->flags &= ~SWP_NOSIZE;
        }
      }
    }
  }
  if (v != nullptr && is_overview_ime_activation_message (msg, wParam) &&
      v->can_auto_focus_overview_search ()) {
    const bool was_dormant =
        v->m_search != nullptr &&
        gtk_widget_has_css_class (v->m_search, "search-dormant");
    const bool was_focused = v->is_search_focused ();
    v->set_search_dormant (false);
    if (msg == WM_KEYDOWN && wParam == VK_PROCESSKEY &&
        !v->is_search_focused () && !v->m_search_preedit_active) {
      v->ensure_search_entry_focus ();
    }
    if (was_dormant || !was_focused) {
      g_print ("[ov-ime] mon=%p msg=0x%x wp=0x%llx was_dormant=%d was_focus=%d now_focus=%d\n",
               static_cast<void *> (v->m_monitor), msg,
               static_cast<unsigned long long> (wParam),
               was_dormant ? 1 : 0, was_focused ? 1 : 0,
               v->is_search_focused () ? 1 : 0);
    }
  }
  if (v != nullptr && (msg == WM_SYSCHAR || msg == WM_MENUCHAR) &&
      overview_active () && v->m_target_state != ControlsState::HIDDEN) {
    return (msg == WM_MENUCHAR) ? MAKELRESULT (0, MNC_CLOSE) : 0;
  }

  WNDPROC prop_orig = reinterpret_cast<WNDPROC> (
      GetPropW (hwnd, kOverviewOrigProcProp));
  WNDPROC orig = (prop_orig != nullptr)
                     ? prop_orig
                     : (v != nullptr ? v->m_orig_overview_wndproc : nullptr);
  if (msg == WM_NCDESTROY) {
    RemovePropW (hwnd, kOverviewOrigProcProp);
    RemovePropW (hwnd, kOverviewViewProp);
    if (v != nullptr && hwnd == v->m_hwnd)
      v->m_orig_overview_wndproc = nullptr;
    if (orig != nullptr) {
      if (GetWindowLongPtrW (hwnd, GWLP_WNDPROC) ==
          reinterpret_cast<LONG_PTR> (overview_subclass_wndproc)) {
        SetWindowLongPtrW (hwnd, GWLP_WNDPROC,
                           reinterpret_cast<LONG_PTR> (orig));
      }
      return CallWindowProcW (orig, hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW (hwnd, msg, wParam, lParam);
  }

  LRESULT res = orig != nullptr
                    ? CallWindowProcW (orig, hwnd, msg, wParam, lParam)
                    : DefWindowProcW (hwnd, msg, wParam, lParam);

  if (v != nullptr) {
    const bool focus_lost_msg =
        (msg == WM_KILLFOCUS) ||
        (msg == WM_ACTIVATE && LOWORD (wParam) == WA_INACTIVE);
    if (focus_lost_msg && overview_active () && v->m_placed &&
        v->m_target_state != ControlsState::HIDDEN) {
      HWND other = (msg == WM_KILLFOCUS) ? reinterpret_cast<HWND> (wParam)
                                         : reinterpret_cast<HWND> (lParam);
      HWND cur_fg = GetForegroundWindow ();
      // Never fight another OverviewView when the user switches focus between
      // simultaneous multi-monitor Overview displays!
      if (!is_any_overview_view_hwnd (other) &&
          !is_any_overview_view_hwnd (cur_fg) &&
          v == g_focused_view && v->can_auto_focus_overview_search () &&
          (is_overview_chrome_or_panel_hwnd (other) ||
           is_overview_chrome_or_panel_hwnd (cur_fg))) {
        v->schedule_focus_verify ();
      }
    } else if ((msg == WM_ACTIVATE && LOWORD (wParam) != WA_INACTIVE) ||
               msg == WM_SETFOCUS) {
      g_focused_view = v;
      g_atomic_can_shift_app_grid.store (
          v->m_target_state == ControlsState::WINDOW_PICKER,
          std::memory_order_release);
      restack_locked ();
      if (v->can_auto_focus_overview_search ())
        v->ensure_search_entry_focus ();
    }
  }
  return res;
}

static void
ensure_overview_subclass (HWND hwnd, OverviewView *view)
{
  if (hwnd == nullptr || !IsWindow (hwnd))
    return;
  SetPropW (hwnd, kOverviewViewProp, reinterpret_cast<HANDLE> (view));
  if (GetPropW (hwnd, kOverviewOrigProcProp) != nullptr)
    return;
  WNDPROC cur = reinterpret_cast<WNDPROC> (
      GetWindowLongPtrW (hwnd, GWLP_WNDPROC));
  if (cur != nullptr && cur != overview_subclass_wndproc) {
    if (view != nullptr)
      view->m_orig_overview_wndproc = cur;
    SetPropW (hwnd, kOverviewOrigProcProp, reinterpret_cast<HANDLE> (cur));
    SetWindowLongPtrW (hwnd, GWLP_WNDPROC,
                       reinterpret_cast<LONG_PTR> (overview_subclass_wndproc));
  }
}

static void
unsubclass_overview_hwnd (OverviewView *view)
{
  if (view == nullptr)
    return;
  HWND hwnd = view->m_hwnd;
  if (hwnd != nullptr && IsWindow (hwnd)) {
    WNDPROC prop_orig = reinterpret_cast<WNDPROC> (
        GetPropW (hwnd, kOverviewOrigProcProp));
    WNDPROC orig =
        prop_orig != nullptr ? prop_orig : view->m_orig_overview_wndproc;
    WNDPROC cur = reinterpret_cast<WNDPROC> (
        GetWindowLongPtrW (hwnd, GWLP_WNDPROC));
    if (orig != nullptr && cur == overview_subclass_wndproc) {
      SetWindowLongPtrW (hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR> (orig));
    }
    RemovePropW (hwnd, kOverviewOrigProcProp);
    RemovePropW (hwnd, kOverviewViewProp);
  }
  view->m_orig_overview_wndproc = nullptr;
}

// --- Unified Z-order across all panels, popovers, chrome & OverviewViews -----

static bool
is_popup_menu_window (HWND h)
{
  if (h == nullptr || !IsWindow (h) || !IsWindowVisible (h) || is_cloaked (h))
    return false;
  if (is_any_panel_hwnd (h) || is_any_overview_view_hwnd (h) ||
      h == g_chrome_hwnd || h == g_icon_hwnd || h == g_caption_hwnd)
    return false;
  RECT r{};
  if (!GetWindowRect (h, &r) || r.right <= r.left || r.bottom <= r.top)
    return false;

  wchar_t cls[128] = L"";
  GetClassNameW (h, cls, 128);
  if (wcscmp (cls, L"#32768") == 0 ||
      wcsstr (cls, L"Xaml_WindowedPopupClass") != nullptr ||
      wcsstr (cls, L"PopupHost") != nullptr) {
    return true;
  }

  DWORD pid = 0;
  GetWindowThreadProcessId (h, &pid);
  if (pid == GetCurrentProcessId ()) {
    HWND owner = GetWindow (h, GW_OWNER);
    return is_any_overview_view_hwnd (owner);
  }

  if (wcscmp (cls, L"Shell_TrayWnd") == 0 ||
      wcscmp (cls, L"Shell_SecondaryTrayWnd") == 0) {
    return false;
  }

  LONG_PTR ex = GetWindowLongPtrW (h, GWL_EXSTYLE);
  LONG_PTR style = GetWindowLongPtrW (h, GWL_STYLE);
  if ((ex & WS_EX_TOPMOST) != 0 && (style & WS_POPUP) != 0 &&
      (style & (WS_CAPTION | WS_THICKFRAME)) == 0) {
    HMONITOR mon = MonitorFromRect (&r, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof (mi);
    if (GetMonitorInfoW (mon, &mi)) {
      int mw = mi.rcMonitor.right - mi.rcMonitor.left;
      int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
      int ww = r.right - r.left;
      int wh = r.bottom - r.top;
      if (ww < (mw * 85) / 100 || wh < (mh * 85) / 100)
        return true;
    }
  }
  return false;
}

static HWND
find_owned_overview_popup_hwnd (void)
{
  const HWND folder_hwnd = active_folder_hwnd ();
  const DWORD self_pid = GetCurrentProcessId ();
  HWND w = GetTopWindow (nullptr);
  for (int i = 0; w != nullptr && i < 48; ++i, w = GetWindow (w, GW_HWNDNEXT)) {
    if (is_any_panel_hwnd (w) || is_any_overview_view_hwnd (w) ||
        w == g_chrome_hwnd || w == g_icon_hwnd || w == g_caption_hwnd ||
        (folder_hwnd != nullptr && w == folder_hwnd))
      continue;
    if (!IsWindowVisible (w) || is_cloaked (w))
      continue;
    DWORD pid = 0;
    GetWindowThreadProcessId (w, &pid);
    if (pid != self_pid)
      continue;
    HWND owner = GetWindow (w, GW_OWNER);
    if (!is_any_overview_view_hwnd (owner) &&
        (folder_hwnd == nullptr || owner != folder_hwnd))
      continue;
    RECT r{};
    if (!GetWindowRect (w, &r) || r.right <= r.left || r.bottom <= r.top)
      continue;
    return w;
  }
  return nullptr;
}

static HWND
find_lowest_popup_menu_above_panel (HWND panel_hwnd)
{
  if (panel_hwnd == nullptr || !IsWindow (panel_hwnd))
    return nullptr;
  for (HWND prev = GetWindow (panel_hwnd, GW_HWNDPREV); prev != nullptr;
       prev = GetWindow (prev, GW_HWNDPREV)) {
    if (is_popup_menu_window (prev))
      return prev;
  }
  return nullptr;
}

static std::vector<HWND>
collect_visible_panels (void)
{
  std::vector<HWND> out;
  const auto monitors = enumerate_monitors ();
  for (const auto &m : monitors) {
    HWND p = monitor_manager_panel_for_monitor (m.hmonitor);
    if (is_visible_hwnd (p) &&
        std::find (out.begin (), out.end (), p) == out.end ()) {
      out.push_back (p);
    }
  }
  if (is_visible_hwnd (g_panel_hwnd) &&
      std::find (out.begin (), out.end (), g_panel_hwnd) == out.end ()) {
    out.push_back (g_panel_hwnd);
  }
  return out;
}

static std::vector<HWND>
collect_visible_overviews (void)
{
  std::vector<HWND> out;
  if (g_focused_view != nullptr && is_visible_hwnd (g_focused_view->m_hwnd))
    out.push_back (g_focused_view->m_hwnd);
  for (const auto &up : g_views) {
    if (up && is_visible_hwnd (up->m_hwnd) &&
        std::find (out.begin (), out.end (), up->m_hwnd) == out.end ()) {
      out.push_back (up->m_hwnd);
    }
  }
  return out;
}

static void
dump_zorder (OverviewView *v, const char *tag)
{
  HWND panel = view_panel_hwnd (v);
  if (panel == nullptr)
    panel = g_panel_hwnd;
  HWND overview = view_hwnd (v);
  HWND popover = static_cast<HWND> (winome_shell_panel_top_popover_hwnd ());

  int pos_panel = -1, pos_overview = -1, pos_popover = -1, pos_chrome = -1,
      pos_caption = -1;

  HWND wnd = GetTopWindow (nullptr);
  for (int i = 0; wnd != nullptr && i < 48; ++i) {
    if (wnd == popover) pos_popover = i;
    if (wnd == panel) pos_panel = i;
    if (wnd == overview) pos_overview = i;
    if (wnd == g_chrome_hwnd || wnd == g_icon_hwnd) pos_chrome = i;
    if (wnd == g_caption_hwnd) pos_caption = i;
    wnd = GetWindow (wnd, GW_HWNDNEXT);
  }

  const bool ok =
      (panel == nullptr && pos_overview >= 0) ||
      (pos_panel >= 0 && pos_overview >= 0 && pos_panel < pos_overview);
  g_print ("[zorder:%s mon=%p] panel@%d overview@%d popover@%d chrome@%d caption@%d contract=%s\n",
           tag, static_cast<void *> (v != nullptr ? v->m_monitor : nullptr),
           pos_panel, pos_overview, pos_popover, pos_chrome, pos_caption,
           ok ? "OK" : "BROKEN");
}

static bool
zorder_drifted (const std::vector<HWND> &panels,
                const std::vector<HWND> &overviews)
{
  if (panels.empty () && overviews.empty ())
    return false;

  std::vector<HWND> chain;
  chain.reserve (panels.size () + overviews.size () + 6);

  HWND panel_popover =
      static_cast<HWND> (winome_shell_panel_top_popover_hwnd ());
  HWND ov_popup = !overviews.empty () ? find_owned_overview_popup_hwnd () : nullptr;
  HWND top_popover = (panel_popover != nullptr) ? panel_popover : ov_popup;
  if (top_popover != nullptr)
    chain.push_back (top_popover);
  for (HWND p : panels)
    chain.push_back (p);
  HWND folder_hwnd = active_folder_hwnd ();
  if (is_visible_hwnd (folder_hwnd))
    chain.push_back (folder_hwnd);
  if (is_visible_hwnd (g_chrome_hwnd))
    chain.push_back (g_chrome_hwnd);
  if (is_visible_hwnd (g_icon_hwnd))
    chain.push_back (g_icon_hwnd);
  if (is_visible_hwnd (g_caption_hwnd))
    chain.push_back (g_caption_hwnd);
  for (HWND ov : overviews)
    chain.push_back (ov);

  for (HWND h : chain) {
    if ((GetWindowLongPtrW (h, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0)
      return true;
  }
  for (size_t i = 1; i < chain.size (); ++i) {
    if (GetWindow (chain[i], GW_HWNDPREV) != chain[i - 1])
      return true;
  }
  if (!chain.empty () && !panels.empty () && chain[0] == panels[0]) {
    for (HWND prev = GetWindow (panels[0], GW_HWNDPREV); prev != nullptr;
         prev = GetWindow (prev, GW_HWNDPREV)) {
      if (is_popup_menu_window (prev) || is_any_panel_hwnd (prev) ||
          is_any_overview_view_hwnd (prev))
        continue;
      if (IsWindowVisible (prev) && !is_cloaked (prev)) {
        wchar_t cls[128] = L"";
        if (GetClassNameW (prev, cls, 128) > 0 &&
            winome::is_system_zorder_overlay_class (cls)) {
          continue;
        }
        RECT r{};
        if (GetWindowRect (prev, &r) && r.right > r.left && r.bottom > r.top) {
          if (overviews.empty ()) {
            bool touches_panel = false;
            for (HWND p : panels) {
              RECT pr{};
              RECT inter{};
              if (GetWindowRect (p, &pr) && IntersectRect (&inter, &r, &pr)) {
                touches_panel = true;
                break;
              }
            }
            if (!touches_panel)
              continue;
          }
          return true;
        }
      }
    }
  }
  return false;
}

static void
restack_locked (void)
{
  std::vector<HWND> panels = collect_visible_panels ();
  std::vector<HWND> overviews = collect_visible_overviews ();
  if (panels.empty () && overviews.empty ())
    return;
  if (!zorder_drifted (panels, overviews))
    return;
  winome::sanitize_gdk_gl_backing_windows ();

  HWND panel_popover =
      static_cast<HWND> (winome_shell_panel_top_popover_hwnd ());
  HWND ov_popup = !overviews.empty () ? find_owned_overview_popup_hwnd () : nullptr;
  HWND top_popover = (panel_popover != nullptr) ? panel_popover : ov_popup;

  if (panel_popover == nullptr && ov_popup != nullptr) {
    ensure_hwnd_topmost (ov_popup);
    SetWindowPos (ov_popup, HWND_TOPMOST, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
  }

  HWND foreign_popup =
      (top_popover == nullptr && !panels.empty ())
          ? find_lowest_popup_menu_above_panel (panels[0])
          : nullptr;
  HWND prev_hwnd = (top_popover != nullptr)
                       ? top_popover
                       : (foreign_popup != nullptr ? foreign_popup
                                                   : HWND_TOPMOST);

  for (HWND p : panels) {
    if (prev_hwnd == HWND_TOPMOST)
      ensure_hwnd_topmost (p);
    if ((GetWindowLongPtrW (p, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0 &&
        prev_hwnd != HWND_TOPMOST && GetWindow (p, GW_HWNDPREV) == prev_hwnd) {
      prev_hwnd = p;
      continue;
    }
    SetWindowPos (p, prev_hwnd, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    prev_hwnd = p;
  }

  HWND folder_hwnd = active_folder_hwnd ();
  HWND chrome_layers[] = {folder_hwnd, g_chrome_hwnd, g_icon_hwnd, g_caption_hwnd};
  for (HWND h : chrome_layers) {
    if (!is_visible_hwnd (h))
      continue;
    chrome_bind_owner (h);
    ensure_hwnd_topmost (h);
    SetWindowPos (h, prev_hwnd, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    prev_hwnd = h;
  }

  for (HWND ov : overviews) {
    ensure_hwnd_topmost (ov);
    SetWindowPos (ov, prev_hwnd, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    prev_hwnd = ov;
  }

  for (HWND h : chrome_layers) {
    if (h == nullptr || !IsWindow (h) || is_visible_hwnd (h))
      continue;
    chrome_bind_owner (h);
    SetWindowPos (h, prev_hwnd, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
  }
}

// --- Cross-view drag helpers & global callbacks ------------------------------

static void
set_external_drop_highlight_all (POINT screen_pt, OverviewView *exclude)
{
  for (auto &up : g_views) {
    if (!up || up.get () == exclude)
      continue;
    up->set_external_drop_highlight (screen_pt);
  }
}

static void
clear_external_drop_highlight_all (OverviewView *exclude)
{
  for (auto &up : g_views) {
    if (!up || up.get () == exclude)
      continue;
    up->clear_external_drop_highlight ();
  }
}

static bool
hit_test_any_open_view (POINT screen_pt, OverviewView *exclude,
                        HMONITOR *out_mon, int *out_ws, bool *out_create_new)
{
  for (auto &up : g_views) {
    if (!up || up.get () == exclude || !up->is_open ())
      continue;
    int ws = -1;
    bool create = false;
    if (up->hit_test_screen_point (screen_pt, &ws, &create)) {
      if (out_mon != nullptr) *out_mon = up->resolve_monitor ();
      if (out_ws != nullptr) *out_ws = ws;
      if (out_create_new != nullptr) *out_create_new = create;
      return true;
    }
  }
  return false;
}

static void
refresh_other_views_dash (OverviewView *exclude)
{
  for (auto &up : g_views) {
    if (up && up.get () != exclude && up->is_open ())
      up->refresh_dash_in_place ();
  }
}

static bool
has_multiple_open_views (void)
{
  int n = 0;
  for (const auto &up : g_views) {
    if (up && up->is_open ()) {
      if (++n >= 2)
        return true;
    }
  }
  return false;
}

static void
sync_sibling_views_workspace_switch (OverviewView *origin, int target,
                                     int known_desktop_count)
{
  for (auto &up : g_views) {
    if (!up || up.get () == origin || !up->is_open ())
      continue;
    up->switch_workspace_animated (target, true, known_desktop_count);
  }
}

static void
cancel_post_hide_workarea (void)
{
  if (g_post_hide_workarea_timer != 0) {
    g_source_remove (g_post_hide_workarea_timer);
    g_post_hide_workarea_timer = 0;
  }
}

static gboolean
post_hide_workarea_cb (gpointer user_data G_GNUC_UNUSED)
{
  g_post_hide_workarea_timer = 0;
  if (!overview_active () && overview_is_open () == 0)
    reassert_panel_workarea_now ();
  return G_SOURCE_REMOVE;
}

static void
schedule_post_hide_workarea (guint delay_ms)
{
  cancel_post_hide_workarea ();
  g_post_hide_workarea_timer = g_timeout_add_full (
      G_PRIORITY_LOW, delay_ms, post_hide_workarea_cb, nullptr, nullptr);
}

static gboolean
overview_idle_trim_cb (gpointer user_data G_GNUC_UNUSED)
{
  g_idle_trim_timer = 0;
  if (!overview_any_open ()) {
    app_icon_cache_trim (64);
    for (auto &up : g_views) {
      if (up && up->m_app_grid_clip != nullptr) {
        WinomeAppGridClip *clip = WINOME_APP_GRID_CLIP (up->m_app_grid_clip);
        app_grid_clip_cancel_capture (clip);
        g_clear_object (&clip->cached_page0_tex);
        clip->cached_page0_version = 0;
      }
    }
  }
  return G_SOURCE_REMOVE;
}

static gboolean
dash_resync_timeout_cb (gpointer user_data G_GNUC_UNUSED)
{
  g_dash_resync_timer = 0;
  for (auto &up : g_views) {
    if (up && up->is_open ())
      up->refresh_dash_in_place ();
  }
  return G_SOURCE_REMOVE;
}

static void
schedule_dash_resync (guint delay_ms)
{
  if (g_dash_resync_timer != 0)
    g_source_remove (g_dash_resync_timer);
  g_dash_resync_timer =
      g_timeout_add (delay_ms, dash_resync_timeout_cb, nullptr);
}

static gboolean
dynamic_workspaces_startup_cb (gpointer user_data G_GNUC_UNUSED)
{
  g_dyn_ws_startup_timer = 0;
  if (!g_global_initialized || overview_any_open ())
    return G_SOURCE_REMOVE;
  HMONITOR mon = monitor_manager_primary_monitor ();
  MONITORINFO mi{};
  mi.cbSize = sizeof (mi);
  if (mon == nullptr || !GetMonitorInfoW (mon, &mi)) {
    mi.rcMonitor = RECT{0, 0, GetSystemMetrics (SM_CXSCREEN),
                        GetSystemMetrics (SM_CYSCREEN)};
  }
  sync_dynamic_workspaces (mon, mi.rcMonitor);
  return G_SOURCE_REMOVE;
}

static void
schedule_dynamic_workspaces_check (guint delay_ms)
{
  if (!g_global_initialized)
    return;
  if (g_dyn_ws_startup_timer != 0)
    g_source_remove (g_dyn_ws_startup_timer);
  g_dyn_ws_startup_timer =
      g_timeout_add (delay_ms, dynamic_workspaces_startup_cb, nullptr);
}

static void
on_overview_desktop_changed_global (HMONITOR monitor, int count,
                                    int active_index,
                                    void *user_data G_GNUC_UNUSED)
{
  if (g_suppress_ws_change_rebuild) {
    for (auto &up : g_views) {
      if (up && up->is_open () &&
          (monitor == nullptr || monitor == up->resolve_monitor ())) {
        up->ensure_overview_visible_on_active_desktop (active_index);
        up->schedule_vd_cloak_guard (60);
      }
    }
    return;
  }
  bool any_handled = false;
  for (auto &up : g_views) {
    if (up && up->is_open ()) {
      any_handled = true;
      up->on_desktop_changed (monitor, count, active_index);
    }
  }
  if (!any_handled && g_global_initialized && !g_in_dynamic_ws_sync)
    schedule_dynamic_workspaces_check (250);
}

static void
on_overview_config_changed_global (void *user_data G_GNUC_UNUSED)
{
  const bool linked = winome::get_config ().multi_monitor_overview_linked;
  if (linked != g_last_overview_linked) {
    g_last_overview_linked = linked;
    if (linked && overview_any_open ()) {
      // Toggling linked mode ON while at least one monitor is open expands
      // all remaining monitors to match the linked invariant.
      const auto monitors = enumerate_monitors ();
      for (const auto &m : monitors) {
        OverviewView *v = get_or_create_view (m.hmonitor);
        if (v != nullptr && !v->is_open ())
          v->show_view (false, false);
      }
      restack_locked ();
    }
  }

  for (auto &up : g_views) {
    if (up && up->is_open ())
      up->refresh_dash_in_place ();
  }

  const std::string live_mode =
      sanitize_live_wallpaper_mode (winome::get_config ().live_wallpaper_mode);
  if (live_mode == "off") {
    OverviewView *prev_owner = g_live_wallpaper_owner;
    const bool was_live = g_using_live_wallpaper;
    g_using_live_wallpaper = false;
    g_live_wallpaper_owner = nullptr;
    live_wallpaper_stop ();
    g_clear_object (&g_live_wallpaper);
    g_clear_object (&g_live_wallpaper_thumb);
    if (was_live && prev_owner != nullptr) {
      if (prev_owner->m_ws_bg != nullptr &&
          gtk_widget_get_visible (prev_owner->m_ws_bg))
        gtk_widget_queue_draw (prev_owner->m_ws_bg);
      if (prev_owner->m_thumbs != nullptr)
        overview_thumbs_set_wallpaper (
            prev_owner->m_thumbs, prev_owner->active_wallpaper_thumb_texture ());
    }
  } else if (OverviewView *v = active_chrome_or_focused_view ()) {
    HMONITOR mon = v->resolve_monitor ();
    MONITORINFO mi{};
    mi.cbSize = sizeof (mi);
    if (GetMonitorInfoW (mon, &mi)) {
      LiveWallpaperTarget target = live_wallpaper_detect_target (mi.rcMonitor);
      if (target.active) {
        g_using_live_wallpaper = true;
        g_live_wallpaper_owner = v;
        live_wallpaper_set_paused (v->m_anim.source != 0 ||
                                   v->m_switch_source != 0);
        live_wallpaper_start (target, mi.rcMonitor, live_mode == "live",
                              on_live_wallpaper_frame, nullptr);
      }
    }
  }
}

static void on_app_index_ready (void *user_data G_GNUC_UNUSED);

static gboolean
deferred_app_index_ready_cb (gpointer user_data G_GNUC_UNUSED)
{
  g_app_index_defer_timer = 0;
  on_app_index_ready (nullptr);
  return G_SOURCE_REMOVE;
}

static void
on_app_index_ready (void *user_data G_GNUC_UNUSED)
{
  if (!g_global_initialized || g_views.empty ())
    return;
  for (const auto &up : g_views) {
    if (up && (up->m_anim.source != 0 || up->m_switch_source != 0 ||
               up->m_search_fade_source != 0)) {
      if (g_app_index_defer_timer == 0) {
        g_app_index_defer_timer = g_timeout_add_full (
            G_PRIORITY_LOW, 350, deferred_app_index_ready_cb, nullptr, nullptr);
      }
      return;
    }
  }
  if (g_wallpaper == nullptr)
    start_wallpaper_prewarm_async ();

  // Prewarm App Grid layout on the primary view only (other monitors build on demand).
  OverviewView *v = g_views.front ().get ();
  if (v == nullptr || v->m_window == nullptr || v->m_app_grid == nullptr)
    return;
  HMONITOR monitor = v->resolve_monitor ();
  MONITORINFO mi{};
  mi.cbSize = sizeof (mi);
  if (!GetMonitorInfoW (monitor, &mi))
    return;
  int mw = mi.rcMonitor.right - mi.rcMonitor.left;
  int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
  int scale = std::max (gtk_widget_get_scale_factor (v->m_window), 1);
  int est_w = std::max (std::min (mw / scale - 32, 1820), 400);
  int est_h = std::max ((int)(mh / scale * 0.58), 260);
  if (!overview_active () && !gtk_widget_get_visible (v->m_window)) {
    int panel_h = winome_shell_panel_height ();
    HWND panel_hwnd = v->panel_hwnd_for_view ();
    RECT pr{};
    if (panel_hwnd != nullptr && IsWindowVisible (panel_hwnd) &&
        GetWindowRect (panel_hwnd, &pr) && pr.bottom > pr.top) {
      panel_h = pr.bottom - pr.top;
    }
    PlacedGeometry geo{};
    if (v->compute_geometry (monitor, mi.rcMonitor, panel_h, &geo)) {
      est_h = std::max (
          static_cast<int> (geo.app_grid_box.bottom - geo.app_grid_box.top) /
              scale,
          260);
    }
  }
  app_grid_clip_set_bounds (v->m_app_grid_clip, est_w, est_h);
  gtk_widget_set_size_request (v->m_app_grid, est_w, est_h);
  app_grid_prewarm (v->m_app_grid, est_w, est_h);
}

static gboolean
overview_prewarm_idle_cb (gpointer user_data G_GNUC_UNUSED)
{
  g_prewarm_timer = 0;
  if (!g_global_initialized || g_views.empty ())
    return G_SOURCE_REMOVE;
  for (const auto &up : g_views) {
    if (up && (up->m_anim.source != 0 || up->m_switch_source != 0 ||
               up->m_search_fade_source != 0)) {
      g_prewarm_timer = g_timeout_add_full (
          G_PRIORITY_LOW, 450, overview_prewarm_idle_cb, nullptr, nullptr);
      return G_SOURCE_REMOVE;
    }
  }
  if (g_prewarm_step == 0) {
    if (g_wallpaper == nullptr)
      start_wallpaper_prewarm_async ();
    g_prewarm_step = 1;
    g_prewarm_timer = g_timeout_add_full (
        G_PRIORITY_LOW, 320, overview_prewarm_idle_cb, nullptr, nullptr);
  } else if (g_prewarm_step == 1) {
    // Stagger-create and pre-realize views for ALL connected monitors so even
    // the very first secondary-monitor or linked open has zero GL cold-start.
    const auto mons = enumerate_monitors ();
    bool realized_one = false;
    for (const auto &m : mons) {
      OverviewView *v = get_or_create_view (m.hmonitor);
      if (v != nullptr && v->m_window != nullptr &&
          !gtk_widget_get_visible (v->m_window) &&
          !gtk_widget_get_realized (v->m_window)) {
        gtk_widget_realize (v->m_window);
        realized_one = true;
        break;
      }
    }
    if (!realized_one)
      g_prewarm_step = 2;
    g_prewarm_timer = g_timeout_add_full (
        G_PRIORITY_LOW, 280, overview_prewarm_idle_cb, nullptr, nullptr);
  } else if (g_prewarm_step >= 2 && g_prewarm_step <= 4) {
    GtkWidget *chrome_wins[] = {g_chrome_window, g_icon_window, g_caption_window};
    GtkWidget *cw = chrome_wins[g_prewarm_step - 2];
    if (cw != nullptr && !gtk_widget_get_realized (cw))
      gtk_widget_realize (cw);
    ++g_prewarm_step;
    if (g_prewarm_step <= 4) {
      g_prewarm_timer = g_timeout_add_full (
          G_PRIORITY_LOW, 280, overview_prewarm_idle_cb, nullptr, nullptr);
    }
  }
  return G_SOURCE_REMOVE;
}

static gboolean
show_idle (gpointer user_data G_GNUC_UNUSED)
{
  if (overview_active ())
    overview_show ();
  return G_SOURCE_REMOVE;
}

static gboolean
hide_idle (gpointer user_data G_GNUC_UNUSED)
{
  if (!overview_active ())
    overview_hide_all ();
  return G_SOURCE_REMOVE;
}

static gboolean
shift_app_grid_idle (gpointer user_data G_GNUC_UNUSED)
{
  g_print ("[trig] double win-key -> app-grid\n");
  OverviewView *v = active_chrome_or_focused_view ();
  if (!overview_active () || v == nullptr || v->m_window == nullptr) {
    g_pending_shift_app_grid.store (false, std::memory_order_release);
    return G_SOURCE_REMOVE;
  }
  if (!gtk_widget_get_visible (v->m_window) || !v->m_placed) {
    g_pending_shift_app_grid.store (true, std::memory_order_release);
    v->m_pending_show_state = ControlsState::APP_GRID;
    g_atomic_can_shift_app_grid.store (false, std::memory_order_release);
    return G_SOURCE_REMOVE;
  }
  g_pending_shift_app_grid.store (false, std::memory_order_release);
  if (v->m_target_state == ControlsState::HIDDEN)
    return G_SOURCE_REMOVE;
  if (v->m_target_state == ControlsState::WINDOW_PICKER)
    v->start_state_transition (ControlsState::APP_GRID, kOpenMs);
  restack_locked ();
  if (v->can_auto_focus_overview_search () &&
      GetForegroundWindow () == v->m_hwnd) {
    v->ensure_search_entry_focus ();
  }
  return G_SOURCE_REMOVE;
}

static void
capture_prev_foreground_if_opening_first (void)
{
  if (overview_any_open ())
    return;
  HWND fg = GetForegroundWindow ();
  g_restore_focus_discarded.store (false, std::memory_order_release);
  g_prev_foreground_hwnd = is_external_foreground_target (fg) ? fg : nullptr;
}

}  // namespace

// --- Public winome:: Overview API (unified multi-monitor engine) -------------

void
overview_init (HWND panel_hwnd)
{
  g_panel_hwnd = panel_hwnd;
  if (g_global_initialized)
    return;
  g_global_initialized = true;
  g_overview_alive = std::make_shared<std::atomic<bool>> (true);
  g_prewarm_step = 0;

  app_index_refresh_async (on_app_index_ready, nullptr);

  chrome_init ();
  icon_chrome_init ();
  caption_chrome_init ();

  ws::add_change_listener (on_overview_desktop_changed_global, nullptr);
  g_last_overview_linked = winome::get_config ().multi_monitor_overview_linked;
  add_config_listener (on_overview_config_changed_global, nullptr);

  // Pre-create initial view on the primary monitor for fast warm startup.
  HMONITOR initial_mon = monitor_manager_primary_monitor ();
  if (initial_mon == nullptr && panel_hwnd != nullptr && IsWindow (panel_hwnd))
    initial_mon = MonitorFromWindow (panel_hwnd, MONITOR_DEFAULTTOPRIMARY);
  if (initial_mon != nullptr)
    get_or_create_view (initial_mon);

  if (g_prewarm_timer != 0)
    g_source_remove (g_prewarm_timer);
  g_prewarm_timer = g_timeout_add_full (
      G_PRIORITY_LOW, 120, overview_prewarm_idle_cb, nullptr, nullptr);
  schedule_dynamic_workspaces_check (1600);
}

void
overview_shutdown (void)
{
  if (!g_global_initialized)
    return;
  g_global_initialized = false;
  if (g_overview_alive)
    g_overview_alive->store (false, std::memory_order_release);

  ws::remove_change_listener (on_overview_desktop_changed_global, nullptr);
  remove_config_listener (on_overview_config_changed_global, nullptr);

  cancel_post_hide_workarea ();
  if (g_idle_trim_timer != 0) {
    g_source_remove (g_idle_trim_timer);
    g_idle_trim_timer = 0;
  }
  if (g_prewarm_timer != 0) {
    g_source_remove (g_prewarm_timer);
    g_prewarm_timer = 0;
  }
  if (g_app_index_defer_timer != 0) {
    g_source_remove (g_app_index_defer_timer);
    g_app_index_defer_timer = 0;
  }
  if (g_dyn_ws_startup_timer != 0) {
    g_source_remove (g_dyn_ws_startup_timer);
    g_dyn_ws_startup_timer = 0;
  }
  if (g_dash_resync_timer != 0) {
    g_source_remove (g_dash_resync_timer);
    g_dash_resync_timer = 0;
  }

  for (auto &up : g_views) {
    if (up)
      up->destroy_view ();
  }
  g_views.clear ();
  g_focused_view = nullptr;
  g_chrome_owner_view = nullptr;
  g_live_wallpaper_owner = nullptr;

  winome_shell_panel_set_overview_progress (0.0);
  winome_shell_panel_set_workspace_scroll_for_monitor (nullptr, -1, -1.0);
  live_wallpaper_stop ();
  g_using_live_wallpaper = false;
  g_wallpaper_cache = WallpaperCacheKey{};
  clear_wallpaper_surfaces ();
  g_clear_object (&g_wallpaper);
  g_clear_object (&g_wallpaper_thumb);
  g_clear_object (&g_live_wallpaper);
  g_clear_object (&g_live_wallpaper_thumb);

  chrome_detach_owners ();
  chrome_cancel_apply (&g_chrome_hooks);
  chrome_cancel_apply (&g_icon_hooks);
  chrome_cancel_apply (&g_caption_hooks);
  chrome_disconnect_after_paint (&g_chrome_hooks);
  chrome_disconnect_after_paint (&g_icon_hooks);
  chrome_disconnect_after_paint (&g_caption_hooks);
  g_chrome_target = nullptr;
  g_icon_target = nullptr;
  g_chrome_button = nullptr;
  g_icon_picture = nullptr;
  g_caption = nullptr;
  g_caption_wrap = nullptr;
  g_chrome_hwnd = nullptr;
  g_icon_hwnd = nullptr;
  g_caption_hwnd = nullptr;
  GtkWidget *chrome_win = g_chrome_window;
  GtkWidget *icon_win = g_icon_window;
  GtkWidget *caption_win = g_caption_window;
  g_chrome_window = nullptr;
  g_icon_window = nullptr;
  g_caption_window = nullptr;
  if (chrome_win != nullptr) gtk_window_destroy (GTK_WINDOW (chrome_win));
  if (icon_win != nullptr) gtk_window_destroy (GTK_WINDOW (icon_win));
  if (caption_win != nullptr) gtk_window_destroy (GTK_WINDOW (caption_win));
  g_prev_foreground_hwnd = nullptr;
  g_restore_focus_discarded.store (false, std::memory_order_release);
}

void
overview_set_target_monitor (HMONITOR monitor)
{
  g_requested_monitor.store (monitor, std::memory_order_release);
}

HMONITOR
overview_target_monitor (void)
{
  if (g_focused_view != nullptr && g_focused_view->is_open ())
    return g_focused_view->resolve_monitor ();
  HMONITOR req = g_requested_monitor.load (std::memory_order_acquire);
  if (req != nullptr)
    return req;
  if (OverviewView *any = find_any_open_view ())
    return any->resolve_monitor ();
  return monitor_manager_primary_monitor ();
}

void
overview_show_for_monitor (HMONITOR monitor)
{
  if (!g_global_initialized)
    return;
  HMONITOR mon = is_valid_monitor (monitor) ? monitor
                                            : monitor_manager_active_monitor ();
  OverviewView *v = get_or_create_view (mon);
  if (v == nullptr)
    return;
  capture_prev_foreground_if_opening_first ();
  const bool shift_requested =
      g_pending_shift_app_grid.exchange (false, std::memory_order_acq_rel);
  v->show_view (shift_requested, true);
}

void
overview_show_all_linked (HMONITOR focus_monitor)
{
  if (!g_global_initialized)
    return;
  const auto monitors = enumerate_monitors ();
  if (monitors.empty ())
    return;
  HMONITOR focus_mon = is_valid_monitor (focus_monitor)
                           ? focus_monitor
                           : monitor_manager_active_monitor ();
  if (!is_valid_monitor (focus_mon))
    focus_mon = monitors.front ().hmonitor;

  capture_prev_foreground_if_opening_first ();
  const bool shift_requested =
      g_pending_shift_app_grid.exchange (false, std::memory_order_acq_rel);

  // Capture ONE global window + virtual desktop snapshot across all monitors,
  // prepare every monitor's DWM thumbnails while DWM-cloaked, then uncloak and
  // start all monitors' open animations in lockstep with zero wallpaper flash.
  SharedOpenSnapshot shared_snap = capture_shared_open_snapshot ();
  std::vector<OverviewView *> batch;
  batch.reserve (monitors.size ());

  for (const auto &m : monitors) {
    if (m.hmonitor == focus_mon)
      continue;
    if (OverviewView *v = get_or_create_view (m.hmonitor)) {
      v->show_view (false, false, true, &shared_snap);
      batch.push_back (v);
    }
  }
  OverviewView *fv = get_or_create_view (focus_mon);
  if (fv != nullptr) {
    fv->show_view (shift_requested, true, true, &shared_snap);
    batch.push_back (fv);
  }

  winome::evaluate_fullscreen_state ();
  for (OverviewView *v : batch)
    v->start_deferred_open_animation ();
  if (fv != nullptr && !fv->m_cloaked_for_open) {
    if (winome_shell_panel_top_popover_hwnd_for_monitor (focus_mon) == nullptr)
      fv->focus_overview_window (false);
    restack_locked ();
  }
}

void
overview_show (void)
{
  HMONITOR req = g_requested_monitor.load (std::memory_order_acquire);
  if (winome::get_config ().multi_monitor_overview_linked)
    overview_show_all_linked (req);
  else
    overview_show_for_monitor (req);
}

void
overview_hide_for_monitor (HMONITOR monitor)
{
  if (monitor == nullptr) {
    overview_hide_all ();
    return;
  }
  if (OverviewView *v = find_view (monitor))
    v->hide_view ();
}

void
overview_hide_all (void)
{
  g_atomic_can_shift_app_grid.store (false, std::memory_order_release);
  g_pending_shift_app_grid.store (false, std::memory_order_release);
  for (auto &up : g_views) {
    if (up && (up->is_open () ||
               (up->m_window != nullptr && gtk_widget_get_visible (up->m_window)))) {
      up->hide_view ();
    }
  }
}

void
overview_hide (void)
{
  if (winome::get_config ().multi_monitor_overview_linked) {
    overview_hide_all ();
  } else if (g_focused_view != nullptr && g_focused_view->is_open ()) {
    g_focused_view->hide_view ();
  } else {
    overview_hide_all ();
  }
}

void
overview_discard_restore_focus (void)
{
  g_restore_focus_discarded.store (true, std::memory_order_release);
  g_prev_foreground_hwnd = nullptr;
  AllowSetForegroundWindow (ASFW_ANY);
}

void
overview_restore_focus_if_active (void)
{
  if (OverviewView *v = active_chrome_or_focused_view ())
    v->restore_focus_if_active ();
}

void
overview_restack (void)
{
  restack_locked ();
}

int
overview_is_open (void)
{
  return overview_any_open () ? 1 : 0;
}

bool
overview_is_open_for_monitor (HMONITOR monitor)
{
  if (monitor == nullptr)
    return overview_any_open ();
  return find_open_view (monitor) != nullptr;
}

bool
overview_any_open (void)
{
  return find_any_open_view () != nullptr;
}

bool
overview_is_hwnd (HWND hwnd)
{
  if (hwnd == nullptr)
    return false;
  return is_any_overview_view_hwnd (hwnd) || hwnd == g_chrome_hwnd ||
         hwnd == g_icon_hwnd || hwnd == g_caption_hwnd;
}

bool
overview_handle_escape (HMONITOR preferred_monitor)
{
  OverviewView *v = find_open_view (preferred_monitor);
  if (v == nullptr && g_focused_view != nullptr && g_focused_view->is_open ())
    v = g_focused_view;
  if (v == nullptr)
    v = find_any_open_view ();
  if (v == nullptr)
    return false;
  return v->handle_escape ();
}

void
overview_refresh_all_open (void)
{
  int open_count = 0;
  for (const auto &up : g_views) {
    if (up && up->is_open ())
      ++open_count;
  }
  if (open_count == 0)
    return;
  if (open_count >= 2) {
    SharedOpenSnapshot snap = capture_shared_open_snapshot ();
    for (auto &up : g_views) {
      if (up && up->is_open ())
        up->rebuild_now (&snap);
    }
    return;
  }
  for (auto &up : g_views) {
    if (up && up->is_open ())
      up->rebuild_now ();
  }
}

void
overview_on_external_window_drag_start (HWND hwnd)
{
  g_ext_drag_hwnd = nullptr;
  if (hwnd == nullptr || !IsWindow (hwnd) || !IsWindowVisible (hwnd))
    return;
  DWORD pid = 0;
  GetWindowThreadProcessId (hwnd, &pid);
  if (pid == 0 || pid == GetCurrentProcessId ())
    return;
  LONG_PTR ex = GetWindowLongPtrW (hwnd, GWL_EXSTYLE);
  if ((ex & WS_EX_TOOLWINDOW) != 0)
    return;
  if (is_overview_default_blacklisted_hwnd (
          hwnd, get_config ().overview_window_blacklist)) {
    return;
  }
  g_ext_drag_hwnd = hwnd;
  GetWindowRect (hwnd, &g_ext_drag_start_rect);
}

void
overview_on_external_window_drag_move (HWND hwnd)
{
  if (hwnd == nullptr || hwnd != g_ext_drag_hwnd || !overview_any_open ())
    return;
  POINT pt{};
  if (!GetCursorPos (&pt))
    return;
  set_external_drop_highlight_all (pt, nullptr);
}

void
overview_on_external_window_drag_end (HWND hwnd)
{
  clear_external_drop_highlight_all (nullptr);

  if (hwnd == nullptr || hwnd != g_ext_drag_hwnd) {
    if (hwnd == g_ext_drag_hwnd)
      g_ext_drag_hwnd = nullptr;
    return;
  }
  g_ext_drag_hwnd = nullptr;

  if (!overview_any_open () || !IsWindow (hwnd))
    return;

  RECT end_rect{};
  if (GetWindowRect (hwnd, &end_rect) &&
      end_rect.left == g_ext_drag_start_rect.left &&
      end_rect.top == g_ext_drag_start_rect.top) {
    return;
  }

  POINT pt{};
  if (!GetCursorPos (&pt))
    return;

  bool moved = false;
  HMONITOR dst_mon = nullptr;
  int dst_ws = -1;
  bool dst_create = false;
  g_suppress_ws_change_rebuild = true;
  if (hit_test_any_open_view (pt, nullptr, &dst_mon, &dst_ws, &dst_create)) {
    if (dst_create && ws::create_workspace (dst_mon)) {
      const int n = ws::count_for_monitor (dst_mon);
      if (n > 0)
        dst_ws = n - 1;
    }
    moved = ws::transfer_window_to_monitor_workspace (hwnd, dst_mon, dst_ws,
                                                      pt, false);
  }
  g_suppress_ws_change_rebuild = false;

  if (moved) {
    overview_refresh_all_open ();
    if (OverviewView *dv = find_open_view (dst_mon)) {
      dv->focus_overview_window (false);
      restack_locked ();
    }
    schedule_dash_resync (120);
  }
}

int
overview_workspace_count (void)
{
  OverviewView *v = active_chrome_or_focused_view ();
  if (v != nullptr)
    return static_cast<int> (v->m_desktops.size ());
  if (!g_views.empty () && g_views.front ())
    return static_cast<int> (g_views.front ()->m_desktops.size ());
  return 0;
}

void
overview_show_async (void)
{
  g_atomic_can_shift_app_grid.store (true, std::memory_order_release);
  g_main_context_invoke (nullptr, show_idle, nullptr);
}

void
overview_hide_async (void)
{
  g_atomic_can_shift_app_grid.store (false, std::memory_order_release);
  g_pending_shift_app_grid.store (false, std::memory_order_release);
  g_main_context_invoke (nullptr, hide_idle, nullptr);
}

void
overview_invalidate_wallpaper (void)
{
  load_wallpaper (true);
  for (auto &up : g_views) {
    if (!up)
      continue;
    if (up->m_thumbs != nullptr)
      overview_thumbs_set_wallpaper (up->m_thumbs,
                                     up->active_wallpaper_thumb_texture ());
    if (up->m_window != nullptr && gtk_widget_get_visible (up->m_window))
      gtk_widget_queue_draw (up->m_window);
  }
}

void
overview_reposition (void)
{
  prune_disconnected_views ();
  for (auto &up : g_views) {
    if (up)
      up->reposition_view ();
  }
}

void
overview_toggle_app_grid (void)
{
  if (OverviewView *v = active_chrome_or_focused_view ())
    v->toggle_app_grid ();
}

void
overview_open_app_grid_on_monitor (HMONITOR monitor)
{
  if (monitor == nullptr)
    return;
  if (OverviewView *v = find_open_view (monitor)) {
    v->toggle_app_grid ();
    return;
  }
  g_pending_shift_app_grid.store (true, std::memory_order_release);
  overview_show_for_monitor (monitor);
}

void
overview_shift_to_app_grid_async (void)
{
  g_pending_shift_app_grid.store (true, std::memory_order_release);
  g_atomic_can_shift_app_grid.store (false, std::memory_order_release);
  g_idle_add_full (G_PRIORITY_HIGH_IDLE, shift_app_grid_idle, nullptr, nullptr);
}

bool
overview_can_shift_to_app_grid (void)
{
  return g_atomic_can_shift_app_grid.load (std::memory_order_acquire);
}

}  // namespace winome
