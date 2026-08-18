// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Activities overview rendered in the host process: a faithful port of the
// GNOME Shell overviewControls.js WINDOW_PICKER state.
//
//   #overviewGroup (root fixed)      background-color: #222226
//     .search-entry (GtkSearchEntry) top center, width 24em, margins 12/6
//     .workspace-background          picker box: wallpaper in a 30px-radius
//                                    rounded clip + CSS box-shadow
//     #dash (overview-dash.cpp)      bottom center
//     .window-caption (GtkLabel)     hover title pill below a preview
//
// Window previews are DWM live thumbnails on the TOP-LEVEL overview window
// (DwmRegisterThumbnail requires a top-level destination), laid out by the
// ported workspace.js strategy (overview-layout.cpp) inside the picker box
// computed from ControlsManagerLayout.vfunc_allocate():
//
//   spacing      = round(workArea.height * 0.02)
//   box.top      = panel + searchHeight + round(spacing * 0.6)
//   box.height   = waH - dashHeight - spacing - searchHeight - round(sp*0.6)
//   dash.bottom  = panel + waH, dash height <= round(waH * 0.16)
//
// Because DWM composites thumbnails ABOVE everything the window paints, the
// per-preview close button lives in a tiny separate "chrome" window (a
// .window-close circle) shown on hover at the preview's top-right corner;
// everything else (dash, search, captions) sits outside thumbnail rects in
// the overview window itself.
//
// Interactions: click a preview restores/focuses that window and closes the
// overview; the close button sends WM_CLOSE and relayouts; a click elsewhere
// closes. Esc / a second Win press close it through overview-trigger.cpp.

#include "overview.h"
#include "overview-layout.h"
#include "overview-dash.h"
#include "overview-trigger.h"
#include "shell-panel.h"
#include "st-engine.h"
#include "system-status.h"

#include <gtk/gtk.h>
#include <gdk/win32/gdkwin32.h>
#include <dwmapi.h>

#include <windows.h>

#include <string>
#include <vector>

