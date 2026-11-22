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
#include "app-grid.h"
#include "app-index.h"
#include "config.h"
#include "easing.h"
#include "fullscreen-watcher.h"
#include "host-utils.h"
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

static int
close_button_size ()
{
  StEngine *engine = StEngine::get ();
  if (engine != nullptr) {
    StEngine::Node node (*engine, nullptr, "", "window-close", "");
    double v = 0;
    if (node.lookup_length ("width", &v) && v > 0)
      return (int)(v + 0.5);
  }
  return kCloseButtonSize;
}

static int
close_icon_size ()
{
  StEngine *engine = StEngine::get ();
  if (engine != nullptr) {
    StEngine::Node node (*engine, nullptr, "", "window-close", "");
    double v = 0;
    if (node.lookup_length ("icon-size", &v) && v > 0)
      return (int)(v + 0.5);
  }
  return kCloseIconSize;
}

static double
window_picker_spacing ()
{
  StEngine *engine = StEngine::get ();
  if (engine != nullptr) {
    StEngine::Node node (*engine, nullptr, "", "window-picker", "");
    double v = 0;
    if (node.lookup_length ("spacing", &v) && v >= 0)
      return v;
  }
  return kWindowPickerSpacing;
}

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

HWND g_panel_hwnd = nullptr;
GtkWidget *g_window = nullptr;
HWND g_overview_hwnd = nullptr;
HWND g_prev_foreground_hwnd = nullptr;
std::atomic<bool> g_restore_focus_discarded{false};
int g_panel_height = 0;   // physical px; clicks above it are panel's

// UI pieces.
GtkWidget *g_root = nullptr;      // GtkFixed named "overviewGroup"
GtkWidget *g_focus_sink = nullptr; // 0x0 focus sink so GtkEntry isn't auto-focused
GtkWidget *g_search = nullptr;    // GtkEntry.search-entry
GtkWidget *g_search_view = nullptr;  // #searchResults (overview-search.cpp)
GtkWidget *g_ws_bg = nullptr;     // workspace-background widget
GtkWidget *g_app_grid_clip = nullptr;  // clipped ControlsManager box for App Grid
GtkWidget *g_app_grid = nullptr;       // paginated App Grid overlay
GtkWidget *g_dash = nullptr;      // overview dash
GtkWidget *g_thumbs = nullptr;    // .workspace-thumbnails row
GtkWidget *g_caption = nullptr;   // .window-caption hover pill
GdkTexture *g_wallpaper = nullptr;
GdkTexture *g_wallpaper_thumb = nullptr; // downscaled <=320x180 RGB for g_thumbs
GdkTexture *g_live_wallpaper = nullptr;
GdkTexture *g_live_wallpaper_thumb = nullptr;
bool g_using_live_wallpaper = false;
bool g_app_grid_mode = false;     // true when state is APP_GRID
bool g_search_mode = false;       // true when #searchResults is active
bool g_folder_dialog_open = false;// true when App Grid folder dialog is open
enum class ControlsState {
  HIDDEN = 0,
  WINDOW_PICKER = 1,
  APP_GRID = 2,
};
constexpr double kGridSlideOffsetPx = 48.0;
ControlsState g_target_state = ControlsState::HIDDEN;
ControlsState g_pending_show_state = ControlsState::WINDOW_PICKER;
std::atomic<bool> g_atomic_can_shift_app_grid{false};
std::atomic<bool> g_pending_shift_app_grid{false};
WNDPROC g_orig_overview_wndproc = nullptr;
bool g_overview_surface_hooked = false;
bool g_placed = false;
bool g_thumbs_has_pills = false;
int g_grid_x = 0, g_grid_y = 0, g_grid_w = 0, g_grid_h = 0;
double g_thumbs_cur_opacity = 0.0;
double g_thumbs_cur_scale = 1.0;
double g_thumbs_cur_ty = 0.0;
double g_grid_cur_opacity = 0.0;
double g_grid_cur_slide_y = kGridSlideOffsetPx;
double g_panel_cur_progress = 0.0;

// Drag-to-desktop state for window previews (dnd.js _Draggable).
int g_drag_slot = -1;
HWND g_drag_hwnd = nullptr;
RECT g_drag_orig_rect{};
int g_drag_start_px = 0;
int g_drag_start_py = 0;
int g_drag_offset_x = 0;
int g_drag_offset_y = 0;
int g_drag_cur_px = 0;
int g_drag_cur_py = 0;
double g_drag_target_scale = 0.55;
gint64 g_drag_scale_start_us = 0;
guint g_drag_tick_source = 0;
int g_drag_snapback_slot = -1;
RECT g_drag_snapback_from{};
BYTE g_drag_snapback_from_op = 215;
gint64 g_drag_snapback_start_us = 0;
guint g_drag_snapback_source = 0;
int g_drag_drop_ws_peek = -1;
bool g_drag_active = false;
bool g_drag_just_ended = false;
bool g_drag_thumbs_revealed = false;
guint g_dyn_ws_startup_timer = 0;
guint g_dash_resync_timer = 0;
std::string g_search_query;       // trimmed lowercased query for active checks
std::string g_search_raw_text;    // case-preserving raw search text

// Smooth 200ms cross-fade between Overview/AppGrid and #searchResults
// (upstream overviewControls.js:623-643 _onSearchChanged).
constexpr guint kSearchFadeMs = 200;
guint g_search_fade_source = 0;
gint64 g_search_fade_start_us = 0;
double g_search_fade_from = 0.0;
double g_search_fade_to = 0.0;
double g_search_fade_cur = 0.0;

// Virtual desktops (virtual-desktop.cpp), refreshed on every placement and
// updated via centralized observer callbacks.
std::vector<vd::DesktopInfo> g_desktops;
int g_desktop_active = -1;   // index of the desktop the layout was built for
bool g_pending_desktop_sync = false;
int g_pending_desktop_target = -1;
guint g_place_opening_source = 0;
guint g_place_idle_source = 0;
guint g_close_rebuild_timer = 0;

// Stage 2 animation frame telemetry, redundant-layout guard, & idle trim.
FrameTimeTracker g_anim_tracker (20000);
int g_grid_last_moved_y = INT_MIN;
BYTE g_mini_target_opacity = 0;
double g_ws_bg_last_radius = -1.0;
bool g_ws_bg_settled_shadow_drawn = false;
guint g_idle_trim_timer = 0;
guint g_prewarm_timer = 0;
int g_prewarm_step = 0;
guint g_settle_hover_timer = 0;
guint g_post_hide_workarea_timer = 0;
guint g_app_index_defer_timer = 0;
std::shared_ptr<std::atomic<bool>> g_overview_alive;
GdkFrameClock *g_overview_clock = nullptr;
gulong g_overview_after_paint_id = 0;
bool g_dwm_frame_pending = false;
bool g_dwm_pending_has_mini = false;
BYTE g_dwm_pending_mini_opacity = 0;

struct WallpaperCacheKey {
  std::wstring path;
  FILETIME mtime{};
  uint64_t size = 0;
  int target_w = 0;
  int target_h = 0;
  bool valid = false;
};
WallpaperCacheKey g_wallpaper_cache;

void on_overview_desktop_changed (int count, int active_index, void *user_data);

// Workspace-background geometry (physical px, overview client space) and the
// animated corner radius (logical px).
RECT g_bg_full = {0, 0, 0, 0};    // HIDDEN state: the whole work area
RECT g_bg_picker = {0, 0, 0, 0};  // WINDOW_PICKER: active aspect workspace box
RECT g_bg_grid = {0, 0, 0, 0};    // APP_GRID: 15% workspace bar reference box
RECT g_bg_final = {0, 0, 0, 0};   // active workspace box (used by ws switch)
RECT g_bg_cur = {0, 0, 0, 0};     // live animated rect of active workspace
std::vector<RECT> g_ws_full_boxes;
std::vector<RECT> g_ws_picker_boxes;
std::vector<RECT> g_ws_grid_boxes;
std::vector<RECT> g_ws_cur_boxes;
std::vector<RECT> g_ws_hist1_boxes;
std::vector<RECT> g_ws_hist2_boxes;
std::vector<RECT> g_ws_anim_from_boxes;
std::vector<RECT> g_ws_anim_to_boxes;
double g_bg_radius_target = kWorkspaceRadiusFallback;
double g_radius_picker = kWorkspaceRadiusFallback;
double g_radius_grid = 12.0;
double g_bg_radius = kWorkspaceRadiusFallback;

// Close-button chrome window (separate toplevel, above the thumbnails).
GtkWidget *g_chrome_window = nullptr;
GtkWidget *g_chrome_button = nullptr;
HWND g_chrome_hwnd = nullptr;
HWND g_chrome_target = nullptr;
RECT g_chrome_want = {0, 0, 0, 0};  // desired screen rect while shown

// App-icon chrome window (hover icon above the thumbnail bottom edge).
GtkWidget *g_icon_window = nullptr;
GtkWidget *g_icon_picture = nullptr;
HWND g_icon_hwnd = nullptr;
HWND g_icon_target = nullptr;      // texture cache key (icon lookups are slow)
RECT g_icon_want = {0, 0, 0, 0};

// Window-caption chrome window (separate toplevel, above the DWM thumbnails).
GtkWidget *g_caption_window = nullptr;
GtkWidget *g_caption_wrap = nullptr;
HWND g_caption_hwnd = nullptr;
RECT g_caption_want = {0, 0, 0, 0};

// One preview slot.
struct SlotEntry {
  RECT canonical_start{};       // raw real window rect inside g_bg_full
  RECT start{};                 // HIDDEN endpoint (remapped to its workspace's full box)
  RECT canonical_picker_rect{}; // slot rect inside centered g_bg_picker
  RECT picker_rect{};           // WINDOW_PICKER slot rect (client space, on its desktop)
  RECT grid_rect{};             // APP_GRID 15% top bar slot rect (client space)
  RECT rect{};                  // active settled target rect (hover/drag/hit-test)
  RECT anim_from{};             // live snapshot at transition start
  RECT anim_to{};               // target rect for active transition
  RECT current{};               // animated rect currently set on the thumbnail
  HWND target = nullptr;
  HTHUMBNAIL thumb = nullptr;
  int desktop_index = 0;
  int z_order = 0;    // 0 = frontmost desktop window from EnumWindows
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
std::vector<SlotEntry> g_slots;

int g_hover_index = -1;

// Mini window previews inside the workspace thumbnails (WorkspaceThumbnail
// clones): one DWM thumbnail per (desktop, window) pair, laid out at the
// window's porthole position scaled into the pill, and dynamically clipped
// against g_ws_cur_boxes so they never bleed over expanding/shrinking workspace
// backgrounds in DWM.
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
std::vector<MiniSlot> g_mini_slots;

// Layout (physical px, overview client space).
int g_monitor_w = 0, g_monitor_h = 0;
int g_placed_scale = 0;
int g_placed_search_h = 0;
int g_dash_top = 0;             // top of the dash band (click exclusion)
RECT g_search_rect = {0, 0, 0, 0}; // physical; clicks here belong to the entry
RECT g_thumbs_rect = {0, 0, 0, 0}; // physical; clicks here belong to the thumbs row
RECT g_ws_box = {0, 0, 0, 0};   // physical workspace box (slot layout area)

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
Animation g_anim;

// Workspace-switch animation (workspacesView.js _scrollToActive:
// WORKSPACE_SWITCH_TIME 250ms EASE_OUT_CUBIC on the scroll adjustment —
// the whole workspace strip slides horizontally while the previous
// desktop's previews slide out and the new one slides in; the thumbnail
// indicator rides the same eased value). `g_leaving` keeps the previous
// desktop's DWM thumbnails alive for the outgoing slide.
constexpr guint kSwitchMs = 250;  // WORKSPACE_SWITCH_TIME
constexpr guint kWorkspaceScrollTimeoutMs = 150; // windowManager.js SCROLL_TIMEOUT_TIME
std::vector<SlotEntry> g_leaving;
guint g_switch_source = 0;
bool g_switch_started = false;
gint64 g_switch_start_us = 0;
double g_switch_from = -1.0, g_switch_to = -1.0, g_switch_cur_pos = 0.0;
int g_switch_width = 0;  // slide distance: workspace box width
gint64 g_last_ws_scroll_us = 0;
gint64 g_last_internal_switch_us = 0;

// --- hover chrome animation ---------------------------------------------------

// Per-slot uniform scale animations (at most one slot above 1.0 at a time:
// the hovered one; the previously hovered one shrinks back in parallel).
struct ScaleAnim {
  int index;
  double from, to;
  gint64 start_us;
};
std::vector<ScaleAnim> g_scale_anims;
guint g_scale_anim_source = 0;
int g_last_hovered_slot = -1;  // tracked for fade-out after hover leaves

// Overlay fade (shared across close button, app icon, and caption pill).
double g_overlay_opacity = 0.0;
double g_overlay_from = 0.0, g_overlay_to = 0.0;
gint64 g_overlay_anim_start = 0;
guint g_overlay_anim_ms = 0;

guint g_hover_idle_source = 0;  // WINDOW_OVERLAY_IDLE_HIDE_TIMEOUT

// Forward declarations (defined below in dependency order).
static GdkTexture *
active_wallpaper_texture (void)
{
  if (g_using_live_wallpaper && g_live_wallpaper != nullptr)
    return g_live_wallpaper;
  return g_wallpaper;
}

static GdkTexture *
active_wallpaper_thumb_texture (void)
{
  if (g_using_live_wallpaper && g_live_wallpaper_thumb != nullptr)
    return g_live_wallpaper_thumb;
  if (g_using_live_wallpaper && g_live_wallpaper != nullptr)
    return g_live_wallpaper;
  return g_wallpaper_thumb != nullptr ? g_wallpaper_thumb : g_wallpaper;
}

static void
on_live_wallpaper_frame (GdkTexture *full_tex, GdkTexture *thumb_tex,
                         void *user_data G_GNUC_UNUSED)
{
  if (full_tex == nullptr || g_window == nullptr ||
      !gtk_widget_get_visible (g_window) || !g_using_live_wallpaper) {
    return;
  }
  g_set_object (&g_live_wallpaper, full_tex);
  if (thumb_tex != nullptr)
    g_set_object (&g_live_wallpaper_thumb, thumb_tex);
  if (g_ws_bg != nullptr && gtk_widget_get_visible (g_ws_bg) &&
      !g_search_mode) {
    gtk_widget_queue_draw (g_ws_bg);
  }
  if (g_thumbs != nullptr && g_thumbs_has_pills && thumb_tex != nullptr) {
    overview_thumbs_set_wallpaper (g_thumbs, thumb_tex);
  }
}

static void place_overview_locked (bool opening, int switch_dir = 0);
static gboolean overview_place_idle (gpointer user_data);
static gboolean overview_place_opening_idle (gpointer user_data);
static void cancel_pending_place_idles (void);
static void apply_pending_desktop_sync_if_any (void);
static void set_hover (int index, bool immediate = false);
static void hover_layout_chrome (void);
static void schedule_hover_idle_hide (void);
static void cancel_hover_idle (void);
static void apply_thumb_rect (SlotEntry &e, const RECT &rect, BYTE opacity,
                              bool force = false);
static void create_previews (bool at_start_rects, LONG x_offset = 0,
                             bool reorder_existing = false);
static void destroy_mini_previews (void);
static void sync_mini_previews_visibility (BYTE opacity);
static void set_mini_opacity (BYTE opacity);
static void rebuild_now (void);
static void reset_drag_state (void);
static void cancel_delayed_rebuild (void);
static void schedule_delayed_rebuild (guint delay_ms);
static void start_state_transition (ControlsState to_state, guint duration_ms);
static void finish_hide (void);
static void clear_pending_dwm_frame (void);
static void cancel_search_fade (void);
static void cancel_settle_hover (void);
static void schedule_settle_hover (guint delay_ms);
static void cancel_post_hide_workarea (void);
static void schedule_post_hide_workarea (guint delay_ms);
static void refresh_dash_in_place (void);
static void restack_locked (void);
static void focus_overview_window (bool focus_search);
static void chrome_hide (void);
static void icon_hide (void);
static void caption_hide (void);
static void overlay_hide_all (void);
static void overlay_fade_start (double to);
static void keep_hover_alive_from_chrome (void);

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

// The chrome (icon, close button, caption) follows the SCALED preview rect
// (windowPreview.js _adjustOverlayOffsets).
RECT
slot_display_rect (int index)
{
  if (index < 0 || index >= (int)g_slots.size ())
    return RECT{0, 0, 0, 0};
  const SlotEntry &slot = g_slots[index];
  if (slot.current_scale != 1.0)
    return scale_rect_around_center (slot.rect, slot.current_scale);
  return slot.rect;
}

RECT
hover_display_rect (void)
{
  int index = g_hover_index >= 0 ? g_hover_index : g_last_hovered_slot;
  return slot_display_rect (index);
}

int
slot_at_cursor (void)
{
  if (g_overview_hwnd == nullptr || g_drag_slot >= 0 || g_drag_active ||
      g_drag_snapback_source != 0)
    return -1;
  POINT pt;
  if (!GetCursorPos (&pt) || !ScreenToClient (g_overview_hwnd, &pt))
    return -1;
  const int active_ws = std::max (g_desktop_active, 0);
  for (int i = 0; i < (int)g_slots.size (); ++i) {
    if (g_slots[i].desktop_index != active_ws)
      continue;
    const RECT r = slot_display_rect (i);
    if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom)
      return i;
  }
  return -1;
}

bool
is_external_foreground_target (HWND fg)
{
  if (fg == nullptr || !IsWindow (fg))
    return false;
  if (fg == g_overview_hwnd || fg == g_panel_hwnd)
    return false;
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

void
promote_slot_thumb_to_top (SlotEntry &e);

void
activate_slot_and_close (int index, const char *reason)
{
  overview_discard_restore_focus ();
  if (index >= 0 && index < (int)g_slots.size () &&
      IsWindow (g_slots[index].target)) {
    promote_slot_thumb_to_top (g_slots[index]);
    HWND target = g_slots[index].target;
    if (IsIconic (target))
      ShowWindow (target, SW_RESTORE);
    SetForegroundWindow (target);
  }
  close_overview (reason);
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
    if (!vd::window_on_current (hwnd) || cloaked != 0)
      return TRUE;
  } else if (!ctx->include_cloaked) {
    if (cloaked != 0)
      return TRUE;
  }

  RECT r = window_rect (hwnd);
  if (r.right - r.left <= 0 || r.bottom - r.top <= 0)
    return TRUE;

  if (MonitorFromRect (&r, MONITOR_DEFAULTTONULL) != ctx->monitor)
    return TRUE;

  OffsetRect (&r, -ctx->monitor_rect.left, -ctx->monitor_rect.top);
  ctx->windows->push_back (OvWindowInfo{hwnd, r});
  return TRUE;
}

// --- workspace background widget ----------------------------------------------

typedef struct {
  GtkWidget parent_instance;
} WinomeWorkspaceBg;

typedef struct {
  GtkWidgetClass parent_class;
} WinomeWorkspaceBgClass;

#define WINOME_WORKSPACE_BG(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), winome_workspace_bg_get_type (), \
                               WinomeWorkspaceBg))

G_DEFINE_TYPE (WinomeWorkspaceBg, winome_workspace_bg, GTK_TYPE_WIDGET)

