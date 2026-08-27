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
// interactive chrome window and the app icon in a second one (clicking the
// icon activates the window, like event propagation to the preview actor);
// both sit above the overview, below the panel. The overlay hides 750ms
// after the pointer leaves (WINDOW_OVERLAY_IDLE_HIDE_TIMEOUT), fading out.
//
// Interactions: click a preview (or its icon/title) restores/focuses that
// window and closes the overview; the close button sends WM_CLOSE and
// relayouts; a click elsewhere closes. Esc / a second Win press close it
// through overview-trigger.cpp. A click that dismissed a panel popover
// opened over the overview is swallowed (GNOME's menu grab consumes it).

#include "overview.h"
#include "overview-layout.h"
#include "overview-dash.h"
#include "overview-thumbs.h"
#include "overview-trigger.h"
#include "shell-panel.h"
#include "st-engine.h"
#include "system-status.h"
#include "fonts.h"
#include "virtual-desktop.h"

#include <gtk/gtk.h>
#include <gdk/win32/gdkwin32.h>
#include <dwmapi.h>

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

namespace winome {

namespace {

// --- theme constants (compiled gnome-shell-dark.css; StEngine-resolved) ----

constexpr int kSearchMarginTop = 12;     // .search-entry margin-top
constexpr int kSearchMarginBottom = 6;   // .search-entry margin-bottom
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

// overviewControls.js ratios.
constexpr double kVerticalSpacingRatio = 0.02;
constexpr double kThumbnailsAdjustTop = 0.6;
constexpr double kThumbnailsAdjustBottom = 0.4;  // THUMBNAILS_SPACING_ADJ_
                                                  // USTMENT_BOTTOM
constexpr double kMaxThumbnailScale = 0.05;      // MAX_THUMBNAIL_SCALE
constexpr double kDashMaxHeightRatio = 0.16;

// Overview.ANIMATION_TIME: opening EASE_OUT_SINE, closing EASE_OUT_QUAD.
constexpr guint kOpenMs = 250;
constexpr guint kCloseMs = 250;

HWND g_panel_hwnd = nullptr;
GtkWidget *g_window = nullptr;
HWND g_overview_hwnd = nullptr;
int g_panel_height = 0;   // physical px; clicks above it are panel's

// UI pieces.
GtkWidget *g_root = nullptr;      // GtkFixed named "overviewGroup"
GtkWidget *g_search = nullptr;    // GtkEntry.search-entry
GtkWidget *g_ws_bg = nullptr;     // workspace-background widget
GtkWidget *g_dash = nullptr;      // overview dash
GtkWidget *g_thumbs = nullptr;    // .workspace-thumbnails row
GtkWidget *g_caption = nullptr;   // .window-caption hover pill
GdkTexture *g_wallpaper = nullptr;

// Virtual desktops (virtual-desktop.cpp), refreshed on every placement.
std::vector<vd::DesktopInfo> g_desktops;
int g_desktop_active = -1;   // index of the desktop the layout was built for
guint g_desktop_poll = 0;    // external-switch watch while visible

// Workspace-background geometry (physical px, overview client space) and the
// animated corner radius (logical px).
RECT g_bg_full = {0, 0, 0, 0};    // HIDDEN state: the whole work area
RECT g_bg_final = {0, 0, 0, 0};   // WINDOW_PICKER: the aspect workspace box
double g_bg_radius_target = kWorkspaceRadiusFallback;
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

// One preview slot.
struct SlotEntry {
  RECT start;      // real window rect (client space) for the fly animation;
                   // a 1x1 point at the workspace corner for minimized
  RECT rect;       // slot rect (client space)
  RECT current;    // animated rect currently set on the thumbnail
  HWND target;
  HTHUMBNAIL thumb;
  bool minimized = false;
};
std::vector<SlotEntry> g_slots;

int g_hover_index = -1;

// Mini window previews inside the workspace thumbnails (WorkspaceThumbnail
// clones): one DWM thumbnail per (desktop, window) pair, laid out at the
// window's porthole position scaled into the pill.
struct MiniSlot {
  HTHUMBNAIL thumb;
  RECT rect;
};
std::vector<MiniSlot> g_mini_slots;

// Layout (physical px, overview client space).
int g_monitor_w = 0, g_monitor_h = 0;
int g_dash_top = 0;             // top of the dash band (click exclusion)
RECT g_search_rect = {0, 0, 0, 0}; // physical; clicks here belong to the entry
RECT g_thumbs_rect = {0, 0, 0, 0}; // physical; clicks here belong to the thumbs row
RECT g_ws_box = {0, 0, 0, 0};   // physical workspace box (slot layout area)

struct Animation {
  guint source = 0;
  bool closing = false;
  gint64 start_us = 0;
  guint duration_ms = 0;
};
Animation g_anim;

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
int g_scaled_slot = -1;         // slot whose scale != 1 (display chrome on it)
double g_scaled_value = 1.0;

// Caption fade.
double g_caption_opacity = 0.0;
double g_caption_from = 0.0, g_caption_to = 0.0;
gint64 g_caption_anim_start = 0;
guint g_caption_anim_ms = 0;

guint g_hover_idle_source = 0;  // WINDOW_OVERLAY_IDLE_HIDE_TIMEOUT

// Forward declarations (defined below in dependency order).
static void place_overview_locked (bool opening);
static gboolean overview_place_idle (gpointer user_data);
static gboolean overview_place_opening_idle (gpointer user_data);
static void set_hover (int index, bool immediate = false);
static void hover_layout_chrome (void);
static void schedule_hover_idle_hide (void);
static void apply_thumb_rect (SlotEntry &e, const RECT &rect, BYTE opacity);
static void destroy_mini_previews (void);
static void set_mini_opacity (BYTE opacity);
static void rebuild_now (void);
static void start_open_animation (void);
static void finish_hide (void);
static gboolean desktop_poll_cb (gpointer user_data);
static void chrome_hide (void);
static void icon_hide (void);

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
hover_display_rect (void)
{
  int index = g_hover_index >= 0 ? g_hover_index : g_scaled_slot;
  if (index < 0 || index >= (int)g_slots.size ())
    return RECT{0, 0, 0, 0};
  const RECT &base = g_slots[index].rect;
  if (index == g_scaled_slot && g_scaled_value != 1.0)
    return scale_rect_around_center (base, g_scaled_value);
  return base;
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

bool
is_cloaked (HWND hwnd)
{
  BOOL cloaked = FALSE;
  DwmGetWindowAttribute (hwnd, DWMWA_CLOAKED, &cloaked, sizeof (cloaked));
  return cloaked != FALSE;
}

// The visible frame bounds (no invisible DWM resize borders); the restored
// placement for minimized windows. Matches MetaWindow's frame rect usage in
// workspace.js bounding boxes.
RECT
window_rect (HWND hwnd)
{
  RECT r = {0, 0, 0, 0};
  if (IsIconic (hwnd)) {
    WINDOWPLACEMENT wp = {sizeof (wp)};
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
};

BOOL CALLBACK
collect_windows (HWND hwnd, LPARAM lparam)
{
  EnumContext *ctx = reinterpret_cast<EnumContext *> (lparam);

  if (is_our_process (hwnd) || !is_alt_tab_window (hwnd))
    return TRUE;

  if (ctx->vd_filter) {
    if (!vd::window_on_current (hwnd))
      return TRUE;
  } else if (!ctx->include_cloaked) {
    if (is_cloaked (hwnd))
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
  // Chain up first so the CSS layer paints .workspace-background's own
  // background and the 0 4px 16px 4px box-shadow; then draw the wallpaper
  // clipped to the animated rounded rect on top.
  GTK_WIDGET_CLASS (winome_workspace_bg_parent_class)
      ->snapshot (widget, snapshot);

  int w = gtk_widget_get_width (widget);
  int h = gtk_widget_get_height (widget);
  if (w <= 0 || h <= 0)
    return;

  graphene_rect_t rect;
  graphene_rect_init (&rect, 0, 0, w, h);
  GskRoundedRect clip;
  gsk_rounded_rect_init_from_rect (&clip, &rect, (float)g_bg_radius);
  gtk_snapshot_push_rounded_clip (snapshot, &clip);

  if (g_wallpaper != nullptr) {
    // MetaBackgroundContent draws the wallpaper cover-style (aspect
    // preserving, overflow cropped), like the desktop itself. The workspace
    // box keeps the workarea aspect, so this is a uniform scale of the
    // covered monitor image.
    double tw = gdk_texture_get_width (g_wallpaper);
    double th = gdk_texture_get_height (g_wallpaper);
    if (tw > 0 && th > 0) {
      double s = std::max (w / tw, h / th);
      double dw = tw * s, dh = th * s;
      graphene_rect_t tex_rect;
      graphene_rect_init (&tex_rect, (w - dw) / 2.0, (h - dh) / 2.0, dw, dh);
      gtk_snapshot_append_texture (snapshot, g_wallpaper, &tex_rect);
    }
  } else {
    // No wallpaper file: paint the desktop solid color.
    DWORD c = GetSysColor (COLOR_DESKTOP);
    GdkRGBA color = {((c >> 0) & 0xff) / 255.0f, ((c >> 8) & 0xff) / 255.0f,
                     ((c >> 16) & 0xff) / 255.0f, 1.0f};
    gtk_snapshot_append_color (snapshot, &color, &rect);
  }
  gtk_snapshot_pop (snapshot);
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

// --- wallpaper ----------------------------------------------------------------

void
load_wallpaper (void)
{
  wchar_t path[MAX_PATH] = L"";
  SystemParametersInfoW (SPI_GETDESKWALLPAPER, MAX_PATH, path, 0);

  if (path[0] != L'\0') {
    char utf8[MAX_PATH * 3];
    int n = WideCharToMultiByte (CP_UTF8, 0, path, -1, utf8, sizeof (utf8),
                                 nullptr, nullptr);
    if (n > 0) {
      GFile *file = g_file_new_for_path (utf8);
      GdkTexture *texture = gdk_texture_new_from_file (file, nullptr);
      g_object_unref (file);
      if (texture != nullptr) {
        g_clear_object (&g_wallpaper);
        g_wallpaper = texture;
        return;
      }
    }
  }
  g_clear_object (&g_wallpaper);
}

// --- close-button chrome window ------------------------------------------------

void
chrome_apply_ex_styles (HWND hwnd)
{
  LONG_PTR ex = GetWindowLongPtrW (hwnd, GWL_EXSTYLE);
  LONG_PTR want = ex | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
  want &= ~WS_EX_APPWINDOW;
  if (want != ex)
    SetWindowLongPtrW (hwnd, GWL_EXSTYLE, want);
}

void
chrome_apply_want (void)
{
  if (g_chrome_hwnd == nullptr || !IsWindow (g_chrome_hwnd))
    return;
  chrome_apply_ex_styles (g_chrome_hwnd);
  SetWindowPos (g_chrome_hwnd, g_panel_hwnd, g_chrome_want.left,
                g_chrome_want.top,
                g_chrome_want.right - g_chrome_want.left,
                g_chrome_want.bottom - g_chrome_want.top,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void
on_chrome_map (GtkWidget *widget, gpointer user_data)
{
  (void)widget;
  (void)user_data;
  // GDK's first-map sequence positions the window itself; re-apply ours
  // right after the map (same pattern as the overview window).
  g_idle_add (+[] (gpointer) -> gboolean {
    chrome_apply_want ();
    return G_SOURCE_REMOVE;
  }, nullptr);
}

void
on_chrome_realize (GtkWidget *widget, gpointer user_data)
{
  (void)user_data;
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (widget));
  if (surface == nullptr)
    return;
  HWND hwnd = static_cast<HWND> (gdk_win32_surface_get_handle (surface));
  g_chrome_hwnd = hwnd;
  chrome_apply_ex_styles (hwnd);
}

void
chrome_hide (void)
{
  g_chrome_target = nullptr;
  if (g_chrome_window != nullptr && gtk_widget_get_visible (g_chrome_window))
    gtk_widget_set_visible (g_chrome_window, FALSE);
}

void
on_chrome_click (GtkGestureClick *gesture, int n_press, double x, double y,
                 gpointer user_data)
{
  (void)gesture;
  (void)n_press;
  (void)x;
  (void)y;
  (void)user_data;
  if (g_chrome_target != nullptr && IsWindow (g_chrome_target)) {
    PostMessageW (g_chrome_target, WM_CLOSE, 0, 0);
    g_print ("[ov] close requested for slot window\n");
  }
  chrome_hide ();
  icon_hide ();
  // Relayout after the window has (probably) closed.
  g_timeout_add (250, +[] (gpointer) -> gboolean {
    if (g_window != nullptr && gtk_widget_get_visible (g_window))
      g_idle_add (+[] (gpointer) -> gboolean {
        rebuild_now ();
        return G_SOURCE_REMOVE;
      }, nullptr);
    return G_SOURCE_REMOVE;
  }, nullptr);
}

void
on_chrome_leave (GtkEventControllerMotion *motion, gpointer user_data)
{
  (void)motion;
  (void)user_data;
  // GNOME hides the overlay 750ms after the pointer left the preview; the
  // close button keeps the overlay alive while hovered (the idle check in
  // schedule_hover_idle_hide looks at the pointer position).
  schedule_hover_idle_hide ();
}

void
chrome_init (void)
{
  g_chrome_window = gtk_window_new ();
  gtk_window_set_decorated (GTK_WINDOW (g_chrome_window), FALSE);
  gtk_window_set_resizable (GTK_WINDOW (g_chrome_window), FALSE);
  gtk_widget_add_css_class (g_chrome_window, "winome-overview-chrome");

  g_signal_connect (g_chrome_window, "realize",
                    G_CALLBACK (on_chrome_realize), nullptr);
  g_signal_connect (g_chrome_window, "map",
                    G_CALLBACK (on_chrome_map), nullptr);

  // .window-close: 32x32 circle, icon at the medium 16px size.
  g_chrome_button = gtk_fixed_new ();
  gtk_widget_add_css_class (g_chrome_button, "window-close");
  gtk_widget_set_size_request (g_chrome_button, kCloseButtonSize,
                               kCloseButtonSize);
  GtkWidget *icon = gtk_image_new_from_icon_name ("window-close-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (icon), kCloseIconSize);
  gtk_fixed_put (GTK_FIXED (g_chrome_button), icon,
                 (kCloseButtonSize - kCloseIconSize) / 2,
                 (kCloseButtonSize - kCloseIconSize) / 2);
  gtk_window_set_child (GTK_WINDOW (g_chrome_window), g_chrome_button);

  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "released", G_CALLBACK (on_chrome_click), nullptr);
  gtk_widget_add_controller (g_chrome_button, GTK_EVENT_CONTROLLER (click));

  GtkEventController *motion = gtk_event_controller_motion_new ();
  g_signal_connect (motion, "leave", G_CALLBACK (on_chrome_leave), nullptr);
  gtk_widget_add_controller (g_chrome_button, motion);
}

// Show the close button centered on the preview's top-right corner (of the
// scaled rect; windowPreview.js centers the button on both edges).
void
chrome_show_at (const RECT &preview_rect)
{
  if (g_chrome_window == nullptr)
    return;

  int scale = gtk_widget_get_scale_factor (g_window);
  HMONITOR monitor = MonitorFromWindow (g_panel_hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {sizeof (mi)};
  GetMonitorInfoW (monitor, &mi);

  int size = kCloseButtonSize * scale;
  g_chrome_want.left = mi.rcMonitor.left + preview_rect.right - size / 2;
  g_chrome_want.top = mi.rcMonitor.top + preview_rect.top - size / 2;
  g_chrome_want.right = g_chrome_want.left + size;
  g_chrome_want.bottom = g_chrome_want.top + size;

  if (gtk_widget_get_visible (g_chrome_window)) {
    chrome_apply_want ();
  } else {
    // First show: on_chrome_map applies the stored rect right after GDK's
    // own map sequence.
    gtk_widget_set_visible (g_chrome_window, TRUE);
  }
}

// --- app-icon chrome window -----------------------------------------------------

void
icon_chrome_apply_ex_styles (HWND hwnd)
{
  LONG_PTR ex = GetWindowLongPtrW (hwnd, GWL_EXSTYLE);
  LONG_PTR want = ex | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
  want &= ~WS_EX_APPWINDOW;
  if (want != ex)
    SetWindowLongPtrW (hwnd, GWL_EXSTYLE, want);
}

void
icon_chrome_apply_want (void)
{
  if (g_icon_hwnd == nullptr || !IsWindow (g_icon_hwnd))
    return;
  icon_chrome_apply_ex_styles (g_icon_hwnd);
  SetWindowPos (g_icon_hwnd, g_chrome_hwnd != nullptr ? g_chrome_hwnd
                                                      : g_panel_hwnd,
                g_icon_want.left, g_icon_want.top,
                g_icon_want.right - g_icon_want.left,
                g_icon_want.bottom - g_icon_want.top,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void
on_icon_map (GtkWidget *widget, gpointer user_data)
{
  (void)widget;
  (void)user_data;
  g_idle_add (+[] (gpointer) -> gboolean {
    icon_chrome_apply_want ();
    return G_SOURCE_REMOVE;
  }, nullptr);
}

void
on_icon_realize (GtkWidget *widget, gpointer user_data)
{
  (void)user_data;
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (widget));
  if (surface == nullptr)
    return;
  g_icon_hwnd = static_cast<HWND> (gdk_win32_surface_get_handle (surface));
  icon_chrome_apply_ex_styles (g_icon_hwnd);
}

void
icon_hide (void)
{
  g_icon_target = nullptr;
  if (g_icon_window != nullptr && gtk_widget_get_visible (g_icon_window))
    gtk_widget_set_visible (g_icon_window, FALSE);
}

// Clicking the icon propagates to the preview upstream (the icon actor has
// no click action of its own): activate the window and leave the overview.
void
on_icon_click (GtkGestureClick *gesture, int n_press, double x, double y,
               gpointer user_data)
{
  (void)gesture;
  (void)n_press;
  (void)x;
  (void)y;
  (void)user_data;
  if (g_hover_index >= 0 && g_hover_index < (int)g_slots.size () &&
      IsWindow (g_slots[g_hover_index].target)) {
    HWND target = g_slots[g_hover_index].target;
    if (IsIconic (target))
      ShowWindow (target, SW_RESTORE);
    SetForegroundWindow (target);
  }
  close_overview ("ov-click-icon");
}

void
icon_chrome_init (void)
{
  g_icon_window = gtk_window_new ();
  gtk_window_set_decorated (GTK_WINDOW (g_icon_window), FALSE);
  gtk_window_set_resizable (GTK_WINDOW (g_icon_window), FALSE);
  gtk_widget_add_css_class (g_icon_window, "winome-overview-chrome");

  g_signal_connect (g_icon_window, "realize",
                    G_CALLBACK (on_icon_realize), nullptr);
  g_signal_connect (g_icon_window, "map",
                    G_CALLBACK (on_icon_map), nullptr);

  // The 64px application icon straddling the preview's bottom edge
  // (ICON_SIZE / ICON_OVERLAP), letterboxed inside its square.
  g_icon_picture = gtk_picture_new ();
  gtk_picture_set_content_fit (GTK_PICTURE (g_icon_picture),
                               GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_size_request (g_icon_picture, kWindowIconSize,
                               kWindowIconSize);
  gtk_window_set_child (GTK_WINDOW (g_icon_window), g_icon_picture);
  gtk_window_set_default_size (GTK_WINDOW (g_icon_window), kWindowIconSize,
                               kWindowIconSize);

  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "released", G_CALLBACK (on_icon_click), nullptr);
  gtk_widget_add_controller (g_icon_picture, GTK_EVENT_CONTROLLER (click));
}

void
icon_show_at (const RECT &preview_rect, HWND target)
{
  if (g_icon_window == nullptr || target == nullptr)
    return;

  int scale = gtk_widget_get_scale_factor (g_window);
  HMONITOR monitor = MonitorFromWindow (g_panel_hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {sizeof (mi)};
  GetMonitorInfoW (monitor, &mi);

  int size = kWindowIconSize * scale;
  double cx = (preview_rect.left + preview_rect.right) / 2.0;
  // ICON_OVERLAP: 70% of the icon overlaps the preview's bottom edge.
  double top = preview_rect.bottom - kIconOverlap * size;

  g_icon_want.left = mi.rcMonitor.left + (LONG)(cx - size / 2.0);
  g_icon_want.top = mi.rcMonitor.top + (LONG)top;
  g_icon_want.right = g_icon_want.left + size;
  g_icon_want.bottom = g_icon_want.top + size;

  // Icon at >= 64 physical px (WM_GETICON / shell item factory); cached per
  // target — the lookup is far too slow for the per-frame hover animation.
  if (g_icon_target != target) {
    GdkTexture *texture = winome_window_app_icon (target, size);
    gtk_picture_set_paintable (GTK_PICTURE (g_icon_picture),
                               GDK_PAINTABLE (texture));
    g_clear_object (&texture);
    g_icon_target = target;
  }

  if (gtk_widget_get_visible (g_icon_window)) {
    icon_chrome_apply_want ();
  } else {
    gtk_widget_set_visible (g_icon_window, TRUE);
  }
}

// --- hover (caption + close button + icon) ---------------------------------------

// Position the caption pill and both chrome windows against the current
// scaled preview rect.
void
hover_layout_chrome (void)
{
  int index = g_hover_index >= 0 ? g_hover_index : g_scaled_slot;
  if (index < 0 || index >= (int)g_slots.size ()) {
    gtk_widget_set_visible (g_caption, FALSE);
    return;
  }

  const SlotEntry &slot = g_slots[index];
  if (!IsWindow (slot.target)) {
    gtk_widget_set_visible (g_caption, FALSE);
    return;
  }

  int scale = gtk_widget_get_scale_factor (g_window);
  RECT disp = hover_display_rect ();

  // Caption pill: centered below the preview, under the icon overhang
  // (iconBottomOverlap + ICON_TITLE_SPACING logical px).
  GtkRequisition natural;
  gtk_widget_get_preferred_size (g_caption, nullptr, &natural);
  double cx = (disp.left + disp.right) / 2.0;
  double x = cx / scale - natural.width / 2.0;
  double y =
      (disp.bottom + (kWindowIconSize * (1.0 - kIconOverlap) +
                      kIconTitleSpacing) * scale) /
      scale;
  // gtk_fixed_move queues a container relayout even when nothing moved;
  // during fade-only frames the pill position is stable — skip it.
  static bool s_caption_pos_valid = false;
  static int s_caption_x = 0, s_caption_y = 0;
  if (!s_caption_pos_valid || (int)x != s_caption_x ||
      (int)y != s_caption_y) {
    gtk_fixed_move (GTK_FIXED (g_root), g_caption, (int)x, (int)y);
    s_caption_x = (int)x;
    s_caption_y = (int)y;
    s_caption_pos_valid = true;
  }
  gtk_widget_set_opacity (g_caption, g_caption_opacity);
  if (g_caption_opacity > 0.01)
    gtk_widget_set_visible (g_caption, TRUE);

  if (g_hover_index >= 0) {
    g_chrome_target = slot.target;
    chrome_show_at (disp);
    icon_show_at (disp, slot.target);
  }
}

// Frame-clock driver for the hover scale + caption fade animations: locked
// to the display refresh instead of a fixed 16ms timer beating against it.
gboolean
hover_anim_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
  (void)widget;
  (void)user_data;
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
    if (anim.index >= 0 && anim.index < (int)g_slots.size ()) {
      SlotEntry &slot = g_slots[anim.index];
      apply_thumb_rect (slot, scale_rect_around_center (slot.rect, s), 255);
      g_scaled_slot = anim.index;
      g_scaled_value = s;
    }
    if (t < 1.0)
      active = true;
    else if (anim.index >= 0 && anim.index < (int)g_slots.size ())
      apply_thumb_rect (g_slots[anim.index],
                        scale_rect_around_center (g_slots[anim.index].rect,
                                                  anim.to),
                        255);
  }
  g_scale_anims.erase (
      std::remove_if (g_scale_anims.begin (), g_scale_anims.end (),
                      [now] (const ScaleAnim &a) {
                        return (now - a.start_us) / 1000.0 >=
                               (double)kWindowScaleMs;
                      }),
      g_scale_anims.end ());

  // Caption fade.
  if (g_caption_opacity != g_caption_to) {
    double t = g_caption_anim_ms > 0
                   ? (now - g_caption_anim_start) / 1000.0 /
                         (double)g_caption_anim_ms
                   : 1.0;
    if (t < 0.0)
      t = 0.0;
    if (t > 1.0)
      t = 1.0;
    double e = ease_out_quad (t);
    g_caption_opacity =
        g_caption_from + (g_caption_to - g_caption_from) * e;
    if (t >= 1.0) {
      g_caption_opacity = g_caption_to;
      if (g_caption_to <= 0.01)
        gtk_widget_set_visible (g_caption, FALSE);
    } else {
      active = true;
    }
  }

  hover_layout_chrome ();

  if (!active && g_scale_anims.empty () && g_caption_opacity == g_caption_to) {
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
  double from = (index == g_scaled_slot) ? g_scaled_value : 1.0;
  if (from == to)
    return;
  for (ScaleAnim &a : g_scale_anims) {
    if (a.index == index) {
      a.from = from;
      a.to = to;
      a.start_us = g_get_monotonic_time ();
      hover_anim_ensure ();
      return;
    }
  }
  g_scale_anims.push_back (
      ScaleAnim{index, from, to, g_get_monotonic_time ()});
  hover_anim_ensure ();
}

void
caption_fade_start (double to)
{
  g_caption_from = g_caption_opacity;
  g_caption_to = to;
  g_caption_anim_start = g_get_monotonic_time ();
  g_caption_anim_ms = kOverlayFadeMs;
  hover_anim_ensure ();
}

// WINDOW_OVERLAY_IDLE_HIDE_TIMEOUT: hide the overlay 750ms after the pointer
// left, unless it is over the preview or one of the overlay pieces (GNOME's
// idle check looks at has-pointer on the close button / icon / title).
gboolean
hover_idle_hide_cb (gpointer user_data)
{
  (void)user_data;

  auto over_window = [] (HWND hwnd) -> bool {
    if (hwnd == nullptr || !IsWindowVisible (hwnd))
      return false;
    RECT r;
    if (!GetWindowRect (hwnd, &r))
      return false;
    POINT pt;
    return GetCursorPos (&pt) && PtInRect (&r, pt) != FALSE;
  };
  if (over_window (g_chrome_hwnd) || over_window (g_icon_hwnd))
    return G_SOURCE_CONTINUE;

  // Pointer back on the preview (it may have travelled over the close
  // button, which swallows the overview's motion events).
  if (g_hover_index >= 0 && g_hover_index < (int)g_slots.size () &&
      g_overview_hwnd != nullptr) {
    RECT r = g_slots[g_hover_index].rect;
    POINT pt;
    if (GetCursorPos (&pt) && ScreenToClient (g_overview_hwnd, &pt) &&
        pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom)
      return G_SOURCE_CONTINUE;
  }

  // Pointer on the caption pill (the title actor is reactive upstream).
  if (g_overview_hwnd != nullptr && gtk_widget_get_visible (g_caption)) {
    POINT pt;
    if (GetCursorPos (&pt) && ScreenToClient (g_overview_hwnd, &pt)) {
      int scale = gtk_widget_get_scale_factor (g_window);
      GtkWidget *picked = gtk_widget_pick (
          g_root, (double)pt.x / scale, (double)pt.y / scale, GTK_PICK_DEFAULT);
      if (picked != nullptr && gtk_widget_is_ancestor (picked, g_caption))
        return G_SOURCE_CONTINUE;
    }
  }

  g_hover_idle_source = 0;
  int index = g_hover_index;
  g_hover_index = -1;
  chrome_hide ();
  icon_hide ();
  caption_fade_start (0.0);
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
    if (g_scale_anim_source != 0) {
      gtk_widget_remove_tick_callback (g_window, g_scale_anim_source);
      g_scale_anim_source = 0;
    }
    // Snap any scaled preview back to its slot rect.
    if (g_scaled_slot >= 0 && g_scaled_slot < (int)g_slots.size ())
      apply_thumb_rect (g_slots[g_scaled_slot], g_slots[g_scaled_slot].rect,
                        255);
    g_scaled_slot = -1;
    g_scaled_value = 1.0;
    g_hover_index = -1;
    g_caption_opacity = 0.0;
    g_caption_to = 0.0;
    gtk_widget_set_visible (g_caption, FALSE);
    chrome_hide ();
    icon_hide ();
    return;
  }

  if (index == g_hover_index)
    return;

  // windowPreview.js: overlayEnabled = active && stateAdjustment === 1 —
  // the hover chrome is disabled until the open animation finished.
  if (g_anim.source != 0 && !g_anim.closing)
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
  if (!IsWindow (slot.target)) {
    if (previous >= 0)
      schedule_hover_idle_hide ();
    return;
  }

  g_hover_index = index;

  // Caption pill below the preview.
  gtk_label_set_text (GTK_LABEL (g_caption),
                      hwnd_title_utf8 (slot.target).c_str ());
  caption_fade_start (1.0);

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

  int scale = gtk_widget_get_scale_factor (g_window);
  int px = (int)(x * scale);
  int py = (int)(y * scale);

  for (int i = 0; i < (int)g_slots.size (); ++i) {
    const RECT &r = g_slots[i].rect;
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
  // Keep the hover alive while the pointer is over the chrome windows (they
  // took the input; we would otherwise see a spurious leave).
  auto over_window = [] (HWND hwnd) -> bool {
    if (hwnd == nullptr || !IsWindowVisible (hwnd))
      return false;
    RECT r;
    if (!GetWindowRect (hwnd, &r))
      return false;
    POINT pt;
    return GetCursorPos (&pt) && PtInRect (&r, pt) != FALSE;
  };
  if (over_window (g_chrome_hwnd) || over_window (g_icon_hwnd))
    return;
  set_hover (-1);
}

// --- previews -------------------------------------------------------------------

void
destroy_previews (void)
{
  for (SlotEntry &e : g_slots)
    if (e.thumb != nullptr)
      DwmUnregisterThumbnail (e.thumb);
  g_slots.clear ();
  g_scaled_slot = -1;
  g_scaled_value = 1.0;
}

void
apply_thumb_rect (SlotEntry &e, const RECT &rect, BYTE opacity)
{
  if (e.thumb == nullptr)
    return;
  DWM_THUMBNAIL_PROPERTIES props = {};
  props.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_RECTDESTINATION |
                  DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
  props.fVisible = TRUE;
  props.opacity = opacity;
  props.rcDestination = rect;
  props.fSourceClientAreaOnly = FALSE;
  DwmUpdateThumbnailProperties (e.thumb, &props);
  e.current = rect;
}

void
create_previews (bool at_start_rects)
{
  if (g_overview_hwnd == nullptr)
    return;

  for (SlotEntry &e : g_slots) {
    HTHUMBNAIL thumb = nullptr;
    if (SUCCEEDED (DwmRegisterThumbnail (g_overview_hwnd, e.target, &thumb)) &&
        thumb != nullptr) {
      e.thumb = thumb;
      if (at_start_rects)
        apply_thumb_rect (e, e.start, e.minimized ? 0 : 255);
      else
        apply_thumb_rect (e, e.rect, 255);
    } else {
      e.thumb = nullptr;
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
}

void
set_mini_opacity (BYTE opacity)
{
  for (MiniSlot &m : g_mini_slots) {
    if (m.thumb == nullptr)
      continue;
    DWM_THUMBNAIL_PROPERTIES props = {};
    props.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_RECTDESTINATION |
                    DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
    props.fVisible = TRUE;
    props.opacity = opacity;
    props.rcDestination = m.rect;
    props.fSourceClientAreaOnly = FALSE;
    DwmUpdateThumbnailProperties (m.thumb, &props);
  }
}

// One DWM thumbnail per (desktop, window) pair inside the pills
// (WorkspaceThumbnail clones the windows of each workspace). Windows on
// other desktops are cloaked, but their thumbnails still render (the same
// mechanism the native Task View uses); registration failures just leave
// that pill wallpaper-only. @initial_opacity starts the fade-in during the
// open animation.
void
create_mini_previews (HMONITOR monitor, const RECT &monitor_rect,
                      int thumbs_x, int thumbs_y, BYTE initial_opacity)
{
  destroy_mini_previews ();
  if (g_overview_hwnd == nullptr || g_desktops.empty () ||
      overview_thumbs_get_height (g_thumbs) <= 0)
    return;

  // Every alt-tab window on the monitor, cloaked ones included; bucketed
  // by desktop afterwards.
  std::vector<OvWindowInfo> all;
  EnumContext ctx{monitor, monitor_rect, &all, false, true};
  EnumWindows (collect_windows, reinterpret_cast<LPARAM> (&ctx));

  // Pre-resolve each window's desktop once.
  std::vector<int> window_desktop (all.size (), -1);
  for (size_t i = 0; i < all.size (); ++i)
    window_desktop[i] = vd::window_desktop_index (all[i].hwnd);

  int scale = gtk_widget_get_scale_factor (g_window);
  int panel = g_panel_height;

  for (int d = 0; d < (int)g_desktops.size (); ++d) {
    GdkRectangle pill;
    if (!overview_thumbs_get_rect (g_thumbs, d, &pill))
      continue;

    // Pill origin in overview client space (physical px).
    double ox = (double)thumbs_x * scale + pill.x * scale;
    double oy = (double)thumbs_y * scale + pill.y * scale;
    // Porthole px -> pill px (physical).
    double s = overview_thumbs_get_scale (g_thumbs) * scale;

    for (size_t i = 0; i < all.size (); ++i) {
      if (window_desktop[i] != d)
        continue;

      // Clip the window rect to the porthole (the workarea) before scaling.
      RECT r = all[i].rect;
      RECT porthole = {0, panel, g_monitor_w, g_monitor_h};
      RECT c;
      c.left = std::max (r.left, porthole.left);
      c.top = std::max (r.top, porthole.top);
      c.right = std::min (r.right, porthole.right);
      c.bottom = std::min (r.bottom, porthole.bottom);
      if (c.right - c.left <= 0 || c.bottom - c.top <= 0)
        continue;

      MiniSlot m;
      m.thumb = nullptr;
      m.rect.left = (LONG)(ox + (c.left - porthole.left) * s);
      m.rect.top = (LONG)(oy + (c.top - porthole.top) * s);
      m.rect.right = (LONG)(ox + (c.right - porthole.left) * s);
      m.rect.bottom = (LONG)(oy + (c.bottom - porthole.top) * s);
      if (m.rect.right - m.rect.left < 1 || m.rect.bottom - m.rect.top < 1)
        continue;

      HTHUMBNAIL thumb = nullptr;
      if (SUCCEEDED (DwmRegisterThumbnail (g_overview_hwnd, all[i].hwnd,
                                           &thumb)) &&
          thumb != nullptr) {
        m.thumb = thumb;
        g_mini_slots.push_back (m);
      }
    }
  }

  set_mini_opacity (initial_opacity);
}

// --- layout ----------------------------------------------------------------------

struct PlacedGeometry {
  int search_x, search_y;      // logical
  int search_w, search_h;      // logical
  int dash_x, dash_y;          // logical
  int thumbs_x, thumbs_y;      // logical
  int thumbs_h;                // physical (0 = hidden)
  RECT ws_box;                 // physical workspace box (slots + background)
};

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

  // Search entry.
  GtkRequisition search_nat;
  gtk_widget_get_preferred_size (g_search, nullptr, &search_nat);
  int search_w = search_nat.width;
  StEngine *engine = StEngine::get ();
  if (engine != nullptr) {
    StEngine::Node node (*engine, nullptr, "", "search-entry", "");
    double v = 0;
    if (node.lookup_length ("width", &v) && v > 0)
      search_w = (int)(v + 0.5);
  }
  int search_h = search_nat.height;
  int search_total =
      (kSearchMarginTop + kSearchMarginBottom) * scale + search_h * scale;

  // Workspace thumbnails (ThumbnailsBox, shown for >1 desktop). The row is
  // capped at MAX_THUMBNAIL_SCALE of the workarea height
  // (ControlsManagerLayout), full width, content centered.
  int thumbs_h = 0;  // physical
  if (g_desktops.size () > 1) {
    int box_h = (int)(wa_h * kMaxThumbnailScale) / scale;  // logical
    overview_thumbs_repopulate (g_thumbs, (int)g_desktops.size (),
                                g_desktop_active, box_h, (double)mw / scale,
                                (double)mw, (double)wa_h, g_wallpaper);
    thumbs_h = overview_thumbs_get_height (g_thumbs) * scale;
  } else {
    overview_thumbs_repopulate (g_thumbs, 0, -1, 0, 0, 0, 0, nullptr);
  }

  // Dash (16% cap, ControlsManagerLayout DASH_MAX_HEIGHT_RATIO).
  int dash_max = (int)(wa_h * kDashMaxHeightRatio + 0.5);
  overview_dash_repopulate (g_dash, monitor, dash_max / scale);
  int dash_w = overview_dash_get_width (g_dash) * scale;
  int dash_h = std::min (overview_dash_get_height (g_dash) * scale, dash_max);

  // Picker container box (ControlsManagerLayout WINDOW_PICKER): the
  // thumbnails row sits between the search entry and the workspace box,
  // which yields to thumbnailsHeight + round(spacing * 0.4) on both edges.
  RECT box;
  box.left = 0;
  box.top = panel_height + search_total + thumb_adjust + thumbs_h +
            thumb_adjust_bottom;
  box.right = mw;
  box.bottom = panel_height + wa_h - dash_h - spacing;

  // Workspace box: as tall as the container box, width preserving the
  // workarea aspect ratio, centered (WorkspacesView
  // _getFirstFitSingleWorkspaceBox + WorkspaceLayout.vfunc_get_preferred_
  // width/height). Window slots and the background live inside it.
  double wa_aspect = (double)mw / (double)wa_h;
  int ws_h = box.bottom - box.top;
  int ws_w = (int)(ws_h * wa_aspect + 0.5);
  ws_w = std::min (ws_w, mw);
  int ws_x = (mw - ws_w) / 2;
  RECT ws_box = {ws_x, box.top, ws_x + ws_w, box.top + ws_h};

  out->ws_box = ws_box;
  out->search_w = search_w;
  out->search_h = search_h;
  out->search_x = (mw / scale - search_w) / 2;
  out->search_y = (panel_height + kSearchMarginTop * scale) / scale;
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
  g_ws_box = ws_box;

  // Workspace-background animation endpoints (both share the workarea
  // aspect, so interpolating the edges keeps the aspect at every step).
  g_bg_full = RECT{0, panel_height, mw, mh};
  g_bg_final = ws_box;

  // .workspace-background border-radius.
  g_bg_radius_target = kWorkspaceRadiusFallback;
  if (engine != nullptr) {
    StEngine::Node node (*engine, nullptr, "", "workspace-background", "");
    double v = 0;
    if (node.lookup_length ("border-radius", &v) && v > 0)
      g_bg_radius_target = v;
  }
  return true;
}

void
apply_bg_rect (const RECT &rect)
{
  if (g_ws_bg == nullptr || g_window == nullptr)
    return;
  int scale = gtk_widget_get_scale_factor (g_window);
  gtk_widget_set_size_request (g_ws_bg, (rect.right - rect.left) / scale,
                               (rect.bottom - rect.top) / scale);
  gtk_fixed_move (GTK_FIXED (g_root), g_ws_bg, rect.left / scale,
                  rect.top / scale);
  gtk_widget_queue_draw (g_ws_bg);
}

void
position_widgets (const PlacedGeometry &geo, bool opening)
{
  gtk_widget_set_size_request (g_search, geo.search_w, geo.search_h);
  gtk_fixed_move (GTK_FIXED (g_root), g_search, geo.search_x, geo.search_y);

  // The background starts at the full work area (radius 0) when opening —
  // the open animation shrinks it into the picker box — and sits at the
  // final box otherwise.
  apply_bg_rect (opening ? g_bg_full : g_bg_final);
  g_bg_radius = opening ? 0.0 : g_bg_radius_target;

  gtk_fixed_move (GTK_FIXED (g_root), g_dash, geo.dash_x, geo.dash_y);

  // Thumbnails row (invisible for a single desktop; the widget's natural
  // size collapses to 0 in that case).
  gtk_fixed_move (GTK_FIXED (g_root), g_thumbs, geo.thumbs_x, geo.thumbs_y);
  gtk_widget_set_visible (g_thumbs, geo.thumbs_h > 0);

  // Search band + thumbnails row in PHYSICAL px (click exclusion compares
  // physical coords). The thumbs row spans the widget's full natural width,
  // centered like the widget itself.
  int scale = gtk_widget_get_scale_factor (g_window);
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
  gtk_widget_set_visible (g_ws_bg, TRUE);
  gtk_widget_set_visible (g_dash, TRUE);
}

// --- z-order ----------------------------------------------------------------------

void
apply_ex_styles (HWND hwnd)
{
  LONG_PTR ex = GetWindowLongPtrW (hwnd, GWL_EXSTYLE);
  LONG_PTR want = ex | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
  want &= ~WS_EX_APPWINDOW;
  if (want != ex)
    SetWindowLongPtrW (hwnd, GWL_EXSTYLE, want);

  HWND after = (g_panel_hwnd != nullptr && IsWindow (g_panel_hwnd))
                   ? g_panel_hwnd
                   : HWND_TOPMOST;
  SetWindowPos (hwnd, after, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static void dump_zorder (const char *tag);
static void restack_locked (void);

// Rebuild slots, thumbnails and widget positions for the current monitor.
// Must run after the overview window is mapped (GDK's first-map sequence
// would overwrite earlier placement).
static void
place_overview_locked (bool opening)
{
  if (g_window == nullptr || g_panel_hwnd == nullptr ||
      !gtk_widget_get_visible (g_window))
    return;

  HMONITOR monitor = MonitorFromWindow (g_panel_hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {sizeof (mi)};
  if (!GetMonitorInfoW (monitor, &mi))
    return;

  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (g_window));
  if (surface == nullptr)
    return;
  g_overview_hwnd =
      static_cast<HWND> (gdk_win32_surface_get_handle (surface));
  apply_ex_styles (g_overview_hwnd);

  int w = mi.rcMonitor.right - mi.rcMonitor.left;
  int h = mi.rcMonitor.bottom - mi.rcMonitor.top;
  SetWindowPos (g_overview_hwnd, g_panel_hwnd, mi.rcMonitor.left,
                mi.rcMonitor.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);

  int panel_height = 0;
  RECT pr;
  if (IsWindowVisible (g_panel_hwnd) && GetWindowRect (g_panel_hwnd, &pr))
    panel_height = pr.bottom - pr.top;
  g_panel_height = panel_height;

  load_wallpaper ();

  // Virtual desktops (single-desktop systems keep the classic layout).
  g_desktops = vd::desktops ();
  g_desktop_active = g_desktops.empty () ? -1 : vd::current_index ();
  if (g_desktop_active < 0)
    g_desktops.clear ();
  g_print ("[ov] desktops=%zu active=%d\n", g_desktops.size (),
           g_desktop_active);

  // Geometry (ControlsManagerLayout + WorkspacesView workspace box).
  PlacedGeometry geo;
  if (!compute_geometry (monitor, mi.rcMonitor, panel_height, &geo))
    return;
  position_widgets (geo, opening);

  // The dark backdrop applies only once the workspace background actually
  // covers the work area: before that, a bare #overviewGroup would flash
  // full-screen dark in the pre-placement frames.
  gtk_widget_set_name (g_root, "overviewGroup");

  // Windows -> slots (workspace.js strategy inside the workspace box, with
  // the chrome-oversize-adjusted spacing). With the virtual desktop API
  // reachable, the picker shows the CURRENT desktop's windows by membership
  // (pinned + suspended included), not by visibility alone.
  std::vector<OvWindowInfo> windows;
  EnumContext ctx{monitor, mi.rcMonitor, &windows, !g_desktops.empty (),
                  false};
  EnumWindows (collect_windows, reinterpret_cast<LPARAM> (&ctx));

  RECT workarea = {0, panel_height, w, h};
  int scale = gtk_widget_get_scale_factor (g_window);
  double chrome_oversize =
      std::max ({kCloseButtonSize / 2.0,
                 kWindowIconSize * (1.0 - kIconOverlap)}) +
      kWindowActiveSizeInc;
  double spacing = (kWindowPickerSpacing + chrome_oversize) * scale;

  std::vector<OvSlot> slots;
  overview_compute_slots (windows, mi.rcMonitor, workarea, geo.ws_box,
                          spacing, spacing, &slots);

  set_hover (-1, true);
  destroy_previews ();
  g_slots.reserve (slots.size ());
  for (const OvSlot &s : slots) {
    SlotEntry e;
    e.target = s.window.hwnd;
    e.start = s.window.rect;
    e.minimized = IsIconic (s.window.hwnd) != FALSE;
    int sw = (int)((s.window.rect.right - s.window.rect.left) * s.scale);
    int sh = (int)((s.window.rect.bottom - s.window.rect.top) * s.scale);
    e.rect.left = (int)s.x;
    e.rect.top = (int)s.y;
    e.rect.right = e.rect.left + sw;
    e.rect.bottom = e.rect.top + sh;
    if (e.rect.right > e.rect.left && e.rect.bottom > e.rect.top) {
      if (e.minimized) {
        // Not showing on its workspace: grow from a zero-size point at the
        // workspace corner while fading in (WorkspaceLayout.vfunc_allocate
        // else-branch + _syncOpacity).
        e.start.left = geo.ws_box.left;
        e.start.top = geo.ws_box.top;
        e.start.right = e.start.left + 1;
        e.start.bottom = e.start.top + 1;
      }
      g_slots.push_back (e);
    }
  }

  // Thumbnails start at the windows' real positions and fly into the slots;
  // a relayout (window closed etc.) snaps to the final rects.
  create_previews (opening);

  // Mini window previews inside the workspace thumbnail pills.
  create_mini_previews (monitor, mi.rcMonitor, geo.thumbs_x, geo.thumbs_y,
                        opening ? 0 : 255);

  // Remember which desktop this layout belongs to (the poll below relayouts
  // when the desktop changed elsewhere, e.g. Win+Ctrl+Left/Right).
  g_desktop_active = g_desktops.empty () ? -1 : vd::current_index ();
  g_print ("[ov] placed %dx%d slots=%zu ws=%ldx%ld dash=%d\n", w, h,
           g_slots.size (), (long)(geo.ws_box.right - geo.ws_box.left),
           (long)(geo.ws_box.bottom - geo.ws_box.top),
           overview_dash_get_width (g_dash));

  restack_locked ();
  if (opening)
    start_open_animation ();
  else {
    for (SlotEntry &s : g_slots)
      apply_thumb_rect (s, s.rect, 255);
    gtk_widget_set_opacity (g_thumbs, 1.0);
  }
  dump_zorder ("placed");
}

static gboolean
overview_place_idle (gpointer user_data)
{
  (void)user_data;
  place_overview_locked (false);
  return G_SOURCE_REMOVE;
}

static gboolean
overview_place_opening_idle (gpointer user_data)
{
  (void)user_data;
  place_overview_locked (true);
  return G_SOURCE_REMOVE;
}

static void
dump_zorder (const char *tag)
{
  HWND panel = g_panel_hwnd;
  HWND overview = g_overview_hwnd;
  HWND popover = static_cast<HWND> (winome_shell_panel_top_popover_hwnd ());

  int pos_panel = -1, pos_overview = -1, pos_popover = -1, pos_chrome = -1;

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
    wnd = GetWindow (wnd, GW_HWNDNEXT);
  }

  g_print ("[zorder:%s] panel@%d overview@%d popover@%d chrome@%d "
           "contract=%s\n",
           tag, pos_panel, pos_overview, pos_popover, pos_chrome,
           (pos_panel >= 0 && pos_overview >= 0 && pos_panel < pos_overview)
               ? "OK"
               : "BROKEN");
}

// Verify the popover > panel > chrome windows > overview chain is already
// in order: the periodic 400ms tick and the popover-raise retries then no-op
// instead of re-sending SWP_FRAMECHANGED SetWindowPos calls, each of which
// forces a WM_NCCALCSIZE round-trip and reshuffles the topmost band.
static bool
zorder_drifted (void)
{
  HWND chain[5];
  int n = 0;
  HWND popover = static_cast<HWND> (winome_shell_panel_top_popover_hwnd ());
  if (popover != nullptr)
    chain[n++] = popover;
  chain[n++] = g_panel_hwnd;
  if (g_chrome_hwnd != nullptr && IsWindow (g_chrome_hwnd) &&
      IsWindowVisible (g_chrome_hwnd))
    chain[n++] = g_chrome_hwnd;
  if (g_icon_hwnd != nullptr && IsWindow (g_icon_hwnd) &&
      IsWindowVisible (g_icon_hwnd))
    chain[n++] = g_icon_hwnd;
  if (g_overview_hwnd != nullptr && IsWindow (g_overview_hwnd) &&
      IsWindowVisible (g_overview_hwnd))
    chain[n++] = g_overview_hwnd;

  // Every entry must sit directly below its predecessor — that is exactly
  // what the SetWindowPos sequence below produces.
  for (int i = 1; i < n; ++i)
    if (GetWindow (chain[i], GW_HWNDPREV) != chain[i - 1])
      return true;
  // Without a popover the panel must be the very top window; with one open
  // whatever sits above the popover (foreign topmost windows) is not ours to
  // police.
  if (chain[0] == g_panel_hwnd)
    return GetWindow (g_panel_hwnd, GW_HWNDPREV) != nullptr;
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

  HWND popover = static_cast<HWND> (winome_shell_panel_top_popover_hwnd ());

  if (!SetWindowPos (g_panel_hwnd, popover != nullptr ? popover : HWND_TOPMOST,
                     0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                         SWP_FRAMECHANGED))
    g_print ("[restack] panel failed gle=%lu\n", GetLastError ());

  // The chrome windows sit directly below the panel (above the overview,
  // hence above the DWM thumbnails): close button first, icon below it.
  HWND below_panel = g_panel_hwnd;
  if (g_chrome_hwnd != nullptr && IsWindow (g_chrome_hwnd) &&
      IsWindowVisible (g_chrome_hwnd)) {
    SetWindowPos (g_chrome_hwnd, below_panel, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                      SWP_FRAMECHANGED);
    below_panel = g_chrome_hwnd;
  }
  if (g_icon_hwnd != nullptr && IsWindow (g_icon_hwnd) &&
      IsWindowVisible (g_icon_hwnd)) {
    SetWindowPos (g_icon_hwnd, below_panel, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                      SWP_FRAMECHANGED);
    below_panel = g_icon_hwnd;
  }

  if (g_overview_hwnd != nullptr && IsWindowVisible (g_overview_hwnd)) {
    if (!SetWindowPos (g_overview_hwnd, below_panel, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                           SWP_FRAMECHANGED))
      g_print ("[restack] overview failed gle=%lu\n", GetLastError ());
  }
}

// --- animation ---------------------------------------------------------------------

gboolean
anim_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
  (void)widget;
  (void)user_data;
  Animation &anim = g_anim;

  // Frame clock keeps the 250ms transition locked to the compositor's
  // cadence (a fixed 16ms timer beats against the display refresh).
  gint64 now = gdk_frame_clock_get_frame_time (clock);
  double t = (now - anim.start_us) / 1000.0 / (double)anim.duration_ms;
  if (t < 0.0)
    t = 0.0;   // frame timestamp can predate the anim start
  if (t > 1.0)
    t = 1.0;

  // Overview.ANIMATION_TIME: opening EASE_OUT_SINE, closing EASE_OUT_QUAD.
  double e = anim.closing ? ease_out_quad (t) : ease_out_sine (t);

  // Workspace background: full work area (radius 0) <-> picker box.
  RECT bg = anim.closing ? lerp_rect (g_bg_final, g_bg_full, e)
                         : lerp_rect (g_bg_full, g_bg_final, e);
  apply_bg_rect (bg);
  g_bg_radius = g_bg_radius_target * (anim.closing ? (1.0 - e) : e);

  // The thumbnails row fades with the transition (its mini previews are DWM
  // thumbnails compositing ABOVE the window, so they fade by opacity —
  // upstream eases expandFraction the same way over the 250ms transition).
  bool thumbs_visible = gtk_widget_get_visible (g_thumbs);
  if (thumbs_visible) {
    double f = anim.closing ? (1.0 - e) : e;
    gtk_widget_set_opacity (g_thumbs, f);
    set_mini_opacity ((BYTE)(255 * f));
  }

  if (anim.closing) {
    // Fly back out: slot -> real window rect, controls go away.
    double k = 1.0 - e;
    for (SlotEntry &s : g_slots) {
      if (s.minimized) {
        apply_thumb_rect (s, lerp_rect (s.rect, s.start, e),
                          (BYTE)(255 * k));
      } else {
        apply_thumb_rect (s, lerp_rect (s.rect, s.start, e), 255);
      }
    }

    if (t >= 1.0) {
      anim.source = 0;
      finish_hide ();
      return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
  }

  // Opening: real window rect -> slot.
  for (SlotEntry &s : g_slots) {
    if (s.minimized)
      apply_thumb_rect (s, lerp_rect (s.start, s.rect, e), (BYTE)(255 * e));
    else
      apply_thumb_rect (s, lerp_rect (s.start, s.rect, e), 255);
  }

  if (t >= 1.0) {
    anim.source = 0;
    for (SlotEntry &s : g_slots)
      apply_thumb_rect (s, s.rect, 255);
    if (thumbs_visible) {
      gtk_widget_set_opacity (g_thumbs, 1.0);
      set_mini_opacity (255);
    }
    // overlayEnabled flips true at stateAdjustment === 1: re-evaluate the
    // hover under the pointer (windowPreview.js checks has-pointer).
    if (g_overview_hwnd != nullptr) {
      POINT pt;
      if (GetCursorPos (&pt) && ScreenToClient (g_overview_hwnd, &pt)) {
        int idx = -1;
        for (int i = 0; i < (int)g_slots.size (); ++i) {
          const RECT &r = g_slots[i].rect;
          if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top &&
              pt.y < r.bottom) {
            idx = i;
            break;
          }
        }
        set_hover (idx);
      }
    }
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

void
start_animation (bool closing, guint duration_ms)
{
  if (g_anim.source != 0) {
    gtk_widget_remove_tick_callback (g_window, g_anim.source);
    g_anim.source = 0;
  }
  g_anim.closing = closing;
  g_anim.duration_ms = duration_ms;
  g_anim.start_us = g_get_monotonic_time ();
  g_anim.source =
      gtk_widget_add_tick_callback (g_window, anim_tick, nullptr, nullptr);
}

void
start_open_animation (void)
{
  start_animation (false, kOpenMs);
}

void
finish_hide (void)
{
  set_hover (-1, true);
  destroy_previews ();
  destroy_mini_previews ();
  if (g_desktop_poll != 0) {
    g_source_remove (g_desktop_poll);
    g_desktop_poll = 0;
  }
  // The side controls stay VISIBLE at their end-of-close geometry (the
  // background rests on the full work area, radius 0): the window keeps its
  // fullscreen size while hidden, so the first frame of the next open shows
  // the wallpaper-covered work area — GNOME's t=0 — instead of a bare
  // #222226 flash while the placement idle is still pending.
  g_bg_radius = 0.0;
  gtk_widget_set_opacity (g_thumbs, 1.0);
  if (g_window != nullptr && gtk_widget_get_visible (g_window))
    gtk_widget_set_visible (g_window, FALSE);
}

// --- clicks --------------------------------------------------------------------------

void
on_click_released (GtkGestureClick *gesture, int n_press, double x, double y,
                   gpointer user_data)
{
  (void)n_press;
  (void)user_data;
  if (gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture)) !=
      GDK_BUTTON_PRIMARY)
    return;

  int scale = g_window != nullptr ? gtk_widget_get_scale_factor (g_window) : 1;
  int px = (int)(x * scale);
  int py = (int)(y * scale);

  // A click that just dismissed a panel popover floating above the overview
  // is consumed by GNOME's menu grab: close the menu only.
  if (winome_shell_panel_popover_just_closed ()) {
    g_print ("[ov] click (%d,%d) swallowed (popover dismiss)\n", px, py);
    return;
  }

  if (g_panel_height > 0 && py < g_panel_height) {
    g_print ("[ov] click (%d,%d) ignored (panel strip)\n", px, py);
    return;
  }

  POINT pt = {px, py};

  // The title pill is reactive upstream and propagates the click to the
  // preview: activate the hovered window.
  if (g_hover_index >= 0 && gtk_widget_get_visible (g_caption)) {
    double cx = 0.0, cy = 0.0;
    if (gtk_widget_translate_coordinates (g_root, g_caption, x, y, &cx, &cy) &&
        gtk_widget_contains (g_caption, cx, cy)) {
      g_print ("[ov] click (%d,%d) hit caption\n", px, py);
      SlotEntry &e = g_slots[g_hover_index];
      if (IsWindow (e.target)) {
        if (IsIconic (e.target))
          ShowWindow (e.target, SW_RESTORE);
        SetForegroundWindow (e.target);
      }
      close_overview ("ov-click-caption");
      return;
    }
  }

  // Inside a preview slot: restore/focus that window and close.
  for (const SlotEntry &e : g_slots) {
    if (pt.x >= e.rect.left && pt.x < e.rect.right && pt.y >= e.rect.top &&
        pt.y < e.rect.bottom) {
      g_print ("[ov] click (%d,%d) hit slot\n", px, py);
      if (IsWindow (e.target)) {
        if (IsIconic (e.target))
          ShowWindow (e.target, SW_RESTORE);
        SetForegroundWindow (e.target);
      }
      close_overview ("ov-click-slot");
      return;
    }
  }

  // Dash band and search row handle their own input.
  if (py >= g_dash_top) {
    g_print ("[ov] click (%d,%d) ignored (dash band)\n", px, py);
    return;
  }
  if (pt.x >= g_search_rect.left && pt.x < g_search_rect.right &&
      pt.y >= g_search_rect.top && pt.y < g_search_rect.bottom) {
    g_print ("[ov] click (%d,%d) ignored (search entry)\n", px, py);
    return;
  }
  // The workspace thumbnails row handles its own input (clicking a pill
  // switches desktop; the overview stays open, exactly like upstream).
  if (g_thumbs_rect.right > g_thumbs_rect.left &&
      pt.x >= g_thumbs_rect.left && pt.x < g_thumbs_rect.right &&
      pt.y >= g_thumbs_rect.top && pt.y < g_thumbs_rect.bottom) {
    g_print ("[ov] click (%d,%d) ignored (thumbs row)\n", px, py);
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
}

void
on_realize (GtkWidget *widget, gpointer user_data)
{
  (void)user_data;
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (widget));
  if (surface != nullptr)
    apply_ex_styles (
        static_cast<HWND> (gdk_win32_surface_get_handle (surface)));
}

void
on_overview_map (GtkWidget *widget, gpointer user_data)
{
  (void)widget;
  (void)user_data;
  // G_PRIORITY_HIGH_IDLE (100) sorts BEFORE the frame clock's repaint source
  // (G_PRIORITY_REDRAW 120): the placement runs before the first post-map
  // paint, so the first frame is the placed overview, not a dark flash.
  g_idle_add_full (G_PRIORITY_HIGH_IDLE, overview_place_opening_idle, nullptr,
                   nullptr);

  // Watch for desktop switches made elsewhere while the overview is open
  // (Win+Ctrl+Left/Right); the overview relayouts to the new desktop.
  if (g_desktop_poll == 0)
    g_desktop_poll = g_timeout_add (400, desktop_poll_cb, nullptr);
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
  if (vd::switch_to (index)) {
    rebuild_now ();
  } else {
    g_print ("[ov] desktop switch failed\n");
  }
}

// Relayout when the active desktop changed outside the overview.
gboolean
desktop_poll_cb (gpointer user_data)
{
  (void)user_data;
  if (g_window == nullptr || !gtk_widget_get_visible (g_window)) {
    g_desktop_poll = 0;
    return G_SOURCE_REMOVE;
  }
  int cur = g_desktops.empty () ? -1 : vd::current_index ();
  if (cur >= 0 && cur != g_desktop_active) {
    g_print ("[ov] desktop changed %d -> %d (external)\n", g_desktop_active,
             cur);
    rebuild_now ();
  }
  return G_SOURCE_CONTINUE;
}

// Relayout without animation (window closed via the chrome button etc.).
void
rebuild_now (void)
{
  if (g_window == nullptr || !gtk_widget_get_visible (g_window))
    return;
  if (g_anim.source != 0 && g_anim.closing)
    return;
  place_overview_locked (false);
}

gboolean
show_idle (gpointer user_data)
{
  (void)user_data;
  overview_show ();
  return G_SOURCE_REMOVE;
}

gboolean
hide_idle (gpointer user_data)
{
  (void)user_data;
  overview_hide ();
  return G_SOURCE_REMOVE;
}

}  // namespace

void
overview_init (HWND panel_hwnd)
{
  g_panel_hwnd = panel_hwnd;

  g_window = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (g_window), "WINOME Overview");
  gtk_window_set_decorated (GTK_WINDOW (g_window), FALSE);
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
  gtk_window_set_child (GTK_WINDOW (g_window), g_root);

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
      strcmp (get_system_language (), "zh") == 0 ? "输入以搜索"
                                                 : "Type to search");
  gtk_fixed_put (GTK_FIXED (g_root), g_search, 0, 0);
  gtk_widget_set_visible (g_search, FALSE);

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
  g_dash = overview_dash_new (g_root);
  gtk_fixed_put (GTK_FIXED (g_root), g_dash, 0, 0);
  gtk_widget_set_visible (g_dash, FALSE);

  // .workspace-thumbnails — one wallpaper pill per virtual desktop, above
  // the dash and below the workspace background (the thumbnailsBox position
  // in ControlsManager.add_child). Invisible until repopulated with >1
  // desktop during placement.
  g_thumbs = overview_thumbs_new ();
  overview_thumbs_set_click_cb (g_thumbs, on_thumb_clicked, nullptr);
  gtk_fixed_put (GTK_FIXED (g_root), g_thumbs, 0, 0);
  gtk_widget_set_visible (g_thumbs, FALSE);

  // .workspace-background — wallpaper in an animated rounded clip (+ CSS
  // shadow), on the workarea-aspect workspace box.
  g_ws_bg = (GtkWidget *)g_object_new (winome_workspace_bg_get_type (),
                                       nullptr);
  gtk_widget_add_css_class (g_ws_bg, "workspace-background");
  gtk_fixed_put (GTK_FIXED (g_root), g_ws_bg, 0, 0);
  gtk_widget_set_visible (g_ws_bg, FALSE);

  // .window-caption hover pill.
  g_caption = gtk_label_new ("");
  gtk_widget_add_css_class (g_caption, "window-caption");
  gtk_fixed_put (GTK_FIXED (g_root), g_caption, 0, 0);
  gtk_widget_set_visible (g_caption, FALSE);

  // Chrome windows (above the DWM thumbnails).
  chrome_init ();
  icon_chrome_init ();

  g_signal_connect (g_window, "realize", G_CALLBACK (on_realize), nullptr);
  g_signal_connect (g_window, "map", G_CALLBACK (on_overview_map), nullptr);

  // Hover tracking for preview close buttons + captions.
  GtkEventController *motion = gtk_event_controller_motion_new ();
  g_signal_connect (motion, "motion", G_CALLBACK (on_root_motion), nullptr);
  g_signal_connect (motion, "leave", G_CALLBACK (on_root_motion_leave),
                    nullptr);
  gtk_widget_add_controller (g_root, GTK_EVENT_CONTROLLER (motion));

  // Slot hit-testing for preview activation; blank clicks close.
  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "released", G_CALLBACK (on_click_released),
                    nullptr);
  gtk_widget_add_controller (g_window, GTK_EVENT_CONTROLLER (click));
}

void
overview_show (void)
{
  if (g_window == nullptr || g_panel_hwnd == nullptr)
    return;

  if (!gtk_widget_get_visible (g_window)) {
    g_print ("[ov] show (requesting map)\n");
    gtk_widget_set_visible (g_window, TRUE);
  } else {
    g_idle_add_full (G_PRIORITY_HIGH_IDLE, overview_place_idle, nullptr,
                     nullptr);
  }
}

void
overview_hide (void)
{
  g_print ("[ov] hide\n");
  set_hover (-1, true);
  if (g_window == nullptr || !gtk_widget_get_visible (g_window)) {
    destroy_previews ();
    return;
  }
  // Animate out, then unmap from finish_hide().
  start_animation (true, kCloseMs);
}

void
overview_restack (void)
{
  restack_locked ();
}

void
overview_show_async (void)
{
  g_main_context_invoke (nullptr, show_idle, nullptr);
}

void
overview_hide_async (void)
{
  g_main_context_invoke (nullptr, hide_idle, nullptr);
}

}  // namespace winome
