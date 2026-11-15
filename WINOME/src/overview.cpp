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
#include "easing.h"
#include "i18n.h"
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
#include <shellapi.h>

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

// overviewControls.js ratios.
constexpr double kVerticalSpacingRatio = 0.02;
constexpr double kThumbnailsAdjustTop = 0.6;
constexpr double kThumbnailsAdjustBottom = 0.4;  // THUMBNAILS_SPACING_ADJ_
                                                  // USTMENT_BOTTOM
constexpr double kMaxThumbnailScale = 0.05;      // MAX_THUMBNAIL_SCALE
constexpr double kDashMaxHeightRatio = 0.16;
constexpr double kSmallWorkspaceRatio = 0.15;    // SMALL_WORKSPACE_RATIO

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
GtkWidget *g_app_grid = nullptr;  // paginated App Grid overlay
GtkWidget *g_dash = nullptr;      // overview dash
GtkWidget *g_thumbs = nullptr;    // .workspace-thumbnails row
GtkWidget *g_caption = nullptr;   // .window-caption hover pill
GdkTexture *g_wallpaper = nullptr;
bool g_app_grid_mode = false;     // true when Show Apps grid is active

// Drag-to-desktop state for window previews.
int g_drag_slot = -1;
HWND g_drag_hwnd = nullptr;
RECT g_drag_orig_rect{};
int g_drag_start_px = 0;
int g_drag_start_py = 0;
bool g_drag_active = false;
bool g_drag_just_ended = false;
bool g_drag_thumbs_revealed = false;
std::string g_search_query;       // lowercased UTF-8 live search filter

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

// Window-caption chrome window (separate toplevel, above the DWM thumbnails).
GtkWidget *g_caption_window = nullptr;
HWND g_caption_hwnd = nullptr;
RECT g_caption_want = {0, 0, 0, 0};

// One preview slot.
struct SlotEntry {
  RECT start;      // real window rect (client space) for the fly animation;
                   // a 1x1 point at the workspace corner for minimized
  RECT rect;       // slot rect (client space)
  RECT current;    // animated rect currently set on the thumbnail
  HWND target;
  HTHUMBNAIL thumb;
  bool minimized = false;
  double current_scale = 1.0;
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

// Workspace-switch animation (workspacesView.js _scrollToActive:
// WORKSPACE_SWITCH_TIME 250ms EASE_OUT_CUBIC on the scroll adjustment —
// the whole workspace strip slides horizontally while the previous
// desktop's previews slide out and the new one slides in; the thumbnail
// indicator rides the same eased value). `g_leaving` keeps the previous
// desktop's DWM thumbnails alive for the outgoing slide.
constexpr guint kSwitchMs = 250;  // WORKSPACE_SWITCH_TIME
std::vector<SlotEntry> g_leaving;
guint g_switch_source = 0;
gint64 g_switch_start_us = 0;
int g_switch_from = -1, g_switch_to = -1;
int g_switch_width = 0;  // slide distance: workspace box width

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
static void place_overview_locked (bool opening);
static gboolean overview_place_idle (gpointer user_data);
static gboolean overview_place_opening_idle (gpointer user_data);
static void set_hover (int index, bool immediate = false);
static void hover_layout_chrome (void);
static void schedule_hover_idle_hide (void);
static void cancel_hover_idle (void);
static void apply_thumb_rect (SlotEntry &e, const RECT &rect, BYTE opacity);
static void destroy_mini_previews (void);
static void set_mini_opacity (BYTE opacity);
static void rebuild_now (void);
static void start_open_animation (void);
static void finish_hide (void);
static gboolean desktop_poll_cb (gpointer user_data);
static void restack_locked (void);
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
  if (g_overview_hwnd == nullptr)
    return -1;
  POINT pt;
  if (!GetCursorPos (&pt) || !ScreenToClient (g_overview_hwnd, &pt))
    return -1;
  for (int i = 0; i < (int)g_slots.size (); ++i) {
    const RECT r = slot_display_rect (i);
    if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom)
      return i;
  }
  return -1;
}