namespace winome {

namespace {

// --- theme constants (compiled gnome-shell-dark.css; StEngine-resolved) ----

constexpr int kSearchMarginTop = 12;     // .search-entry margin-top
constexpr int kSearchMarginBottom = 6;   // .search-entry margin-bottom
constexpr double kWorkspaceRadiusFallback = 30.0;
constexpr int kCaptionOffset = 12;       // window-caption below the preview
constexpr int kCloseButtonSize = 32;     // .window-close width/height
constexpr int kCloseIconSize = 24;       // .window-close StIcon icon-size

// overviewControls.js ratios.
constexpr double kVerticalSpacingRatio = 0.02;
constexpr double kThumbnailsAdjustTop = 0.6;
constexpr double kDashMaxHeightRatio = 0.16;

// Animation (Overview.ANIMATION_TIME / SIDE_CONTROLS_ANIMATION_TIME).
constexpr guint kOpenMs = 250;
constexpr guint kCloseMs = 200;

HWND g_panel_hwnd = nullptr;
GtkWidget *g_window = nullptr;
HWND g_overview_hwnd = nullptr;
int g_panel_height = 0;   // physical px; clicks above it are panel's

// UI pieces.
GtkWidget *g_root = nullptr;      // GtkFixed named "overviewGroup"
GtkWidget *g_search = nullptr;    // GtkSearchEntry.search-entry
GtkWidget *g_ws_bg = nullptr;     // workspace-background widget
GtkWidget *g_dash = nullptr;      // overview dash
GtkWidget *g_caption = nullptr;   // .window-caption hover pill
GdkTexture *g_wallpaper = nullptr;

// Close-button chrome window (separate toplevel, above the thumbnails).
GtkWidget *g_chrome_window = nullptr;
GtkWidget *g_chrome_button = nullptr;
HWND g_chrome_hwnd = nullptr;
HWND g_chrome_target = nullptr;
RECT g_chrome_want = {0, 0, 0, 0};  // desired screen rect while shown

// One preview slot.
struct SlotEntry {
  RECT start;      // real window rect (client space) for the fly animation
  RECT rect;       // slot rect (client space)
  RECT current;    // animated rect currently set on the thumbnail
  HWND target;
  HTHUMBNAIL thumb;
};
std::vector<SlotEntry> g_slots;

int g_hover_index = -1;

// Layout (physical px, overview client space).
int g_monitor_w = 0, g_monitor_h = 0;
int g_dash_top = 0;             // top of the dash band (click exclusion)
RECT g_search_rect = {0, 0, 0, 0}; // physical; clicks here belong to the entry

struct Animation {
  guint source = 0;
  bool closing = false;
  gint64 start_us = 0;
  guint duration_ms = 0;
};
Animation g_anim;

// Forward declarations (defined below in dependency order).
static gboolean overview_place_idle (gpointer user_data);
void set_hover (int index);
void rebuild_now (void);
void start_open_animation (void);
void finish_hide (void);

// --- helpers -----------------------------------------------------------------

double
ease_out_quad (double t)
{
  return 1.0 - (1.0 - t) * (1.0 - t);
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
};

BOOL CALLBACK
collect_windows (HWND hwnd, LPARAM lparam)
{
  EnumContext *ctx = reinterpret_cast<EnumContext *> (lparam);

  if (is_our_process (hwnd) || is_cloaked (hwnd) || !is_alt_tab_window (hwnd))
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
  // clipped to the 30px rounded rect on top.
  GTK_WIDGET_CLASS (winome_workspace_bg_parent_class)
      ->snapshot (widget, snapshot);

  int w = gtk_widget_get_width (widget);
  int h = gtk_widget_get_height (widget);
  if (w <= 0 || h <= 0)
    return;

  double radius = kWorkspaceRadiusFallback;
  StEngine *engine = StEngine::get ();
  if (engine != nullptr) {
    StEngine::Node node (*engine, nullptr, "", "workspace-background", "");
    double v = 0;
    if (node.lookup_length ("border-radius", &v) && v > 0)
      radius = v;
  }

  graphene_rect_t rect;
  graphene_rect_init (&rect, 0, 0, w, h);
  GskRoundedRect clip;
  gsk_rounded_rect_init_from_rect (&clip, &rect, (float)radius);
  gtk_snapshot_push_rounded_clip (snapshot, &clip);

  if (g_wallpaper != nullptr) {
    gtk_snapshot_append_texture (snapshot, g_wallpaper, &rect);
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
  chrome_hide ();
  set_hover (-1);
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

  // .window-close: 32x32 circle, window-close-symbolic at 24px.
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

// Show the close button centered on the preview's top-right corner.
void
chrome_show_at (const RECT &slot_rect)
{
  if (g_chrome_window == nullptr)
    return;

  int scale = gtk_widget_get_scale_factor (g_window);
  HMONITOR monitor = MonitorFromWindow (g_panel_hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {sizeof (mi)};
  GetMonitorInfoW (monitor, &mi);

  int size = kCloseButtonSize * scale;
  g_chrome_want.left = mi.rcMonitor.left + slot_rect.right - size / 2;
  g_chrome_want.top = mi.rcMonitor.top + slot_rect.top - size / 2;
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

// --- hover (caption + chrome) ---------------------------------------------------

void
set_hover (int index)
{
  if (index == g_hover_index)
    return;
  g_hover_index = index;

  if (index < 0 || index >= (int)g_slots.size ()) {
    gtk_widget_set_visible (g_caption, FALSE);
    chrome_hide ();
    return;
  }

  const SlotEntry &slot = g_slots[index];
  if (!IsWindow (slot.target)) {
    gtk_widget_set_visible (g_caption, FALSE);
    chrome_hide ();
    return;
  }

  // Caption pill below the preview.
  gtk_label_set_text (GTK_LABEL (g_caption),
                      hwnd_title_utf8 (slot.target).c_str ());
  GtkRequisition natural;
  gtk_widget_get_preferred_size (g_caption, nullptr, &natural);

  int scale = gtk_widget_get_scale_factor (g_window);
  double cx = (slot.rect.left + slot.rect.right) / 2.0;
  double x = cx / scale - natural.width / 2.0;
  double y = (slot.rect.bottom + kCaptionOffset) / scale;
  gtk_fixed_move (GTK_FIXED (g_root), g_caption, (int)x, (int)y);
  gtk_widget_set_visible (g_caption, TRUE);

  // Close button at the corner.
  g_chrome_target = slot.target;
  chrome_show_at (slot.rect);
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
  // Keep the hover alive while the pointer is over the close-button window
  // (it took the input; we would otherwise see a spurious leave).
  if (g_chrome_hwnd != nullptr && IsWindowVisible (g_chrome_hwnd)) {
    POINT pt;
    if (GetCursorPos (&pt) && ScreenToClient (g_chrome_hwnd, &pt) &&
        pt.x >= 0 && pt.y >= 0 &&
        pt.x < kCloseButtonSize * gtk_widget_get_scale_factor (g_window) &&
        pt.y < kCloseButtonSize * gtk_widget_get_scale_factor (g_window))
      return;
  }
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
}

void
apply_thumb_rect (SlotEntry &e, const RECT &rect)
{
  if (e.thumb == nullptr)
    return;
  DWM_THUMBNAIL_PROPERTIES props = {};
  props.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_RECTDESTINATION |
                  DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
  props.fVisible = TRUE;
  props.opacity = 255;
  props.rcDestination = rect;
  props.fSourceClientAreaOnly = FALSE;
  if (FAILED (DwmUpdateThumbnailProperties (e.thumb, &props)))
    e.current = rect;
  else
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
      apply_thumb_rect (e, at_start_rects ? e.start : e.rect);
    } else {
      e.thumb = nullptr;
    }
  }
}

// --- layout ----------------------------------------------------------------------

struct PlacedGeometry {
  int search_x, search_y;      // logical
  int search_w, search_h;      // logical
  int bg_x, bg_y, bg_w, bg_h;  // logical
  int dash_x, dash_y;          // logical
  RECT box;                    // physical picker box
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

  // Dash (16% cap, ControlsManagerLayout DASH_MAX_HEIGHT_RATIO).
  int dash_max = (int)(wa_h * kDashMaxHeightRatio + 0.5);
  overview_dash_repopulate (g_dash, monitor, dash_max / scale);
  int dash_w = overview_dash_get_width (g_dash) * scale;
  int dash_h = std::min (overview_dash_get_height (g_dash) * scale, dash_max);

  // Picker box.
  RECT box;
  box.left = 0;
  box.top = panel_height + search_total + thumb_adjust;
  box.right = mw;
  box.bottom = panel_height + wa_h - dash_h - spacing;

  out->box = box;
  out->search_w = search_w;
  out->search_h = search_h;
  out->search_x = (mw / scale - search_w) / 2;
  out->search_y = (panel_height + kSearchMarginTop * scale) / scale;
  out->bg_x = 0;
  out->bg_y = box.top / scale;
  out->bg_w = mw / scale;
  out->bg_h = (box.bottom - box.top) / scale;
  out->dash_x = (mw - dash_w) / 2 / scale;
  out->dash_y = (panel_height + wa_h - dash_h) / scale;

  g_monitor_w = mw;
  g_monitor_h = mh;
  g_dash_top = panel_height + wa_h - dash_h;
  return true;
}

void
position_widgets (const PlacedGeometry &geo)
{
  gtk_widget_set_size_request (g_search, geo.search_w, geo.search_h);
  gtk_fixed_move (GTK_FIXED (g_root), g_search, geo.search_x, geo.search_y);

  gtk_widget_set_size_request (g_ws_bg, geo.bg_w, geo.bg_h);
  gtk_fixed_move (GTK_FIXED (g_root), g_ws_bg, geo.bg_x, geo.bg_y);

  gtk_fixed_move (GTK_FIXED (g_root), g_dash, geo.dash_x, geo.dash_y);

  // Search band in PHYSICAL px (click exclusion compares physical coords).
  int scale = gtk_widget_get_scale_factor (g_window);
  g_search_rect.left = geo.search_x * scale;
  g_search_rect.top = geo.search_y * scale;
  g_search_rect.right = (geo.search_x + geo.search_w) * scale;
  g_search_rect.bottom = (geo.search_y + geo.search_h) * scale;

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
static gboolean
overview_place_idle (gpointer user_data)
{
  (void)user_data;
  if (g_window == nullptr || g_panel_hwnd == nullptr ||
      !gtk_widget_get_visible (g_window))
    return G_SOURCE_REMOVE;

  HMONITOR monitor = MonitorFromWindow (g_panel_hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {sizeof (mi)};
  if (!GetMonitorInfoW (monitor, &mi))
    return G_SOURCE_REMOVE;

  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (g_window));
  if (surface == nullptr)
    return G_SOURCE_REMOVE;
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

  // Geometry (ControlsManagerLayout, WINDOW_PICKER, no thumbnails box).
  PlacedGeometry geo;
  if (!compute_geometry (monitor, mi.rcMonitor, panel_height, &geo))
    return G_SOURCE_REMOVE;
  position_widgets (geo);

  // Windows -> slots (workspace.js strategy; scale vs work area, slots vs
  // picker box).
  std::vector<OvWindowInfo> windows;
  EnumContext ctx{monitor, mi.rcMonitor, &windows};
  EnumWindows (collect_windows, reinterpret_cast<LPARAM> (&ctx));

  RECT workarea = {0, panel_height, w, h};
  std::vector<OvSlot> slots;
  overview_compute_slots (windows, mi.rcMonitor, workarea, geo.box, &slots);

  set_hover (-1);
  destroy_previews ();
  g_slots.reserve (slots.size ());
  for (const OvSlot &s : slots) {
    SlotEntry e;
    e.target = s.window.hwnd;
    e.start = s.window.rect;
    int sw = (int)((s.window.rect.right - s.window.rect.left) * s.scale);
    int sh = (int)((s.window.rect.bottom - s.window.rect.top) * s.scale);
    e.rect.left = (int)s.x;
    e.rect.top = (int)s.y;
    e.rect.right = e.rect.left + sw;
    e.rect.bottom = e.rect.top + sh;
    if (e.rect.right > e.rect.left && e.rect.bottom > e.rect.top)
      g_slots.push_back (e);
  }

  // Thumbnails start at the windows' real positions and fly into the slots.
  create_previews (true);
  restack_locked ();
  start_open_animation ();

  g_print ("[ov] placed %ldx%ld slots=%zu dash=%d\n", w, h, g_slots.size (),
           overview_dash_get_width (g_dash));
  dump_zorder ("placed");
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
    if (wnd == g_chrome_hwnd)
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

// Re-assert popover > panel > chrome > overview > apps.
static void
restack_locked (void)
{
  if (g_panel_hwnd == nullptr || !IsWindow (g_panel_hwnd))
    return;

  HWND popover = static_cast<HWND> (winome_shell_panel_top_popover_hwnd ());

  if (!SetWindowPos (g_panel_hwnd, popover != nullptr ? popover : HWND_TOPMOST,
                     0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                         SWP_FRAMECHANGED))
    g_print ("[restack] panel failed gle=%lu\n", GetLastError ());

  // The close-button window sits directly below the panel (above the
  // overview, hence above the DWM thumbnails).
  HWND below_panel = g_panel_hwnd;
  if (g_chrome_hwnd != nullptr && IsWindow (g_chrome_hwnd) &&
      IsWindowVisible (g_chrome_hwnd)) {
    SetWindowPos (g_chrome_hwnd, below_panel, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                      SWP_FRAMECHANGED);
    below_panel = g_chrome_hwnd;
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
anim_tick (gpointer user_data)
{
  (void)user_data;
  Animation &anim = g_anim;

  gint64 now = g_get_monotonic_time ();
  double t = (now - anim.start_us) / 1000.0 / (double)anim.duration_ms;
  if (t > 1.0)
    t = 1.0;

  double e = ease_out_quad (t);

  if (anim.closing) {
    // Fly back out: slot -> real window rect, controls fade away.
    double k = 1.0 - e;
    for (SlotEntry &s : g_slots) {
      RECT r;
      r.left = (LONG)(s.rect.left + (s.start.left - s.rect.left) * k);
      r.top = (LONG)(s.rect.top + (s.start.top - s.rect.top) * k);
      r.right = (LONG)(s.rect.right + (s.start.right - s.rect.right) * k);
      r.bottom = (LONG)(s.rect.bottom + (s.start.bottom - s.rect.bottom) * k);
      apply_thumb_rect (s, r);
    }
    gtk_widget_set_opacity (g_search, k);
    gtk_widget_set_opacity (g_dash, k);

    if (t >= 1.0) {
      anim.source = 0;
      finish_hide ();
      return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
  }

  // Opening: real window rect -> slot, controls fade in.
  for (SlotEntry &s : g_slots) {
    RECT r;
    r.left = (LONG)(s.start.left + (s.rect.left - s.start.left) * e);
    r.top = (LONG)(s.start.top + (s.rect.top - s.start.top) * e);
    r.right = (LONG)(s.start.right + (s.rect.right - s.start.right) * e);
    r.bottom = (LONG)(s.start.bottom + (s.rect.bottom - s.start.bottom) * e);
    apply_thumb_rect (s, r);
  }
  gtk_widget_set_opacity (g_search, e);
  gtk_widget_set_opacity (g_dash, e);

  if (t >= 1.0) {
    anim.source = 0;
    for (SlotEntry &s : g_slots)
      apply_thumb_rect (s, s.rect);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

void
start_animation (bool closing, guint duration_ms)
{
  if (g_anim.source != 0) {
    g_source_remove (g_anim.source);
    g_anim.source = 0;
  }
  g_anim.closing = closing;
  g_anim.duration_ms = duration_ms;
  g_anim.start_us = g_get_monotonic_time ();
  g_anim.source = g_timeout_add (16, anim_tick, nullptr);
}

void
start_open_animation (void)
{
  start_animation (false, kOpenMs);
}

void
finish_hide (void)
{
  set_hover (-1);
  destroy_previews ();
  gtk_widget_set_opacity (g_search, 1.0);
  gtk_widget_set_opacity (g_dash, 1.0);
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

  if (g_panel_height > 0 && py < g_panel_height) {
    g_print ("[ov] click (%d,%d) ignored (panel strip)\n", px, py);
    return;
  }

  // Inside a preview slot: restore/focus that window and close.
  POINT pt = {px, py};
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

  g_print ("[ov] click (%d,%d) blank -> close\n", px, py);
  close_overview ("ov-click-blank");
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
  g_idle_add (overview_place_idle, nullptr);
}

// Relayout without animation (window closed via the chrome button etc.).
void
rebuild_now (void)
{
  if (g_window == nullptr || !gtk_widget_get_visible (g_window))
    return;
  if (g_anim.source != 0 && g_anim.closing)
    return;
  overview_place_idle (nullptr);
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
  gtk_widget_add_css_class (g_window, "winome-overview");

  // #overviewGroup { background-color: #222226 } — GTK matches #id against
  // the widget name.
  g_root = gtk_fixed_new ();
  gtk_widget_set_name (g_root, "overviewGroup");
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

  // .workspace-background — wallpaper in a 30px rounded clip (+ CSS shadow).
  g_ws_bg = (GtkWidget *)g_object_new (winome_workspace_bg_get_type (),
                                       nullptr);
  gtk_widget_add_css_class (g_ws_bg, "workspace-background");
  gtk_fixed_put (GTK_FIXED (g_root), g_ws_bg, 0, 0);
  gtk_widget_set_visible (g_ws_bg, FALSE);

  // Dash.
  g_dash = overview_dash_new (g_root);
  gtk_fixed_put (GTK_FIXED (g_root), g_dash, 0, 0);
  gtk_widget_set_visible (g_dash, FALSE);

  // .window-caption hover pill.
  g_caption = gtk_label_new ("");
  gtk_widget_add_css_class (g_caption, "window-caption");
  gtk_fixed_put (GTK_FIXED (g_root), g_caption, 0, 0);
  gtk_widget_set_visible (g_caption, FALSE);

  // Close-button chrome window (above the DWM thumbnails).
  chrome_init ();

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
    gtk_widget_set_opacity (g_search, 0.0);
    gtk_widget_set_opacity (g_dash, 0.0);
    gtk_widget_set_visible (g_window, TRUE);
  } else {
    g_idle_add (overview_place_idle, nullptr);
  }
}

void
overview_hide (void)
{
  g_print ("[ov] hide\n");
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