static void
winome_workspace_bg_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  int w = gtk_widget_get_width (widget);
  int h = gtk_widget_get_height (widget);
  if (w <= 0 || h <= 0)
    return;

  int scale = std::max (gtk_widget_get_scale_factor (widget), 1);
  const bool settled = (g_anim.source == 0 && g_switch_source == 0);
  g_ws_bg_settled_shadow_drawn = settled;

  const RECT single_fallback[1] = {g_bg_cur};
  const RECT *boxes =
      !g_ws_cur_boxes.empty () ? g_ws_cur_boxes.data () : single_fallback;
  const int box_count =
      !g_ws_cur_boxes.empty () ? static_cast<int> (g_ws_cur_boxes.size ()) : 1;
  const int active_ws =
      (box_count > 1) ? std::clamp (g_desktop_active, 0, box_count - 1) : 0;

  auto draw_ws_box = [&] (int d) {
    const RECT &b = boxes[d];
    if (b.right <= g_bg_full.left || b.left >= g_bg_full.right ||
        b.bottom <= g_bg_full.top || b.top >= g_bg_full.bottom) {
      return;
    }
    float rx = static_cast<float> (b.left - g_bg_full.left) / scale;
    float ry = static_cast<float> (b.top - g_bg_full.top) / scale;
    float rw = static_cast<float> (b.right - b.left) / scale;
    float rh = static_cast<float> (b.bottom - b.top) / scale;
    if (rw <= 0.0f || rh <= 0.0f)
      return;

    graphene_rect_t rect;
    graphene_rect_init (&rect, rx, ry, rw, rh);
    const bool use_rounded = (g_bg_radius > 0.5);
    GskRoundedRect clip;
    if (use_rounded)
      gsk_rounded_rect_init_from_rect (&clip, &rect, (float)g_bg_radius);

    // Render the soft outset drop-shadow only when animations have settled:
    // re-blurring Gaussian shadow FBOs on every frame of a 250ms resize
    // causes frame drops on integrated GPUs.
    if (settled && use_rounded) {
      const GdkRGBA shadow_rgba = {0.0f, 0.0f, 0.0f, 0.2f};
      gtk_snapshot_append_outset_shadow (snapshot, &clip, &shadow_rgba,
                                         0.0f, 4.0f, 4.0f, 16.0f);
    }

    // Keep strictly ONE child node inside push_rounded_clip so GTK4's
    // GskGLRenderer stays on its single-pass rounded-texture shader fast path
    // instead of allocating a full-screen offscreen FBO on every frame.
    if (use_rounded)
      gtk_snapshot_push_rounded_clip (snapshot, &clip);
    else
      gtk_snapshot_push_clip (snapshot, &rect);

    bool painted_tex = false;
    GdkTexture *wp_tex = active_wallpaper_texture ();
    if (wp_tex != nullptr) {
      double tw = gdk_texture_get_width (wp_tex);
      double th = gdk_texture_get_height (wp_tex);
      if (tw > 0 && th > 0) {
        const double wa_h_px =
            std::max<double> (g_bg_full.bottom - g_bg_full.top, 1.0);
        const double panel_ratio =
            (g_panel_height > 0) ? (static_cast<double> (g_panel_height) / wa_h_px)
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

    if (g_drag_active && d == g_drag_drop_ws_peek && d != g_desktop_active) {
      float widths[4] = {3.0f, 3.0f, 3.0f, 3.0f};
      GdkRGBA hi = {0.208f, 0.518f, 0.894f, 0.95f};
      GdkRGBA colors[4] = {hi, hi, hi, hi};
      if (!use_rounded)
        gsk_rounded_rect_init_from_rect (&clip, &rect, 0.0f);
      gtk_snapshot_append_border (snapshot, &clip, widths, colors);
    }
  };

  // Paint inactive workspaces first and the active workspace last so expanding
  // or shrinking the active workspace never gets overlapped by a neighbor.
  for (int d = 0; d < box_count; ++d) {
    if (d != active_ws)
      draw_ws_box (d);
  }
  if (active_ws >= 0 && active_ws < box_count)
    draw_ws_box (active_ws);
}

static void
winome_workspace_bg_class_init (WinomeWorkspaceBgClass *klass)
{
  GTK_WIDGET_CLASS (klass)->snapshot = winome_workspace_bg_snapshot;
}

static void
winome_workspace_bg_init (WinomeWorkspaceBg *self)
{
  (void)self;
}

// --- clipped App Grid ControlsManager container ------------------------------

typedef struct {
  GtkWidget parent_instance;
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
app_grid_clip_new (GtkWidget *child)
{
  auto *clip = static_cast<WinomeAppGridClip *> (
      g_object_new (winome_app_grid_clip_get_type (), nullptr));
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
      g_anim.source != 0 || g_switch_source != 0 ||
      g_target_state != ControlsState::APP_GRID || g_search_mode ||
      g_folder_dialog_open) {
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

void
load_wallpaper (bool force = false)
{
  wchar_t path[MAX_PATH] = L"";
  SystemParametersInfoW (SPI_GETDESKWALLPAPER, MAX_PATH, path, 0);

  int target_w = g_monitor_w > 0 ? g_monitor_w : GetSystemMetrics (SM_CXSCREEN);
  int target_h = g_monitor_h > 0 ? g_monitor_h : GetSystemMetrics (SM_CYSCREEN);
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
        g_wallpaper_cache = {path, mtime, file_size, target_w, target_h, true};
        return;
      }
      g_clear_object (&thumb_tex);
    }
  }
  g_wallpaper_cache = WallpaperCacheKey{};
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
  if (alive && g_window != nullptr && g_wallpaper == nullptr &&
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

static void
start_wallpaper_prewarm_async (void)
{
  if (g_wallpaper != nullptr || !g_overview_alive)
    return;
  const int target_w =
      std::max (g_monitor_w > 0 ? g_monitor_w : GetSystemMetrics (SM_CXSCREEN),
                1280);
  const int target_h =
      std::max (g_monitor_h > 0 ? g_monitor_h : GetSystemMetrics (SM_CYSCREEN),
                720);
  std::shared_ptr<std::atomic<bool>> alive = g_overview_alive;
  std::thread ([alive, target_w, target_h] () {
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

static HWND
active_folder_hwnd (void)
{
  return g_app_grid != nullptr
             ? static_cast<HWND> (app_grid_get_folder_hwnd (g_app_grid))
             : nullptr;
}

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

struct ChromeSurfaceHooks {
  GtkWidget **window;
  HWND *hwnd;
  guint layout_idle;
  void (*apply_want) (void);
};

static gboolean
chrome_surface_idle_cb (gpointer user_data)
{
  auto *hooks = static_cast<ChromeSurfaceHooks *> (user_data);
  hooks->layout_idle = 0;
  if (*hooks->window != nullptr && gtk_widget_get_visible (*hooks->window)) {
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
on_chrome_window_realize (GtkWidget *widget, gpointer user_data)
{
  auto *hooks = static_cast<ChromeSurfaceHooks *> (user_data);
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (widget));
  if (surface == nullptr)
    return;
  HWND hwnd = static_cast<HWND> (gdk_win32_surface_get_handle (surface));
  *hooks->hwnd = hwnd;
  chrome_apply_base_ex_styles (hwnd);
  chrome_bind_owner (hwnd);
  ensure_hwnd_topmost (hwnd);
  g_signal_connect (surface, "layout", G_CALLBACK (on_chrome_surface_layout),
                    hooks);
}

static void
on_chrome_window_map (GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
  auto *hooks = static_cast<ChromeSurfaceHooks *> (user_data);
  chrome_schedule_apply (hooks);
}

static GtkWidget *
make_chrome_toplevel (ChromeSurfaceHooks *hooks)
{
  GtkWidget *win = gtk_window_new ();
  gtk_window_set_decorated (GTK_WINDOW (win), FALSE);
  gtk_window_set_resizable (GTK_WINDOW (win), FALSE);
  gtk_window_set_hide_on_close (GTK_WINDOW (win), TRUE);
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
on_overview_scroll (GtkEventControllerScroll *ctrl, double dx, double dy,
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
                      keep_hover_alive_from_chrome ();
                    }),
                    nullptr);
  g_signal_connect (motion, "motion",
                    G_CALLBACK (+[] (GtkEventControllerMotion *m G_GNUC_UNUSED,
                                     double x G_GNUC_UNUSED,
                                     double y G_GNUC_UNUSED,
                                     gpointer d G_GNUC_UNUSED) {
                      keep_hover_alive_from_chrome ();
                    }),
                    nullptr);
  g_signal_connect (motion, "leave",
                    G_CALLBACK (+[] (GtkEventControllerMotion *m G_GNUC_UNUSED,
                                     gpointer d G_GNUC_UNUSED) {
                      schedule_hover_idle_hide ();
                    }),
                    nullptr);
  gtk_widget_add_controller (widget, motion);

  GtkEventController *scroll = gtk_event_controller_scroll_new (
      GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  g_signal_connect (scroll, "scroll", G_CALLBACK (on_overview_scroll), nullptr);
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

// --- close-button chrome window ------------------------------------------------

void
chrome_apply_want (void)
{
  chrome_window_apply_want (g_chrome_window, g_chrome_hwnd, g_chrome_want,
                            g_panel_hwnd);
}

ChromeSurfaceHooks g_chrome_hooks{&g_chrome_window, &g_chrome_hwnd, 0,
                                  chrome_apply_want};

void
chrome_hide (void)
{
  chrome_cancel_apply (&g_chrome_hooks);
  g_chrome_target = nullptr;
  if (g_chrome_window != nullptr && gtk_widget_get_visible (g_chrome_window))
    gtk_widget_set_visible (g_chrome_window, FALSE);
}

void
on_chrome_clicked (GtkButton *btn G_GNUC_UNUSED,
                   gpointer user_data G_GNUC_UNUSED)
{
  if (g_chrome_target != nullptr && IsWindow (g_chrome_target)) {
    PostMessageW (g_chrome_target, WM_CLOSE, 0, 0);
    g_print ("[ov] close requested for slot window\n");
  }
  set_hover (-1, true);
  // Relayout after the window has (probably) closed.
  schedule_delayed_rebuild (250);
}

void
chrome_init (void)
{
  g_chrome_window = make_chrome_toplevel (&g_chrome_hooks);

  // .window-close: circular close button with 16px preview-close-symbolic
  // icon, wrapped with small shadow margins so CSS padding/border/drop-shadow
  // are never clipped by the chrome HWND bounds.
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

// Show the close button centered on the preview's top-right corner (of the
// scaled rect; windowPreview.js centers the button on both edges).
void
chrome_show_at (const RECT &preview_rect)
{
  if (g_chrome_window == nullptr || g_chrome_button == nullptr ||
      g_window == nullptr)
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

  int scale = gtk_widget_get_scale_factor (g_window);
  HMONITOR monitor = MonitorFromWindow (g_panel_hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi {};
  mi.cbSize = sizeof (mi);
  GetMonitorInfoW (monitor, &mi);

  int w = want_def_w * scale;
  int h = want_def_h * scale;
  int pad = gtk_widget_get_margin_start (g_chrome_button);
  int btn_cx = (pad + std::max (btn_nat.width, fallback) / 2) * scale;
  int btn_cy = (pad + std::max (btn_nat.height, fallback) / 2) * scale;
  g_chrome_want.left = mi.rcMonitor.left + preview_rect.right - btn_cx;
  g_chrome_want.top = mi.rcMonitor.top + preview_rect.top - btn_cy;
  g_chrome_want.right = g_chrome_want.left + w;
  g_chrome_want.bottom = g_chrome_want.top + h;

  gtk_widget_set_opacity (g_chrome_button, g_overlay_opacity);
  if (!gtk_widget_get_visible (g_chrome_window))
    gtk_widget_set_visible (g_chrome_window, TRUE);
  chrome_apply_want ();
}

// --- app-icon chrome window -----------------------------------------------------

void
icon_chrome_apply_want (void)
{
  HWND after = is_visible_hwnd (g_chrome_hwnd) ? g_chrome_hwnd : g_panel_hwnd;
  chrome_window_apply_want (g_icon_window, g_icon_hwnd, g_icon_want, after);
}

ChromeSurfaceHooks g_icon_hooks{&g_icon_window, &g_icon_hwnd, 0,
                                icon_chrome_apply_want};

void
icon_hide (void)
{
  chrome_cancel_apply (&g_icon_hooks);
  g_icon_target = nullptr;
  if (g_icon_window != nullptr && gtk_widget_get_visible (g_icon_window))
    gtk_widget_set_visible (g_icon_window, FALSE);
}

// Clicking the icon propagates to the preview upstream (the icon actor has
// no click action of its own): activate the window and leave the overview.
void
on_icon_click (GtkGestureClick *gesture G_GNUC_UNUSED,
               int n_press G_GNUC_UNUSED, double x G_GNUC_UNUSED,
               double y G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
  int index = g_hover_index >= 0 ? g_hover_index : g_last_hovered_slot;
  activate_slot_and_close (index, "ov-click-icon");
}

constexpr int kIconShadowPad = 6; // Room for .icon-dropshadow blur/offset

void
icon_chrome_init (void)
{
  g_icon_window = make_chrome_toplevel (&g_icon_hooks);

  // The 64px application icon straddling the preview's bottom edge
  // (ICON_SIZE / ICON_OVERLAP), letterboxed inside its square and padded for
  // its CSS drop shadow.
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

void
icon_show_at (const RECT &preview_rect, HWND target)
{
  if (g_icon_window == nullptr || target == nullptr)
    return;

  int scale = gtk_widget_get_scale_factor (g_window);
  HMONITOR monitor = MonitorFromWindow (g_panel_hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi {};
  mi.cbSize = sizeof (mi);
  GetMonitorInfoW (monitor, &mi);

  int size = kWindowIconSize * scale;
  int pad = kIconShadowPad * scale;
  double cx = (preview_rect.left + preview_rect.right) / 2.0;
  // ICON_OVERLAP: 70% of the icon overlaps the preview's bottom edge.
  double top = preview_rect.bottom - kIconOverlap * size;

  g_icon_want.left = mi.rcMonitor.left + (LONG)(cx - size / 2.0) - pad;
  g_icon_want.top = mi.rcMonitor.top + (LONG)top - pad;
  g_icon_want.right = g_icon_want.left + size + 2 * pad;
  g_icon_want.bottom = g_icon_want.top + size + 2 * pad;

  // Icon at >= 64 physical px (WM_GETICON / shell item factory); cached per
  // target — the lookup is far too slow for the per-frame hover animation.
  if (g_icon_target != target) {
    GdkTexture *texture = winome_window_app_icon (target, size);
    gtk_picture_set_paintable (GTK_PICTURE (g_icon_picture),
                               GDK_PAINTABLE (texture));
    g_clear_object (&texture);
    g_icon_target = target;
  }

  gtk_widget_set_opacity (g_icon_picture, g_overlay_opacity);
  if (!gtk_widget_get_visible (g_icon_window))
    gtk_widget_set_visible (g_icon_window, TRUE);
  icon_chrome_apply_want ();
}

// --- window-caption chrome window -----------------------------------------------

void
caption_chrome_apply_want (void)
{
  HWND after = is_visible_hwnd (g_icon_hwnd)
                   ? g_icon_hwnd
                   : (is_visible_hwnd (g_chrome_hwnd) ? g_chrome_hwnd
                                                      : g_panel_hwnd);
  chrome_window_apply_want (g_caption_window, g_caption_hwnd, g_caption_want,
                            after);
}

ChromeSurfaceHooks g_caption_hooks{&g_caption_window, &g_caption_hwnd, 0,
                                   caption_chrome_apply_want};

void
caption_hide (void)
{
  chrome_cancel_apply (&g_caption_hooks);
  if (g_caption_window != nullptr &&
      gtk_widget_get_visible (g_caption_window))
    gtk_widget_set_visible (g_caption_window, FALSE);
}

void
on_caption_click (GtkGestureClick *gesture G_GNUC_UNUSED,
                  int n_press G_GNUC_UNUSED, double x G_GNUC_UNUSED,
                  double y G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
  int index = g_hover_index >= 0 ? g_hover_index : g_last_hovered_slot;
  activate_slot_and_close (index, "ov-click-caption");
}

void
caption_chrome_init (void)
{
  g_caption_window = make_chrome_toplevel (&g_caption_hooks);
  // Keep resizable so gtk_window_set_default_size() and Win32 SetWindowPos()
  // resize GTK4's surface cleanly when switching between short and long window
  // titles (non-resizable GtkWindow caches its empty-text realized width and
  // ellipsizes subsequent titles to "W..." pinned to the left edge).
  gtk_window_set_resizable (GTK_WINDOW (g_caption_window), TRUE);

  g_caption = gtk_label_new ("");
  gtk_widget_add_css_class (g_caption, "window-caption");
  gtk_label_set_single_line_mode (GTK_LABEL (g_caption), TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (g_caption), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (g_caption), 40);
  gtk_widget_set_halign (g_caption, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (g_caption, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (g_caption, TRUE);

  // Centered wrapper ensures the pill stays dead-center beneath the preview
  // app icon even during transient surface-resize frames.
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

void
caption_show_at (const RECT &preview_rect)
{
  if (g_caption_window == nullptr || g_caption == nullptr ||
      g_caption_wrap == nullptr || g_window == nullptr)
    return;

  const char *txt = gtk_label_get_text (GTK_LABEL (g_caption));
  if (txt == nullptr || txt[0] == '\0') {
    caption_hide ();
    return;
  }

  int scale = std::max (gtk_widget_get_scale_factor (g_window), 1);
  HMONITOR monitor = MonitorFromWindow (g_panel_hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{};
  mi.cbSize = sizeof (mi);
  GetMonitorInfoW (monitor, &mi);

  int mon_w_logical =
      std::max ((int)(mi.rcMonitor.right - mi.rcMonitor.left) / scale, 120);
  int preview_w_logical =
      std::max ((int)(preview_rect.right - preview_rect.left) / scale, 120);
  int max_cap_w = std::min (mon_w_logical - 24,
                            std::max (preview_w_logical + 96, 280));

  gtk_widget_set_size_request (g_caption, -1, -1);
  gtk_widget_set_size_request (g_caption_wrap, -1, -1);
  int min_w = 0, nat_w = 0;
  gtk_widget_measure (g_caption, GTK_ORIENTATION_HORIZONTAL, -1, &min_w, &nat_w,
                      nullptr, nullptr);
  if (nat_w <= 0) {
    caption_hide ();
    return;
  }
  int want_def_w = std::clamp (nat_w + 4, std::min (min_w + 4, max_cap_w),
                               std::max (max_cap_w, 32));
  int min_h = 0, nat_h = 0;
  gtk_widget_measure (g_caption, GTK_ORIENTATION_VERTICAL, want_def_w, &min_h,
                      &nat_h, nullptr, nullptr);
  int want_def_h = std::max (nat_h, 1);

  gtk_widget_set_size_request (g_caption_wrap, want_def_w, want_def_h);
  int cur_def_w = 0, cur_def_h = 0;
  gtk_window_get_default_size (GTK_WINDOW (g_caption_window), &cur_def_w,
                               &cur_def_h);
  if (cur_def_w != want_def_w || cur_def_h != want_def_h) {
    gtk_window_set_default_size (GTK_WINDOW (g_caption_window), want_def_w,
                                 want_def_h);
    gtk_widget_queue_resize (g_caption_window);
  }

  int w = want_def_w * scale;
  int h = want_def_h * scale;
  double cx = (preview_rect.left + preview_rect.right) / 2.0;
  double top = preview_rect.bottom +
               (kWindowIconSize * (1.0 - kIconOverlap) + kIconTitleSpacing) *
                   scale;

  LONG left = mi.rcMonitor.left + static_cast<LONG> (std::lround (cx - w / 2.0));
  LONG top_px = mi.rcMonitor.top + static_cast<LONG> (std::lround (top));
  LONG min_left = mi.rcMonitor.left + 8 * scale;
  LONG max_left = std::max (min_left, mi.rcMonitor.right - 8 * scale - w);
  left = std::clamp (left, min_left, max_left);
  LONG max_top = std::max (mi.rcMonitor.top, mi.rcMonitor.bottom - 8 * scale - h);
  top_px = std::min (top_px, max_top);

  g_caption_want.left = left;
  g_caption_want.top = top_px;
  g_caption_want.right = left + w;
  g_caption_want.bottom = top_px + h;

  gtk_widget_set_opacity (g_caption, g_overlay_opacity);
  if (!gtk_widget_get_visible (g_caption_window))
    gtk_widget_set_visible (g_caption_window, TRUE);
  caption_chrome_apply_want ();
}

// --- hover (caption + close button + icon) ---------------------------------------

void
overlay_hide_all (void)
{
  chrome_hide ();
  icon_hide ();
  caption_hide ();
}

void
keep_hover_alive_from_chrome (void)
{
  if (g_app_grid_mode || g_search_mode || g_search_fade_cur > 0.005) {
    set_hover (-1, true);
    return;
  }
  cancel_hover_idle ();
  if (g_hover_index < 0 && g_last_hovered_slot >= 0 &&
      g_last_hovered_slot < (int)g_slots.size ()) {
    set_hover (g_last_hovered_slot);
  } else if (g_hover_index >= 0 && g_overlay_to < 1.0) {
    overlay_fade_start (1.0);
    hover_layout_chrome ();
  }
}

// Position the close button, app icon, and caption pill against the current
// scaled preview rect.
void
hover_layout_chrome (void)
{
  int index = g_hover_index >= 0 ? g_hover_index : g_last_hovered_slot;
  if (g_app_grid_mode || g_search_mode || g_search_fade_cur > 0.005 ||
      index < 0 || index >= (int)g_slots.size () ||
      !IsWindow (g_slots[index].target) ||
      (g_overlay_opacity <= 0.001 && g_overlay_to <= 0.01)) {
    overlay_hide_all ();
    return;
  }
  GtkWidget *chrome_wins[] = {g_chrome_window, g_icon_window, g_caption_window};
  for (GtkWidget *cw : chrome_wins) {
    if (cw != nullptr && !gtk_widget_get_realized (cw))
      gtk_widget_realize (cw);
  }

  const SlotEntry &slot = g_slots[index];
  RECT disp = hover_display_rect ();
  g_chrome_target = slot.target;
  chrome_show_at (disp);
  icon_show_at (disp, slot.target);
  caption_show_at (disp);
  restack_locked ();
}

// Frame-clock driver for the hover scale + caption fade animations: locked
// to the display refresh instead of a fixed 16ms timer beating against it.
gboolean
hover_anim_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
  (void)widget;
  (void)user_data;
  if (g_app_grid_mode || g_search_mode || g_search_fade_cur > 0.005 ||
      g_anim.source != 0 || g_switch_source != 0 ||
      g_target_state == ControlsState::HIDDEN) {
    g_scale_anims.clear ();
    g_hover_index = -1;
    g_last_hovered_slot = -1;
    g_overlay_opacity = 0.0;
    g_overlay_from = 0.0;
    g_overlay_to = 0.0;
    overlay_hide_all ();
    g_scale_anim_source = 0;
    return G_SOURCE_REMOVE;
  }
  gint64 now = gdk_frame_clock_get_frame_time (clock);

  bool active = false;
  for (ScaleAnim &anim : g_scale_anims) {
    double t = (now - anim.start_us) / 1000.0 / (double)kWindowScaleMs;
    if (t < 0.0)
      t = 0.0;   // frame timestamp can predate the anim start
    if (t > 1.0)
      t = 1.0;
    double e = ease_out_quad (t);
    double s = anim.from + (anim.to - anim.from) * e;
    double final_s = (t < 1.0) ? s : anim.to;
    if (anim.index >= 0 && anim.index < (int)g_slots.size () &&
        anim.index != g_drag_slot && anim.index != g_drag_snapback_slot) {
      SlotEntry &slot = g_slots[anim.index];
      slot.current_scale = final_s;
      apply_thumb_rect (slot, scale_rect_around_center (slot.rect, final_s),
                        255);
    }
    if (t < 1.0)
      active = true;
  }
  g_scale_anims.erase (
      std::remove_if (g_scale_anims.begin (), g_scale_anims.end (),
                      [now] (const ScaleAnim &a) {
                        return (now - a.start_us) / 1000.0 >=
                               (double)kWindowScaleMs;
                      }),
      g_scale_anims.end ());

  // Unified overlay fade across close button, app icon, and caption pill.
  if (g_overlay_opacity != g_overlay_to) {
    double t = g_overlay_anim_ms > 0
                   ? (now - g_overlay_anim_start) / 1000.0 /
                         (double)g_overlay_anim_ms
                   : 1.0;
    if (t < 0.0)
      t = 0.0;
    if (t > 1.0)
      t = 1.0;
    double e = ease_out_quad (t);
    g_overlay_opacity =
        g_overlay_from + (g_overlay_to - g_overlay_from) * e;
    if (t >= 1.0) {
      g_overlay_opacity = g_overlay_to;
      if (g_overlay_to <= 0.01)
        overlay_hide_all ();
    } else {
      active = true;
    }
  }

  hover_layout_chrome ();

  if (!active && g_scale_anims.empty () &&
      g_overlay_opacity == g_overlay_to) {
    if (g_hover_index < 0)
      g_last_hovered_slot = -1;
    g_scale_anim_source = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

void
hover_anim_ensure (void)
{
  if (g_scale_anim_source == 0)
    g_scale_anim_source = gtk_widget_add_tick_callback (
        g_window, hover_anim_tick, nullptr, nullptr);
}

void
hover_scale_start (int index, double to)
{
  if (index < 0 || index >= (int)g_slots.size ())
    return;
  double from = g_slots[index].current_scale;
  for (ScaleAnim &a : g_scale_anims) {
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
  g_scale_anims.push_back (
      ScaleAnim{index, from, to, g_get_monotonic_time ()});
  hover_anim_ensure ();
}

void
overlay_fade_start (double to)
{
  if (to > 0.5 && g_overlay_opacity <= 0.01)
    g_overlay_opacity = 0.02;
  g_overlay_from = g_overlay_opacity;
  g_overlay_to = to;
  g_overlay_anim_start = g_get_monotonic_time ();
  g_overlay_anim_ms = kOverlayFadeMs;
  hover_anim_ensure ();
}

// WINDOW_OVERLAY_IDLE_HIDE_TIMEOUT: hide the overlay 750ms after the pointer
// left, unless it is over the preview or one of the overlay pieces (GNOME's
// idle check looks at has-pointer on the close button / icon / title).
gboolean
hover_idle_hide_cb (gpointer user_data)
{
  (void)user_data;

  if (g_drag_slot >= 0 || g_drag_active || g_drag_snapback_source != 0 ||
      g_app_grid_mode || g_search_mode || g_search_fade_cur > 0.005) {
    g_hover_idle_source = 0;
    if (g_app_grid_mode || g_search_mode || g_search_fade_cur > 0.005)
      overlay_hide_all ();
    return G_SOURCE_REMOVE;
  }

  if (cursor_over_chrome_window ())
    return G_SOURCE_CONTINUE;

  // Pointer back on the preview (it may have travelled over a chrome
  // window, which swallows the overview's motion events).
  if (g_hover_index >= 0 && g_hover_index < (int)g_slots.size () &&
      g_overview_hwnd != nullptr) {
    RECT r = slot_display_rect (g_hover_index);
    POINT pt;
    if (GetCursorPos (&pt) && ScreenToClient (g_overview_hwnd, &pt) &&
        pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom) {
      g_hover_idle_source = 0;
      return G_SOURCE_REMOVE;
    }
  }

  g_hover_idle_source = 0;
  int index = g_hover_index;
  g_last_hovered_slot = index;
  g_hover_index = -1;
  overlay_fade_start (0.0);
  hover_scale_start (index, 1.0);
  return G_SOURCE_REMOVE;
}

void
schedule_hover_idle_hide (void)
{
  if (g_hover_idle_source != 0)
    return;
  g_hover_idle_source =
      g_timeout_add (kOverlayIdleHideMs, hover_idle_hide_cb, nullptr);
}

void
cancel_hover_idle (void)
{
  if (g_hover_idle_source != 0) {
    g_source_remove (g_hover_idle_source);
    g_hover_idle_source = 0;
  }
}

void
set_hover (int index, bool immediate)
{
  if (immediate) {
    cancel_hover_idle ();
    g_scale_anims.clear ();
    if (g_scale_anim_source != 0 && g_window != nullptr) {
      gtk_widget_remove_tick_callback (g_window, g_scale_anim_source);
      g_scale_anim_source = 0;
    }
    // Snap any scaled preview back to its slot rect (skipping any actively
    // dragged or snapping-back slot).
    for (int i = 0; i < (int)g_slots.size (); ++i) {
      SlotEntry &s = g_slots[i];
      if (i == g_drag_slot || i == g_drag_snapback_slot) {
        s.current_scale = 1.0;
        continue;
      }
      if (s.current_scale != 1.0) {
        BYTE op = (g_search_mode || g_search_fade_cur > 0.005)
                      ? 0
                      : (g_app_grid_mode ? s.grid_opacity : 255);
        apply_thumb_rect (s, s.rect, op);
        s.current_scale = 1.0;
      }
    }
    g_last_hovered_slot = -1;
    g_hover_index = -1;
    g_overlay_opacity = 0.0;
    g_overlay_from = 0.0;
    g_overlay_to = 0.0;
    overlay_hide_all ();
    return;
  }

  if (g_drag_slot >= 0 || g_drag_active || g_drag_snapback_source != 0 ||
      g_app_grid_mode || g_search_mode || g_search_fade_cur > 0.005 ||
      g_anim.source != 0 || g_switch_source != 0 ||
      g_search_fade_source != 0 || g_target_state == ControlsState::HIDDEN)
    return;
  cancel_settle_hover ();

  if (index == g_hover_index) {
    if (index >= 0) {
      cancel_hover_idle ();
      bool missing_chrome =
          (g_chrome_window != nullptr &&
           !gtk_widget_get_visible (g_chrome_window)) ||
          (g_icon_window != nullptr &&
           !gtk_widget_get_visible (g_icon_window)) ||
          (g_caption_window != nullptr &&
           !gtk_widget_get_visible (g_caption_window));
      if (missing_chrome || g_overlay_to < 1.0) {
        overlay_fade_start (1.0);
        hover_layout_chrome ();
      }
    }
    return;
  }

  // windowPreview.js: overlayEnabled = active && stateAdjustment === 1 —
  // the hover chrome is disabled until open/transition/workspace-switch
  // animations settle on WINDOW_PICKER (and never active during search).
  if (g_anim.source != 0 || g_switch_source != 0 ||
      g_app_grid_mode || g_search_mode || g_search_fade_cur > 0.005 ||
      g_target_state != ControlsState::WINDOW_PICKER)
    return;

  int previous = g_hover_index;
  cancel_hover_idle ();

  if (index < 0 || index >= (int)g_slots.size ()) {
    // GNOME keeps the overlay for 750ms after the pointer left the preview.
    if (previous >= 0)
      schedule_hover_idle_hide ();
    return;
  }

  const SlotEntry &slot = g_slots[index];
  if (!IsWindow (slot.target) ||
      slot.desktop_index != std::max (g_desktop_active, 0)) {
    if (previous >= 0)
      schedule_hover_idle_hide ();
    return;
  }

  g_hover_index = index;
  g_last_hovered_slot = index;

  // Caption pill below the preview.
  std::string cap_title = hwnd_title_utf8 (slot.target);
  if (cap_title.empty ())
    cap_title = hwnd_process_name_utf8 (slot.target);
  gtk_label_set_text (GTK_LABEL (g_caption), cap_title.c_str ());
  overlay_fade_start (1.0);

  // Scale up the preview (WINDOW_SCALE_TIME EASE_OUT_QUAD); the previous
  // one shrinks back at the same time.
  int scale = gtk_widget_get_scale_factor (g_window);
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
  if (g_window == nullptr)
    return;
  if (g_drag_slot >= 0 || g_drag_active || g_drag_snapback_source != 0 ||
      g_app_grid_mode || g_search_mode || g_search_fade_cur > 0.005 ||
      g_anim.source != 0 || g_switch_source != 0 ||
      g_search_fade_source != 0 || g_target_state == ControlsState::HIDDEN)
    return;
  cancel_settle_hover ();
  if (g_app_grid_mode || g_search_mode || g_search_fade_cur > 0.005) {
    set_hover (-1, true);
    return;
  }

  int scale = gtk_widget_get_scale_factor (g_window);
  int px = (int)(x * scale);
  int py = (int)(y * scale);
  const int active_ws = std::max (g_desktop_active, 0);

  for (int i = 0; i < (int)g_slots.size (); ++i) {
    if (g_slots[i].desktop_index != active_ws)
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
  if (g_drag_slot >= 0 || g_drag_active || g_drag_snapback_source != 0 ||
      g_app_grid_mode || g_search_mode || g_search_fade_cur > 0.005 ||
      g_anim.source != 0 || g_switch_source != 0 ||
      g_search_fade_source != 0 || g_target_state == ControlsState::HIDDEN)
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
  for (SlotEntry &e : g_slots)
    if (e.thumb != nullptr)
      DwmUnregisterThumbnail (e.thumb);
  g_slots.clear ();
  g_scale_anims.clear ();
  g_hover_index = -1;
  g_last_hovered_slot = -1;
  reset_drag_state ();
}

void
apply_thumb_rect (SlotEntry &e, const RECT &rect, BYTE opacity, bool force)
{
  if (e.thumb == nullptr)
    return;
  const bool on_screen =
      (g_monitor_w <= 0 ||
       (rect.right > 0 && rect.left < g_monitor_w && rect.bottom > 0 &&
        rect.top < g_monitor_h));
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
  if (g_overview_hwnd == nullptr || e.target == nullptr || e.thumb == nullptr)
    return;
  DwmUnregisterThumbnail (e.thumb);
  e.thumb = nullptr;
  e.thumb_applied = false;
  HTHUMBNAIL thumb = nullptr;
  if (SUCCEEDED (DwmRegisterThumbnail (g_overview_hwnd, e.target, &thumb)) &&
      thumb != nullptr) {
    e.thumb = thumb;
    apply_thumb_rect (e, e.current, e.current_opacity, true);
  }
}

void
create_previews (bool at_start_rects, LONG x_offset, bool reorder_existing)
{
  if (g_overview_hwnd == nullptr)
    return;

  // DWM composites thumbnails registered on the same destination HWND in
  // registration order (earlier = bottom, later = top). Register in reverse
  // desktop Z-order (backmost window first, frontmost window z_order == 0
  // last) so the active foreground window always renders on top of background
  // windows during the open/close transitions — and always AFTER g_mini_slots
  // so workspace thumbnail previews never float above real workspace windows.
  // Register inactive workspaces' windows first and active workspace windows
  // last (each group back-to-front by desktop z_order) so neighboring
  // workspaces never fly on top of active workspace windows during APP_GRID
  // or close transitions.
  const int ws_count = std::max (static_cast<int> (g_desktops.size ()), 1);
  const int active_ws = std::clamp (g_desktop_active, 0, ws_count - 1);
  std::vector<size_t> order (g_slots.size ());
  for (size_t i = 0; i < g_slots.size (); ++i)
    order[i] = i;
  std::stable_sort (order.begin (), order.end (),
                    [active_ws] (size_t a, size_t b) {
                      const bool a_act = (g_slots[a].desktop_index == active_ws);
                      const bool b_act = (g_slots[b].desktop_index == active_ws);
                      if (a_act != b_act)
                        return !a_act && b_act;
                      return g_slots[a].z_order > g_slots[b].z_order;
                    });

  for (size_t idx : order) {
    SlotEntry &e = g_slots[idx];
    const bool was_reused = (e.thumb != nullptr);
    if (reorder_existing && e.thumb != nullptr) {
      DwmUnregisterThumbnail (e.thumb);
      e.thumb = nullptr;
    }
    if (e.thumb == nullptr) {
      HTHUMBNAIL thumb = nullptr;
      if (SUCCEEDED (DwmRegisterThumbnail (g_overview_hwnd, e.target, &thumb)) &&
          thumb != nullptr) {
        e.thumb = thumb;
      }
    }
    if (e.thumb != nullptr) {
      e.thumb_applied = false;
      if (was_reused && x_offset == 0) {
        apply_thumb_rect (e, e.current, e.current_opacity, true);
      } else if (g_search_mode) {
        apply_thumb_rect (e, translate_rect_x (e.rect, x_offset), 0, true);
      } else if (at_start_rects) {
        apply_thumb_rect (e, e.start, e.minimized ? 0 : 255, true);
      } else {
        BYTE op = g_app_grid_mode ? e.grid_opacity : 255;
        if (g_app_grid_mode && g_folder_dialog_open && op > 0)
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
  for (MiniSlot &m : g_mini_slots)
    if (m.thumb != nullptr)
      DwmUnregisterThumbnail (m.thumb);
  g_mini_slots.clear ();
  g_mini_target_opacity = 0;
}

static void
build_mini_occluder_boxes (std::vector<RECT> *out)
{
  out->clear ();
  if (!g_ws_cur_boxes.empty ())
    *out = g_ws_cur_boxes;
  else if (g_bg_cur.right > g_bg_cur.left && g_bg_cur.bottom > g_bg_cur.top)
    out->push_back (g_bg_cur);
}

static void
flush_pending_dwm_frame (void)
{
  if (!g_dwm_frame_pending)
    return;
  g_dwm_frame_pending = false;
  for (SlotEntry &s : g_slots) {
    if (s.has_pending_dwm) {
      s.has_pending_dwm = false;
      apply_thumb_rect (s, s.pending_rect, s.pending_opacity);
    }
  }
  if (g_dwm_pending_has_mini) {
    g_dwm_pending_has_mini = false;
    set_mini_opacity (g_dwm_pending_mini_opacity);
  }
}

static void
clear_pending_dwm_frame (void)
{
  g_dwm_frame_pending = false;
  g_dwm_pending_has_mini = false;
  for (SlotEntry &s : g_slots)
    s.has_pending_dwm = false;
}

static void
on_overview_after_paint (GdkFrameClock *clock G_GNUC_UNUSED,
                         gpointer user_data G_GNUC_UNUSED)
{
  flush_pending_dwm_frame ();
}

static void
disconnect_after_paint_hook (void)
{
  if (g_overview_clock != nullptr && g_overview_after_paint_id != 0) {
    g_signal_handler_disconnect (g_overview_clock, g_overview_after_paint_id);
  }
  g_overview_clock = nullptr;
  g_overview_after_paint_id = 0;
}

static void
ensure_after_paint_hook (GdkFrameClock *clock)
{
  if (clock == nullptr)
    return;
  if (g_overview_clock == clock && g_overview_after_paint_id != 0)
    return;
  disconnect_after_paint_hook ();
  g_overview_clock = clock;
  g_overview_after_paint_id = g_signal_connect (
      clock, "after-paint", G_CALLBACK (on_overview_after_paint), nullptr);
}

void
sync_mini_previews_visibility (BYTE opacity)
{
  if (g_mini_slots.empty ())
    return;

  std::vector<RECT> occluders;
  build_mini_occluder_boxes (&occluders);

  const int dpi_scale =
      g_window != nullptr ? std::max (gtk_widget_get_scale_factor (g_window), 1)
                          : 1;
  const double pivot_x =
      (static_cast<double> (g_thumbs_rect.left) + g_thumbs_rect.right) * 0.5;
  const double pivot_y =
      (static_cast<double> (g_thumbs_rect.top) + g_thumbs_rect.bottom) * 0.5;
  const double cur_scale = std::clamp (g_thumbs_cur_scale, 0.05, 2.0);
  const double cur_ty_px = g_thumbs_cur_ty * dpi_scale;
  const BYTE effective_opacity = opacity;

  for (MiniSlot &m : g_mini_slots) {
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
  g_mini_target_opacity = opacity;
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
  if (g_overview_hwnd == nullptr || g_desktops.empty () ||
      overview_thumbs_get_height (g_thumbs) <= 0)
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
      int wd = vd::window_desktop_index (w.hwnd);
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

  int scale = gtk_widget_get_scale_factor (g_window);
  int panel = g_panel_height;

  for (int d = 0; d < (int)g_desktops.size (); ++d) {
    GdkRectangle pill;
    if (!overview_thumbs_get_rect (g_thumbs, d, &pill))
      continue;

    const double pad = 2.0 * scale;
    // Pill origin in overview client space (physical px), inset slightly so
    // maximized windows never poke sharp corners outside the rounded pill clip.
    double ox = (double)thumbs_x * scale + pill.x * scale + pad;
    double oy = (double)thumbs_y * scale + pill.y * scale + pad;
    double pill_w = std::max ((double)pill.width * scale - 2.0 * pad, 1.0);
    double pill_h = std::max ((double)pill.height * scale - 2.0 * pad, 1.0);
    RECT porthole = {0, panel, g_monitor_w, g_monitor_h};
    double pw = std::max ((double)(porthole.right - porthole.left), 1.0);
    double ph = std::max ((double)(porthole.bottom - porthole.top), 1.0);
    double sx = pill_w / pw;
    double sy = pill_h / ph;

    // Register back-to-front so frontmost windows render on top inside pills.
    for (size_t i = all.size (); i-- > 0;) {
      if (window_desktop[i] != d &&
          !(window_desktop[i] < 0 && d == g_desktop_active))
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
      if (SUCCEEDED (DwmRegisterThumbnail (g_overview_hwnd, all[i].hwnd,
                                           &thumb)) &&
          thumb != nullptr) {
        m.thumb = thumb;
        SIZE src_sz{};
        if (SUCCEEDED (DwmQueryThumbnailSourceSize (thumb, &src_sz)) &&
            src_sz.cx > 0 && src_sz.cy > 0) {
          m.src_size = src_sz;
          m.has_src_size = true;
        }
        g_mini_slots.push_back (m);
      }
    }
  }

  set_mini_opacity (g_search_mode ? 0 : initial_opacity);
}

// Synchronize Windows virtual desktops with GNOME Shell's dynamic workspace
// rules (windowManager.js WorkspaceTracker._checkWorkspaces):
// - Minimum 2 workspaces at all times (1 workspace -> 2 workspaces on startup).
// - Always 1 trailing empty workspace at the end (if last workspace has a
//   window, append a new empty workspace).
// - Any empty intermediate or extra trailing workspace is removed.
static bool g_in_dynamic_ws_sync = false;

static bool
sync_dynamic_workspaces (HMONITOR monitor, const RECT &monitor_rect,
                         std::vector<OvWindowInfo> *out_all = nullptr,
                         std::vector<int> *out_window_desktop = nullptr,
                         std::vector<vd::DesktopInfo> *out_desktops = nullptr)
{
  std::vector<OvWindowInfo> raw_all;
  EnumContext ctx{monitor, monitor_rect, &raw_all, false, true,
                  winome::get_config ().overview_window_blacklist};
  EnumWindows (collect_windows, reinterpret_cast<LPARAM> (&ctx));

  std::vector<vd::DesktopInfo> ds = vd::desktops ();
  std::vector<OvWindowInfo> all;
  std::vector<int> win_desk;
  all.reserve (raw_all.size ());
  win_desk.reserve (raw_all.size ());
  for (const OvWindowInfo &w : raw_all) {
    const int d = ds.empty () ? -1 : vd::window_desktop_index (w.hwnd);
    // Cloaked windows are only legitimate when virtual desktops are active and
    // the window belongs to a known desktop (DWM_CLOAKED_SHELL on another WS).
    // Drop cloaked system/UWP ghosts that have no desktop (d < 0) so they never
    // fall back onto the active workspace.
    if (window_cloaked_flags (w.hwnd) != 0 && (ds.empty () || d < 0))
      continue;
    all.push_back (w);
    win_desk.push_back (d);
  }

  const bool can_manage =
      !ds.empty () &&
      vd::vtable_slots_for_build (vd::windows_build_number ())
          .can_manage_desktops;
  bool changed = false;

  if (can_manage) {
    std::vector<int> counts (ds.size (), 0);
    for (size_t i = 0; i < all.size (); ++i) {
      int d = win_desk[i];
      if (d >= 0 && d < static_cast<int> (counts.size ()))
        ++counts[static_cast<size_t> (d)];
    }

    DynamicWorkspacePlan plan = compute_dynamic_workspace_plan (counts);
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
      ds = vd::desktops ();
      if (!plan.remove_indices.empty ()) {
        // Removing an intermediate desktop shifts higher desktop indices down.
        for (size_t i = 0; i < all.size (); ++i)
          win_desk[i] = vd::window_desktop_index (all[i].hwnd);
      }
    }
  }

  if (out_all != nullptr)
    *out_all = std::move (all);
  if (out_window_desktop != nullptr)
    *out_window_desktop = std::move (win_desk);
  if (out_desktops != nullptr)
    *out_desktops = std::move (ds);
  return changed;
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

bool
compute_geometry (HMONITOR monitor, const RECT &monitor_rect, int panel_height,
                  PlacedGeometry *out)
{
  int scale = gtk_widget_get_scale_factor (g_window);
  int mw = monitor_rect.right - monitor_rect.left;
  int mh = monitor_rect.bottom - monitor_rect.top;
  int wa_h = mh - panel_height;
  if (wa_h <= 0 || scale <= 0)
    return false;

  int spacing = (int)(wa_h * kVerticalSpacingRatio + 0.5);
  int thumb_adjust = (int)(spacing * kThumbnailsAdjustTop + 0.5);
  int thumb_adjust_bottom = (int)(spacing * kThumbnailsAdjustBottom + 0.5);

  // Search entry: must be visible (and realized alongside g_window) before
  // measuring; an unmapped/hidden GtkEntry reports preferred height 0 on the
  // very first overview open, pushing ws_box_picker upward across the bar.
  if (g_search != nullptr && !gtk_widget_get_visible (g_search))
    gtk_widget_set_visible (g_search, TRUE);
  if (g_search != nullptr && gtk_widget_get_realized (g_window) &&
      !gtk_widget_get_realized (g_search)) {
    gtk_widget_realize (g_search);
  }
  GtkRequisition search_nat{};
  if (g_search != nullptr)
    gtk_widget_get_preferred_size (g_search, nullptr, &search_nat);
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
  g_placed_scale = scale;
  g_placed_search_h = search_h;
  int search_total = search_h * scale;

  // Workspace thumbnails (ThumbnailsBox, shown in WINDOW_PICKER for >1
  // desktop or during drag; cross-faded cleanly during state shifts).
  bool can_manage =
      vd::vtable_slots_for_build (vd::windows_build_number ()).can_manage_desktops;
  int thumbs_h = 0;  // physical
  g_thumbs_has_pills =
      g_desktops.size () > 1 || (g_drag_active && !g_desktops.empty ());
  if (g_thumbs_has_pills) {
    int box_h = (int)(wa_h * kMaxThumbnailScale) / scale;  // logical
    const double panel_ratio =
        (panel_height > 0 && wa_h > 0)
            ? (static_cast<double> (panel_height) / static_cast<double> (wa_h))
            : 0.0;
    overview_thumbs_repopulate (
        g_thumbs, (int)g_desktops.size (), g_desktop_active, box_h,
        (double)mw / scale, (double)mw, (double)wa_h,
        active_wallpaper_thumb_texture (),
        can_manage, g_drag_active, panel_ratio);
    thumbs_h = overview_thumbs_get_height (g_thumbs) * scale;
  } else {
    overview_thumbs_repopulate (g_thumbs, 0, -1, 0, 0, 0, 0, nullptr, false,
                                false);
  }
  g_thumbs_has_pills = thumbs_h > 0;

  // Dash (16% cap, ControlsManagerLayout DASH_MAX_HEIGHT_RATIO).
  int dash_max = (int)(wa_h * kDashMaxHeightRatio + 0.5);
  overview_dash_repopulate (g_dash, monitor, dash_max / scale,
                            g_desktop_active);
  int dash_w = overview_dash_get_width (g_dash) * scale;
  int dash_h = std::min (overview_dash_get_height (g_dash) * scale, dash_max);

  // Picker container box (ControlsManagerLayout WINDOW_PICKER): the
  // thumbnails row sits between the search entry and the workspace box,
  // which yields to thumbnailsHeight + round(spacing * 0.4) on both edges.
  int avail_bottom = panel_height + wa_h - dash_h - spacing;
  RECT box;
  box.left = 0;
  box.top = panel_height + search_total + thumb_adjust + thumbs_h +
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
  // and margin-bottom (18px), so grid_ws_top starts directly at
  // panel_height + search_total (overviewControls.js:126).
  int grid_ws_top = panel_height + search_total;
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
  out->search_y = panel_height / scale;
  int sv_top = panel_height + search_total + spacing;
  int sv_w = std::clamp (mw / scale - 48, 400, 1044);
  int sv_h = std::max ((avail_bottom - sv_top) / scale, 220);
  out->search_view_w = sv_w;
  out->search_view_h = sv_h;
  out->search_view_x = (mw / scale - sv_w) / 2;
  out->search_view_y = sv_top / scale;
  out->dash_x = (mw - dash_w) / 2 / scale;
  out->dash_y = (panel_height + wa_h - dash_h) / scale;
  out->thumbs_h = thumbs_h;
  out->thumbs_x =
      thumbs_h > 0 ? (mw / scale - overview_thumbs_get_width (g_thumbs)) / 2
                   : 0;
  out->thumbs_y = (panel_height + search_total + thumb_adjust) / scale;

  g_monitor_w = mw;
  g_monitor_h = mh;
  g_dash_top = panel_height + wa_h - dash_h;

  g_bg_full = RECT{0, panel_height, mw, mh};
  g_bg_picker = ws_box_picker;
  g_bg_grid = ws_box_grid;
  g_bg_final = g_app_grid_mode ? ws_box_grid : ws_box_picker;

  const int ws_count = std::max (static_cast<int> (g_desktops.size ()), 1);
  const int active_ws = std::clamp (g_desktop_active, 0, ws_count - 1);
  g_ws_full_boxes.resize (static_cast<size_t> (ws_count));
  g_ws_picker_boxes.resize (static_cast<size_t> (ws_count));
  g_ws_grid_boxes.resize (static_cast<size_t> (ws_count));
  for (int d = 0; d < ws_count; ++d) {
    g_ws_full_boxes[static_cast<size_t> (d)] =
        full_box_for_desktop (d, active_ws, g_bg_full, ws_box_picker, mw,
                              scale);
    g_ws_picker_boxes[static_cast<size_t> (d)] =
        picker_box_for_desktop (d, static_cast<double> (active_ws),
                                ws_box_picker, mw, scale);
    g_ws_grid_boxes[static_cast<size_t> (d)] =
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
  g_radius_picker = out->radius_picker;
  g_radius_grid = out->radius_grid;
  g_bg_radius_target = g_app_grid_mode ? g_radius_grid : g_radius_picker;
  g_ws_box = g_app_grid_mode ? ws_box_grid : ws_box_picker;
  return true;
}

static bool
apply_ws_boxes (const std::vector<RECT> &boxes, bool sync_mini = true)
{
  const bool settled = (g_anim.source == 0 && g_switch_source == 0);
  const int active_ws =
      !boxes.empty ()
          ? std::clamp (g_desktop_active, 0, static_cast<int> (boxes.size ()) - 1)
          : 0;
  const RECT active_rect =
      !boxes.empty () ? boxes[static_cast<size_t> (active_ws)] : g_bg_cur;
  bool same = (boxes.size () == g_ws_cur_boxes.size ()) &&
              EqualRect (&g_bg_cur, &active_rect) &&
              g_ws_bg_last_radius == g_bg_radius &&
              g_ws_bg_settled_shadow_drawn == settled;
  if (same) {
    for (size_t i = 0; i < boxes.size (); ++i) {
      if (!EqualRect (&boxes[i], &g_ws_cur_boxes[i])) {
        same = false;
        break;
      }
    }
  }
  if (same) {
    if (settled && (!g_ws_hist1_boxes.empty () || !g_ws_hist2_boxes.empty ())) {
      g_ws_hist1_boxes.clear ();
      g_ws_hist2_boxes.clear ();
      if (sync_mini)
        sync_mini_previews_visibility (g_mini_target_opacity);
    }
    return false;
  }
  if (settled) {
    g_ws_hist1_boxes.clear ();
    g_ws_hist2_boxes.clear ();
  } else {
    g_ws_hist2_boxes = std::move (g_ws_hist1_boxes);
    g_ws_hist1_boxes = g_ws_cur_boxes;
  }
  g_ws_cur_boxes = boxes;
  g_bg_cur = active_rect;
  g_ws_bg_last_radius = g_bg_radius;
  if (sync_mini)
    sync_mini_previews_visibility (g_mini_target_opacity);
  if (g_ws_bg != nullptr) {
    gtk_widget_queue_draw (g_ws_bg);
    return true;
  }
  return false;
}

bool
apply_bg_rect (const RECT &rect, bool sync_mini = true)
{
  const bool settled = (g_anim.source == 0 && g_switch_source == 0);
  if (g_ws_cur_boxes.size () <= 1 && EqualRect (&g_bg_cur, &rect) &&
      g_ws_bg_last_radius == g_bg_radius &&
      g_ws_bg_settled_shadow_drawn == settled) {
    if (settled && (!g_ws_hist1_boxes.empty () || !g_ws_hist2_boxes.empty ())) {
      g_ws_hist1_boxes.clear ();
      g_ws_hist2_boxes.clear ();
      if (sync_mini)
        sync_mini_previews_visibility (g_mini_target_opacity);
    }
    return false;
  }
  if (settled) {
    g_ws_hist1_boxes.clear ();
    g_ws_hist2_boxes.clear ();
  } else {
    g_ws_hist2_boxes = std::move (g_ws_hist1_boxes);
    g_ws_hist1_boxes = g_ws_cur_boxes;
  }
  g_bg_cur = rect;
  if (g_ws_cur_boxes.size () <= 1) {
    g_ws_cur_boxes.assign (1, rect);
  } else {
    const int active_ws = std::clamp (
        g_desktop_active, 0, static_cast<int> (g_ws_cur_boxes.size ()) - 1);
    g_ws_cur_boxes[static_cast<size_t> (active_ws)] = rect;
  }
  g_ws_bg_last_radius = g_bg_radius;
  if (sync_mini)
    sync_mini_previews_visibility (g_mini_target_opacity);
  if (g_ws_bg != nullptr) {
    gtk_widget_queue_draw (g_ws_bg);
    return true;
  }
  return false;
}

static void
cancel_settle_hover (void)
{
  if (g_settle_hover_timer != 0) {
    g_source_remove (g_settle_hover_timer);
    g_settle_hover_timer = 0;
  }
}

static gboolean
settle_hover_cb (gpointer user_data G_GNUC_UNUSED)
{
  g_settle_hover_timer = 0;
  if (g_window == nullptr || !gtk_widget_get_visible (g_window) || !g_placed ||
      g_anim.source != 0 || g_switch_source != 0 || g_search_fade_source != 0 ||
      g_app_grid_mode || g_search_mode ||
      g_target_state != ControlsState::WINDOW_PICKER) {
    return G_SOURCE_REMOVE;
  }
  set_hover (slot_at_cursor ());
  return G_SOURCE_REMOVE;
}

static void
schedule_settle_hover (guint delay_ms)
{
  cancel_settle_hover ();
  g_settle_hover_timer = g_timeout_add_full (
      G_PRIORITY_LOW, delay_ms, settle_hover_cb, nullptr, nullptr);
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

static void
ensure_app_grid_synced (bool refresh_running = false)
{
  if (g_app_grid == nullptr || g_grid_w <= 0 || g_grid_h <= 0)
    return;
  app_grid_clip_set_bounds (g_app_grid_clip, g_grid_w, g_grid_h);
  gtk_widget_set_size_request (g_app_grid, g_grid_w, g_grid_h);
  app_grid_set_query (g_app_grid, "");
  app_grid_rebuild (g_app_grid, g_grid_w, g_grid_h, refresh_running);
}

static void
sync_app_grid_visible (bool visible)
{
  if (g_app_grid_clip != nullptr)
    gtk_widget_set_visible (g_app_grid_clip, visible ? TRUE : FALSE);
  if (g_app_grid != nullptr)
    gtk_widget_set_visible (g_app_grid, visible ? TRUE : FALSE);
}

void
position_widgets (const PlacedGeometry &geo, bool opening, LONG switch_off_x = 0)
{
  gtk_widget_set_size_request (g_search, geo.search_w, geo.search_h);
  gtk_fixed_move (GTK_FIXED (g_root), g_search, geo.search_x, geo.search_y);

  // Keep g_ws_bg fixed to the full workarea allocation and paint/clip g_bg_cur
  // directly inside winome_workspace_bg_snapshot so 250ms animations do zero
  // GtkFixed re-layout or size_request churn.
  int scale = std::max (gtk_widget_get_scale_factor (g_window), 1);
  if (g_ws_bg != nullptr) {
    const int full_w = std::max (
        static_cast<int> (g_bg_full.right - g_bg_full.left) / scale, 1);
    const int full_h = std::max (
        static_cast<int> (g_bg_full.bottom - g_bg_full.top) / scale, 1);
    gtk_widget_set_size_request (g_ws_bg, full_w, full_h);
    gtk_fixed_move (GTK_FIXED (g_root), g_ws_bg,
                    static_cast<int> (g_bg_full.left) / scale,
                    static_cast<int> (g_bg_full.top) / scale);
  }

  // The background starts at the full work area (radius 0) when opening —
  // the open animation shrinks it into the target box — and sits at the
  // settled (or slide-in start) box on relayout.
  if (opening) {
    g_bg_radius = 0.0;
    apply_ws_boxes (g_ws_full_boxes);
  } else {
    const std::vector<RECT> &target_boxes =
        g_app_grid_mode ? g_ws_grid_boxes : g_ws_picker_boxes;
    g_bg_radius = g_app_grid_mode ? g_radius_grid : g_radius_picker;
    if (switch_off_x == 0) {
      apply_ws_boxes (target_boxes);
    } else {
      std::vector<RECT> shifted = target_boxes;
      for (RECT &r : shifted)
        r = translate_rect_x (r, switch_off_x);
      apply_ws_boxes (shifted);
    }
  }

  gtk_fixed_move (GTK_FIXED (g_root), g_dash, geo.dash_x, geo.dash_y);

  // Thumbnails row (invisible for a single desktop, settled APP_GRID, or search).
  gtk_fixed_move (GTK_FIXED (g_root), g_thumbs, geo.thumbs_x, geo.thumbs_y);
  bool show_thumbs = !g_search_mode &&
                     (opening ? (geo.thumbs_h > 0)
                              : (!g_app_grid_mode && geo.thumbs_h > 0));
  gtk_widget_set_visible (g_thumbs, show_thumbs);

  if (g_search_view != nullptr) {
    gtk_widget_set_size_request (g_search_view, geo.search_view_w,
                                 geo.search_view_h);
    gtk_fixed_move (GTK_FIXED (g_root), g_search_view,
                    geo.search_view_x, geo.search_view_y);
    gtk_widget_set_visible (g_search_view,
                            (g_search_mode || g_search_fade_cur > 0.005)
                                ? TRUE
                                : FALSE);
  }

  g_search_rect.left = geo.search_x * scale;
  g_search_rect.top = geo.search_y * scale;
  g_search_rect.right = (geo.search_x + geo.search_w) * scale;
  g_search_rect.bottom = (geo.search_y + geo.search_h) * scale;

  if (geo.thumbs_h > 0) {
    int thumbs_w = overview_thumbs_get_width (g_thumbs);
    g_thumbs_rect.left = geo.thumbs_x * scale;
    g_thumbs_rect.top = geo.thumbs_y * scale;
    g_thumbs_rect.right = (geo.thumbs_x + thumbs_w) * scale;
    g_thumbs_rect.bottom = (geo.thumbs_y * scale) + geo.thumbs_h;
  } else {
    g_thumbs_rect = {0, 0, 0, 0};
  }

  gtk_widget_set_visible (g_search, TRUE);
  gtk_widget_set_visible (g_ws_bg, g_search_mode ? FALSE : TRUE);
  gtk_widget_set_visible (g_dash, TRUE);
  if (!opening)
    sync_app_grid_visible (g_app_grid_mode && !g_search_mode);
}

// --- z-order ----------------------------------------------------------------------

static void dump_zorder (const char *tag);
static void restack_locked (void);
static void switch_finish (bool restore_hover = true);

static bool
is_popup_menu_window (HWND h)
{
  if (h == nullptr || !IsWindow (h) || !IsWindowVisible (h) || is_cloaked (h))
    return false;
  if (h == g_panel_hwnd || h == g_overview_hwnd || h == g_chrome_hwnd ||
      h == g_icon_hwnd || h == g_caption_hwnd)
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
    return g_overview_hwnd != nullptr &&
           GetWindow (h, GW_OWNER) == g_overview_hwnd;
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
  if (g_overview_hwnd == nullptr || !IsWindow (g_overview_hwnd))
    return nullptr;
  const HWND folder_hwnd = active_folder_hwnd ();
  const DWORD self_pid = GetCurrentProcessId ();
  HWND w = GetTopWindow (nullptr);
  for (int i = 0; w != nullptr && i < 40; ++i, w = GetWindow (w, GW_HWNDNEXT)) {
    if (w == g_panel_hwnd || w == g_overview_hwnd || w == g_chrome_hwnd ||
        w == g_icon_hwnd || w == g_caption_hwnd ||
        (folder_hwnd != nullptr && w == folder_hwnd))
      continue;
    if (!IsWindowVisible (w) || is_cloaked (w))
      continue;
    DWORD pid = 0;
    GetWindowThreadProcessId (w, &pid);
    if (pid != self_pid)
      continue;
    HWND owner = GetWindow (w, GW_OWNER);
    if (owner != g_overview_hwnd &&
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
find_lowest_popup_menu_above_panel (void)
{
  if (g_panel_hwnd == nullptr || !IsWindow (g_panel_hwnd))
    return nullptr;
  for (HWND prev = GetWindow (g_panel_hwnd, GW_HWNDPREV); prev != nullptr;
       prev = GetWindow (prev, GW_HWNDPREV)) {
    if (is_popup_menu_window (prev))
      return prev;
  }
  return nullptr;
}

static LRESULT CALLBACK
overview_subclass_wndproc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  if (msg == WM_WINDOWPOSCHANGING && lParam != 0 &&
      (GetWindowLongPtrW (hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0 &&
      g_panel_hwnd != nullptr && IsWindow (g_panel_hwnd) &&
      IsWindowVisible (g_panel_hwnd) &&
      (GetWindowLongPtrW (g_panel_hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
    auto *wp = reinterpret_cast<WINDOWPOS *> (lParam);
    if ((wp->flags & SWP_NOZORDER) == 0) {
      HWND folder_hwnd = active_folder_hwnd ();
      HWND allowed_anchor = g_panel_hwnd;
      HWND chrome_layers[] = {folder_hwnd, g_chrome_hwnd, g_icon_hwnd,
                              g_caption_hwnd};
      for (HWND h : chrome_layers) {
        if (is_visible_hwnd (h) &&
            (GetWindowLongPtrW (h, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
          allowed_anchor = h;
        }
      }
      if (wp->hwndInsertAfter != g_panel_hwnd &&
          wp->hwndInsertAfter != folder_hwnd &&
          wp->hwndInsertAfter != g_chrome_hwnd &&
          wp->hwndInsertAfter != g_icon_hwnd &&
          wp->hwndInsertAfter != g_caption_hwnd) {
        wp->hwndInsertAfter = allowed_anchor;
        wp->flags |= SWP_NOOWNERZORDER;
      }
    }
  }
  WNDPROC orig = g_orig_overview_wndproc;
  LRESULT res = orig != nullptr
                    ? CallWindowProcW (orig, hwnd, msg, wParam, lParam)
                    : DefWindowProcW (hwnd, msg, wParam, lParam);
  if (msg == WM_ACTIVATE && LOWORD (wParam) != WA_INACTIVE) {
    restack_locked ();
  }
  return res;
}

static void
ensure_overview_subclass (HWND hwnd)
{
  if (hwnd == nullptr || !IsWindow (hwnd))
    return;
  WNDPROC cur = reinterpret_cast<WNDPROC> (
      GetWindowLongPtrW (hwnd, GWLP_WNDPROC));
  if (cur != nullptr && cur != overview_subclass_wndproc) {
    g_orig_overview_wndproc = cur;
    SetWindowLongPtrW (hwnd, GWLP_WNDPROC,
                       reinterpret_cast<LONG_PTR> (overview_subclass_wndproc));
  }
}

static void
unsubclass_overview_hwnd (void)
{
  if (g_overview_hwnd != nullptr && IsWindow (g_overview_hwnd) &&
      g_orig_overview_wndproc != nullptr) {
    WNDPROC cur = reinterpret_cast<WNDPROC> (
        GetWindowLongPtrW (g_overview_hwnd, GWLP_WNDPROC));
    if (cur == overview_subclass_wndproc) {
      SetWindowLongPtrW (
          g_overview_hwnd, GWLP_WNDPROC,
          reinterpret_cast<LONG_PTR> (g_orig_overview_wndproc));
    }
  }
  g_orig_overview_wndproc = nullptr;
}

void
apply_ex_styles (HWND hwnd)
{
  // Allow g_overview_hwnd to activate/take keyboard focus so overview-wide
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
  ensure_hwnd_topmost (hwnd);
  ensure_overview_subclass (hwnd);

  HWND after = (g_panel_hwnd != nullptr && IsWindow (g_panel_hwnd))
                   ? g_panel_hwnd
                   : HWND_TOPMOST;
  SetWindowPos (hwnd, after, 0, 0, 0, 0, swp_flags);
}

// Rebuild slots, thumbnails and widget positions for the current monitor.
// Must run after the overview window is mapped (GDK's first-map sequence
// would overwrite earlier placement).
static void
place_overview_locked (bool opening, int switch_dir)
{
  if (g_window == nullptr || g_panel_hwnd == nullptr ||
      !gtk_widget_get_visible (g_window))
    return;

  // A plain relayout (window closed etc.) ends any in-flight workspace
  // switch instantly: the rebuilt slots are the new truth.
  if (g_switch_source != 0)
    switch_finish (false);

  HMONITOR monitor = MonitorFromWindow (g_panel_hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi {};
  mi.cbSize = sizeof (mi);
  if (!GetMonitorInfoW (monitor, &mi))
    return;

  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (g_window));
  if (surface == nullptr)
    return;
  g_overview_hwnd =
      static_cast<HWND> (gdk_win32_surface_get_handle (surface));
  apply_ex_styles (g_overview_hwnd);

  // Bind owner for any already-realized chrome windows; unrealized chrome
  // windows are realized lazily on first hover (or staggered low-priority
  // startup prewarm) so opening Overview right after startup never blocks on
  // 3 extra OpenGL window realizations.
  HWND chrome_hwnds[] = {g_chrome_hwnd, g_icon_hwnd, g_caption_hwnd};
  for (HWND ch : chrome_hwnds)
    chrome_bind_owner (ch);

  int w = mi.rcMonitor.right - mi.rcMonitor.left;
  int h = mi.rcMonitor.bottom - mi.rcMonitor.top;
  SetWindowPos (g_overview_hwnd, g_panel_hwnd, mi.rcMonitor.left,
                mi.rcMonitor.top, w, h,
                SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);

  int panel_height = 0;
  RECT pr;
  if (IsWindowVisible (g_panel_hwnd) && GetWindowRect (g_panel_hwnd, &pr))
    panel_height = pr.bottom - pr.top;
  g_panel_height = panel_height;

  load_wallpaper ();

  // Hybrid third-party live wallpaper engine support (Option C):
  // fast (<50us) probe for Wallpaper Engine / Lively / Sucrose under Progman /
  // WorkerW. When absent or disabled, zero WGC overhead is incurred.
  const std::string live_mode =
      sanitize_live_wallpaper_mode (winome::get_config ().live_wallpaper_mode);
  if (live_mode != "off") {
    LiveWallpaperTarget target = live_wallpaper_detect_target (mi.rcMonitor);
    if (target.active) {
      g_using_live_wallpaper = true;
      live_wallpaper_set_paused (opening || g_anim.source != 0 ||
                                 g_switch_source != 0);
      live_wallpaper_start (target, mi.rcMonitor, live_mode == "live",
                            on_live_wallpaper_frame, nullptr);
    } else {
      g_using_live_wallpaper = false;
      live_wallpaper_stop ();
      g_clear_object (&g_live_wallpaper);
      g_clear_object (&g_live_wallpaper_thumb);
    }
  } else {
    g_using_live_wallpaper = false;
    live_wallpaper_stop ();
    g_clear_object (&g_live_wallpaper);
    g_clear_object (&g_live_wallpaper_thumb);
  }

  // Synchronize dynamic workspaces (GNOME WorkspaceTracker._checkWorkspaces)
  // and enumerate all desktops' eligible windows in a single pass.
  std::vector<OvWindowInfo> all_windows;
  std::vector<int> window_desktop;
  std::vector<vd::DesktopInfo> synced_desktops;
  sync_dynamic_workspaces (monitor, mi.rcMonitor, &all_windows,
                           &window_desktop, &synced_desktops);

  g_desktops = std::move (synced_desktops);
  g_desktop_active = g_desktops.empty () ? -1 : vd::current_index ();
  if (g_desktop_active < 0)
    g_desktops.clear ();
  g_pending_desktop_sync = false;
  g_pending_desktop_target = -1;
  if (g_drag_active || g_drag_slot >= 0 || g_drag_thumbs_revealed)
    reset_drag_state ();
  g_print ("[ov] desktops=%zu active=%d\n", g_desktops.size (),
           g_desktop_active);
  winome_shell_panel_set_workspace_scroll (
      std::max (static_cast<int> (g_desktops.size ()), 1),
      static_cast<double> (std::max (g_desktop_active, 0)));

  const std::vector<RECT> prev_ws_cur_boxes = g_ws_cur_boxes;

  // Geometry (ControlsManagerLayout + WorkspacesView workspace box).
  PlacedGeometry geo;
  if (!compute_geometry (monitor, mi.rcMonitor, panel_height, &geo))
    return;
  if (opening) {
    g_target_state = ControlsState::HIDDEN;
    g_app_grid_mode = false;
  }
  const RECT &active_bg = g_app_grid_mode ? g_bg_grid : g_bg_picker;
  const LONG switch_off_x =
      (!opening && switch_dir != 0)
          ? static_cast<LONG> (switch_dir * (active_bg.right - active_bg.left))
          : 0;
  position_widgets (geo, opening, switch_off_x);

  // The dark backdrop applies only once the workspace background actually
  // covers the work area: before that, a bare #overviewGroup would flash
  // full-screen dark in the pre-placement frames.
  gtk_widget_set_name (g_root, "overviewGroup");

  int scale = gtk_widget_get_scale_factor (g_window);
  g_grid_w = std::max (std::min (w / scale - 32, 1820), 400);
  g_grid_h = std::max (
      (int)(geo.app_grid_box.bottom - geo.app_grid_box.top) / scale, 260);
  g_grid_x = (w / scale - g_grid_w) / 2;
  g_grid_y = (int)geo.app_grid_box.top / scale;
  if (g_app_grid != nullptr) {
    app_grid_set_host_bounds (g_app_grid, mi.rcMonitor.left, mi.rcMonitor.top,
                              w, h, panel_height, g_dash_top);
    if (g_app_grid_clip != nullptr) {
      gtk_fixed_move (GTK_FIXED (g_root), g_app_grid_clip, g_grid_x, g_grid_y);
      app_grid_clip_set_bounds (g_app_grid_clip, g_grid_w, g_grid_h);
    } else {
      gtk_fixed_move (GTK_FIXED (g_root), g_app_grid, g_grid_x, g_grid_y);
    }
    const bool need_grid_now =
        !opening || g_app_grid_mode ||
        g_pending_show_state == ControlsState::APP_GRID ||
        !g_search_query.empty ();
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
    saved_thumbs.reserve (g_slots.size ());
    for (SlotEntry &old_s : g_slots) {
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
  if (g_anim.source != 0) {
    gtk_widget_remove_tick_callback (g_window, g_anim.source);
    g_anim.source = 0;
  }

  const int ws_count = std::max (static_cast<int> (g_desktops.size ()), 1);
  const int active_ws = std::clamp (g_desktop_active, 0, ws_count - 1);
  bool need_relayout_anim = false;

  for (int d = 0; d < ws_count; ++d) {
    std::vector<OvWindowInfo> desk_windows;
    for (size_t i = 0; i < all_windows.size (); ++i) {
      int wd = (i < window_desktop.size ()) ? window_desktop[i] : -1;
      if (g_desktops.empty () || wd == d || (wd < 0 && d == active_ws))
        desk_windows.push_back (all_windows[i]);
    }
    if (desk_windows.empty ())
      continue;

    const RECT &ws_picker_d = g_ws_picker_boxes[static_cast<size_t> (d)];
    const RECT &ws_grid_d = g_ws_grid_boxes[static_cast<size_t> (d)];
    const RECT &ws_full_d = g_ws_full_boxes[static_cast<size_t> (d)];

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
      e.canonical_start = s.window.rect;
      e.start = (d == active_ws)
                    ? s.window.rect
                    : remap_rect_between_boxes (s.window.rect, g_bg_full,
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
      e.rect = g_app_grid_mode ? e.grid_rect : e.picker_rect;
      e.anim_from = opening ? e.start : e.rect;
      e.anim_to = e.rect;
      e.current = opening ? e.start : e.rect;
      BYTE settled_op = g_app_grid_mode ? e.grid_opacity : 255;
      if (g_app_grid_mode && g_folder_dialog_open && settled_op > 0)
        settled_op = 76;
      e.current_opacity = opening ? (e.minimized ? 0 : 255)
                                  : (g_search_mode ? 0 : settled_op);

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

      g_slots.push_back (e);
    }
  }

  for (SavedThumb &st : saved_thumbs) {
    if (st.thumb != nullptr)
      DwmUnregisterThumbnail (st.thumb);
  }

  // Settle focus and Win32 z-order BEFORE registering DWM thumbnails so that
  // SetForegroundWindow / BringWindowToTop / SetWindowPos do not reshuffle the
  // window or flash the bare wallpaper surface after thumbnails appear at t=0.
  if (opening && winome_shell_panel_top_popover_hwnd () == nullptr)
    focus_overview_window (false);
  restack_locked ();

  g_ws_hist1_boxes.clear ();
  g_ws_hist2_boxes.clear ();

  const bool opening_to_grid =
      opening && g_pending_show_state == ControlsState::APP_GRID;
  g_thumbs_cur_scale = (opening_to_grid || (!opening && g_app_grid_mode)) ? 0.5 : 1.0;
  g_thumbs_cur_ty = (opening_to_grid || (!opening && g_app_grid_mode))
                        ? (overview_thumbs_get_height (g_thumbs) * 0.5)
                        : 0.0;
  if (g_thumbs != nullptr)
    overview_thumbs_set_transform (g_thumbs, g_thumbs_cur_scale, g_thumbs_cur_ty);

  // Register mini window previews inside the workspace thumbnail pills FIRST
  // so they always sit underneath real workspace window clones in DWM's
  // composition order. When opening to WINDOW_PICKER, mini previews start at
  // opacity 255 (clipped behind g_ws_full_boxes at t=0) and unveil smoothly
  // with zero flash as the active workspace shrinks — matching GNOME Shell's
  // overviewControls.js _getThumbnailsBoxParams().
  const bool had_reused_slots = std::any_of (
      g_slots.begin (), g_slots.end (),
      [] (const SlotEntry &e) { return e.thumb != nullptr; });
  create_mini_previews (monitor, mi.rcMonitor, geo.thumbs_x, geo.thumbs_y,
                        (opening_to_grid || (!opening && g_app_grid_mode))
                            ? 0
                            : 255,
                        &all_windows, &window_desktop);

  // Register workspace window clones AFTER mini previews (in back-to-front
  // desktop Z-order): start at real desktop rects when opening, slide-in
  // start offset when switching desktops, or current/settled rects otherwise.
  create_previews (opening, switch_off_x,
                   had_reused_slots && !g_mini_slots.empty ());

  g_placed = true;
  g_print ("[ov] placed %dx%d slots=%zu ws=%ldx%ld grid=%dx%d@(%d,%d) dash=%d\n",
           w, h, g_slots.size (), (long)(g_ws_box.right - g_ws_box.left),
           (long)(g_ws_box.bottom - g_ws_box.top), g_grid_w, g_grid_h, g_grid_x,
           g_grid_y, overview_dash_get_width (g_dash));

  if (opening) {
    ControlsState desired = g_pending_show_state;
    g_bg_cur = g_bg_full;
    g_ws_cur_boxes = g_ws_full_boxes;
    g_bg_radius = 0.0;
    const bool start_thumbs_ready =
        g_thumbs_has_pills && desired != ControlsState::APP_GRID;
    g_thumbs_cur_opacity = start_thumbs_ready ? 1.0 : 0.0;
    g_thumbs_cur_scale = start_thumbs_ready ? 1.0 : 0.5;
    g_thumbs_cur_ty = start_thumbs_ready
                          ? 0.0
                          : (overview_thumbs_get_height (g_thumbs) * 0.5);
    g_grid_cur_opacity = 0.0;
    g_grid_cur_slide_y = kGridSlideOffsetPx;
    if (g_thumbs != nullptr) {
      overview_thumbs_set_transform (g_thumbs, g_thumbs_cur_scale,
                                     g_thumbs_cur_ty);
      gtk_widget_set_opacity (g_thumbs, g_thumbs_cur_opacity);
      gtk_widget_set_visible (g_thumbs, FALSE);
    }
    if (g_app_grid != nullptr) {
      app_grid_set_transition_opacity (g_app_grid, true, 1.0);
      app_grid_clip_set_transition (g_app_grid_clip,
                                    desired == ControlsState::APP_GRID, 0.0,
                                    kGridSlideOffsetPx, true);
      sync_app_grid_visible (desired == ControlsState::APP_GRID);
    }
    start_state_transition (desired, kOpenMs);
  } else {
    g_ws_box = g_app_grid_mode ? g_bg_grid : g_bg_picker;
    g_bg_radius_target = g_app_grid_mode ? g_radius_grid : g_radius_picker;
    g_bg_radius = g_bg_radius_target;
    g_thumbs_cur_opacity = (!g_app_grid_mode && g_thumbs_has_pills) ? 1.0 : 0.0;
    g_thumbs_cur_scale = g_app_grid_mode ? 0.5 : 1.0;
    g_thumbs_cur_ty =
        g_app_grid_mode ? (overview_thumbs_get_height (g_thumbs) * 0.5) : 0.0;
    if (g_thumbs != nullptr) {
      overview_thumbs_set_transform (g_thumbs, g_thumbs_cur_scale,
                                     g_thumbs_cur_ty);
      gtk_widget_set_visible (g_thumbs,
                              !g_app_grid_mode && g_thumbs_has_pills);
      gtk_widget_set_opacity (g_thumbs, g_thumbs_cur_opacity);
    }
    set_mini_opacity (g_app_grid_mode ? 0 : 255);
    if (g_app_grid != nullptr) {
      app_grid_set_transition_opacity (g_app_grid, false, 1.0);
      app_grid_clip_set_transition (g_app_grid_clip, false,
                                    g_app_grid_mode ? 1.0 : 0.0,
                                    g_app_grid_mode ? 0.0 : kGridSlideOffsetPx,
                                    true);
      sync_app_grid_visible (g_app_grid_mode);
    }
    g_grid_cur_opacity = g_app_grid_mode ? 1.0 : 0.0;
    g_grid_cur_slide_y = g_app_grid_mode ? 0.0 : kGridSlideOffsetPx;

    if (switch_dir == 0 && need_relayout_anim) {
      if (!prev_ws_cur_boxes.empty ()) {
        g_ws_cur_boxes = prev_ws_cur_boxes;
        const std::vector<RECT> &tgt_boxes =
            g_app_grid_mode ? g_ws_grid_boxes : g_ws_picker_boxes;
        if (g_ws_cur_boxes.size () < tgt_boxes.size ()) {
          for (size_t di = g_ws_cur_boxes.size (); di < tgt_boxes.size (); ++di)
            g_ws_cur_boxes.push_back (tgt_boxes[di]);
        } else if (g_ws_cur_boxes.size () > tgt_boxes.size ()) {
          g_ws_cur_boxes.resize (tgt_boxes.size ());
        }
      }
      for (SlotEntry &s : g_slots)
        apply_thumb_rect (s, s.current, s.current_opacity, true);
      start_state_transition (g_target_state, kRelayoutMs);
    } else {
      const std::vector<RECT> &target_boxes =
          g_app_grid_mode ? g_ws_grid_boxes : g_ws_picker_boxes;
      if (switch_off_x == 0) {
        apply_ws_boxes (target_boxes);
      } else {
        std::vector<RECT> shifted = target_boxes;
        for (RECT &r : shifted)
          r = translate_rect_x (r, switch_off_x);
        apply_ws_boxes (shifted);
      }
      for (SlotEntry &s : g_slots) {
        s.rect = g_app_grid_mode ? s.grid_rect : s.picker_rect;
        BYTE op = g_search_mode ? 0 : (g_app_grid_mode ? s.grid_opacity : 255);
        if (g_app_grid_mode && g_folder_dialog_open && op > 0)
          op = 76;
        apply_thumb_rect (s, translate_rect_x (s.rect, switch_off_x), op);
      }
      if (!g_app_grid_mode && !g_search_mode && switch_dir == 0)
        set_hover (slot_at_cursor ());
    }
  }
  dump_zorder ("placed");
}

static void
cancel_pending_place_idles (void)
{
  if (g_place_opening_source != 0) {
    g_source_remove (g_place_opening_source);
    g_place_opening_source = 0;
  }
  if (g_place_idle_source != 0) {
    g_source_remove (g_place_idle_source);
    g_place_idle_source = 0;
  }
}

static gboolean
overview_place_idle (gpointer user_data)
{
  (void)user_data;
  g_place_idle_source = 0;
  place_overview_locked (false);
  return G_SOURCE_REMOVE;
}

static gboolean
overview_place_opening_idle (gpointer user_data)
{
  (void)user_data;
  g_place_opening_source = 0;
  if (!g_placed)
    place_overview_locked (true);
  else
    restack_locked ();
  return G_SOURCE_REMOVE;
}

static void
dump_zorder (const char *tag)
{
  HWND panel = g_panel_hwnd;
  HWND overview = g_overview_hwnd;
  HWND popover = static_cast<HWND> (winome_shell_panel_top_popover_hwnd ());

  int pos_panel = -1, pos_overview = -1, pos_popover = -1, pos_chrome = -1,
      pos_caption = -1;

  HWND wnd = GetTopWindow (nullptr);
  for (int i = 0; wnd != nullptr && i < 40; ++i) {
    if (wnd == popover)
      pos_popover = i;
    if (wnd == panel)
      pos_panel = i;
    if (wnd == overview)
      pos_overview = i;
    if (wnd == g_chrome_hwnd || wnd == g_icon_hwnd)
      pos_chrome = i;
    if (wnd == g_caption_hwnd)
      pos_caption = i;
    wnd = GetWindow (wnd, GW_HWNDNEXT);
  }

  g_print ("[zorder:%s] panel@%d overview@%d popover@%d chrome@%d caption@%d "
           "contract=%s\n",
           tag, pos_panel, pos_overview, pos_popover, pos_chrome, pos_caption,
           (pos_panel >= 0 && pos_overview >= 0 && pos_panel < pos_overview)
               ? "OK"
               : "BROKEN");
}

// Verify the popover > panel > chrome windows > overview chain is already
// in order: the periodic 400ms tick and the popover-raise retries then no-op
// instead of re-sending SetWindowPos calls, each of which reshuffles the
// topmost band.
static bool
zorder_drifted (void)
{
  if (g_panel_hwnd == nullptr || !IsWindow (g_panel_hwnd))
    return false;
  if (!is_visible_hwnd (g_panel_hwnd) && !is_visible_hwnd (g_overview_hwnd))
    return false;

  HWND chain[8];
  int n = 0;
  HWND panel_popover =
      static_cast<HWND> (winome_shell_panel_top_popover_hwnd ());
  HWND ov_popup = is_visible_hwnd (g_overview_hwnd)
                      ? find_owned_overview_popup_hwnd ()
                      : nullptr;
  HWND top_popover = (panel_popover != nullptr) ? panel_popover : ov_popup;
  if (top_popover != nullptr)
    chain[n++] = top_popover;
  chain[n++] = g_panel_hwnd;
  HWND folder_hwnd = active_folder_hwnd ();
  if (is_visible_hwnd (folder_hwnd))
    chain[n++] = folder_hwnd;
  if (is_visible_hwnd (g_chrome_hwnd))
    chain[n++] = g_chrome_hwnd;
  if (is_visible_hwnd (g_icon_hwnd))
    chain[n++] = g_icon_hwnd;
  if (is_visible_hwnd (g_caption_hwnd))
    chain[n++] = g_caption_hwnd;
  if (is_visible_hwnd (g_overview_hwnd))
    chain[n++] = g_overview_hwnd;

  for (int i = 0; i < n; ++i) {
    if ((GetWindowLongPtrW (chain[i], GWL_EXSTYLE) & WS_EX_TOPMOST) == 0)
      return true;
  }

  // Every entry must sit directly below its predecessor — that is exactly
  // what the SetWindowPos sequence below produces.
  for (int i = 1; i < n; ++i)
    if (GetWindow (chain[i], GW_HWNDPREV) != chain[i - 1])
      return true;
  // Without a popover the panel must sit above all visible topmost windows,
  // EXCEPT active native/tray right-click popup menus (which must float above
  // the panel until dismissed); ignore invisible/cloaked/zero-sized OS helpers.
  if (chain[0] == g_panel_hwnd) {
    for (HWND prev = GetWindow (g_panel_hwnd, GW_HWNDPREV); prev != nullptr;
         prev = GetWindow (prev, GW_HWNDPREV)) {
      if (is_popup_menu_window (prev))
        continue;
      if (IsWindowVisible (prev) && !is_cloaked (prev)) {
        RECT r{};
        if (GetWindowRect (prev, &r) && r.right > r.left && r.bottom > r.top)
          return true;
      }
    }
  }
  return false;
}

// Re-assert popover > panel > chrome windows > overview > apps.
static void
restack_locked (void)
{
  if (g_panel_hwnd == nullptr || !IsWindow (g_panel_hwnd))
    return;

  if (!zorder_drifted ())
    return;

  HWND panel_popover =
      static_cast<HWND> (winome_shell_panel_top_popover_hwnd ());
  HWND ov_popup = is_visible_hwnd (g_overview_hwnd)
                      ? find_owned_overview_popup_hwnd ()
                      : nullptr;
  HWND top_popover = (panel_popover != nullptr) ? panel_popover : ov_popup;

  if (panel_popover == nullptr && ov_popup != nullptr) {
    ensure_hwnd_topmost (ov_popup);
    SetWindowPos (ov_popup, HWND_TOPMOST, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                      SWP_NOOWNERZORDER);
  }

  HWND foreign_popup =
      (top_popover == nullptr) ? find_lowest_popup_menu_above_panel () : nullptr;
  HWND panel_anchor = (top_popover != nullptr)
                          ? top_popover
                          : (foreign_popup != nullptr ? foreign_popup
                                                      : HWND_TOPMOST);

  if (top_popover == nullptr && foreign_popup == nullptr)
    ensure_hwnd_topmost (g_panel_hwnd);
  if (!SetWindowPos (g_panel_hwnd, panel_anchor, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                         SWP_NOOWNERZORDER))
    g_print ("[restack] panel failed gle=%lu\n", GetLastError ());

  HWND prev_hwnd = g_panel_hwnd;
  HWND folder_hwnd = active_folder_hwnd ();
  HWND chrome_layers[] = {folder_hwnd, g_chrome_hwnd, g_icon_hwnd,
                          g_caption_hwnd};
  for (HWND h : chrome_layers) {
    if (!is_visible_hwnd (h))
      continue;
    chrome_bind_owner (h);
    ensure_hwnd_topmost (h);
    SetWindowPos (h, prev_hwnd, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                      SWP_NOOWNERZORDER);
    prev_hwnd = h;
  }

  if (is_visible_hwnd (g_overview_hwnd)) {
    ensure_hwnd_topmost (g_overview_hwnd);
    if (!SetWindowPos (g_overview_hwnd, prev_hwnd, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                           SWP_NOOWNERZORDER)) {
      g_print ("[restack] overview failed gle=%lu\n", GetLastError ());
    }
    prev_hwnd = g_overview_hwnd;
  }

  // Park hidden chrome popups below the overview so they never sit between
  // g_panel_hwnd and g_overview_hwnd in the Win32 z-order chain.
  for (HWND h : chrome_layers) {
    if (h == nullptr || !IsWindow (h) || is_visible_hwnd (h))
      continue;
    chrome_bind_owner (h);
    SetWindowPos (h, prev_hwnd, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                      SWP_NOOWNERZORDER);
  }
}

// --- animation ---------------------------------------------------------------------

gboolean
anim_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
  (void)widget;
  (void)user_data;
  Animation &anim = g_anim;

  flush_pending_dwm_frame ();
  ensure_after_paint_hook (clock);

  // Frame clock keeps the 250ms transition locked to the compositor's
  // cadence (a fixed 16ms timer beats against the display refresh). Anchor
  // start_us on the first rendered frame so one-time map/GL setup time never
  // eats the opening third of the transition.
  gint64 now = gdk_frame_clock_get_frame_time (clock);
  if (!anim.started) {
    anim.started = true;
    anim.start_us = now - 8000;
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
  g_bg_radius = anim.radius_from + (anim.radius_to - anim.radius_from) * e;
  g_panel_cur_progress =
      std::clamp (anim.panel_from + (anim.panel_to - anim.panel_from) * e,
                  0.0, 1.0);
  winome_shell_panel_set_overview_progress (g_panel_cur_progress);
  bool gtk_will_paint = false;
  if (!g_ws_anim_to_boxes.empty () &&
      g_ws_anim_from_boxes.size () == g_ws_anim_to_boxes.size ()) {
    std::vector<RECT> cur_boxes (g_ws_anim_to_boxes.size ());
    for (size_t i = 0; i < g_ws_anim_to_boxes.size (); ++i)
      cur_boxes[i] =
          lerp_rect (g_ws_anim_from_boxes[i], g_ws_anim_to_boxes[i], e);
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

  g_thumbs_cur_opacity =
      anim.thumbs_from + (anim.thumbs_to - anim.thumbs_from) * p_thumbs;
  g_thumbs_cur_scale =
      anim.thumbs_scale_from +
      (anim.thumbs_scale_to - anim.thumbs_scale_from) * p_thumbs;
  g_thumbs_cur_ty =
      anim.thumbs_ty_from + (anim.thumbs_ty_to - anim.thumbs_ty_from) * p_thumbs;

  bool update_mini = false;
  BYTE desired_mini_op = 0;
  if (g_thumbs != nullptr && g_thumbs_has_pills) {
    const double f = std::clamp (g_thumbs_cur_opacity, 0.0, 1.0);
    const int dpi_scale =
        g_window != nullptr ? std::max (gtk_widget_get_scale_factor (g_window), 1)
                            : 1;
    const double pivot_x =
        (static_cast<double> (g_thumbs_rect.left) + g_thumbs_rect.right) * 0.5;
    const double pivot_y =
        (static_cast<double> (g_thumbs_rect.top) + g_thumbs_rect.bottom) * 0.5;
    const RECT thumbs_live_rect = transform_rect_around_pivot (
        g_thumbs_rect, pivot_x, pivot_y, g_thumbs_cur_scale,
        g_thumbs_cur_ty * dpi_scale);
    const bool covered =
        is_rect_fully_covered_by_boxes (thumbs_live_rect, g_ws_cur_boxes);
    const bool want_vis = (!covered && f > 0.01);
    if (gtk_widget_get_visible (g_thumbs) != (want_vis ? TRUE : FALSE)) {
      gtk_widget_set_visible (g_thumbs, want_vis ? TRUE : FALSE);
      gtk_will_paint = true;
    }
    if (want_vis) {
      overview_thumbs_set_transform (g_thumbs, g_thumbs_cur_scale,
                                     g_thumbs_cur_ty);
      if (std::fabs (gtk_widget_get_opacity (g_thumbs) - f) > 0.015) {
        gtk_widget_set_opacity (g_thumbs, f);
        gtk_will_paint = true;
      }
    }
    update_mini = true;
    desired_mini_op =
        want_vis ? static_cast<BYTE> (std::lround (255.0 * f)) : 0;
  }

  g_grid_cur_opacity =
      anim.grid_opacity_from + (anim.grid_opacity_to - anim.grid_opacity_from) * e;
  g_grid_cur_slide_y =
      anim.grid_slide_from + (anim.grid_slide_to - anim.grid_slide_from) * e;
  if (g_app_grid != nullptr && g_app_grid_clip != nullptr &&
      gtk_widget_get_visible (g_app_grid_clip)) {
    app_grid_clip_set_transition (
        g_app_grid_clip, true, std::clamp (g_grid_cur_opacity, 0.0, 1.0),
        g_grid_cur_slide_y);
  }

  // Defer DWM thumbnail commits to GdkFrameClock::after-paint whenever GTK is
  // painting a new frame this tick (including the final settle frame for
  // WINDOW_PICKER / APP_GRID), so wglSwapBuffers (g_ws_bg + g_thumbs) and
  // DwmUpdateThumbnailProperties (g_slots + g_mini_slots) hit DWM's compositor
  // in the exact same VSync with zero 1-frame empty-pill flash or lag.
  const bool defer_to_after_paint =
      ((t < 1.0 || anim.to_state != ControlsState::HIDDEN) &&
       gtk_will_paint && g_overview_after_paint_id != 0);
  if (defer_to_after_paint) {
    for (SlotEntry &s : g_slots) {
      int op = (int)(s.anim_from_opacity +
                     ((double)s.anim_to_opacity - s.anim_from_opacity) * e + 0.5);
      s.pending_rect = lerp_rect (s.anim_from, s.anim_to, e);
      s.pending_opacity = static_cast<BYTE> (std::clamp (op, 0, 255));
      s.has_pending_dwm = true;
    }
    g_dwm_pending_has_mini = update_mini;
    g_dwm_pending_mini_opacity = desired_mini_op;
    g_dwm_frame_pending = true;
  } else {
    clear_pending_dwm_frame ();
    for (SlotEntry &s : g_slots) {
      int op = (int)(s.anim_from_opacity +
                     ((double)s.anim_to_opacity - s.anim_from_opacity) * e + 0.5);
      BYTE clamped_op = static_cast<BYTE> (std::clamp (op, 0, 255));
      apply_thumb_rect (s, lerp_rect (s.anim_from, s.anim_to, e), clamped_op);
    }
    if (update_mini)
      set_mini_opacity (desired_mini_op);
  }

  if (t >= 1.0) {
    anim.source = 0;
    anim.started = false;
    auto fstats = g_anim_tracker.snapshot ();
    if (fstats.frames > 0) {
      perf_record_overview_frame_stats (fstats);
      g_print ("[perf] overview anim frames=%llu dropped=%llu avg=%.2fms p95=%.2fms max=%.2fms\n",
               static_cast<unsigned long long> (fstats.frames),
               static_cast<unsigned long long> (fstats.dropped_frames),
               fstats.avg_frame_ms, fstats.p95_frame_ms, fstats.max_frame_ms);
    }
    if (anim.to_state == ControlsState::HIDDEN) {
      finish_hide ();
      return G_SOURCE_REMOVE;
    }
    // Re-draw settled workspace boxes once anim.source == 0 to paint drop-shadows.
    bool settle_painted = false;
    if (!g_ws_anim_to_boxes.empty ())
      settle_painted = apply_ws_boxes (g_ws_anim_to_boxes, false);
    else
      settle_painted = apply_bg_rect (bg, false);
    const bool settle_defer =
        (settle_painted || defer_to_after_paint) &&
        g_overview_after_paint_id != 0;
    if (anim.to_state == ControlsState::APP_GRID) {
      g_thumbs_cur_opacity = 0.0;
      g_thumbs_cur_scale = 0.5;
      g_thumbs_cur_ty = overview_thumbs_get_height (g_thumbs) * 0.5;
      if (g_thumbs != nullptr) {
        overview_thumbs_set_transform (g_thumbs, g_thumbs_cur_scale,
                                       g_thumbs_cur_ty);
        if (gtk_widget_get_visible (g_thumbs)) {
          gtk_widget_set_opacity (g_thumbs, 0.0);
          gtk_widget_set_visible (g_thumbs, FALSE);
        }
      }
      if (settle_defer) {
        g_dwm_pending_has_mini = true;
        g_dwm_pending_mini_opacity = 0;
        g_dwm_frame_pending = true;
      } else {
        set_mini_opacity (0);
      }
      if (g_app_grid != nullptr) {
        sync_app_grid_visible (true);
        app_grid_clip_set_transition (g_app_grid_clip, false, 1.0, 0.0, true);
        app_grid_set_transition_opacity (g_app_grid, false, 1.0);
        app_grid_refresh_running (g_app_grid);
        app_grid_clip_schedule_page0_capture (g_app_grid_clip);
      }
      g_grid_cur_opacity = 1.0;
      g_grid_cur_slide_y = 0.0;
      live_wallpaper_set_paused (false);
      apply_pending_desktop_sync_if_any ();
      return G_SOURCE_REMOVE;
    }
    // Settled on WINDOW_PICKER.
    if (g_app_grid != nullptr && g_app_grid_clip != nullptr &&
        gtk_widget_get_visible (g_app_grid_clip)) {
      app_grid_clip_set_transition (g_app_grid_clip, false, 0.0,
                                    kGridSlideOffsetPx, true);
      app_grid_set_transition_opacity (g_app_grid, false, 1.0);
      sync_app_grid_visible (false);
    }
    g_grid_cur_opacity = 0.0;
    g_grid_cur_slide_y = kGridSlideOffsetPx;
    g_thumbs_cur_opacity = g_thumbs_has_pills ? 1.0 : 0.0;
    g_thumbs_cur_scale = 1.0;
    g_thumbs_cur_ty = 0.0;
    if (g_thumbs != nullptr && g_thumbs_has_pills) {
      overview_thumbs_set_transform (g_thumbs, 1.0, 0.0);
      gtk_widget_set_visible (g_thumbs, TRUE);
      gtk_widget_set_opacity (g_thumbs, 1.0);
      if (settle_defer) {
        g_dwm_pending_has_mini = true;
        g_dwm_pending_mini_opacity = 255;
        g_dwm_frame_pending = true;
      } else {
        set_mini_opacity (255);
      }
    }
    if (g_pending_desktop_sync)
      apply_pending_desktop_sync_if_any ();
    else
      schedule_settle_hover (140);
    if (g_anim.source == 0 && g_switch_source == 0)
      live_wallpaper_set_paused (false);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

// --- workspace switch animation ---------------------------------------------------

// Cancel an in-flight switch, destroying the outgoing previews without
// snapping (the overview itself is closing).
static void
switch_cancel (void)
{
  clear_pending_dwm_frame ();
  if (g_switch_source != 0) {
    gtk_widget_remove_tick_callback (g_window, g_switch_source);
    g_switch_source = 0;
  }
  g_switch_started = false;
  for (SlotEntry &l : g_leaving)
    if (l.thumb != nullptr)
      DwmUnregisterThumbnail (l.thumb);
  g_leaving.clear ();
  g_ws_hist1_boxes.clear ();
  g_ws_hist2_boxes.clear ();
}

// Snap-finish an in-flight switch with the overview still on screen.
static void
switch_finish (bool restore_hover)
{
  bool was_running = g_switch_source != 0 || !g_leaving.empty ();
  switch_cancel ();
  if (!was_running || g_window == nullptr || !gtk_widget_get_visible (g_window))
    return;
  const RECT &target_bg = g_app_grid_mode ? g_bg_grid : g_bg_picker;
  g_bg_final = target_bg;
  apply_ws_boxes (g_app_grid_mode ? g_ws_grid_boxes : g_ws_picker_boxes);
  overview_thumbs_set_indicator_value (g_thumbs, -1.0);
  winome_shell_panel_set_workspace_scroll (
      std::max (static_cast<int> (g_desktops.size ()), 1),
      static_cast<double> (std::max (g_desktop_active, 0)));
  for (SlotEntry &s : g_slots) {
    BYTE op = g_search_mode ? 0 : (g_app_grid_mode ? s.grid_opacity : 255);
    if (g_app_grid_mode && g_folder_dialog_open && op > 0)
      op = 76;
    apply_thumb_rect (s, s.rect, op);
  }
  if (winome::get_config ().dash_isolate_workspaces)
    refresh_dash_in_place ();
  if (restore_hover && !g_app_grid_mode && !g_search_mode &&
      g_target_state == ControlsState::WINDOW_PICKER) {
    set_hover (slot_at_cursor ());
  }
  if (g_anim.source == 0)
    live_wallpaper_set_paused (false);
}

static gboolean
switch_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
  (void)widget;
  (void)user_data;
  flush_pending_dwm_frame ();
  ensure_after_paint_hook (clock);
  const gint64 now = gdk_frame_clock_get_frame_time (clock);
  if (!g_switch_started) {
    g_switch_started = true;
    g_switch_start_us = now - 8000;
  }
  double t = (now - g_switch_start_us) / 1000.0 / (double)kSwitchMs;
  if (t < 0.0)
    t = 0.0;
  if (t > 1.0)
    t = 1.0;
  double e = ease_out_cubic (t);

  // WorkspacesView._scrollToActive + _updateWorkspacesState:
  // Continuous scroll adjustment position slides the entire horizontal
  // workspace strip while smoothly interpolating each workspace's scale
  // between WORKSPACE_INACTIVE_SCALE (0.94) and 1.0.
  const double scroll_pos = g_switch_from + (g_switch_to - g_switch_from) * e;
  g_switch_cur_pos = scroll_pos;
  const int ws_count = std::max (static_cast<int> (g_desktops.size ()), 1);
  const int scale =
      g_window != nullptr ? std::max (gtk_widget_get_scale_factor (g_window), 1)
                          : 1;

  if (!g_app_grid_mode) {
    std::vector<RECT> cur_boxes (static_cast<size_t> (ws_count));
    for (int d = 0; d < ws_count; ++d) {
      cur_boxes[static_cast<size_t> (d)] = picker_box_for_desktop (
          d, scroll_pos, g_bg_picker, g_monitor_w, scale);
    }
    const bool gtk_will_paint = apply_ws_boxes (cur_boxes, false);
    const bool defer =
        (t < 1.0 && gtk_will_paint && g_overview_after_paint_id != 0);
    for (SlotEntry &s : g_slots) {
      int d = std::clamp (s.desktop_index, 0, ws_count - 1);
      RECT r = remap_rect_between_boxes (
          s.canonical_picker_rect, g_bg_picker,
          cur_boxes[static_cast<size_t> (d)]);
      BYTE op = g_search_mode ? 0 : 255;
      if (defer) {
        s.pending_rect = r;
        s.pending_opacity = op;
        s.has_pending_dwm = true;
      } else {
        apply_thumb_rect (s, r, op);
      }
    }
    if (defer) {
      g_dwm_pending_has_mini = false;
      g_dwm_frame_pending = true;
    }
  } else {
    apply_ws_boxes (g_ws_grid_boxes);
    for (SlotEntry &s : g_slots) {
      BYTE op = g_search_mode ? 0 : s.grid_opacity;
      if (g_folder_dialog_open && op > 0)
        op = 76;
      apply_thumb_rect (s, s.grid_rect, op);
    }
  }
  overview_thumbs_set_indicator_value (g_thumbs, scroll_pos);
  winome_shell_panel_set_workspace_scroll (ws_count, scroll_pos);

  if (t >= 1.0) {
    clear_pending_dwm_frame ();
    switch_finish (true);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void
refresh_dash_in_place (void)
{
  if (g_dash == nullptr || g_window == nullptr || g_root == nullptr ||
      g_panel_hwnd == nullptr || !gtk_widget_get_visible (g_window) ||
      !g_placed || g_target_state == ControlsState::HIDDEN) {
    return;
  }
  HMONITOR monitor = MonitorFromWindow (g_panel_hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{};
  mi.cbSize = sizeof (mi);
  if (!GetMonitorInfoW (monitor, &mi))
    return;
  int scale = std::max (gtk_widget_get_scale_factor (g_window), 1);
  int mw = mi.rcMonitor.right - mi.rcMonitor.left;
  int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
  int wa_h = std::max (mh - g_panel_height, 1);
  int dash_max = (int)(wa_h * kDashMaxHeightRatio + 0.5);
  overview_dash_repopulate (g_dash, monitor, dash_max / scale,
                            g_desktop_active);
  int dash_w = overview_dash_get_width (g_dash) * scale;
  int dash_h = std::min (overview_dash_get_height (g_dash) * scale, dash_max);
  g_dash_top = g_panel_height + wa_h - dash_h;
  gtk_fixed_move (GTK_FIXED (g_root), g_dash, (mw - dash_w) / 2 / scale,
                  g_dash_top / scale);
}

static gboolean
dash_resync_timeout_cb (gpointer user_data G_GNUC_UNUSED)
{
  g_dash_resync_timer = 0;
  refresh_dash_in_place ();
  return G_SOURCE_REMOVE;
}

static void
schedule_dash_resync (guint delay_ms = 120)
{
  if (g_dash_resync_timer != 0)
    g_source_remove (g_dash_resync_timer);
  g_dash_resync_timer =
      g_timeout_add (delay_ms, dash_resync_timeout_cb, nullptr);
}

static void
on_overview_config_changed (void *user_data G_GNUC_UNUSED)
{
  refresh_dash_in_place ();
  const std::string live_mode =
      sanitize_live_wallpaper_mode (winome::get_config ().live_wallpaper_mode);
  if (live_mode == "off") {
    const bool was_live = g_using_live_wallpaper;
    g_using_live_wallpaper = false;
    live_wallpaper_stop ();
    g_clear_object (&g_live_wallpaper);
    g_clear_object (&g_live_wallpaper_thumb);
    if (was_live) {
      if (g_ws_bg != nullptr && gtk_widget_get_visible (g_ws_bg))
        gtk_widget_queue_draw (g_ws_bg);
      if (g_thumbs != nullptr)
        overview_thumbs_set_wallpaper (g_thumbs,
                                       active_wallpaper_thumb_texture ());
    }
  } else if (g_window != nullptr && gtk_widget_get_visible (g_window) &&
             g_panel_hwnd != nullptr) {
    HMONITOR mon = MonitorFromWindow (g_panel_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof (mi);
    if (GetMonitorInfoW (mon, &mi)) {
      LiveWallpaperTarget target = live_wallpaper_detect_target (mi.rcMonitor);
      if (target.active) {
        g_using_live_wallpaper = true;
        live_wallpaper_set_paused (g_anim.source != 0 || g_switch_source != 0);
        live_wallpaper_start (target, mi.rcMonitor, live_mode == "live",
                              on_live_wallpaper_frame, nullptr);
      }
    }
  }
}

// Animated workspace switch (WorkspacesView._scrollToActive: 250ms
// EASE_OUT_CUBIC on the scroll adjustment; zero DWM thumbnail teardown or
// EnumWindows re-enumeration when desktop count is unchanged).
static void
switch_workspace_animated (int to)
{
  if (g_window == nullptr || !gtk_widget_get_visible (g_window) ||
      !g_placed || g_target_state == ControlsState::HIDDEN) {
    return;
  }
  if (to < 0 || to == g_desktop_active)
    return;
  if (g_anim.source != 0) {
    // Open or picker<->grid transition is in flight; defer desktop sync until
    // anim_tick completes so the workspace switch is never silently dropped.
    g_pending_desktop_sync = true;
    g_pending_desktop_target = to;
    return;
  }

  // Validate against live virtual desktop enumeration (not stale g_desktops),
  // so an externally created desktop index is accepted on first arrival.
  std::vector<vd::DesktopInfo> fresh = vd::desktops ();
  if (fresh.empty () || to >= (int)fresh.size ())
    return;

  const bool was_switching = (g_switch_source != 0);
  const double from_pos =
      was_switching ? g_switch_cur_pos
                    : static_cast<double> (std::max (g_desktop_active, 0));
  switch_cancel ();

  set_hover (-1, true);
  reset_drag_state ();

  if (fresh.size () != g_desktops.size ()) {
    place_overview_locked (false, 0);
    return;
  }

  g_desktop_active = to;
  const int ws_count = std::max (static_cast<int> (g_desktops.size ()), 1);
  const int scale = std::max (gtk_widget_get_scale_factor (g_window), 1);
  g_ws_picker_boxes.resize (static_cast<size_t> (ws_count));
  g_ws_full_boxes.resize (static_cast<size_t> (ws_count));
  for (int d = 0; d < ws_count; ++d) {
    g_ws_picker_boxes[static_cast<size_t> (d)] =
        picker_box_for_desktop (d, static_cast<double> (to), g_bg_picker,
                                g_monitor_w, scale);
    g_ws_full_boxes[static_cast<size_t> (d)] =
        full_box_for_desktop (d, to, g_bg_full, g_bg_picker, g_monitor_w,
                              scale);
  }
  for (SlotEntry &s : g_slots) {
    int d = std::clamp (s.desktop_index, 0, ws_count - 1);
    const RECT &ws_picker_d = g_ws_picker_boxes[static_cast<size_t> (d)];
    const RECT &ws_full_d = g_ws_full_boxes[static_cast<size_t> (d)];
    s.picker_rect = (d == to)
                        ? s.canonical_picker_rect
                        : remap_rect_between_boxes (
                              s.canonical_picker_rect, g_bg_picker, ws_picker_d);
    s.rect = g_app_grid_mode ? s.grid_rect : s.picker_rect;
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
                    : remap_rect_between_boxes (s.canonical_start, g_bg_full,
                                                ws_full_d);
    }
  }

  if (g_thumbs != nullptr && g_thumbs_has_pills) {
    int wa_h = std::max (g_monitor_h - g_panel_height, 1);
    int box_h = (int)(wa_h * kMaxThumbnailScale) / scale;
    bool can_manage =
        vd::vtable_slots_for_build (vd::windows_build_number ())
            .can_manage_desktops;
    const double panel_ratio =
        (g_panel_height > 0 && wa_h > 0)
            ? (static_cast<double> (g_panel_height) / static_cast<double> (wa_h))
            : 0.0;
    overview_thumbs_repopulate (
        g_thumbs, (int)g_desktops.size (), g_desktop_active, box_h,
        (double)g_monitor_w / scale, (double)g_monitor_w, (double)wa_h,
        active_wallpaper_thumb_texture (),
        can_manage, false, panel_ratio);
  }

  live_wallpaper_set_paused (true);
  g_switch_from = from_pos;
  g_switch_to = static_cast<double> (to);
  g_switch_cur_pos = from_pos;
  g_switch_width = g_ws_box.right - g_ws_box.left;
  g_switch_started = false;
  g_switch_start_us = g_get_monotonic_time ();
  g_last_internal_switch_us = g_switch_start_us;
  overview_thumbs_set_indicator_value (g_thumbs, from_pos);
  winome_shell_panel_set_workspace_scroll (
      std::max (static_cast<int> (g_desktops.size ()), 1), from_pos);
  g_switch_source =
      gtk_widget_add_tick_callback (g_window, switch_tick, nullptr, nullptr);
  g_print ("[ov] workspace switch %.2f -> %d (animated)\n", from_pos, to);
}

void
start_state_transition (ControlsState to_state, guint duration_ms)
{
  if (g_window == nullptr)
    return;
  live_wallpaper_set_paused (true);
  flush_pending_dwm_frame ();
  if (g_switch_source != 0)
    switch_finish (false);
  if (g_anim.source != 0) {
    gtk_widget_remove_tick_callback (g_window, g_anim.source);
    g_anim.source = 0;
  }
  cancel_search_fade ();
  cancel_settle_hover ();
  cancel_post_hide_workarea ();
  set_hover (-1, true);
  reset_drag_state ();

  ControlsState prev_target = g_target_state;
  g_target_state = to_state;
  g_app_grid_mode = (to_state == ControlsState::APP_GRID);
  g_atomic_can_shift_app_grid.store (to_state == ControlsState::WINDOW_PICKER,
                                     std::memory_order_release);

  if (to_state != ControlsState::APP_GRID && g_app_grid != nullptr) {
    app_grid_close_open_folder (g_app_grid);
    app_grid_close_menu (g_app_grid);
  }
  if (g_dash != nullptr)
    overview_dash_set_apps_active (g_dash, to_state == ControlsState::APP_GRID);

  g_anim.from_state = prev_target;
  g_anim.to_state = to_state;
  g_anim.duration_ms = std::max (duration_ms, 1u);
  g_anim.started = false;
  g_grid_last_moved_y = INT_MIN;

  // Snapshot live visual state so mid-flight interruptions are completely
  // seamless with zero jump.
  g_anim.bg_from = g_bg_cur;
  g_ws_anim_from_boxes = g_ws_cur_boxes;
  g_anim.radius_from = g_bg_radius;
  g_anim.thumbs_from = g_thumbs_cur_opacity;
  g_anim.thumbs_scale_from = g_thumbs_cur_scale;
  g_anim.thumbs_ty_from = g_thumbs_cur_ty;
  g_anim.grid_opacity_from = g_grid_cur_opacity;
  g_anim.grid_slide_from = g_grid_cur_slide_y;
  g_anim.panel_from = g_panel_cur_progress;
  g_anim.panel_to = (to_state == ControlsState::HIDDEN) ? 0.0 : 1.0;

  const double thumbs_grid_ty =
      g_thumbs != nullptr ? (overview_thumbs_get_height (g_thumbs) * 0.5) : 0.0;

  if (to_state == ControlsState::WINDOW_PICKER) {
    g_anim.bg_to = g_bg_picker;
    g_ws_anim_to_boxes = g_ws_picker_boxes;
    g_bg_final = g_bg_picker;
    g_anim.radius_to = g_radius_picker;
    g_bg_radius_target = g_radius_picker;
    g_ws_box = g_bg_picker;
    g_anim.thumbs_to = g_thumbs_has_pills ? 1.0 : 0.0;
    g_anim.thumbs_scale_to = 1.0;
    g_anim.thumbs_ty_to = 0.0;
    g_anim.grid_opacity_to = 0.0;
    g_anim.grid_slide_to = kGridSlideOffsetPx;
    if (g_app_grid != nullptr && g_app_grid_clip != nullptr &&
        gtk_widget_get_visible (g_app_grid_clip)) {
      app_grid_set_transition_opacity (g_app_grid, true, 1.0);
      const bool using_cache = app_grid_clip_begin_transition (
          g_app_grid_clip, std::clamp (g_grid_cur_opacity, 0.0, 1.0),
          g_grid_cur_slide_y);
      if (using_cache)
        gtk_widget_set_visible (g_app_grid, FALSE);
    }
    for (SlotEntry &s : g_slots) {
      s.anim_from = s.current;
      s.anim_from_opacity = s.current_opacity;
      s.anim_to = s.picker_rect;
      s.anim_to_opacity = 255;
      s.rect = s.picker_rect;
    }
  } else if (to_state == ControlsState::APP_GRID) {
    g_anim.bg_to = g_bg_grid;
    g_ws_anim_to_boxes = g_ws_grid_boxes;
    g_bg_final = g_bg_grid;
    g_anim.radius_to = g_radius_grid;
    g_bg_radius_target = g_radius_grid;
    g_ws_box = g_bg_grid;
    // Match GNOME Shell overviewControls.js _getThumbnailsBoxParams():
    // fade opacity 1->0, scale 1->0.5 around (0.5, 0.5), translate_y 0->h/2.
    g_anim.thumbs_to = 0.0;
    g_anim.thumbs_scale_to = 0.5;
    g_anim.thumbs_ty_to = thumbs_grid_ty;
    g_anim.grid_opacity_to = 1.0;
    g_anim.grid_slide_to = 0.0;
    if (g_app_grid != nullptr) {
      ensure_app_grid_synced (false);
      const bool was_hidden =
          (prev_target != ControlsState::APP_GRID &&
           (g_app_grid_clip == nullptr ||
            !gtk_widget_get_visible (g_app_grid_clip)));
      if (was_hidden) {
        app_grid_reset_page (g_app_grid);
        g_grid_cur_opacity = 0.0;
        g_grid_cur_slide_y = kGridSlideOffsetPx;
        g_anim.grid_opacity_from = 0.0;
        g_anim.grid_slide_from = kGridSlideOffsetPx;
      }
      app_grid_set_transition_opacity (g_app_grid, true, 1.0);
      const bool using_cache = app_grid_clip_begin_transition (
          g_app_grid_clip, std::clamp (g_grid_cur_opacity, 0.0, 1.0),
          g_grid_cur_slide_y);
      if (using_cache && g_app_grid_clip != nullptr) {
        gtk_widget_set_visible (g_app_grid_clip, TRUE);
        gtk_widget_set_visible (g_app_grid, FALSE);
      } else {
        sync_app_grid_visible (true);
      }
    }
    for (SlotEntry &s : g_slots) {
      s.anim_from = s.current;
      s.anim_from_opacity = s.current_opacity;
      s.anim_to = s.grid_rect;
      s.anim_to_opacity = s.grid_opacity;
      s.rect = s.grid_rect;
    }
  } else {
    g_pending_desktop_sync = false;
    g_pending_desktop_target = -1;
    g_anim.bg_to = g_bg_full;
    g_ws_anim_to_boxes = g_ws_full_boxes;
    g_anim.radius_to = 0.0;
    g_bg_radius_target = 0.0;
    // Match GNOME Shell overviewControls.js: when closing from WINDOW_PICKER,
    // keep the thumbnails bar (and its mini window previews) stationary at
    // opacity 1.0 behind the expanding workspace so it is smoothly covered
    // without flashing; when closing directly from APP_GRID, keep it hidden.
    const bool keep_hidden_from_grid =
        (prev_target == ControlsState::APP_GRID && g_thumbs_cur_opacity <= 0.01);
    if (keep_hidden_from_grid) {
      g_anim.thumbs_to = 0.0;
      g_anim.thumbs_scale_to = 0.5;
      g_anim.thumbs_ty_to = thumbs_grid_ty;
    } else {
      g_anim.thumbs_to = g_thumbs_has_pills ? 1.0 : 0.0;
      g_anim.thumbs_scale_to = 1.0;
      g_anim.thumbs_ty_to = 0.0;
    }
    g_anim.grid_opacity_to = 0.0;
    g_anim.grid_slide_to = kGridSlideOffsetPx;
    if (g_app_grid != nullptr && g_app_grid_clip != nullptr &&
        gtk_widget_get_visible (g_app_grid_clip)) {
      app_grid_set_transition_opacity (g_app_grid, true, 1.0);
      const bool using_cache = app_grid_clip_begin_transition (
          g_app_grid_clip, std::clamp (g_grid_cur_opacity, 0.0, 1.0),
          g_grid_cur_slide_y);
      if (using_cache)
        gtk_widget_set_visible (g_app_grid, FALSE);
    }
    for (SlotEntry &s : g_slots) {
      s.anim_from = s.current;
      s.anim_from_opacity = s.current_opacity;
      s.anim_to = s.start;
      s.anim_to_opacity = s.minimized_any ? 0 : 255;
    }
  }

  if (g_ws_anim_from_boxes.size () != g_ws_anim_to_boxes.size ())
    g_ws_anim_from_boxes = g_ws_anim_to_boxes;

  g_ws_hist1_boxes.clear ();
  g_ws_hist2_boxes.clear ();

  restack_locked ();
  g_anim.start_us = g_get_monotonic_time ();
  g_anim_tracker.reset ();
  g_anim.source =
      gtk_widget_add_tick_callback (g_window, anim_tick, nullptr, nullptr);
}

gboolean
overview_idle_trim_cb (gpointer user_data)
{
  (void)user_data;
  g_idle_trim_timer = 0;
  if (g_window == nullptr || !gtk_widget_get_visible (g_window))
    app_icon_cache_trim (64);
  return G_SOURCE_REMOVE;
}

void
finish_hide (void)
{
  g_panel_cur_progress = 0.0;
  winome_shell_panel_set_overview_progress (0.0);
  winome_shell_panel_set_workspace_scroll (-1, -1.0);
  live_wallpaper_stop ();
  clear_pending_dwm_frame ();
  cancel_settle_hover ();
  set_hover (-1, true);
  cancel_pending_place_idles ();
  cancel_delayed_rebuild ();
  if (g_anim.source != 0) {
    gtk_widget_remove_tick_callback (g_window, g_anim.source);
    g_anim.source = 0;
  }
  bool was_overview_fg =
      (g_overview_hwnd != nullptr && GetForegroundWindow () == g_overview_hwnd);
  if (g_window != nullptr && gtk_widget_get_visible (g_window))
    gtk_widget_set_visible (g_window, FALSE);
  destroy_previews ();
  switch_cancel ();
  destroy_mini_previews ();
  g_slots.shrink_to_fit ();
  g_mini_slots.shrink_to_fit ();
  g_placed = false;
  g_target_state = ControlsState::HIDDEN;
  g_pending_show_state = ControlsState::WINDOW_PICKER;
  g_pending_desktop_sync = false;
  g_pending_desktop_target = -1;
  g_atomic_can_shift_app_grid.store (false, std::memory_order_release);
  g_pending_shift_app_grid.store (false, std::memory_order_release);
  reset_drag_state ();
  g_drag_just_ended = false;
  if (g_dash != nullptr) {
    overview_dash_close_menu (g_dash);
    overview_dash_set_apps_active (g_dash, false);
  }
  if (g_app_grid != nullptr) {
    app_grid_close_open_folder (g_app_grid);
    app_grid_close_menu (g_app_grid);
    app_grid_reset_page (g_app_grid);
    app_grid_clip_set_transition (g_app_grid_clip, false, 1.0, 0.0, true);
    app_grid_set_transition_opacity (g_app_grid, false, 1.0);
    sync_app_grid_visible (false);
  }
  g_folder_dialog_open = false;
  if (g_search_fade_source != 0 && g_window != nullptr) {
    gtk_widget_remove_tick_callback (g_window, g_search_fade_source);
    g_search_fade_source = 0;
  }
  g_search_fade_cur = 0.0;
  g_search_fade_from = 0.0;
  g_search_fade_to = 0.0;
  g_search_mode = false;
  if (g_search != nullptr) {
    gtk_widget_remove_css_class (g_search, "search-active");
    gtk_widget_set_opacity (g_search, 1.0);
    gtk_widget_set_can_target (g_search, TRUE);
  }
  if (g_dash != nullptr) {
    gtk_widget_set_opacity (g_dash, 1.0);
    gtk_widget_set_can_target (g_dash, TRUE);
  }
  if (g_ws_bg != nullptr)
    gtk_widget_set_opacity (g_ws_bg, 1.0);
  if (g_app_grid_clip != nullptr)
    gtk_widget_set_opacity (g_app_grid_clip, 1.0);
  if (g_search_view != nullptr) {
    overview_search_view_reset (g_search_view);
    gtk_widget_set_opacity (g_search_view, 0.0);
    gtk_widget_set_visible (g_search_view, FALSE);
  }
  g_grid_cur_opacity = 0.0;
  g_grid_cur_slide_y = kGridSlideOffsetPx;
  g_grid_last_moved_y = INT_MIN;
  g_app_grid_mode = false;
  if (g_idle_trim_timer != 0) {
    g_source_remove (g_idle_trim_timer);
    g_idle_trim_timer = 0;
  }
  if (g_window != nullptr) {
    g_idle_trim_timer =
        g_timeout_add_seconds (15, overview_idle_trim_cb, nullptr);
  }
  // The side controls stay VISIBLE at their end-of-close geometry (the
  // background rests on the full work area, radius 0): the window keeps its
  // fullscreen size while hidden, so the first frame of the next open shows
  // the wallpaper-covered work area — GNOME's t=0 — instead of a bare
  // #222226 flash while the placement idle is still pending.
  g_bg_cur = g_bg_full;
  g_ws_cur_boxes.assign (1, g_bg_full);
  g_ws_hist1_boxes.clear ();
  g_ws_hist2_boxes.clear ();
  g_bg_radius = 0.0;
  g_ws_bg_last_radius = -1.0;
  g_ws_bg_settled_shadow_drawn = false;
  g_thumbs_cur_opacity = 0.0;
  g_thumbs_cur_scale = 1.0;
  g_thumbs_cur_ty = 0.0;
  if (g_thumbs != nullptr) {
    overview_thumbs_set_transform (g_thumbs, 1.0, 0.0);
    gtk_widget_set_opacity (g_thumbs, 0.0);
    gtk_widget_set_visible (g_thumbs, FALSE);
  }
  if (g_overview_hwnd != nullptr && IsWindow (g_overview_hwnd))
    apply_ex_styles (g_overview_hwnd);
  bool was_discarded =
      g_restore_focus_discarded.exchange (false, std::memory_order_acq_rel);
  HWND restore_hwnd = g_prev_foreground_hwnd;
  g_prev_foreground_hwnd = nullptr;
  AllowSetForegroundWindow (ASFW_ANY);
  if (!was_discarded && was_overview_fg &&
      is_external_foreground_target (restore_hwnd) &&
      IsWindowVisible (restore_hwnd) && !IsIconic (restore_hwnd) &&
      !is_cloaked (restore_hwnd) &&
      (g_desktops.empty () || vd::window_on_current (restore_hwnd))) {
    SetForegroundWindow (restore_hwnd);
  }
  schedule_post_hide_workarea (180);
}

static void
cancel_delayed_rebuild (void)
{
  if (g_close_rebuild_timer != 0) {
    g_source_remove (g_close_rebuild_timer);
    g_close_rebuild_timer = 0;
  }
}

static gboolean
delayed_rebuild_cb (gpointer user_data G_GNUC_UNUSED)
{
  if (g_drag_slot >= 0 || g_drag_active)
    return G_SOURCE_CONTINUE;
  g_close_rebuild_timer = 0;
  if (g_window != nullptr && gtk_widget_get_visible (g_window) && g_placed &&
      g_target_state != ControlsState::HIDDEN && overview_active ()) {
    rebuild_now ();
  }
  return G_SOURCE_REMOVE;
}

static void
schedule_delayed_rebuild (guint delay_ms)
{
  cancel_delayed_rebuild ();
  g_close_rebuild_timer =
      g_timeout_add (delay_ms, delayed_rebuild_cb, nullptr);
}

static void
reset_drag_state (void)
{
  if (g_drag_tick_source != 0 && g_window != nullptr) {
    gtk_widget_remove_tick_callback (g_window, g_drag_tick_source);
    g_drag_tick_source = 0;
  }
  if (g_drag_snapback_source != 0 && g_window != nullptr) {
    gtk_widget_remove_tick_callback (g_window, g_drag_snapback_source);
    g_drag_snapback_source = 0;
  }
  if (g_drag_snapback_slot >= 0 &&
      g_drag_snapback_slot < static_cast<int> (g_slots.size ())) {
    SlotEntry &s = g_slots[static_cast<size_t> (g_drag_snapback_slot)];
    apply_thumb_rect (s, s.rect, 255);
  }
  g_drag_snapback_slot = -1;
  g_drag_slot = -1;
  g_drag_hwnd = nullptr;
  g_drag_active = false;
  g_drag_thumbs_revealed = false;
  if (g_drag_drop_ws_peek != -1) {
    g_drag_drop_ws_peek = -1;
    if (g_ws_bg != nullptr)
      gtk_widget_queue_draw (g_ws_bg);
  }
  if (g_thumbs != nullptr)
    overview_thumbs_set_drop_target (g_thumbs, -1);
}

static bool
update_dragged_slot_transform (gint64 now_us)
{
  if (!g_drag_active || g_drag_slot < 0 ||
      g_drag_slot >= static_cast<int> (g_slots.size ()) ||
      g_window == nullptr) {
    return false;
  }
  const int scale = std::max (gtk_widget_get_scale_factor (g_window), 1);
  double t = (now_us - g_drag_scale_start_us) / 1000.0 /
             static_cast<double> (kDragScaleMs);
  t = std::clamp (t, 0.0, 1.0);
  const double e = ease_out_quad (t);
  const double cur_scale = 1.0 + (g_drag_target_scale - 1.0) * e;
  const BYTE cur_op = static_cast<BYTE> (std::clamp (
      static_cast<int> (
          std::lround (255.0 + (kDraggingWindowOpacity - 255.0) * e)),
      64, 255));

  const int orig_w = g_drag_orig_rect.right - g_drag_orig_rect.left;
  const int orig_h = g_drag_orig_rect.bottom - g_drag_orig_rect.top;
  const int dw =
      std::max (static_cast<int> (std::lround (orig_w * cur_scale)), 48 * scale);
  const int dh =
      std::max (static_cast<int> (std::lround (orig_h * cur_scale)), 36 * scale);
  // dnd.js _updateActorPosition: scale around the grab point so the preview
  // never teleports to the cursor center at t=0.
  const int left =
      g_drag_cur_px + static_cast<int> (std::lround (g_drag_offset_x * cur_scale));
  const int top =
      g_drag_cur_py + static_cast<int> (std::lround (g_drag_offset_y * cur_scale));
  RECT moved{left, top, left + dw, top + dh};
  apply_thumb_rect (g_slots[static_cast<size_t> (g_drag_slot)], moved, cur_op);
  return t < 1.0;
}

static gboolean
drag_scale_tick (GtkWidget *widget G_GNUC_UNUSED, GdkFrameClock *clock,
                 gpointer user_data G_GNUC_UNUSED)
{
  if (!g_drag_active || g_drag_slot < 0) {
    g_drag_tick_source = 0;
    return G_SOURCE_REMOVE;
  }
  const gint64 now = gdk_frame_clock_get_frame_time (clock);
  if (!update_dragged_slot_transform (now)) {
    g_drag_tick_source = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static gboolean
drag_snapback_tick (GtkWidget *widget G_GNUC_UNUSED, GdkFrameClock *clock,
                    gpointer user_data G_GNUC_UNUSED)
{
  if (g_drag_snapback_slot < 0 ||
      g_drag_snapback_slot >= static_cast<int> (g_slots.size ())) {
    g_drag_snapback_slot = -1;
    g_drag_snapback_source = 0;
    return G_SOURCE_REMOVE;
  }
  const gint64 now = gdk_frame_clock_get_frame_time (clock);
  double t = (now - g_drag_snapback_start_us) / 1000.0 /
             static_cast<double> (kDragSnapBackMs);
  t = std::clamp (t, 0.0, 1.0);
  const double e = ease_out_quad (t);
  SlotEntry &s = g_slots[static_cast<size_t> (g_drag_snapback_slot)];
  RECT cur = lerp_rect (g_drag_snapback_from, s.rect, e);
  BYTE op = static_cast<BYTE> (std::clamp (
      static_cast<int> (std::lround (
          g_drag_snapback_from_op + (255.0 - g_drag_snapback_from_op) * e)),
      0, 255));
  apply_thumb_rect (s, cur, op);
  if (t >= 1.0) {
    apply_thumb_rect (s, s.rect, 255);
    g_drag_snapback_slot = -1;
    g_drag_snapback_source = 0;
    if (!g_app_grid_mode && g_target_state == ControlsState::WINDOW_PICKER)
      set_hover (slot_at_cursor ());
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static int
resolve_drag_drop_workspace (int px, int py, int scale, int *out_thumb_pill,
                             int *out_peek_ws)
{
  if (out_thumb_pill != nullptr)
    *out_thumb_pill = -1;
  if (out_peek_ws != nullptr)
    *out_peek_ws = -1;

  // 1. ThumbnailsBox pills at the top (workspaceThumbnail.js handleDragOver).
  if (g_thumbs != nullptr && g_thumbs_rect.right > g_thumbs_rect.left) {
    const int pad = 8 * scale;
    if (px >= g_thumbs_rect.left - pad && px <= g_thumbs_rect.right + pad &&
        py >= g_thumbs_rect.top - pad && py <= g_thumbs_rect.bottom + pad) {
      double lx = static_cast<double> (px - g_thumbs_rect.left) / scale;
      double ly = static_cast<double> (py - g_thumbs_rect.top) / scale;
      int pill = overview_thumbs_drop_target_at (g_thumbs, lx, ly);
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
  const int active_ws = std::max (g_desktop_active, 0);
  const RECT dragged_rect =
      (g_drag_slot >= 0 && g_drag_slot < static_cast<int> (g_slots.size ()))
          ? g_slots[static_cast<size_t> (g_drag_slot)].current
          : RECT{px, py, px + 1, py + 1};
  const int dragged_w = std::max<int> (dragged_rect.right - dragged_rect.left, 1);

  for (int d = 0; d < static_cast<int> (g_ws_cur_boxes.size ()); ++d) {
    if (d == active_ws)
      continue;
    const RECT &b = g_ws_cur_boxes[static_cast<size_t> (d)];
    if (b.right <= 0 || b.left >= g_monitor_w)
      continue;
    bool hit = (px >= b.left && px < b.right && py >= b.top && py < b.bottom);
    if (!hit && !g_app_grid_mode &&
        py >= g_bg_picker.top && py < g_bg_picker.bottom) {
      if (d == active_ws - 1 &&
          (px < g_bg_picker.left ||
           dragged_rect.left < b.right - dragged_w / 4)) {
        hit = true;
      } else if (d == active_ws + 1 &&
                 (px >= g_bg_picker.right ||
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

  if (g_app_grid_mode || g_search_mode || g_search_fade_cur > 0.005 ||
      g_folder_dialog_open || g_desktops.empty () || g_window == nullptr ||
      g_target_state == ControlsState::HIDDEN || !overview_active ()) {
    gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_DENIED);
    return;
  }

  if (g_switch_source != 0)
    switch_finish (false);

  int scale = std::max (gtk_widget_get_scale_factor (g_window), 1);
  int px = (int)(x * scale);
  int py = (int)(y * scale);
  if ((g_panel_height > 0 && py < g_panel_height) || py >= g_dash_top ||
      (px >= g_search_rect.left && px < g_search_rect.right &&
       py >= g_search_rect.top && py < g_search_rect.bottom) ||
      (g_thumbs_rect.right > g_thumbs_rect.left &&
       px >= g_thumbs_rect.left && px < g_thumbs_rect.right &&
       py >= g_thumbs_rect.top && py < g_thumbs_rect.bottom)) {
    gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_DENIED);
    return;
  }

  const int active_ws = std::max (g_desktop_active, 0);
  for (int i = 0; i < (int)g_slots.size (); ++i) {
    if (g_slots[i].desktop_index != active_ws)
      continue;
    const RECT r = slot_display_rect (i);
    if (px >= r.left && px < r.right && py >= r.top && py < r.bottom) {
      set_hover (-1, true);
      g_drag_slot = i;
      g_drag_hwnd = g_slots[i].target;
      g_drag_orig_rect = g_slots[i].rect;
      g_drag_start_px = px;
      g_drag_start_py = py;
      g_drag_cur_px = px;
      g_drag_cur_py = py;
      g_drag_offset_x = g_drag_orig_rect.left - px;
      g_drag_offset_y = g_drag_orig_rect.top - py;
      const int orig_w = g_drag_orig_rect.right - g_drag_orig_rect.left;
      const int orig_h = g_drag_orig_rect.bottom - g_drag_orig_rect.top;
      const int max_dim = std::max (orig_w, orig_h);
      g_drag_target_scale =
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
  if (g_drag_slot < 0 || g_drag_slot >= (int)g_slots.size () ||
      g_window == nullptr)
    return;

  int scale = std::max (gtk_widget_get_scale_factor (g_window), 1);
  g_drag_cur_px = g_drag_start_px + (int)(dx * scale);
  g_drag_cur_py = g_drag_start_py + (int)(dy * scale);

  if (!g_drag_active) {
    if (dx * dx + dy * dy < 36.0)
      return;
    g_drag_active = true;
    // Claim the gesture sequence so GtkGestureClick (on_click_released, which
    // runs before on_drag_end on button release) does not misinterpret the
    // drop release over a peeking workspace as a workspace-switch click.
    gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED);
    set_hover (-1, true);
    promote_slot_thumb_to_top (g_slots[g_drag_slot]);
    g_drag_scale_start_us = g_get_monotonic_time ();
    if (g_drag_tick_source == 0) {
      g_drag_tick_source = gtk_widget_add_tick_callback (
          g_window, drag_scale_tick, nullptr, nullptr);
    }
  }

  update_dragged_slot_transform (g_get_monotonic_time ());

  int thumb_pill = -1;
  int peek_ws = -1;
  resolve_drag_drop_workspace (g_drag_cur_px, g_drag_cur_py, scale, &thumb_pill,
                               &peek_ws);
  if (g_thumbs != nullptr)
    overview_thumbs_set_drop_target (g_thumbs, thumb_pill);
  if (g_drag_drop_ws_peek != peek_ws) {
    g_drag_drop_ws_peek = peek_ws;
    if (g_ws_bg != nullptr)
      gtk_widget_queue_draw (g_ws_bg);
  }
}

void
on_drag_end (GtkGestureDrag *drag G_GNUC_UNUSED, double dx, double dy,
             gpointer user_data G_GNUC_UNUSED)
{
  if (g_drag_slot < 0)
    return;

  if (g_drag_tick_source != 0 && g_window != nullptr) {
    gtk_widget_remove_tick_callback (g_window, g_drag_tick_source);
    g_drag_tick_source = 0;
  }

  const int dragged_slot = g_drag_slot;
  const bool was_drag = g_drag_active;
  const HWND target_hwnd = g_drag_hwnd;
  const int scale =
      g_window != nullptr ? std::max (gtk_widget_get_scale_factor (g_window), 1)
                          : 1;
  const int end_px = g_drag_start_px + (int)(dx * scale);
  const int end_py = g_drag_start_py + (int)(dy * scale);

  g_drag_active = false;
  g_drag_slot = -1;
  g_drag_hwnd = nullptr;

  if (g_drag_drop_ws_peek != -1) {
    g_drag_drop_ws_peek = -1;
    if (g_ws_bg != nullptr)
      gtk_widget_queue_draw (g_ws_bg);
  }
  if (g_thumbs != nullptr)
    overview_thumbs_set_drop_target (g_thumbs, -1);

  if (!was_drag)
    return;

  g_drag_just_ended = true;

  int thumb_pill = -1;
  int peek_ws = -1;
  const int tgt = resolve_drag_drop_workspace (end_px, end_py, scale,
                                               &thumb_pill, &peek_ws);

  bool moved = false;
  if (tgt >= 0 && target_hwnd != nullptr && IsWindow (target_hwnd)) {
    if (tgt < (int)g_desktops.size () && tgt != g_desktop_active) {
      moved = vd::move_window_to_desktop (target_hwnd, tgt);
    } else if (tgt == (int)g_desktops.size () && vd::create_desktop ()) {
      int n = vd::count ();
      if (n > 0)
        moved = vd::move_window_to_desktop (target_hwnd, n - 1);
    }
  }

  if (moved) {
    reset_drag_state ();
    rebuild_now ();
    // Explorer may finish committing the moved window's desktop GUID a few
    // dozen ms asynchronously; run a lightweight follow-up dash refresh so
    // workspace-isolated mode never leaves stale icons behind.
    schedule_dash_resync (120);
    return;
  }

  // dnd.js _cancelDrag: smoothly animate the dragged preview back to its slot
  // over SNAP_BACK_ANIMATION_TIME (250ms EASE_OUT_QUAD) instead of rebuilding.
  if (dragged_slot >= 0 &&
      dragged_slot < static_cast<int> (g_slots.size ()) &&
      g_window != nullptr) {
    g_drag_snapback_slot = dragged_slot;
    g_drag_snapback_from = g_slots[static_cast<size_t> (dragged_slot)].current;
    g_drag_snapback_from_op =
        g_slots[static_cast<size_t> (dragged_slot)].current_opacity;
    g_drag_snapback_start_us = g_get_monotonic_time ();
    g_drag_snapback_source = gtk_widget_add_tick_callback (
        g_window, drag_snapback_tick, nullptr, nullptr);
  }
}

// --- clicks --------------------------------------------------------------------------

static void
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
          SetForegroundWindow (hwnd);
          AttachThreadInput (cur_tid, fg_tid, FALSE);
        }
      }
    }
  }
  restack_locked ();
}

static void
focus_overview_window (bool focus_search)
{
  acquire_overview_foreground (g_overview_hwnd);
  if (g_window == nullptr)
    return;
  if (focus_search) {
    if (g_search != nullptr)
      gtk_widget_grab_focus (g_search);
  } else if (g_focus_sink != nullptr) {
    gtk_window_set_focus (GTK_WINDOW (g_window), g_focus_sink);
  } else if (g_root != nullptr) {
    gtk_widget_grab_focus (g_root);
  }
}

static void
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

  if (g_drag_active) {
    return;
  }
  if (g_drag_just_ended) {
    g_drag_just_ended = false;
    return;
  }
  if (button != GDK_BUTTON_PRIMARY && button != GDK_BUTTON_MIDDLE)
    return;

  int scale = g_window != nullptr ? gtk_widget_get_scale_factor (g_window) : 1;
  int px = (int)(x * scale);
  int py = (int)(y * scale);

  // A click that just dismissed a panel popover floating above the overview
  // is consumed by GNOME's menu grab: close the menu only.
  if (g_target_state == ControlsState::HIDDEN || !overview_active ())
    return;

  if (winome_shell_panel_popover_just_closed ()) {
    g_print ("[ov] click (%d,%d) swallowed (popover dismiss)\n", px, py);
    return;
  }

  if (g_panel_height > 0 && py < g_panel_height) {
    g_print ("[ov] click (%d,%d) ignored (panel strip)\n", px, py);
    restack_locked ();
    return;
  }

  // Clicking outside the search entry when no search query is active unfocuses
  // the search box so its focus ring clears cleanly.
  const bool in_search_rect =
      (px >= g_search_rect.left && px < g_search_rect.right &&
       py >= g_search_rect.top && py < g_search_rect.bottom);
  if (!in_search_rect && g_search_query.empty () && g_focus_sink != nullptr) {
    gtk_window_set_focus (GTK_WINDOW (g_window), g_focus_sink);
  }

  if (g_search_mode) {
    if (in_search_rect)
      focus_overview_for_search ();
    restack_locked ();
    return;
  }

  const int active_ws = std::max (g_desktop_active, 0);

  if (g_app_grid_mode) {
    if (g_folder_dialog_open && g_app_grid != nullptr) {
      app_grid_close_open_folder (g_app_grid);
      restack_locked ();
      return;
    }
    POINT pt = {px, py};
    for (int d = 0; d < static_cast<int> (g_ws_cur_boxes.size ()); ++d) {
      const RECT &b = g_ws_cur_boxes[static_cast<size_t> (d)];
      if (pt.x >= b.left && pt.x < b.right && pt.y >= b.top && pt.y < b.bottom) {
        if (d != active_ws && vd::switch_to (d))
          switch_workspace_animated (d);
        g_print ("[ov] click (%d,%d) small workspace %d -> window picker\n", px,
                 py, d);
        overview_toggle_app_grid ();
        return;
      }
    }
    if (pt.x >= g_ws_box.left && pt.x < g_ws_box.right &&
        pt.y >= g_ws_box.top && pt.y < g_ws_box.bottom) {
      g_print ("[ov] click (%d,%d) small workspace -> window picker\n", px,
               py);
      overview_toggle_app_grid ();
      return;
    }
    restack_locked ();
    return;
  }

  POINT pt = {px, py};

  // Clicking on an inactive peeking workspace (or a window preview inside an
  // inactive peeking workspace) switches to that workspace while keeping the
  // overview open (workspace.js _init clickGesture + _onCloneSelected).
  for (int d = 0; d < static_cast<int> (g_ws_cur_boxes.size ()); ++d) {
    if (d == active_ws)
      continue;
    const RECT &b = g_ws_cur_boxes[static_cast<size_t> (d)];
    if (b.right <= 0 || b.left >= g_monitor_w)
      continue;
    if (pt.x >= b.left && pt.x < b.right && pt.y >= b.top && pt.y < b.bottom) {
      if (button == GDK_BUTTON_PRIMARY) {
        g_print ("[ov] click (%d,%d) peeking workspace %d -> switch\n", px, py,
                 d);
        if (vd::switch_to (d))
          switch_workspace_animated (d);
      } else if (button == GDK_BUTTON_MIDDLE) {
        for (const SlotEntry &e : g_slots) {
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
    for (const SlotEntry &e : g_slots) {
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
  for (int i = 0; i < (int)g_slots.size (); ++i) {
    if (g_slots[i].desktop_index != active_ws)
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
  if (py >= g_dash_top) {
    g_print ("[ov] click (%d,%d) ignored (dash band)\n", px, py);
    restack_locked ();
    return;
  }
  if (pt.x >= g_search_rect.left && pt.x < g_search_rect.right &&
      pt.y >= g_search_rect.top && pt.y < g_search_rect.bottom) {
    g_print ("[ov] click (%d,%d) focus search entry\n", px, py);
    focus_overview_for_search ();
    return;
  }
  // The workspace thumbnails row handles its own input (clicking a pill
  // switches desktop; the overview stays open, exactly like upstream).
  if (g_thumbs_rect.right > g_thumbs_rect.left &&
      pt.x >= g_thumbs_rect.left && pt.x < g_thumbs_rect.right &&
      pt.y >= g_thumbs_rect.top && pt.y < g_thumbs_rect.bottom) {
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
  if (pt.x >= g_ws_box.left && pt.x < g_ws_box.right &&
      pt.y >= g_ws_box.top && pt.y < g_ws_box.bottom) {
    g_print ("[ov] click (%d,%d) blank inside workspace -> close\n", px, py);
    close_overview ("ov-click-blank");
    return;
  }

  g_print ("[ov] click (%d,%d) ignored (outside workspace)\n", px, py);
  restack_locked ();
}

static void
on_overview_surface_layout (GdkSurface *surface G_GNUC_UNUSED,
                            int width G_GNUC_UNUSED,
                            int height G_GNUC_UNUSED,
                            gpointer user_data G_GNUC_UNUSED)
{
  if (g_window == nullptr || !gtk_widget_get_visible (g_window))
    return;
  if (g_anim.source != 0 || g_switch_source != 0)
    return;
  restack_locked ();
  if (g_placed && g_target_state != ControlsState::HIDDEN) {
    int scale = std::max (gtk_widget_get_scale_factor (g_window), 1);
    int cur_search_h = g_placed_search_h;
    if (g_search != nullptr && gtk_widget_get_visible (g_search)) {
      GtkRequisition nat{};
      gtk_widget_get_preferred_size (g_search, nullptr, &nat);
      if (nat.height > 0)
        cur_search_h = nat.height;
    }
    if (scale != g_placed_scale || cur_search_h != g_placed_search_h)
      overview_reposition ();
  }
}

void
on_realize (GtkWidget *widget, gpointer user_data)
{
  (void)user_data;
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (widget));
  if (surface != nullptr) {
    HWND hwnd = static_cast<HWND> (gdk_win32_surface_get_handle (surface));
    g_overview_hwnd = hwnd;
    apply_ex_styles (hwnd);
    if (!g_overview_surface_hooked) {
      g_overview_surface_hooked = true;
      g_signal_connect (surface, "layout",
                        G_CALLBACK (on_overview_surface_layout), nullptr);
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
  g_place_opening_source = g_idle_add_full (
      G_PRIORITY_HIGH_IDLE, overview_place_opening_idle, nullptr, nullptr);
}

// ThumbnailsBox click action -> Workspace.activate: switch the desktop and
// relayout (the overview stays open, exactly like upstream).
void
on_thumb_clicked (int index, void *user_data)
{
  (void)user_data;
  g_print ("[ov] thumb click -> desktop %d (current %d)\n", index,
           g_desktop_active);
  if (index < 0 || index == g_desktop_active)
    return;
  g_last_ws_scroll_us = g_get_monotonic_time ();
  if (vd::switch_to (index)) {
    switch_workspace_animated (index);
  } else {
    g_print ("[ov] desktop switch failed\n");
  }
}

// WorkspacesDisplay._onScrollEvent + WindowManager.handleWorkspaceScroll:
// Scroll wheel anywhere on the Overview workspace area switches to the
// previous/next workspace (throttled by SCROLL_TIMEOUT_TIME = 150ms).
static gboolean
on_overview_scroll (GtkEventControllerScroll *ctrl G_GNUC_UNUSED, double dx,
                    double dy, gpointer user_data G_GNUC_UNUSED)
{
  if (g_window == nullptr || !gtk_widget_get_visible (g_window) ||
      !g_placed || g_target_state == ControlsState::HIDDEN ||
      !overview_active ()) {
    return GDK_EVENT_PROPAGATE;
  }
  if (g_drag_active || g_drag_slot >= 0 || g_drag_snapback_source != 0 ||
      g_search_mode || g_folder_dialog_open)
    return GDK_EVENT_PROPAGATE;

  // In APP_GRID mode, let the app grid's own scroll controller handle wheel
  // scrolling when the pointer is over the app grid area; scrolling above the
  // app grid (e.g. over the top miniature workspaces row) switches workspaces.
  if (g_app_grid_mode || g_target_state == ControlsState::APP_GRID) {
    if (g_overview_hwnd != nullptr) {
      int scale = std::max (gtk_widget_get_scale_factor (g_window), 1);
      POINT pt{};
      if (GetCursorPos (&pt) && ScreenToClient (g_overview_hwnd, &pt)) {
        if (pt.y >= g_grid_y * scale)
          return GDK_EVENT_PROPAGATE;
      }
    }
  }

  if (g_desktops.size () <= 1 || g_desktop_active < 0)
    return GDK_EVENT_PROPAGATE;

  double delta = (std::fabs (dy) >= std::fabs (dx)) ? dy : dx;
  if (std::fabs (delta) < 1e-4)
    return GDK_EVENT_PROPAGATE;

  const gint64 now_us = g_get_monotonic_time ();
  if (now_us - g_last_ws_scroll_us <
      static_cast<gint64> (kWorkspaceScrollTimeoutMs) * 1000) {
    return GDK_EVENT_STOP;
  }

  const int step = (delta > 0.0) ? 1 : -1;
  const int target = g_desktop_active + step;
  if (target < 0 || target >= static_cast<int> (g_desktops.size ()))
    return GDK_EVENT_STOP;

  g_last_ws_scroll_us = now_us;
  if (vd::switch_to (target))
    switch_workspace_animated (target);
  return GDK_EVENT_STOP;
}

void
on_thumb_add_desktop (void *user_data G_GNUC_UNUSED)
{
  if (vd::create_desktop ()) {
    int n = vd::count ();
    if (n > 0)
      vd::switch_to (n - 1);
    rebuild_now ();
  }
}

void
on_thumb_remove_desktop (int index, void *user_data G_GNUC_UNUSED)
{
  if (vd::remove_desktop (index))
    rebuild_now ();
}

void
on_dash_show_apps_clicked (void *user_data G_GNUC_UNUSED)
{
  overview_toggle_app_grid ();
}

void
on_dash_changed (void *user_data G_GNUC_UNUSED)
{
  rebuild_now ();
}

void
on_app_grid_launch_done (void *user_data G_GNUC_UNUSED)
{
  overview_discard_restore_focus ();
  close_overview ("app-grid-launch");
}

void
on_app_grid_pins_changed (void *user_data G_GNUC_UNUSED)
{
  if (g_dash != nullptr && g_window != nullptr &&
      gtk_widget_get_visible (g_window)) {
    if (g_target_state == ControlsState::APP_GRID && g_panel_hwnd != nullptr) {
      refresh_dash_in_place ();
      if (g_app_grid != nullptr)
        app_grid_refresh_running (g_app_grid);
      return;
    }
    rebuild_now ();
  }
}

gboolean
dynamic_workspaces_startup_cb (gpointer user_data)
{
  (void)user_data;
  g_dyn_ws_startup_timer = 0;
  if (g_window == nullptr)
    return G_SOURCE_REMOVE;
  if (g_anim.source != 0 || g_switch_source != 0 ||
      (gtk_widget_get_visible (g_window) && g_placed &&
       g_target_state != ControlsState::HIDDEN && overview_active ())) {
    return G_SOURCE_REMOVE;
  }
  HMONITOR mon = (g_panel_hwnd != nullptr)
                     ? MonitorFromWindow (g_panel_hwnd, MONITOR_DEFAULTTOPRIMARY)
                     : MonitorFromPoint (POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi{};
  mi.cbSize = sizeof (mi);
  if (!GetMonitorInfoW (mon, &mi)) {
    mi.rcMonitor = RECT{0, 0, GetSystemMetrics (SM_CXSCREEN),
                        GetSystemMetrics (SM_CYSCREEN)};
  }
  sync_dynamic_workspaces (mon, mi.rcMonitor);
  return G_SOURCE_REMOVE;
}

void
schedule_dynamic_workspaces_check (guint delay_ms = 250)
{
  if (g_window == nullptr)
    return;
  if (g_dyn_ws_startup_timer != 0)
    g_source_remove (g_dyn_ws_startup_timer);
  g_dyn_ws_startup_timer =
      g_timeout_add (delay_ms, dynamic_workspaces_startup_cb, nullptr);
}

// Relayout when the active desktop changed outside the overview (event-driven
// via centralized VD observer).
void
on_overview_desktop_changed (int count, int active_index, void *user_data)
{
  (void)user_data;
  if (g_window == nullptr || !gtk_widget_get_visible (g_window) ||
      !g_placed || g_target_state == ControlsState::HIDDEN ||
      !overview_active ()) {
    if (g_window != nullptr && !g_in_dynamic_ws_sync)
      schedule_dynamic_workspaces_check (250);
    return;
  }
  const bool count_changed = (count != (int)g_desktops.size ());
  const bool active_changed =
      (active_index >= 0 && active_index != g_desktop_active);
  if (!count_changed && !active_changed)
    return;

  // Ignore delayed Explorer EVENT_SYSTEM_DESKTOPSWITCH echoes from an earlier
  // step of an internal Overview workspace switch sequence (e.g. scrolling
  // 0 -> 1 -> 2 when Explorer's 0 -> 1 notification arrives after we already
  // switched to 2).
  if (!count_changed && active_changed &&
      (g_switch_source != 0 ||
       (g_get_monotonic_time () - g_last_internal_switch_us < 1000000))) {
    return;
  }

  if (g_anim.source != 0) {
    g_pending_desktop_sync = true;
    g_pending_desktop_target = active_changed ? active_index : -1;
    return;
  }

  if (active_changed) {
    g_print ("[ov] desktop changed %d -> %d (count=%d, external event)\n",
             g_desktop_active, active_index, count);
    switch_workspace_animated (active_index);
  } else {
    g_print ("[ov] desktop count changed %zu -> %d (external event)\n",
             g_desktops.size (), count);
    rebuild_now ();
  }
}

// Relayout without animation (window closed via the chrome button etc.).
void
rebuild_now (void)
{
  if (g_window == nullptr || !gtk_widget_get_visible (g_window))
    return;
  if (g_target_state == ControlsState::HIDDEN)
    return;
  place_overview_locked (false);
}

static void
apply_pending_desktop_sync_if_any (void)
{
  if (!g_pending_desktop_sync)
    return;
  int target_ws = g_pending_desktop_target;
  g_pending_desktop_sync = false;
  g_pending_desktop_target = -1;
  if (g_window == nullptr || !gtk_widget_get_visible (g_window) ||
      !g_placed || g_target_state == ControlsState::HIDDEN ||
      !overview_active ()) {
    return;
  }
  int cur_idx = vd::current_index ();
  int cur_count = vd::count ();
  int desired = (cur_idx >= 0) ? cur_idx : target_ws;
  if (desired >= 0 && desired != g_desktop_active)
    switch_workspace_animated (desired);
  else if (cur_count > 0 && cur_count != (int)g_desktops.size ())
    rebuild_now ();
}

void on_app_index_ready (void *user_data G_GNUC_UNUSED);

static gboolean
deferred_app_index_ready_cb (gpointer user_data G_GNUC_UNUSED)
{
  g_app_index_defer_timer = 0;
  on_app_index_ready (nullptr);
  return G_SOURCE_REMOVE;
}

void
on_app_index_ready (void *user_data G_GNUC_UNUSED)
{
  if (g_window == nullptr || g_app_grid == nullptr || g_panel_hwnd == nullptr)
    return;
  // Never run synchronous App Grid prewarm/page build while an Overview
  // animation is running — defer briefly until idle so startup stays snappy.
  if (g_anim.source != 0 || g_switch_source != 0 || g_search_fade_source != 0) {
    if (g_app_index_defer_timer == 0) {
      g_app_index_defer_timer = g_timeout_add_full (
          G_PRIORITY_LOW, 350, deferred_app_index_ready_cb, nullptr, nullptr);
    }
    return;
  }
  HMONITOR monitor = MonitorFromWindow (g_panel_hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{};
  mi.cbSize = sizeof (mi);
  if (!GetMonitorInfoW (monitor, &mi))
    return;
  int mw = mi.rcMonitor.right - mi.rcMonitor.left;
  int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
  int scale = std::max (gtk_widget_get_scale_factor (g_window), 1);

  if (g_wallpaper == nullptr)
    start_wallpaper_prewarm_async ();

  int est_w = std::max (std::min (mw / scale - 32, 1820), 400);
  int est_h = std::max ((int)(mh / scale * 0.58), 260);
  if (!overview_active () && !gtk_widget_get_visible (g_window)) {
    int panel_h = winome_shell_panel_height ();
    RECT pr{};
    if (IsWindowVisible (g_panel_hwnd) && GetWindowRect (g_panel_hwnd, &pr) &&
        pr.bottom > pr.top) {
      panel_h = pr.bottom - pr.top;
    }
    PlacedGeometry geo{};
    if (compute_geometry (monitor, mi.rcMonitor, panel_h, &geo)) {
      est_h = std::max (
          static_cast<int> (geo.app_grid_box.bottom - geo.app_grid_box.top) /
              scale,
          260);
    }
  }
  app_grid_clip_set_bounds (g_app_grid_clip, est_w, est_h);
  gtk_widget_set_size_request (g_app_grid, est_w, est_h);
  app_grid_prewarm (g_app_grid, est_w, est_h);
}

gboolean
show_idle (gpointer user_data)
{
  (void)user_data;
  if (overview_active ())
    overview_show ();
  return G_SOURCE_REMOVE;
}

gboolean
hide_idle (gpointer user_data)
{
  (void)user_data;
  if (!overview_active ())
    overview_hide ();
  return G_SOURCE_REMOVE;
}

gboolean
shift_app_grid_idle (gpointer user_data)
{
  (void)user_data;
  g_print ("[trig] double win-key -> app-grid\n");
  if (!overview_active () || g_window == nullptr) {
    g_pending_shift_app_grid.store (false, std::memory_order_release);
    return G_SOURCE_REMOVE;
  }
  if (!gtk_widget_get_visible (g_window) || !g_placed) {
    g_pending_shift_app_grid.store (true, std::memory_order_release);
    g_pending_show_state = ControlsState::APP_GRID;
    g_atomic_can_shift_app_grid.store (false, std::memory_order_release);
    return G_SOURCE_REMOVE;
  }
  g_pending_shift_app_grid.store (false, std::memory_order_release);
  if (g_target_state == ControlsState::HIDDEN)
    return G_SOURCE_REMOVE;
  if (g_target_state == ControlsState::WINDOW_PICKER) {
    start_state_transition (ControlsState::APP_GRID, kOpenMs);
  }
  restack_locked ();
  return G_SOURCE_REMOVE;
}

// --- Dedicated GNOME Search Results View (#searchResults) --------------------
// Backed by overview-search.cpp (ported from GNOME Shell search.js,
// searchController.js, appDisplay.js, misc/systemActions.js, misc/util.js)
// with smooth 200ms cross-fade (overviewControls.js:623-643 _onSearchChanged).

static void
on_search_view_close_overview (const char *reason, bool discard_focus,
                               void *user_data G_GNUC_UNUSED)
{
  if (discard_focus)
    overview_discard_restore_focus ();
  close_overview (reason != nullptr ? reason : "search-activate");
}

static void
on_search_view_activate_window (HWND hwnd, int desktop_index,
                                void *user_data G_GNUC_UNUSED)
{
  overview_discard_restore_focus ();
  int target_ws = desktop_index;
  if (target_ws < 0 && hwnd != nullptr) {
    for (const SlotEntry &s : g_slots) {
      if (s.target == hwnd) {
        target_ws = s.desktop_index;
        break;
      }
    }
  }
  if (target_ws >= 0 && target_ws != g_desktop_active)
    vd::switch_to (target_ws);
  if (hwnd != nullptr && IsWindow (hwnd)) {
    if (IsIconic (hwnd))
      ShowWindow (hwnd, SW_RESTORE);
    SetForegroundWindow (hwnd);
  }
  close_overview ("search-result-window");
}

static std::vector<SearchWindowCandidate>
on_search_view_get_open_windows (void *user_data G_GNUC_UNUSED)
{
  std::vector<SearchWindowCandidate> out;
  out.reserve (g_slots.size ());
  for (const SlotEntry &s : g_slots) {
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

static void
on_search_view_open_settings (const char *page, void *user_data G_GNUC_UNUSED)
{
  if (page != nullptr && page[0] != '\0')
    open_settings_window_page (page);
  else
    open_settings_window ();
}

static void
cancel_search_fade (void)
{
  if (g_search_fade_source != 0 && g_window != nullptr) {
    gtk_widget_remove_tick_callback (g_window, g_search_fade_source);
    g_search_fade_source = 0;
  }
}

static void
apply_search_fade_progress (double p)
{
  p = std::clamp (p, 0.0, 1.0);
  g_search_fade_cur = p;
  const double inv = 1.0 - p;

  if (g_search_view != nullptr) {
    gtk_widget_set_opacity (g_search_view, p);
    gtk_widget_set_visible (g_search_view, p > 0.005 ? TRUE : FALSE);
  }
  if (g_ws_bg != nullptr) {
    double bg_base = g_folder_dialog_open ? 0.30 : 1.0;
    gtk_widget_set_opacity (g_ws_bg, inv * bg_base);
    gtk_widget_set_visible (g_ws_bg, inv > 0.005 ? TRUE : FALSE);
  }
  const bool want_thumbs = !g_app_grid_mode && g_thumbs_has_pills;
  if (g_thumbs != nullptr) {
    gtk_widget_set_opacity (g_thumbs, want_thumbs ? inv : 0.0);
    gtk_widget_set_visible (g_thumbs,
                            (want_thumbs && inv > 0.005) ? TRUE : FALSE);
  }
  set_mini_opacity (
      want_thumbs ? static_cast<BYTE> (std::lround (inv * 255.0)) : 0);

  if (g_app_grid_clip != nullptr) {
    if (g_app_grid_mode) {
      gtk_widget_set_opacity (g_app_grid_clip, inv);
      sync_app_grid_visible (inv > 0.005);
    } else {
      gtk_widget_set_opacity (g_app_grid_clip, 1.0);
      sync_app_grid_visible (false);
    }
  }

  for (SlotEntry &s : g_slots) {
    s.rect = g_app_grid_mode ? s.grid_rect : s.picker_rect;
    int base_op = g_app_grid_mode ? static_cast<int> (s.grid_opacity) : 255;
    if (g_app_grid_mode && g_folder_dialog_open && base_op > 0)
      base_op = 76;
    BYTE op = static_cast<BYTE> (
        std::clamp<long> (std::lround (base_op * inv), 0, 255));
    apply_thumb_rect (s, s.rect, op, true);
  }
}

static gboolean
search_fade_tick (GtkWidget *widget G_GNUC_UNUSED,
                  GdkFrameClock *clock G_GNUC_UNUSED,
                  gpointer user_data G_GNUC_UNUSED)
{
  const gint64 now = g_get_monotonic_time ();
  const double elapsed_ms =
      static_cast<double> (now - g_search_fade_start_us) / 1000.0;
  const double raw_t =
      std::clamp (elapsed_ms / static_cast<double> (kSearchFadeMs), 0.0, 1.0);
  const double eased = ease_out_quad (raw_t);
  const double cur =
      g_search_fade_from + (g_search_fade_to - g_search_fade_from) * eased;
  apply_search_fade_progress (cur);

  if (raw_t >= 1.0) {
    g_search_fade_source = 0;
    if (g_search_fade_to <= 0.001) {
      if (g_search_view != nullptr)
        overview_search_view_reset (g_search_view);
      if (g_app_grid_clip != nullptr)
        gtk_widget_set_opacity (g_app_grid_clip, 1.0);
      if (!g_app_grid_mode && !g_search_mode)
        set_hover (slot_at_cursor ());
    }
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void
start_search_fade (bool to_search)
{
  const double target = to_search ? 1.0 : 0.0;
  if (g_window == nullptr) {
    apply_search_fade_progress (target);
    return;
  }
  // Never run search cross-fade concurrently with workspace switch or
  // open/app-grid transitions (both drive DWM thumbnail rects & opacities).
  if (g_switch_source != 0)
    switch_finish (false);
  if (g_anim.source != 0 && g_target_state != ControlsState::HIDDEN) {
    gtk_widget_remove_tick_callback (g_window, g_anim.source);
    g_anim.source = 0;
    g_anim.started = false;
    if (!g_ws_anim_to_boxes.empty ())
      apply_ws_boxes (g_ws_anim_to_boxes);
    if (g_target_state == ControlsState::APP_GRID && g_app_grid != nullptr) {
      sync_app_grid_visible (true);
      app_grid_clip_set_transition (g_app_grid_clip, false, 1.0, 0.0, true);
      app_grid_set_transition_opacity (g_app_grid, false, 1.0);
      g_grid_cur_opacity = 1.0;
      g_grid_cur_slide_y = 0.0;
    } else if (g_target_state == ControlsState::WINDOW_PICKER) {
      if (g_app_grid_clip != nullptr) {
        app_grid_clip_set_transition (g_app_grid_clip, false, 0.0,
                                      kGridSlideOffsetPx, true);
        sync_app_grid_visible (false);
      }
      g_grid_cur_opacity = 0.0;
      g_grid_cur_slide_y = kGridSlideOffsetPx;
    }
  }
  if (std::fabs (g_search_fade_cur - target) < 0.005 &&
      g_search_fade_source == 0) {
    apply_search_fade_progress (target);
    return;
  }
  g_search_fade_from = g_search_fade_cur;
  g_search_fade_to = target;
  g_search_fade_start_us = g_get_monotonic_time ();
  if (g_search_fade_source == 0) {
    g_search_fade_source =
        gtk_widget_add_tick_callback (g_window, search_fade_tick, nullptr,
                                      nullptr);
  }
}

static bool
is_search_entry_caret_at_end (void)
{
  if (g_search == nullptr)
    return true;
  const char *txt = gtk_editable_get_text (GTK_EDITABLE (g_search));
  if (txt == nullptr)
    return true;
  const int pos = gtk_editable_get_position (GTK_EDITABLE (g_search));
  const int len = static_cast<int> (g_utf8_strlen (txt, -1));
  return pos >= len;
}

static void
sync_search_mode_ui (void)
{
  const bool want_search = !g_search_query.empty ();
  const bool prev_mode = g_search_mode;
  g_search_mode = want_search;

  if (g_search != nullptr) {
    if (want_search)
      gtk_widget_add_css_class (g_search, "search-active");
    else
      gtk_widget_remove_css_class (g_search, "search-active");
  }

  if (g_window == nullptr || !gtk_widget_get_visible (g_window) || !g_placed ||
      g_target_state == ControlsState::HIDDEN) {
    cancel_search_fade ();
    g_search_fade_cur = 0.0;
    if (g_search_view != nullptr) {
      overview_search_view_reset (g_search_view);
      gtk_widget_set_visible (g_search_view, FALSE);
    }
    return;
  }

  if (want_search) {
    set_hover (-1, true);
    if (!prev_mode && g_app_grid != nullptr)
      app_grid_close_open_folder (g_app_grid);
    if (g_search_view != nullptr)
      overview_search_view_set_query (g_search_view, g_search_raw_text);
    start_search_fade (true);
  } else {
    start_search_fade (false);
  }
}

static void
on_app_grid_folder_toggled (bool open, void *user_data G_GNUC_UNUSED)
{
  g_folder_dialog_open = open;
  if (g_search != nullptr) {
    gtk_widget_set_opacity (g_search, open ? 0.30 : 1.0);
    gtk_widget_set_can_target (g_search, open ? FALSE : TRUE);
  }
  if (g_dash != nullptr) {
    gtk_widget_set_opacity (g_dash, open ? 0.30 : 1.0);
    gtk_widget_set_can_target (g_dash, open ? FALSE : TRUE);
  }
  if (g_ws_bg != nullptr && !g_search_mode)
    gtk_widget_set_opacity (g_ws_bg, open ? 0.30 : 1.0);
  if (g_app_grid_mode && !g_search_mode) {
    for (SlotEntry &s : g_slots) {
      BYTE op = (open && s.grid_opacity > 0) ? 76 : s.grid_opacity;
      apply_thumb_rect (s, s.grid_rect, op, true);
    }
  }
  restack_locked ();
}

void
on_search_changed (GtkEditable *editable, gpointer user_data G_GNUC_UNUSED)
{
  const char *text = gtk_editable_get_text (editable);
  std::string raw = text != nullptr ? text : "";
  std::vector<std::string> terms = split_search_terms (raw);
  std::string norm;
  for (size_t i = 0; i < terms.size (); ++i) {
    if (i > 0)
      norm.push_back (' ');
    norm += terms[i];
  }

  if (raw.empty () && g_search != nullptr) {
    gtk_entry_set_icon_from_icon_name (GTK_ENTRY (g_search),
                                       GTK_ENTRY_ICON_SECONDARY, nullptr);
  } else if (!raw.empty () && g_search != nullptr) {
    gtk_entry_set_icon_from_icon_name (GTK_ENTRY (g_search),
                                       GTK_ENTRY_ICON_SECONDARY,
                                       "edit-clear-symbolic");
  }

  if (raw == g_search_raw_text && norm == g_search_query)
    return;
  g_search_raw_text = std::move (raw);
  g_search_query = std::move (norm);
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
  if (g_search_mode) {
    if (g_search_view != nullptr)
      overview_search_view_activate_selected (g_search_view, false);
    return;
  }
  if (g_app_grid_mode && g_app_grid != nullptr) {
    AllowSetForegroundWindow (ASFW_ANY);
    if (app_grid_launch_first (g_app_grid)) {
      overview_discard_restore_focus ();
      close_overview ("app-grid-enter");
    }
    return;
  }
  if (!g_slots.empty ()) {
    overview_discard_restore_focus ();
    HWND target = g_slots[0].target;
    if (IsWindow (target)) {
      if (IsIconic (target))
        ShowWindow (target, SW_RESTORE);
      SetForegroundWindow (target);
    }
    close_overview ("search-enter-window");
    return;
  }
}

gboolean
on_overview_key_pressed (GtkEventControllerKey *controller,
                         guint keyval,
                         guint keycode G_GNUC_UNUSED,
                         GdkModifierType state,
                         gpointer user_data G_GNUC_UNUSED)
{
  if (keyval == GDK_KEY_Escape) {
    if (!g_search_raw_text.empty () || !g_search_query.empty ()) {
      gtk_editable_set_text (GTK_EDITABLE (g_search), "");
      focus_overview_window (false);
      return GDK_EVENT_STOP;
    }
    if (g_app_grid_mode) {
      if (g_app_grid != nullptr && app_grid_close_open_folder (g_app_grid))
        return GDK_EVENT_STOP;
      overview_toggle_app_grid ();
      return GDK_EVENT_STOP;
    }
    close_overview ("escape");
    return GDK_EVENT_STOP;
  }
  if (g_search_mode && g_search_view != nullptr) {
    if ((state & GDK_CONTROL_MASK) &&
        (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)) {
      if (overview_search_view_activate_selected (g_search_view, true))
        return GDK_EVENT_STOP;
    }
    if (overview_search_view_navigate (g_search_view, keyval,
                                       is_search_entry_caret_at_end ())) {
      return GDK_EVENT_STOP;
    }
  }
  if ((state & GDK_CONTROL_MASK) && (state & GDK_SHIFT_MASK) &&
      (keyval == GDK_KEY_n || keyval == GDK_KEY_N)) {
    on_thumb_add_desktop (nullptr);
    return GDK_EVENT_STOP;
  }
  if (g_app_grid_mode && g_app_grid != nullptr && g_search_query.empty () &&
      (keyval == GDK_KEY_Home || keyval == GDK_KEY_End)) {
    if (keyval == GDK_KEY_Home)
      app_grid_first_page (g_app_grid);
    else
      app_grid_last_page (g_app_grid);
    return GDK_EVENT_STOP;
  }
  if (keyval == GDK_KEY_Page_Up || keyval == GDK_KEY_Page_Down) {
    if (g_search_mode)
      return GDK_EVENT_PROPAGATE;
    if (g_app_grid_mode && g_app_grid != nullptr) {
      if (keyval == GDK_KEY_Page_Up)
        app_grid_prev_page (g_app_grid);
      else
        app_grid_next_page (g_app_grid);
      return GDK_EVENT_STOP;
    }
    if (g_desktops.size () > 1 && g_desktop_active >= 0) {
      int target = g_desktop_active + (keyval == GDK_KEY_Page_Up ? -1 : 1);
      if (target >= 0 && target < (int)g_desktops.size () &&
          vd::switch_to (target)) {
        switch_workspace_animated (target);
      }
      return GDK_EVENT_STOP;
    }
  }
  if ((state & GDK_CONTROL_MASK) &&
      (keyval == GDK_KEY_f || keyval == GDK_KEY_F)) {
    focus_overview_for_search ();
    return GDK_EVENT_STOP;
  }
  if (g_placed && g_target_state != ControlsState::HIDDEN &&
      g_search != nullptr &&
      (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) == 0) {
    GtkWidget *focus =
        g_window != nullptr ? gtk_window_get_focus (GTK_WINDOW (g_window))
                            : nullptr;
    if (focus == nullptr || !GTK_IS_EDITABLE (focus)) {
      const gunichar uc = gdk_keyval_to_unicode (keyval);
      if (uc >= 0x20 && uc != 0x7f &&
          !(uc == ' ' && g_search_query.empty ())) {
        focus_overview_for_search ();
        GtkWidget *search_focus = gtk_window_get_focus (GTK_WINDOW (g_window));
        if (controller != nullptr && search_focus != nullptr)
          gtk_event_controller_key_forward (controller, search_focus);
        return GDK_EVENT_STOP;
      }
    }
  }
  return GDK_EVENT_PROPAGATE;
}

gboolean
overview_prewarm_idle_cb (gpointer user_data)
{
  (void)user_data;
  g_prewarm_timer = 0;
  if (g_window == nullptr)
    return G_SOURCE_REMOVE;
  // Never steal main-thread frames while the user is opening or animating.
  if (g_anim.source != 0 || g_switch_source != 0 || g_search_fade_source != 0) {
    g_prewarm_timer = g_timeout_add_full (
        G_PRIORITY_LOW, 450, overview_prewarm_idle_cb, nullptr, nullptr);
    return G_SOURCE_REMOVE;
  }
  // Stagger startup prewarm across small low-priority slices instead of
  // blocking the main thread for 100-180ms right after launch:
  //   step 0 -> kick off off-thread wallpaper decode/scale
  //   step 1 -> realize g_window
  //   step 2..4 -> realize hover chrome windows one per slice
  if (g_prewarm_step == 0) {
    if (g_wallpaper == nullptr)
      start_wallpaper_prewarm_async ();
    g_prewarm_step = 1;
    g_prewarm_timer = g_timeout_add_full (
        G_PRIORITY_LOW, 320, overview_prewarm_idle_cb, nullptr, nullptr);
  } else if (g_prewarm_step == 1) {
    if (!gtk_widget_get_visible (g_window) &&
        !gtk_widget_get_realized (g_window)) {
      gtk_widget_realize (g_window);
    }
    g_prewarm_step = 2;
    g_prewarm_timer = g_timeout_add_full (
        G_PRIORITY_LOW, 350, overview_prewarm_idle_cb, nullptr, nullptr);
  } else if (g_prewarm_step >= 2 && g_prewarm_step <= 4) {
    GtkWidget *chrome_wins[] = {g_chrome_window, g_icon_window,
                                g_caption_window};
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

}  // namespace

void
overview_init (HWND panel_hwnd)
{
  g_panel_hwnd = panel_hwnd;
  g_overview_alive = std::make_shared<std::atomic<bool>> (true);
  g_prewarm_step = 0;

  g_window = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (g_window), "WINOME Overview");
  gtk_window_set_decorated (GTK_WINDOW (g_window), FALSE);
  gtk_window_set_hide_on_close (GTK_WINDOW (g_window), TRUE);
  // Fullscreen from surface creation: GDK sizes a fresh window against the
  // WORK AREA (monitor minus the reserved panel strip), and growing it to
  // the monitor size afterwards exposes a bottom strip of exactly panel
  // height before the first paint of the new size (the desktop wallpaper
  // flashing through). A fullscreen surface is created monitor-sized, so
  // the later SetWindowPos in place_overview_locked only fixes z-order.
  gtk_window_fullscreen (GTK_WINDOW (g_window));
  gtk_widget_add_css_class (g_window, "winome-overview");
  // GNOME's UI font (the theme's em values resolve against Sans 11).
  install_bundled_fonts (g_window);

  // #overviewGroup { background-color: #222226 } — GTK matches #id against
  // the widget name. The name is applied only after the first placement
  // succeeded (place_overview_locked): the window maps BEFORE any widget is
  // positioned, and a name set here would paint the full-screen dark
  // backdrop for the frames in between (the map-time flash).
  g_root = gtk_fixed_new ();
  gtk_widget_set_focusable (g_root, TRUE);
  gtk_widget_set_focus_on_click (g_root, FALSE);
  gtk_window_set_child (GTK_WINDOW (g_window), g_root);

  // 0x0 leaf focus sink so opening Overview / App Grid keeps keyboard input
  // active without auto-focusing .search-entry (prevents stray focus ring).
  g_focus_sink = gtk_drawing_area_new ();
  gtk_widget_set_size_request (g_focus_sink, 0, 0);
  gtk_widget_set_focusable (g_focus_sink, TRUE);
  gtk_widget_set_can_target (g_focus_sink, FALSE);
  gtk_fixed_put (GTK_FIXED (g_root), g_focus_sink, 0, 0);

  // .search-entry — GNOME's pill entry (StEntry + search-entry-icon),
  // realized as a GtkEntry with a primary magnifier icon; placeholder
  // localized.
  g_search = gtk_entry_new ();
  gtk_widget_add_css_class (g_search, "search-entry");
  gtk_entry_set_icon_from_icon_name (GTK_ENTRY (g_search),
                                     GTK_ENTRY_ICON_PRIMARY,
                                     "edit-find-symbolic");
  gtk_entry_set_placeholder_text (
      GTK_ENTRY (g_search),
      winome::tr ("Type to search", "输入以搜索"));
  GtkEventController *search_nav_keys = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (search_nav_keys,
                                              GTK_PHASE_CAPTURE);
  g_signal_connect (
      search_nav_keys, "key-pressed",
      G_CALLBACK (+[] (GtkEventControllerKey *c G_GNUC_UNUSED, guint keyval,
                       guint keycode G_GNUC_UNUSED,
                       GdkModifierType state,
                       gpointer ud G_GNUC_UNUSED) -> gboolean {
        if (g_search_mode && g_search_view != nullptr) {
          // Ctrl+Enter launches a new instance of the selected app
          // (upstream search.js SearchEntry activate-new-instance).
          if ((state & GDK_CONTROL_MASK) &&
              (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)) {
            if (overview_search_view_activate_selected (g_search_view, true))
              return GDK_EVENT_STOP;
          }
          if (overview_search_view_navigate (
                  g_search_view, keyval, is_search_entry_caret_at_end ())) {
            return GDK_EVENT_STOP;
          }
        }
        if (g_app_grid_mode && g_app_grid != nullptr &&
            g_search_query.empty () &&
            (keyval == GDK_KEY_Home || keyval == GDK_KEY_End)) {
          if (keyval == GDK_KEY_Home)
            app_grid_first_page (g_app_grid);
          else
            app_grid_last_page (g_app_grid);
          return GDK_EVENT_STOP;
        }
        return GDK_EVENT_PROPAGATE;
      }),
      nullptr);
  gtk_widget_add_controller (g_search, search_nav_keys);
  g_signal_connect (g_search, "changed", G_CALLBACK (on_search_changed),
                    nullptr);
  g_signal_connect (g_search, "icon-press", G_CALLBACK (on_search_icon_press),
                    nullptr);
  g_signal_connect (g_search, "activate", G_CALLBACK (on_search_activate),
                    nullptr);
  gtk_fixed_put (GTK_FIXED (g_root), g_search, 0, 0);

  // Dedicated GNOME Search Results View (#searchResults, overview-search.cpp).
  OverviewSearchCallbacks search_cbs{};
  search_cbs.on_close_overview = on_search_view_close_overview;
  search_cbs.on_activate_window = on_search_view_activate_window;
  search_cbs.get_open_windows = on_search_view_get_open_windows;
  search_cbs.on_open_settings = on_search_view_open_settings;
  g_search_view = overview_search_view_new (search_cbs);
  gtk_widget_set_opacity (g_search_view, 0.0);
  gtk_fixed_put (GTK_FIXED (g_root), g_search_view, 0, 0);
  gtk_widget_set_visible (g_search_view, FALSE);

  // Z-ORDER mirrors ControlsManager.add_child():
  // [searchEntryBin, appDisplay, dash, searchController, thumbnailsBox,
  //  workspacesDisplay] — the workspaces display (workspace background +
  // window clones) is the LAST child, i.e. it renders ON TOP of the dash
  // and the search entry. During the state transition the interpolating
  // wallpaper rect covers them and they emerge from behind its shrinking
  // edges — drawing the dash above the moving rect instead is exactly the
  // "fighting" look this avoids. (The window clones are DWM thumbnails,
  // composited above everything anyway, matching the topmost
  // workspacesDisplay.) The caption stays topmost, above the background.
  app_index_refresh_async (on_app_index_ready, nullptr);

  g_app_grid = app_grid_new (on_app_grid_launch_done,
                             on_app_grid_pins_changed, nullptr);
  app_grid_set_folder_toggled_cb (g_app_grid, on_app_grid_folder_toggled,
                                  nullptr);
  g_app_grid_clip = app_grid_clip_new (g_app_grid);
  gtk_fixed_put (GTK_FIXED (g_root), g_app_grid_clip, 0, 0);
  sync_app_grid_visible (false);

  g_dash = overview_dash_new (g_root);
  overview_dash_set_show_apps_cb (g_dash, on_dash_show_apps_clicked, nullptr);
  overview_dash_set_changed_cb (g_dash, on_dash_changed, nullptr);
  gtk_fixed_put (GTK_FIXED (g_root), g_dash, 0, 0);

  // .workspace-thumbnails — one wallpaper pill per virtual desktop, above
  // the dash and below the workspace background (the thumbnailsBox position
  // in ControlsManager.add_child). Invisible until repopulated with >1
  // desktop during placement.
  g_thumbs = overview_thumbs_new ();
  overview_thumbs_set_click_cb (g_thumbs, on_thumb_clicked, nullptr);
  overview_thumbs_set_manage_cbs (g_thumbs, on_thumb_add_desktop,
                                  on_thumb_remove_desktop, nullptr);
  gtk_fixed_put (GTK_FIXED (g_root), g_thumbs, 0, 0);
  gtk_widget_set_visible (g_thumbs, FALSE);

  // Wallpaper in an animated rounded clip (shadow painted directly by
  // winome_workspace_bg_snapshot when settled; avoid per-frame CSS blur FBO).
  g_ws_bg = (GtkWidget *)g_object_new (winome_workspace_bg_get_type (),
                                       nullptr);
  gtk_widget_set_can_target (g_ws_bg, FALSE);
  gtk_fixed_put (GTK_FIXED (g_root), g_ws_bg, 0, 0);

  // Chrome windows (above the DWM thumbnails).
  chrome_init ();
  icon_chrome_init ();
  caption_chrome_init ();

  g_signal_connect (g_window, "realize", G_CALLBACK (on_realize), nullptr);
  g_signal_connect (g_window, "map", G_CALLBACK (on_overview_map), nullptr);

  // Hover tracking for preview close buttons + captions.
  GtkEventController *motion = gtk_event_controller_motion_new ();
  g_signal_connect (motion, "enter", G_CALLBACK (on_root_motion), nullptr);
  g_signal_connect (motion, "motion", G_CALLBACK (on_root_motion), nullptr);
  g_signal_connect (motion, "leave", G_CALLBACK (on_root_motion_leave),
                    nullptr);
  gtk_widget_add_controller (g_root, GTK_EVENT_CONTROLLER (motion));

  g_signal_connect (
      g_window, "notify::is-active",
      G_CALLBACK (+[] (GtkWindow *win G_GNUC_UNUSED,
                       GParamSpec *pspec G_GNUC_UNUSED,
                       gpointer user_data G_GNUC_UNUSED) {
        restack_locked ();
      }),
      nullptr);

  // Window drag-to-desktop + slot hit-testing (left click activate / middle
  // click close / blank click dismiss).
  GtkGesture *drag = gtk_gesture_drag_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (drag),
                                 GDK_BUTTON_PRIMARY);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), nullptr);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), nullptr);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), nullptr);
  gtk_widget_add_controller (g_window, GTK_EVENT_CONTROLLER (drag));

  GtkGesture *click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);
  g_signal_connect (click, "released", G_CALLBACK (on_click_released),
                    nullptr);
  gtk_widget_add_controller (g_window, GTK_EVENT_CONTROLLER (click));

  GtkEventController *keys = gtk_event_controller_key_new ();
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_overview_key_pressed),
                    nullptr);
  gtk_widget_add_controller (g_window, keys);

  GtkEventController *ws_scroll = gtk_event_controller_scroll_new (
      GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  g_signal_connect (ws_scroll, "scroll", G_CALLBACK (on_overview_scroll),
                    nullptr);
  gtk_widget_add_controller (g_window, ws_scroll);

  vd::add_change_listener (on_overview_desktop_changed, nullptr);
  add_config_listener (on_overview_config_changed, nullptr);
  g_signal_connect (
      g_window, "close-request",
      G_CALLBACK (+[] (GtkWindow *win G_GNUC_UNUSED,
                       gpointer ud G_GNUC_UNUSED) -> gboolean {
        close_overview ("close-request");
        return TRUE;
      }),
      nullptr);
  g_signal_connect (
      g_window, "unrealize",
      G_CALLBACK (+[] (GtkWidget *w G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
        clear_pending_dwm_frame ();
        disconnect_after_paint_hook ();
        unsubclass_overview_hwnd ();
        g_overview_surface_hooked = false;
        chrome_detach_owners ();
      }),
      nullptr);
  g_signal_connect (
      g_window, "destroy",
      G_CALLBACK (+[] (GtkWidget *w, gpointer ud G_GNUC_UNUSED) {
        clear_pending_dwm_frame ();
        disconnect_after_paint_hook ();
        unsubclass_overview_hwnd ();
        g_overview_surface_hooked = false;
        cancel_pending_place_idles ();
        cancel_delayed_rebuild ();
        cancel_hover_idle ();
        cancel_settle_hover ();
        cancel_post_hide_workarea ();
        if (g_overview_alive)
          g_overview_alive->store (false, std::memory_order_release);
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
        if (g_drag_tick_source != 0) {
          gtk_widget_remove_tick_callback (w, g_drag_tick_source);
          g_drag_tick_source = 0;
        }
        if (g_drag_snapback_source != 0) {
          gtk_widget_remove_tick_callback (w, g_drag_snapback_source);
          g_drag_snapback_source = 0;
        }
        if (g_scale_anim_source != 0) {
          gtk_widget_remove_tick_callback (w, g_scale_anim_source);
          g_scale_anim_source = 0;
        }
        g_scale_anims.clear ();
        if (g_search_fade_source != 0) {
          gtk_widget_remove_tick_callback (w, g_search_fade_source);
          g_search_fade_source = 0;
        }
        if (g_anim.source != 0) {
          gtk_widget_remove_tick_callback (w, g_anim.source);
          g_anim.source = 0;
        }
        switch_cancel ();
        destroy_previews ();
        destroy_mini_previews ();
        vd::remove_change_listener (on_overview_desktop_changed, nullptr);
        remove_config_listener (on_overview_config_changed, nullptr);
        g_panel_cur_progress = 0.0;
        winome_shell_panel_set_overview_progress (0.0);
        winome_shell_panel_set_workspace_scroll (-1, -1.0);
        live_wallpaper_stop ();
        g_using_live_wallpaper = false;
        g_wallpaper_cache = WallpaperCacheKey{};
        g_clear_object (&g_wallpaper);
        g_clear_object (&g_wallpaper_thumb);
        g_clear_object (&g_live_wallpaper);
        g_clear_object (&g_live_wallpaper_thumb);

        chrome_cancel_apply (&g_chrome_hooks);
        chrome_cancel_apply (&g_icon_hooks);
        chrome_cancel_apply (&g_caption_hooks);
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
        if (chrome_win != nullptr)
          gtk_window_destroy (GTK_WINDOW (chrome_win));
        if (icon_win != nullptr)
          gtk_window_destroy (GTK_WINDOW (icon_win));
        if (caption_win != nullptr)
          gtk_window_destroy (GTK_WINDOW (caption_win));

        g_window = nullptr;
        g_overview_hwnd = nullptr;
        g_prev_foreground_hwnd = nullptr;
        g_restore_focus_discarded.store (false, std::memory_order_release);
        g_root = nullptr;
        g_focus_sink = nullptr;
        g_search = nullptr;
        g_search_view = nullptr;
        g_search_query.clear ();
        g_search_raw_text.clear ();
        g_search_fade_cur = 0.0;
        g_search_fade_from = 0.0;
        g_search_fade_to = 0.0;
        g_search_mode = false;
        g_folder_dialog_open = false;
        g_ws_bg = nullptr;
        g_app_grid_clip = nullptr;
        g_app_grid = nullptr;
        g_dash = nullptr;
        g_thumbs = nullptr;
        g_placed = false;
        g_target_state = ControlsState::HIDDEN;
        g_pending_show_state = ControlsState::WINDOW_PICKER;
        g_app_grid_mode = false;
        g_thumbs_has_pills = false;
        g_pending_desktop_sync = false;
        g_pending_desktop_target = -1;
        g_atomic_can_shift_app_grid.store (false, std::memory_order_release);
        g_pending_shift_app_grid.store (false, std::memory_order_release);
        g_drag_active = false;
        g_drag_slot = -1;
        g_drag_hwnd = nullptr;
        g_drag_just_ended = false;
        g_drag_thumbs_revealed = false;
        g_drag_drop_ws_peek = -1;
        g_ws_full_boxes.clear ();
        g_ws_picker_boxes.clear ();
        g_ws_grid_boxes.clear ();
        g_ws_cur_boxes.clear ();
        g_ws_anim_from_boxes.clear ();
        g_ws_anim_to_boxes.clear ();
        g_hover_index = -1;
        g_last_hovered_slot = -1;
        g_overlay_opacity = 0.0;
        g_overlay_from = 0.0;
        g_overlay_to = 0.0;

        if (overview_active ())
          close_overview ("destroy");
      }),
      nullptr);

  if (g_prewarm_timer != 0)
    g_source_remove (g_prewarm_timer);
  g_prewarm_step = 0;
  g_prewarm_timer = g_timeout_add_full (
      G_PRIORITY_LOW, 120, overview_prewarm_idle_cb, nullptr, nullptr);
  schedule_dynamic_workspaces_check (1600);
}

void
overview_show (void)
{
  if (g_window == nullptr || g_panel_hwnd == nullptr)
    return;

  if (g_prewarm_timer != 0) {
    g_source_remove (g_prewarm_timer);
    g_prewarm_timer = 0;
  }
  cancel_settle_hover ();
  cancel_post_hide_workarea ();
  cancel_delayed_rebuild ();

  cancel_search_fade ();
  g_search_fade_cur = 0.0;
  g_search_fade_from = 0.0;
  g_search_fade_to = 0.0;
  if (g_search != nullptr) {
    const char *cur_text = gtk_editable_get_text (GTK_EDITABLE (g_search));
    if (!g_search_query.empty () || !g_search_raw_text.empty () ||
        (cur_text != nullptr && cur_text[0] != '\0')) {
      gtk_editable_set_text (GTK_EDITABLE (g_search), "");
      g_search_query.clear ();
      g_search_raw_text.clear ();
      gtk_entry_set_icon_from_icon_name (GTK_ENTRY (g_search),
                                         GTK_ENTRY_ICON_SECONDARY, nullptr);
    }
    gtk_widget_remove_css_class (g_search, "search-active");
    gtk_widget_set_opacity (g_search, 1.0);
    gtk_widget_set_can_target (g_search, TRUE);
  }
  if (g_dash != nullptr) {
    gtk_widget_set_opacity (g_dash, 1.0);
    gtk_widget_set_can_target (g_dash, TRUE);
  }
  if (g_app_grid != nullptr)
    app_grid_close_open_folder (g_app_grid);
  g_search_mode = false;
  g_folder_dialog_open = false;
  if (g_search_view != nullptr) {
    overview_search_view_reset (g_search_view);
    gtk_widget_set_opacity (g_search_view, 0.0);
    gtk_widget_set_visible (g_search_view, FALSE);
  }
  if (g_ws_bg != nullptr)
    gtk_widget_set_opacity (g_ws_bg, 1.0);
  if (g_app_grid_clip != nullptr)
    gtk_widget_set_opacity (g_app_grid_clip, 1.0);

  const bool shift_requested =
      g_pending_shift_app_grid.exchange (false, std::memory_order_acq_rel);
  g_pending_show_state = shift_requested ? ControlsState::APP_GRID
                                         : ControlsState::WINDOW_PICKER;
  g_atomic_can_shift_app_grid.store (!shift_requested,
                                     std::memory_order_release);

  if (!gtk_widget_get_visible (g_window)) {
    HWND fg = GetForegroundWindow ();
    g_restore_focus_discarded.store (false, std::memory_order_release);
    g_prev_foreground_hwnd =
        is_external_foreground_target (fg) ? fg : nullptr;
    cancel_pending_place_idles ();
    g_placed = false;
    g_target_state = ControlsState::HIDDEN;
    g_app_grid_mode = false;
    g_print ("[ov] show (requesting map)\n");
    gtk_widget_set_visible (g_window, TRUE);
    if (!g_placed) {
      place_overview_locked (true);
    }
    if (g_placed) {
      cancel_pending_place_idles ();
    } else if (g_place_opening_source == 0) {
      g_place_opening_source = g_idle_add_full (
          G_PRIORITY_HIGH_IDLE, overview_place_opening_idle, nullptr, nullptr);
    }
  } else if (g_placed && g_target_state == ControlsState::HIDDEN) {
    // Closing animation was mid-flight; reverse seamlessly back to picker or grid.
    start_state_transition (g_pending_show_state, kOpenMs);
    if (winome_shell_panel_top_popover_hwnd () == nullptr)
      focus_overview_window (false);
    restack_locked ();
  } else if (g_placed) {
    if (g_place_idle_source == 0) {
      g_place_idle_source = g_idle_add_full (
          G_PRIORITY_HIGH_IDLE, overview_place_idle, nullptr, nullptr);
    }
  }
}

void
overview_hide (void)
{
  g_print ("[ov] hide\n");
  g_atomic_can_shift_app_grid.store (false, std::memory_order_release);
  g_pending_shift_app_grid.store (false, std::memory_order_release);
  set_hover (-1, true);
  if (g_app_grid != nullptr)
    app_grid_close_open_folder (g_app_grid);
  if (g_search_mode || g_search_fade_source != 0) {
    cancel_search_fade ();
    g_search_fade_cur = 0.0;
    g_search_mode = false;
    g_thumbs_cur_opacity = 0.0;
    if (g_thumbs != nullptr) {
      gtk_widget_set_opacity (g_thumbs, 0.0);
      gtk_widget_set_visible (g_thumbs, FALSE);
    }
    set_mini_opacity (0);
    if (g_search_view != nullptr) {
      overview_search_view_reset (g_search_view);
      gtk_widget_set_opacity (g_search_view, 0.0);
      gtk_widget_set_visible (g_search_view, FALSE);
    }
    if (g_ws_bg != nullptr) {
      gtk_widget_set_opacity (g_ws_bg, 1.0);
      gtk_widget_set_visible (g_ws_bg, TRUE);
    }
    if (g_app_grid_clip != nullptr)
      gtk_widget_set_opacity (g_app_grid_clip, 1.0);
  }
  if (g_window == nullptr || !gtk_widget_get_visible (g_window) || !g_placed) {
    finish_hide ();
    return;
  }
  // Animate out smoothly from either WINDOW_PICKER or APP_GRID, then unmap
  // from finish_hide().
  start_state_transition (ControlsState::HIDDEN, kCloseMs);
}

void
overview_discard_restore_focus (void)
{
  g_restore_focus_discarded.store (true, std::memory_order_release);
  g_prev_foreground_hwnd = nullptr;
  AllowSetForegroundWindow (ASFW_ANY);
}

void
overview_restack (void)
{
  restack_locked ();
}

int
overview_is_open (void)
{
  return g_window != nullptr && gtk_widget_get_visible (g_window) ? 1 : 0;
}

int
overview_workspace_count (void)
{
  return (int)g_desktops.size ();
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
  if (g_window != nullptr && gtk_widget_get_visible (g_window))
    gtk_widget_queue_draw (g_window);
}

void
overview_reposition (void)
{
  if (g_window == nullptr || !gtk_widget_get_visible (g_window))
    return;
  if (!g_placed) {
    if (!overview_active ())
      finish_hide ();
    return;
  }
  if (g_target_state == ControlsState::HIDDEN) {
    // A display/DPI change during close animation must finish hiding cleanly
    // instead of cancelling g_anim and leaving the fullscreen window trapped.
    finish_hide ();
    return;
  }
  place_overview_locked (false);
}

void
overview_toggle_app_grid (void)
{
  if (g_window == nullptr || !gtk_widget_get_visible (g_window) ||
      !overview_active ()) {
    return;
  }
  if (!g_placed) {
    g_pending_show_state =
        (g_pending_show_state == ControlsState::APP_GRID)
            ? ControlsState::WINDOW_PICKER
            : ControlsState::APP_GRID;
    g_atomic_can_shift_app_grid.store (
        g_pending_show_state == ControlsState::WINDOW_PICKER,
        std::memory_order_release);
    return;
  }
  if (g_target_state == ControlsState::HIDDEN)
    return;
  if (g_search_mode && g_search != nullptr) {
    gtk_editable_set_text (GTK_EDITABLE (g_search), "");
    focus_overview_window (false);
    if (g_target_state == ControlsState::APP_GRID) {
      restack_locked ();
      return;
    }
    cancel_search_fade ();
    apply_search_fade_progress (0.0);
    if (g_search_view != nullptr)
      overview_search_view_reset (g_search_view);
  }
  if (g_app_grid != nullptr)
    app_grid_close_open_folder (g_app_grid);
  ControlsState next = (g_target_state == ControlsState::APP_GRID)
                           ? ControlsState::WINDOW_PICKER
                           : ControlsState::APP_GRID;
  start_state_transition (next, kOpenMs);
  if (winome_shell_panel_top_popover_hwnd () == nullptr)
    focus_overview_window (false);
  restack_locked ();
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