void
activate_slot_and_close (int index, const char *reason)
{
  if (index >= 0 && index < (int)g_slots.size () &&
      IsWindow (g_slots[index].target)) {
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
utf8_tolower (const std::string &s)
{
  if (s.empty ())
    return {};
  gchar *down = g_utf8_strdown (s.c_str (), static_cast<gssize> (s.size ()));
  if (down == nullptr)
    return {};
  std::string out (down);
  g_free (down);
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

bool
window_matches_search (HWND hwnd, const std::string &query_lower)
{
  if (query_lower.empty ())
    return true;
  std::string title = utf8_tolower (hwnd_title_utf8 (hwnd));
  if (title.find (query_lower) != std::string::npos)
    return true;
  std::string proc = utf8_tolower (hwnd_process_name_utf8 (hwnd));
  return proc.find (query_lower) != std::string::npos;
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

  if (!ctx->include_cloaked && !g_search_query.empty () &&
      !window_matches_search (hwnd, g_search_query))
    return TRUE;

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
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
}

static void
chrome_bind_owner (HWND hwnd)
{
  if (hwnd == nullptr || !IsWindow (hwnd) || hwnd == g_overview_hwnd ||
      g_overview_hwnd == nullptr || !IsWindow (g_overview_hwnd))
    return;
  LONG_PTR owner = reinterpret_cast<LONG_PTR> (g_overview_hwnd);
  if (GetWindowLongPtrW (hwnd, GWLP_HWNDPARENT) != owner)
    SetWindowLongPtrW (hwnd, GWLP_HWNDPARENT, owner);
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
    UINT flags = SWP_NOACTIVATE | (vis ? 0 : SWP_SHOWWINDOW) |
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
  gtk_widget_add_css_class (win, "winome-overview-chrome");
  install_bundled_fonts (win);
  g_signal_connect (win, "realize", G_CALLBACK (on_chrome_window_realize),
                    hooks);
  g_signal_connect (win, "map", G_CALLBACK (on_chrome_window_map), hooks);
  return win;
}

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
  overlay_hide_all ();
  g_hover_index = -1;
  g_last_hovered_slot = -1;
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

  g_caption = gtk_label_new ("");
  gtk_widget_add_css_class (g_caption, "window-caption");
  gtk_label_set_single_line_mode (GTK_LABEL (g_caption), TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (g_caption), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (g_caption), 44);
  gtk_widget_set_halign (g_caption, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (g_caption, GTK_ALIGN_CENTER);
  gtk_window_set_child (GTK_WINDOW (g_caption_window), g_caption);

  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "released", G_CALLBACK (on_caption_click), nullptr);
  gtk_widget_add_controller (g_caption, GTK_EVENT_CONTROLLER (click));

  attach_chrome_hover_motion (g_caption);
}

void
caption_show_at (const RECT &preview_rect)
{
  if (g_caption_window == nullptr || g_caption == nullptr ||
      g_window == nullptr)
    return;

  int scale = gtk_widget_get_scale_factor (g_window);
  HMONITOR monitor = MonitorFromWindow (g_panel_hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{};
  mi.cbSize = sizeof (mi);
  GetMonitorInfoW (monitor, &mi);

  int mon_w_logical =
      std::max ((int)(mi.rcMonitor.right - mi.rcMonitor.left) / std::max (scale, 1),
                120);
  int preview_w_logical =
      std::max ((int)(preview_rect.right - preview_rect.left) / std::max (scale, 1),
                120);
  int max_cap_w = std::min (mon_w_logical - 16,
                            std::max (preview_w_logical + 120, 260));

  GtkRequisition natural{};
  gtk_widget_get_preferred_size (g_caption, nullptr, &natural);
  if (natural.width <= 0 || natural.height <= 0) {
    caption_hide ();
    return;
  }

  int want_def_w = std::clamp (natural.width, 1, std::max (max_cap_w, 1));
  int want_def_h = std::max (natural.height, 1);
  int cur_def_w = 0, cur_def_h = 0;
  gtk_window_get_default_size (GTK_WINDOW (g_caption_window), &cur_def_w,
                               &cur_def_h);
  if (cur_def_w != want_def_w || cur_def_h != want_def_h)
    gtk_window_set_default_size (GTK_WINDOW (g_caption_window), want_def_w,
                                 want_def_h);

  int w = want_def_w * scale;
  int h = want_def_h * scale;
  double cx = (preview_rect.left + preview_rect.right) / 2.0;
  double top = preview_rect.bottom +
               (kWindowIconSize * (1.0 - kIconOverlap) + kIconTitleSpacing) *
                   scale;

  LONG left = mi.rcMonitor.left + (LONG)(cx - w / 2.0);
  LONG top_px = mi.rcMonitor.top + (LONG)top;
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
  if (index < 0 || index >= (int)g_slots.size () ||
      !IsWindow (g_slots[index].target) ||
      (g_overlay_opacity <= 0.001 && g_overlay_to <= 0.01)) {
    overlay_hide_all ();
    return;
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
    if (anim.index >= 0 && anim.index < (int)g_slots.size ()) {
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
    if (g_scale_anim_source != 0) {
      gtk_widget_remove_tick_callback (g_window, g_scale_anim_source);
      g_scale_anim_source = 0;
    }
    // Snap any scaled preview back to its slot rect.
    for (SlotEntry &s : g_slots) {
      if (s.current_scale != 1.0) {
        apply_thumb_rect (s, s.rect, 255);
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
  g_last_hovered_slot = index;

  // Caption pill below the preview.
  gtk_label_set_text (GTK_LABEL (g_caption),
                      hwnd_title_utf8 (slot.target).c_str ());
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
  if (g_app_grid_mode) {
    set_hover (-1, true);
    return;
  }

  int scale = gtk_widget_get_scale_factor (g_window);
  int px = (int)(x * scale);
  int py = (int)(y * scale);

  for (int i = 0; i < (int)g_slots.size (); ++i) {
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
  for (SlotEntry &e : g_slots)
    if (e.thumb != nullptr)
      DwmUnregisterThumbnail (e.thumb);
  g_slots.clear ();
  g_hover_index = -1;
  g_last_hovered_slot = -1;
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
  RECT app_grid_box;           // physical App Grid container box
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

  // Workspace thumbnails (ThumbnailsBox, shown for >1 desktop or during drag).
  bool can_manage =
      vd::vtable_slots_for_build (vd::windows_build_number ()).can_manage_desktops;
  int thumbs_h = 0;  // physical
  if (!g_app_grid_mode &&
      (g_desktops.size () > 1 || (g_drag_active && !g_desktops.empty ()))) {
    int box_h = (int)(wa_h * kMaxThumbnailScale) / scale;  // logical
    overview_thumbs_repopulate (g_thumbs, (int)g_desktops.size (),
                                g_desktop_active, box_h, (double)mw / scale,
                                (double)mw, (double)wa_h, g_wallpaper,
                                can_manage, g_drag_active);
    thumbs_h = overview_thumbs_get_height (g_thumbs) * scale;
  } else {
    overview_thumbs_repopulate (g_thumbs, 0, -1, 0, 0, 0, 0, nullptr, false,
                                false);
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

  // Workspace box & App Grid box (ControlsManagerLayout):
  // - WINDOW_PICKER: thumbnails row sits between search and ws_box.
  // - APP_GRID: workspace box shrinks to SMALL_WORKSPACE_RATIO (0.15) under
  //   search, and AppGrid occupies the vertical band between ws_box and dash.
  double wa_aspect = (double)mw / (double)wa_h;
  RECT ws_box{};
  RECT app_grid_box{};
  if (g_app_grid_mode) {
    int ws_top = panel_height + search_total + spacing;
    int avail_bottom = panel_height + wa_h - dash_h - spacing;
    int max_ws_h = std::max (72, avail_bottom - ws_top - spacing - 320);
    int ws_h = std::clamp ((int)(wa_h * kSmallWorkspaceRatio + 0.5), 72,
                           max_ws_h);
    int ws_w = std::min ((int)(ws_h * wa_aspect + 0.5), mw);
    int ws_x = (mw - ws_w) / 2;
    ws_box = {ws_x, ws_top, ws_x + ws_w, ws_top + ws_h};
    app_grid_box = {0, ws_box.bottom + spacing, mw, avail_bottom};
  } else {
    int ws_h = box.bottom - box.top;
    int ws_w = (int)(ws_h * wa_aspect + 0.5);
    ws_w = std::min (ws_w, mw);
    int ws_x = (mw - ws_w) / 2;
    ws_box = {ws_x, box.top, ws_x + ws_w, box.top + ws_h};
    app_grid_box = {0, box.bottom, mw, box.bottom};
  }

  out->ws_box = ws_box;
  out->app_grid_box = app_grid_box;
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

  // .workspace-background border-radius (proportionally scaled in APP_GRID).
  g_bg_radius_target = kWorkspaceRadiusFallback;
  if (engine != nullptr) {
    StEngine::Node node (*engine, nullptr, "", "workspace-background", "");
    double v = 0;
    if (node.lookup_length ("border-radius", &v) && v > 0)
      g_bg_radius_target = v;
  }
  if (g_app_grid_mode && wa_h > 0) {
    double ratio = (double)(ws_box.bottom - ws_box.top) / (double)wa_h;
    g_bg_radius_target =
        std::clamp (g_bg_radius_target * ratio * 1.8, 10.0, 16.0);
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
  gtk_widget_set_visible (g_ws_bg, !g_app_grid_mode);
  gtk_widget_set_visible (g_dash, TRUE);
  gtk_widget_set_visible (g_app_grid, g_app_grid_mode);
}

// --- z-order ----------------------------------------------------------------------

void
apply_ex_styles (HWND hwnd)
{
  chrome_apply_base_ex_styles (hwnd);
  ensure_hwnd_topmost (hwnd);

  HWND after = (g_panel_hwnd != nullptr && IsWindow (g_panel_hwnd))
                   ? g_panel_hwnd
                   : HWND_TOPMOST;
  SetWindowPos (hwnd, after, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static void dump_zorder (const char *tag);
static void restack_locked (void);
static void switch_finish (void);

// Rebuild slots, thumbnails and widget positions for the current monitor.
// Must run after the overview window is mapped (GDK's first-map sequence
// would overwrite earlier placement).
static void
place_overview_locked (bool opening)
{
  if (g_window == nullptr || g_panel_hwnd == nullptr ||
      !gtk_widget_get_visible (g_window))
    return;

  // A plain relayout (window closed etc.) ends any in-flight workspace
  // switch instantly: the rebuilt slots are the new truth.
  if (g_switch_source != 0)
    switch_finish ();

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

  GtkWidget *chrome_wins[] = {g_chrome_window, g_icon_window,
                              g_caption_window};
  HWND chrome_hwnds[] = {g_chrome_hwnd, g_icon_hwnd, g_caption_hwnd};
  for (GtkWidget *cw : chrome_wins) {
    if (cw != nullptr && !gtk_widget_get_realized (cw))
      gtk_widget_realize (cw);
  }
  for (HWND ch : chrome_hwnds)
    chrome_bind_owner (ch);

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
  position_widgets (geo, opening && !g_app_grid_mode);

  // The dark backdrop applies only once the workspace background actually
  // covers the work area: before that, a bare #overviewGroup would flash
  // full-screen dark in the pre-placement frames.
  gtk_widget_set_name (g_root, "overviewGroup");

  if (g_app_grid_mode && g_app_grid != nullptr) {
    set_hover (-1, true);
    destroy_previews ();
    destroy_mini_previews ();
    if (g_ws_bg != nullptr)
      gtk_widget_set_visible (g_ws_bg, TRUE);

    // Keep shrunk live workspace window thumbnails inside the top bar
    // (SMALL_WORKSPACE_RATIO = 0.15 in overviewControls.js).
    std::vector<OvWindowInfo> small_windows;
    EnumContext sctx{monitor, mi.rcMonitor, &small_windows, !g_desktops.empty (),
                     false};
    EnumWindows (collect_windows, reinterpret_cast<LPARAM> (&sctx));
    RECT workarea = {0, panel_height, w, h};
    int scale = gtk_widget_get_scale_factor (g_window);
    double small_gap = (window_picker_spacing () * kSmallWorkspaceRatio + 4.0) *
                       scale;
    std::vector<OvSlot> small_slots;
    overview_compute_slots (small_windows, mi.rcMonitor, workarea, geo.ws_box,
                            small_gap, small_gap, &small_slots);
    g_slots.reserve (small_slots.size ());
    for (const OvSlot &s : small_slots) {
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
      if (e.rect.right > e.rect.left && e.rect.bottom > e.rect.top)
        g_slots.push_back (e);
    }
    create_previews (false);

    int grid_w = std::max (std::min (w / scale - 48, 1320), 400);
    int grid_h = std::max (
        (int)(geo.app_grid_box.bottom - geo.app_grid_box.top) / scale, 260);
    int grid_x = (w / scale - grid_w) / 2;
    int grid_y = (int)geo.app_grid_box.top / scale;
    app_grid_set_query (g_app_grid, g_search_query);
    app_grid_rebuild (g_app_grid, grid_w, grid_h);
    gtk_widget_set_size_request (g_app_grid, grid_w, grid_h);
    gtk_fixed_move (GTK_FIXED (g_root), g_app_grid, grid_x, grid_y);
    gtk_widget_set_visible (g_app_grid, TRUE);
    restack_locked ();
    return;
  }

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
      std::max ({close_button_size () / 2.0,
                 kWindowIconSize * (1.0 - kIconOverlap)}) +
      kWindowActiveSizeInc;
  double spacing = (window_picker_spacing () + chrome_oversize) * scale;

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
    set_hover (slot_at_cursor ());
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
// instead of re-sending SWP_FRAMECHANGED SetWindowPos calls, each of which
// forces a WM_NCCALCSIZE round-trip and reshuffles the topmost band.
static bool
zorder_drifted (void)
{
  HWND chain[6];
  int n = 0;
  HWND popover = static_cast<HWND> (winome_shell_panel_top_popover_hwnd ());
  if (popover != nullptr)
    chain[n++] = popover;
  chain[n++] = g_panel_hwnd;
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

  if (popover == nullptr)
    ensure_hwnd_topmost (g_panel_hwnd);
  if (!SetWindowPos (g_panel_hwnd, popover != nullptr ? popover : HWND_TOPMOST,
                     0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                         SWP_FRAMECHANGED))
    g_print ("[restack] panel failed gle=%lu\n", GetLastError ());

  // The chrome windows sit directly below the panel (above the overview,
  // hence above the DWM thumbnails): close button, app icon, caption pill.
  HWND below_panel = g_panel_hwnd;
  HWND layers[] = {g_chrome_hwnd, g_icon_hwnd, g_caption_hwnd, g_overview_hwnd};
  for (HWND h : layers) {
    if (!is_visible_hwnd (h))
      continue;
    ensure_hwnd_topmost (h);
    if (!SetWindowPos (h, below_panel, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                           SWP_FRAMECHANGED)) {
      if (h == g_overview_hwnd)
        g_print ("[restack] overview failed gle=%lu\n", GetLastError ());
    }
    below_panel = h;
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
    set_hover (slot_at_cursor ());
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
  if (g_switch_source != 0) {
    gtk_widget_remove_tick_callback (g_window, g_switch_source);
    g_switch_source = 0;
  }
  for (SlotEntry &l : g_leaving)
    if (l.thumb != nullptr)
      DwmUnregisterThumbnail (l.thumb);
  g_leaving.clear ();
}

// Snap-finish an in-flight switch with the overview still on screen.
static void
switch_finish (void)
{
  bool was_running = g_switch_source != 0 || !g_leaving.empty ();
  switch_cancel ();
  if (!was_running || g_window == nullptr || !gtk_widget_get_visible (g_window))
    return;
  apply_bg_rect (g_bg_final);
  overview_thumbs_set_indicator_value (g_thumbs, -1.0);
  for (SlotEntry &s : g_slots)
    apply_thumb_rect (s, s.rect, 255);
}

static gboolean
switch_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
  (void)widget;
  (void)user_data;
  double t = (gdk_frame_clock_get_frame_time (clock) - g_switch_start_us) /
             1000.0 / (double)kSwitchMs;
  if (t < 0.0)
    t = 0.0;
  if (t > 1.0)
    t = 1.0;
  double e = ease_out_cubic (t);

  // The strip slides towards the target: the new desktop slides in from the
  // direction of its index, the previous one slides out the other way, the
  // workspace background box travels along, and the thumbnail indicator
  // rides the same eased value.
  int dir = (g_switch_to > g_switch_from) ? 1 : -1;
  int width = g_switch_width > 0 ? g_switch_width
                                  : (g_ws_box.right - g_ws_box.left);
  LONG in_off = (LONG)(dir * (1.0 - e) * width);
  LONG out_off = (LONG)(-dir * e * width);

  for (SlotEntry &s : g_slots)
    apply_thumb_rect (s, translate_rect_x (s.rect, in_off), 255);
  for (SlotEntry &l : g_leaving)
    apply_thumb_rect (l, translate_rect_x (l.rect, out_off), 255);
  apply_bg_rect (translate_rect_x (g_bg_final, in_off));
  overview_thumbs_set_indicator_value (
      g_thumbs, g_switch_from + (g_switch_to - g_switch_from) * e);

  if (t >= 1.0) {
    switch_finish ();
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

// Animated workspace switch (WorkspacesView._scrollToActive: the previews of
// the previous desktop slide out while the new desktop's slide in, 250ms
// EASE_OUT_CUBIC). Falls back to the plain relayout when the relayout did
// not land on @to.
static void
switch_workspace_animated (int to)
{
  if (g_window == nullptr || !gtk_widget_get_visible (g_window))
    return;
  if (g_desktops.empty () || to < 0 || to >= (int)g_desktops.size ())
    return;
  if (g_anim.source != 0)
    return;  // open/close animation in flight; the next poll relayouts
  if (to == g_desktop_active)
    return;

  // Finish a previous in-flight switch instantly (rapid pill clicks).
  switch_finish ();

  int from = g_desktop_active;

  // Hand the current previews (with their live DWM thumbnails) to the
  // outgoing group; they keep rendering while sliding out.
  for (SlotEntry &e : g_slots)
    g_leaving.push_back (std::move (e));
  g_slots.clear ();
  g_hover_index = -1;
  g_last_hovered_slot = -1;

  // Rebuild for the new desktop; the first tick offsets everything to the
  // slide-in start so no final-position frame flashes. An empty slot list
  // is a valid result (switching to an empty desktop).
  place_overview_locked (false);
  if (to != g_desktop_active) {
    for (SlotEntry &l : g_leaving)
      if (l.thumb != nullptr)
        DwmUnregisterThumbnail (l.thumb);
    g_leaving.clear ();
    return;
  }

  g_switch_from = from;
  g_switch_to = to;
  g_switch_width = g_ws_box.right - g_ws_box.left;
  g_switch_start_us = g_get_monotonic_time ();
  g_switch_source =
      gtk_widget_add_tick_callback (g_window, switch_tick, nullptr, nullptr);
  g_print ("[ov] workspace switch %d -> %d (animated)\n", from, to);
}

void
start_animation (bool closing, guint duration_ms)
{
  if (g_switch_source != 0)
    switch_finish ();
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
  switch_cancel ();
  destroy_mini_previews ();
  g_drag_active = false;
  g_drag_slot = -1;
  g_drag_hwnd = nullptr;
  g_drag_just_ended = false;
  g_drag_thumbs_revealed = false;
  if (g_thumbs != nullptr)
    overview_thumbs_set_drop_target (g_thumbs, -1);
  if (g_dash != nullptr) {
    overview_dash_close_menu (g_dash);
    overview_dash_set_apps_active (g_dash, false);
  }
  if (g_app_grid != nullptr) {
    app_grid_close_menu (g_app_grid);
    app_grid_reset_page (g_app_grid);
    tween_opacity (g_app_grid, 1.0, 1.0, 0, Ease::linear);
    gtk_widget_set_opacity (g_app_grid, 1.0);
    gtk_widget_set_visible (g_app_grid, FALSE);
  }
  g_app_grid_mode = false;
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
  if (g_overview_hwnd != nullptr && IsWindow (g_overview_hwnd))
    apply_ex_styles (g_overview_hwnd);
  if (g_window != nullptr && gtk_widget_get_visible (g_window))
    gtk_widget_set_visible (g_window, FALSE);
}

gboolean
delayed_rebuild_cb (gpointer user_data G_GNUC_UNUSED)
{
  rebuild_now ();
  return G_SOURCE_REMOVE;
}

void
on_drag_begin (GtkGestureDrag *drag, double x, double y,
               gpointer user_data G_GNUC_UNUSED)
{
  g_drag_slot = -1;
  g_drag_hwnd = nullptr;
  g_drag_active = false;
  g_drag_thumbs_revealed = false;

  if (g_app_grid_mode || g_desktops.empty () || g_window == nullptr) {
    gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_DENIED);
    return;
  }

  int scale = gtk_widget_get_scale_factor (g_window);
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

  for (int i = 0; i < (int)g_slots.size (); ++i) {
    const RECT r = slot_display_rect (i);
    if (px >= r.left && px < r.right && py >= r.top && py < r.bottom) {
      g_drag_slot = i;
      g_drag_hwnd = g_slots[i].target;
      g_drag_orig_rect = g_slots[i].rect;
      g_drag_start_px = px;
      g_drag_start_py = py;
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

  if (!g_drag_active) {
    if (dx * dx + dy * dy < 64.0)
      return;
    g_drag_active = true;
    set_hover (-1, true);

    int scale = gtk_widget_get_scale_factor (g_window);
    bool can_manage =
        vd::vtable_slots_for_build (vd::windows_build_number ())
            .can_manage_desktops;
    if (g_desktops.size () == 1 && can_manage && g_thumbs != nullptr) {
      int mw = g_monitor_w;
      int wa_h = std::max (g_monitor_h - g_panel_height, 1);
      int spacing = (int)(wa_h * kVerticalSpacingRatio + 0.5);
      int thumb_adjust = (int)(spacing * kThumbnailsAdjustTop + 0.5);
      GtkRequisition search_nat{};
      gtk_widget_get_preferred_size (g_search, nullptr, &search_nat);
      int search_total =
          (kSearchMarginTop + kSearchMarginBottom + search_nat.height) * scale;
      int box_h = (int)(wa_h * kMaxThumbnailScale) / scale;
      overview_thumbs_repopulate (g_thumbs, 1, 0, box_h, (double)mw / scale,
                                  (double)mw, (double)wa_h, g_wallpaper, true,
                                  true);
      int thumbs_w = overview_thumbs_get_width (g_thumbs);
      int thumbs_h = overview_thumbs_get_height (g_thumbs) * scale;
      int thumbs_x = (mw / scale - thumbs_w) / 2;
      int thumbs_y = (g_panel_height + search_total + thumb_adjust) / scale;
      gtk_fixed_move (GTK_FIXED (g_root), g_thumbs, thumbs_x, thumbs_y);
      gtk_widget_set_visible (g_thumbs, thumbs_h > 0);
      if (thumbs_h > 0) {
        g_thumbs_rect.left = thumbs_x * scale;
        g_thumbs_rect.top = thumbs_y * scale;
        g_thumbs_rect.right = (thumbs_x + thumbs_w) * scale;
        g_thumbs_rect.bottom = (thumbs_y * scale) + thumbs_h;
        g_drag_thumbs_revealed = true;
      }
    }
  }

  int scale = gtk_widget_get_scale_factor (g_window);
  int orig_w = g_drag_orig_rect.right - g_drag_orig_rect.left;
  int orig_h = g_drag_orig_rect.bottom - g_drag_orig_rect.top;
  int dw = std::max ((int)(orig_w * 0.55), 80 * scale);
  int dh = std::max ((int)(orig_h * 0.55), 60 * scale);
  int cur_px = g_drag_start_px + (int)(dx * scale);
  int cur_py = g_drag_start_py + (int)(dy * scale);
  RECT moved{cur_px - dw / 2, cur_py - dh / 2, cur_px + dw / 2,
             cur_py + dh / 2};
  apply_thumb_rect (g_slots[g_drag_slot], moved, 215);

  if (g_thumbs != nullptr && g_thumbs_rect.right > g_thumbs_rect.left &&
      cur_px >= g_thumbs_rect.left && cur_px < g_thumbs_rect.right &&
      cur_py >= g_thumbs_rect.top && cur_py < g_thumbs_rect.bottom) {
    double lx = (double)(cur_px - g_thumbs_rect.left) / scale;
    double ly = (double)(cur_py - g_thumbs_rect.top) / scale;
    overview_thumbs_set_drop_target (
        g_thumbs, overview_thumbs_drop_target_at (g_thumbs, lx, ly));
  } else if (g_thumbs != nullptr) {
    overview_thumbs_set_drop_target (g_thumbs, -1);
  }
}

void
on_drag_end (GtkGestureDrag *drag G_GNUC_UNUSED, double dx, double dy,
             gpointer user_data G_GNUC_UNUSED)
{
  if (g_drag_slot < 0)
    return;

  bool was_drag = g_drag_active;
  HWND target_hwnd = g_drag_hwnd;
  int scale = g_window != nullptr ? gtk_widget_get_scale_factor (g_window) : 1;
  int end_px = g_drag_start_px + (int)(dx * scale);
  int end_py = g_drag_start_py + (int)(dy * scale);

  g_drag_active = false;
  g_drag_slot = -1;
  g_drag_hwnd = nullptr;

  if (!was_drag)
    return;

  g_drag_just_ended = true;

  int tgt = -1;
  if (g_thumbs != nullptr && g_thumbs_rect.right > g_thumbs_rect.left &&
      end_px >= g_thumbs_rect.left && end_px < g_thumbs_rect.right &&
      end_py >= g_thumbs_rect.top && end_py < g_thumbs_rect.bottom) {
    double lx = (double)(end_px - g_thumbs_rect.left) / scale;
    double ly = (double)(end_py - g_thumbs_rect.top) / scale;
    tgt = overview_thumbs_drop_target_at (g_thumbs, lx, ly);
  }
  if (g_thumbs != nullptr)
    overview_thumbs_set_drop_target (g_thumbs, -1);

  if (tgt >= 0 && target_hwnd != nullptr && IsWindow (target_hwnd)) {
    if (tgt < (int)g_desktops.size () && tgt != g_desktop_active) {
      vd::move_window_to_desktop (target_hwnd, tgt);
    } else if (tgt == (int)g_desktops.size () && vd::create_desktop ()) {
      int n = vd::count ();
      if (n > 0)
        vd::move_window_to_desktop (target_hwnd, n - 1);
    }
  }
  g_drag_thumbs_revealed = false;
  rebuild_now ();
}

// --- clicks --------------------------------------------------------------------------

static void
focus_overview_for_search (void)
{
  if (g_overview_hwnd != nullptr && IsWindow (g_overview_hwnd)) {
    LONG_PTR ex = GetWindowLongPtrW (g_overview_hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_NOACTIVATE)
      SetWindowLongPtrW (g_overview_hwnd, GWL_EXSTYLE, ex & ~WS_EX_NOACTIVATE);
    SetForegroundWindow (g_overview_hwnd);
    restack_locked ();
  }
  gtk_widget_grab_focus (g_search);
}

void
on_click_released (GtkGestureClick *gesture, int n_press, double x, double y,
                   gpointer user_data)
{
  (void)n_press;
  (void)user_data;
  guint button =
      gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));

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
  if (winome_shell_panel_popover_just_closed ()) {
    g_print ("[ov] click (%d,%d) swallowed (popover dismiss)\n", px, py);
    return;
  }

  if (g_panel_height > 0 && py < g_panel_height) {
    g_print ("[ov] click (%d,%d) ignored (panel strip)\n", px, py);
    return;
  }

  if (g_app_grid_mode) {
    POINT pt = {px, py};
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

  if (button == GDK_BUTTON_MIDDLE) {
    for (const SlotEntry &e : g_slots) {
      if (pt.x >= e.rect.left && pt.x < e.rect.right && pt.y >= e.rect.top &&
          pt.y < e.rect.bottom) {
        if (IsWindow (e.target)) {
          PostMessageW (e.target, WM_CLOSE, 0, 0);
          g_timeout_add (140, delayed_rebuild_cb, nullptr);
        }
        return;
      }
    }
    return;
  }

  // Inside a preview slot: restore/focus that window and close.
  for (int i = 0; i < (int)g_slots.size (); ++i) {
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
    switch_workspace_animated (index);
  } else {
    g_print ("[ov] desktop switch failed\n");
  }
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
  close_overview ("app-grid-launch");
}

void
on_app_grid_pins_changed (void *user_data G_GNUC_UNUSED)
{
  if (g_dash != nullptr && g_window != nullptr &&
      gtk_widget_get_visible (g_window)) {
    rebuild_now ();
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
    switch_workspace_animated (cur);
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

void
on_search_changed (GtkEditable *editable, gpointer user_data G_GNUC_UNUSED)
{
  const char *text = gtk_editable_get_text (editable);
  std::string next = utf8_tolower (text != nullptr ? text : "");
  if (next == g_search_query)
    return;
  g_search_query = std::move (next);
  gtk_entry_set_icon_from_icon_name (GTK_ENTRY (g_search),
                                     GTK_ENTRY_ICON_SECONDARY,
                                     g_search_query.empty () ? nullptr
                                                             : "edit-clear-symbolic");
  if (g_app_grid != nullptr)
    app_grid_set_query (g_app_grid, g_search_query);
  if (g_window != nullptr && gtk_widget_get_visible (g_window))
    rebuild_now ();
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
  if (g_app_grid_mode && g_app_grid != nullptr) {
    if (app_grid_launch_first (g_app_grid))
      close_overview ("app-grid-enter");
    return;
  }
  if (!g_slots.empty ()) {
    HWND target = g_slots[0].target;
    if (IsWindow (target)) {
      if (IsIconic (target))
        ShowWindow (target, SW_RESTORE);
      SetForegroundWindow (target);
    }
    close_overview ("search-enter-window");
    return;
  }
  if (!g_search_query.empty () &&
      app_index_launch_best_match (g_search_query)) {
    close_overview ("search-enter-app");
  }
}

gboolean
on_overview_key_pressed (GtkEventControllerKey *controller G_GNUC_UNUSED,
                         guint keyval,
                         guint keycode G_GNUC_UNUSED,
                         GdkModifierType state,
                         gpointer user_data G_GNUC_UNUSED)
{
  if (keyval == GDK_KEY_Escape) {
    if (!g_search_query.empty ()) {
      gtk_editable_set_text (GTK_EDITABLE (g_search), "");
      return GDK_EVENT_STOP;
    }
    if (g_app_grid_mode) {
      overview_toggle_app_grid ();
      return GDK_EVENT_STOP;
    }
    close_overview ("escape");
    return GDK_EVENT_STOP;
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
  return GDK_EVENT_PROPAGATE;
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
      winome::tr ("Type to search", "输入以搜索"));
  g_signal_connect (g_search, "changed", G_CALLBACK (on_search_changed),
                    nullptr);
  g_signal_connect (g_search, "icon-press", G_CALLBACK (on_search_icon_press),
                    nullptr);
  g_signal_connect (g_search, "activate", G_CALLBACK (on_search_activate),
                    nullptr);
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
  app_index_refresh_async ();

  g_app_grid = app_grid_new (on_app_grid_launch_done,
                             on_app_grid_pins_changed, nullptr);
  gtk_fixed_put (GTK_FIXED (g_root), g_app_grid, 0, 0);
  gtk_widget_set_visible (g_app_grid, FALSE);

  g_dash = overview_dash_new (g_root);
  overview_dash_set_show_apps_cb (g_dash, on_dash_show_apps_clicked, nullptr);
  overview_dash_set_changed_cb (g_dash, on_dash_changed, nullptr);
  gtk_fixed_put (GTK_FIXED (g_root), g_dash, 0, 0);
  gtk_widget_set_visible (g_dash, FALSE);

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

  // .workspace-background — wallpaper in an animated rounded clip (+ CSS
  // shadow), on the workarea-aspect workspace box.
  g_ws_bg = (GtkWidget *)g_object_new (winome_workspace_bg_get_type (),
                                       nullptr);
  gtk_widget_add_css_class (g_ws_bg, "workspace-background");
  gtk_fixed_put (GTK_FIXED (g_root), g_ws_bg, 0, 0);
  gtk_widget_set_visible (g_ws_bg, FALSE);

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
}

void
overview_show (void)
{
  if (g_window == nullptr || g_panel_hwnd == nullptr)
    return;

  if (!g_search_query.empty () && g_search != nullptr) {
    g_search_query.clear ();
    gtk_editable_set_text (GTK_EDITABLE (g_search), "");
  }

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
    switch_cancel ();
    g_app_grid_mode = false;
    return;
  }
  if (g_app_grid_mode) {
    finish_hide ();
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
  g_main_context_invoke (nullptr, show_idle, nullptr);
}

void
overview_hide_async (void)
{
  g_main_context_invoke (nullptr, hide_idle, nullptr);
}

void
overview_invalidate_wallpaper (void)
{
  load_wallpaper ();
  if (g_window != nullptr && gtk_widget_get_visible (g_window))
    gtk_widget_queue_draw (g_window);
}

void
overview_reposition (void)
{
  if (g_window != nullptr && gtk_widget_get_visible (g_window))
    place_overview_locked (false);
}

void
overview_toggle_app_grid (void)
{
  if (g_window == nullptr || !gtk_widget_get_visible (g_window))
    return;
  g_app_grid_mode = !g_app_grid_mode;
  if (g_dash != nullptr)
    overview_dash_set_apps_active (g_dash, g_app_grid_mode);
  if (g_app_grid_mode && g_app_grid != nullptr)
    app_grid_reset_page (g_app_grid);
  place_overview_locked (false);
  if (g_app_grid != nullptr) {
    if (g_app_grid_mode) {
      gtk_widget_set_opacity (g_app_grid, 0.0);
      tween_opacity (g_app_grid, 0.0, 1.0, 220, Ease::ease_out_cubic);
    } else {
      tween_opacity (g_app_grid, 1.0, 1.0, 0, Ease::linear);
      gtk_widget_set_opacity (g_app_grid, 1.0);
    }
  }
}

}  // namespace winome
