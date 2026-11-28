// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// workspaceSwitcherPopup.js port. Geometry and colors from
// gnome-shell-sass:
//   .workspace-switcher { @extend %osd_panel; margin-bottom: 4em;
//     spacing: $base_padding * 2; padding: $base_padding * 2 $base_padding * 3 }
//   .ws-switcher-indicator {
//     background-color: transparentize($osd_fg_color, 0.5);
//     padding: $ws_dot_inactive / 2; margin: ($ws_indicator_height -
// $ws_dot_inactive) / 2; border-radius: $ws_indicator_height;
//     &:active { background-color: $osd_fg_color;
//                padding: $ws_dot_active / 2;
//                margin: ($ws_indicator_height - $ws_dot_active) / 2 } }
// with $ws_indicator_height: 32px, $ws_dot_active: 32/3, $ws_dot_inactive:
// 32/6, $base_padding: 6px, $osd_fg_color: white, $osd_bg_color:
// lighten(#222226, 5%), border 1px transparentize(white, 0.98) and
// border-radius 999px (capsule). The 4em bottom margin of the default
// 11pt shell font is ~60px.
//
// The popup reacts to virtual-desktop change notifications (via virtual-desktop.cpp's
// unified WinEvent + hotkey observer); switches made while the overview is open
// are handled by the overview's own animated relayout and never trigger the OSD.

#include "ws-osd.h"
#include "fonts.h"
#include "fullscreen-watcher.h"
#include "host-utils.h"
#include "monitor-manager.h"
#include "overview.h"
#include "st-engine.h"
#include "virtual-desktop.h"
#include "workspace-manager.h"

#include <unordered_map>

#include <gtk/gtk.h>
#include <gdk/win32/gdkwin32.h>

#include <windows.h>
#include <dwmapi.h>

#include <algorithm>

namespace winome {

namespace {

// workspaceSwitcherPopup.js constants.
constexpr int kAnimMs = 100;        // ANIMATION_TIME (fade in/out)
constexpr int kDisplayTimeoutMs = 600;  // DISPLAY_TIMEOUT

// _workspace-switcher.scss values (LOGICAL px).
constexpr double kPaddingY = 12.0;  // base_padding * 2
constexpr double kPaddingX = 18.0;  // base_padding * 3
constexpr double kSpacing = 12.0;   // base_padding * 2
constexpr double kUnit = 32.0;      // $ws_indicator_height
constexpr double kDotActive = 32.0 / 3.0;
constexpr double kDotInactive = 32.0 / 6.0;
constexpr double kMarginBottom = 60.0;  // 4em @ 11pt
constexpr double kShadowPad = 8.0;      // transparent gutter for OSD elevation shadow

// %osd_panel: lighten(#222226, 5%) with a 1px
// transparentize(white, 0.98) border; the 999px radius clamps to a capsule.
constexpr double kOsdBgR = 0x2e / 255.0;
constexpr double kOsdBgG = 0x2e / 255.0;
constexpr double kOsdBgB = 0x33 / 255.0;

struct OsdMetrics {
  double padding_x = kPaddingX;
  double padding_y = kPaddingY;
  double spacing = kSpacing;
  double unit = kUnit;
  double dot_active = kDotActive;
  double dot_inactive = kDotInactive;
  double margin_bottom = kMarginBottom;
  double bg_r = kOsdBgR;
  double bg_g = kOsdBgG;
  double bg_b = kOsdBgB;
  bool resolved = false;
};

OsdMetrics g_osd_metrics;

const OsdMetrics &
osd_metrics ()
{
  if (g_osd_metrics.resolved)
    return g_osd_metrics;
  g_osd_metrics.resolved = true;

  StEngine *e = StEngine::get ();
  if (e == nullptr)
    return g_osd_metrics;

  StEngine::Node panel (*e, nullptr, "", "workspace-switcher", "");
  double v = 0.0;
  if (panel.lookup_length ("spacing", &v) && v > 0.0)
    g_osd_metrics.spacing = v;
  if (panel.lookup_length ("padding-top", &v) && v > 0.0)
    g_osd_metrics.padding_y = v;
  if (panel.lookup_length ("padding-left", &v) && v > 0.0)
    g_osd_metrics.padding_x = v;
  if (panel.lookup_length ("margin-bottom", &v) && v > 0.0)
    g_osd_metrics.margin_bottom = v;
  std::string bg_hex;
  if (panel.lookup_color ("background-color", &bg_hex)) {
    GdkRGBA rgba{};
    if (gdk_rgba_parse (&rgba, bg_hex.c_str ())) {
      g_osd_metrics.bg_r = rgba.red;
      g_osd_metrics.bg_g = rgba.green;
      g_osd_metrics.bg_b = rgba.blue;
    }
  }

  StEngine::Node ind (*e, nullptr, "", "ws-switcher-indicator", "");
  if (ind.lookup_length ("border-radius", &v) && v > 0.0)
    g_osd_metrics.unit = v;
  if (ind.lookup_length ("padding-top", &v) && v > 0.0)
    g_osd_metrics.dot_inactive = 2.0 * v;

  StEngine::Node ind_act (*e, nullptr, "", "ws-switcher-indicator", "active");
  if (ind_act.lookup_length ("padding-top", &v) && v > 0.0)
    g_osd_metrics.dot_active = 2.0 * v;

  return g_osd_metrics;
}

GtkWidget *g_window = nullptr;
GtkWidget *g_area = nullptr;
HWND g_hwnd = nullptr;

int g_count = 0;          // workspace count (slow poll)
int g_active = -1;        // active workspace shown
double g_fade = 0.0;      // 0..1, drawn premultiplied into every color

guint g_timeout_id = 0;
guint g_fade_source = 0;
gint64 g_fade_start_us = 0;
guint g_fade_duration_ms = 0;
double g_fade_from = 0.0, g_fade_to = 0.0;

double
ease_out_quad (double t)
{
  return 1.0 - (1.0 - t) * (1.0 - t);
}

// --- drawing -----------------------------------------------------------------

static void
draw_osd (GtkDrawingArea *area, cairo_t *cr, int width, int height,
          gpointer user_data)
{
  (void)area;
  (void)user_data;

  // Always clear to full alpha=0 first so frame 0 or empty states never leave
  // opaque pixels behind the capsule.
  cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint (cr);
  cairo_set_operator (cr, CAIRO_OPERATOR_OVER);

  if (g_count <= 0 || width <= 0 || height <= 0)
    return;

  // Draw the OSD pill at full alpha=1.0; fade_tick() animates GPU widget
  // opacity via gtk_widget_set_opacity() so fading never re-rasterizes Cairo.
  const double f = 1.0;

  // Capsule panel ($forced_circular_radius 999px clamps to ch/2) centered
  // inside the transparent shadow gutter.
  const OsdMetrics &m = osd_metrics ();
  const double pad = kShadowPad;
  const double cw = std::max (1.0, width - 2.0 * pad);
  const double ch = std::max (1.0, height - 2.0 * pad);
  const double r = ch / 2.0;

  auto pill_path = [cr] (double x, double y, double pw, double rad) {
    cairo_new_sub_path (cr);
    cairo_arc (cr, x + pw - rad, y + rad, rad, -G_PI_2, G_PI_2);
    cairo_arc (cr, x + rad, y + rad, rad, G_PI_2, 3 * G_PI_2);
    cairo_close_path (cr);
  };

  // Soft 3-layer elevation shadow matching GNOME Shell's floating OSD pill.
  for (int layer = 3; layer >= 1; --layer) {
    const double spread = static_cast<double> (layer) * 1.6;
    const double dy = static_cast<double> (layer) * 0.9;
    const double alpha = (0.11 - 0.025 * layer) * f;
    pill_path (pad - spread, pad + dy - spread * 0.5,
               cw + 2.0 * spread, r + spread * 0.5);
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, alpha);
    cairo_fill (cr);
  }

  // Main %osd_panel capsule (#2e2e33) + crisp 1px inset hairline border.
  pill_path (pad + 0.5, pad + 0.5, cw - 1.0,
             std::max (1.0, (ch - 1.0) / 2.0));
  cairo_set_source_rgba (cr, m.bg_r, m.bg_g, m.bg_b, 0.98 * f);
  cairo_fill_preserve (cr);
  cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.08 * f);
  cairo_set_line_width (cr, 1.0);
  cairo_stroke (cr);

  // One 32px unit per workspace; dots grow via the padding/margin pairs
  // from _workspace-switcher.scss (drawn centered on each unit).
  double x = pad + m.padding_x + m.unit / 2.0;
  double cy = pad + m.padding_y + m.unit / 2.0;
  for (int i = 0; i < g_count; ++i) {
    bool active = i == g_active;
    double dot = (active ? m.dot_active : m.dot_inactive) / 2.0;
    double alpha = (active ? 1.0 : 0.5) * f;
    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, alpha);
    cairo_arc (cr, x, cy, dot, 0.0, 2.0 * G_PI);
    cairo_fill (cr);
    x += m.unit + m.spacing;
  }
}

// --- fade / placement ----------------------------------------------------------

static gboolean
fade_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
  (void)widget;
  (void)user_data;
  double t = (gdk_frame_clock_get_frame_time (clock) - g_fade_start_us) /
             1000.0 / (double)g_fade_duration_ms;
  if (t < 0.0)
    t = 0.0;
  if (t > 1.0)
    t = 1.0;
  g_fade = g_fade_from + (g_fade_to - g_fade_from) * ease_out_quad (t);
  if (g_area != nullptr)
    gtk_widget_set_opacity (g_area, std::clamp (g_fade, 0.0, 1.0));
  if (t >= 1.0) {
    g_fade_source = 0;
    if (g_fade_to <= 0.0 && g_window != nullptr)
      gtk_widget_set_visible (g_window, FALSE);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void
fade_to (double to)
{
  if (g_fade_source != 0) {
    gtk_widget_remove_tick_callback (g_window, g_fade_source);
    g_fade_source = 0;
  }
  g_fade_from = g_fade;
  g_fade_to = to;
  g_fade_duration_ms = kAnimMs;
  g_fade_start_us = g_get_monotonic_time ();
  if (g_area != nullptr)
    gtk_widget_set_opacity (g_area, std::clamp (g_fade, 0.0, 1.0));
  g_fade_source =
      gtk_widget_add_tick_callback (g_window, fade_tick, nullptr, nullptr);
}

static HMONITOR g_osd_monitor = nullptr;
static std::unordered_map<HMONITOR, int> g_last_index_by_mon;
static int g_osd_last_w = -1;
static int g_osd_last_h = -1;
static RECT g_osd_want_rect = {0, 0, 0, 0};
static guint g_osd_map_idle = 0;

static constexpr const wchar_t *kWsOsdOrigProcProp = L"WINOME_WsOsdOrigProc";

static LRESULT CALLBACK
osd_subclass_wndproc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  if (msg == WM_ERASEBKGND)
    return 1;
  if (msg == WM_MOUSEACTIVATE)
    return MA_NOACTIVATE;
  if (msg == WM_STYLECHANGING && wParam == static_cast<WPARAM> (GWL_EXSTYLE) &&
      lParam != 0) {
    auto *ss = reinterpret_cast<STYLESTRUCT *> (lParam);
    ss->styleNew = (static_cast<LONG_PTR> (ss->styleNew) |
                    WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT) &
                   ~WS_EX_APPWINDOW;
  }
  if (msg == WM_GETMINMAXINFO && lParam != 0 &&
      g_osd_want_rect.right > g_osd_want_rect.left &&
      g_osd_want_rect.bottom > g_osd_want_rect.top) {
    auto *mmi = reinterpret_cast<MINMAXINFO *> (lParam);
    const LONG want_w = g_osd_want_rect.right - g_osd_want_rect.left;
    const LONG want_h = g_osd_want_rect.bottom - g_osd_want_rect.top;
    mmi->ptMinTrackSize = POINT{want_w, want_h};
    mmi->ptMaxTrackSize = POINT{want_w, want_h};
    return 0;
  }

  if (msg == WM_WINDOWPOSCHANGING && lParam != 0) {
    auto *wp = reinterpret_cast<WINDOWPOS *> (lParam);
    wp->flags |= SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    if ((wp->flags & SWP_HIDEWINDOW) == 0 &&
        g_osd_want_rect.right > g_osd_want_rect.left &&
        g_osd_want_rect.bottom > g_osd_want_rect.top) {
      RECT cur_rect{};
      const bool has_cur = GetWindowRect (hwnd, &cur_rect) != FALSE;
      RECT eff_rect{};
      const bool has_eff = winome::resolve_panel_windowpos_rect (
          cur_rect, has_cur, wp->x, wp->y, wp->cx, wp->cy, wp->flags,
          &eff_rect);
      if (!has_eff || !EqualRect (&eff_rect, &g_osd_want_rect)) {
        const int want_w = g_osd_want_rect.right - g_osd_want_rect.left;
        const int want_h = g_osd_want_rect.bottom - g_osd_want_rect.top;
        const bool need_move =
            !has_cur || (wp->flags & SWP_NOMOVE) == 0 ||
            cur_rect.left != g_osd_want_rect.left ||
            cur_rect.top != g_osd_want_rect.top;
        const bool need_size =
            !has_cur || (wp->flags & SWP_NOSIZE) == 0 ||
            (cur_rect.right - cur_rect.left) != want_w ||
            (cur_rect.bottom - cur_rect.top) != want_h;
        if (need_move) {
          wp->x = g_osd_want_rect.left;
          wp->y = g_osd_want_rect.top;
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

  WNDPROC orig = reinterpret_cast<WNDPROC> (
      GetPropW (hwnd, kWsOsdOrigProcProp));
  if (msg == WM_NCDESTROY) {
    RemovePropW (hwnd, kWsOsdOrigProcProp);
    if (orig != nullptr) {
      if (GetWindowLongPtrW (hwnd, GWLP_WNDPROC) ==
          reinterpret_cast<LONG_PTR> (osd_subclass_wndproc)) {
        SetWindowLongPtrW (hwnd, GWLP_WNDPROC,
                           reinterpret_cast<LONG_PTR> (orig));
      }
      return CallWindowProcW (orig, hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW (hwnd, msg, wParam, lParam);
  }
  return orig != nullptr ? CallWindowProcW (orig, hwnd, msg, wParam, lParam)
                         : DefWindowProcW (hwnd, msg, wParam, lParam);
}

static void
ensure_osd_subclass (HWND hwnd)
{
  if (hwnd == nullptr || !IsWindow (hwnd))
    return;
  if (GetPropW (hwnd, kWsOsdOrigProcProp) != nullptr)
    return;
  WNDPROC cur = reinterpret_cast<WNDPROC> (
      GetWindowLongPtrW (hwnd, GWLP_WNDPROC));
  if (cur != nullptr && cur != osd_subclass_wndproc) {
    SetPropW (hwnd, kWsOsdOrigProcProp, reinterpret_cast<HANDLE> (cur));
    SetWindowLongPtrW (hwnd, GWLP_WNDPROC,
                       reinterpret_cast<LONG_PTR> (osd_subclass_wndproc));
  }
}

static void
unsubclass_osd_hwnd (HWND hwnd)
{
  if (hwnd == nullptr || !IsWindow (hwnd))
    return;
  WNDPROC orig = reinterpret_cast<WNDPROC> (
      GetPropW (hwnd, kWsOsdOrigProcProp));
  WNDPROC cur = reinterpret_cast<WNDPROC> (
      GetWindowLongPtrW (hwnd, GWLP_WNDPROC));
  if (orig != nullptr && cur == osd_subclass_wndproc)
    SetWindowLongPtrW (hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR> (orig));
  RemovePropW (hwnd, kWsOsdOrigProcProp);
}

// (Re)position the window for the current workspace count and optionally show it.
static void
osd_present (bool ensure_visible = true)
{
  if (g_window == nullptr)
    return;

  const OsdMetrics &m = osd_metrics ();
  int n = std::max (g_count, 1);
  int w = (int)(2 * (m.padding_x + kShadowPad) + n * m.unit +
                (n - 1) * m.spacing + 0.5);
  int h = (int)(2 * (m.padding_y + kShadowPad) + m.unit + 0.5);
  if (w != g_osd_last_w || h != g_osd_last_h) {
    gtk_widget_set_size_request (g_area, w, h);
    gtk_window_set_default_size (GTK_WINDOW (g_window), w, h);
    gtk_widget_queue_resize (g_window);
    g_osd_last_w = w;
    g_osd_last_h = h;
  }

  if (!gtk_widget_get_realized (g_window))
    gtk_widget_realize (g_window);
  if (g_hwnd == nullptr || !IsWindow (g_hwnd))
    return;

  HMONITOR mon = g_osd_monitor != nullptr
                     ? g_osd_monitor
                     : winome::monitor_manager_active_monitor ();
  MONITORINFO mi = {};
  mi.cbSize = sizeof (mi);
  if (mon == nullptr || !GetMonitorInfoW (mon, &mi))
    return;

  int scale = std::max (1, gtk_widget_get_scale_factor (g_window));
  const RECT want = compute_ws_osd_hwnd_rect (
      mi.rcMonitor, w, h, m.margin_bottom, kShadowPad, scale);
  g_osd_want_rect = want;

  winome::vd::pin_window_async (g_hwnd);
  DWORD cloaked = 0;
  DwmGetWindowAttribute (g_hwnd, DWMWA_CLOAKED, &cloaked, sizeof (cloaked));
  if ((cloaked & 0x2u /* DWM_CLOAKED_SHELL */) != 0) {
    int target_vd = 0;
    if (winome::ws::effective_mode () !=
        winome::ws::MultiMonitorWorkspaceMode::Independent) {
      const auto snap = winome::vd::cached_snapshot ();
      target_vd = snap.valid ? snap.active_index : winome::vd::current_index ();
    }
    if (target_vd >= 0) {
      winome::vd::move_window_to_desktop (g_hwnd, target_vd);
      winome::vd::pin_window_async (g_hwnd);
    }
  }

  RECT cur_rect{};
  const bool has_cur = GetWindowRect (g_hwnd, &cur_rect) != FALSE;
  const bool need_show = ensure_visible && !IsWindowVisible (g_hwnd);
  if (!has_cur || !EqualRect (&cur_rect, &want) || need_show) {
    UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    if (ensure_visible)
      flags |= SWP_SHOWWINDOW;
    SetWindowPos (g_hwnd, HWND_TOPMOST, want.left, want.top,
                  want.right - want.left, want.bottom - want.top, flags);
  }
}

static gboolean
on_display_timeout (gpointer user_data)
{
  (void)user_data;
  g_timeout_id = 0;
  fade_to (0.0);  // EASE_OUT_QUAD 100ms, then unmap
  return G_SOURCE_REMOVE;
}

static void
osd_display (int active)
{
  g_active = active;
  if (g_area != nullptr)
    gtk_widget_queue_draw (g_area);

  // Realize, subclass, and lock g_osd_want_rect BEFORE GTK maps/shows the
  // toplevel so GDK's initial configure cannot move the OSD to (0, 0).
  osd_present (false);
  if (g_window != nullptr && !gtk_widget_get_visible (g_window))
    gtk_widget_set_visible (g_window, TRUE);
  osd_present (true);

  // Re-arm the display timeout; a visible popup just refreshes its dots
  // (upstream: duration 0 when already visible).
  if (g_timeout_id != 0)
    g_source_remove (g_timeout_id);
  g_timeout_id = g_timeout_add (kDisplayTimeoutMs, on_display_timeout, nullptr);

  if (g_fade < 1.0)
    fade_to (1.0);
}

// --- event-driven observer ---------------------------------------------------

static void
on_ws_osd_desktop_changed (HMONITOR monitor, int count, int cur,
                           void *user_data G_GNUC_UNUSED)
{
  HMONITOR key = monitor;
  if (count <= 1) {
    g_last_index_by_mon[key] = cur;
    return;
  }

  // Switches made from the overview (pill clicks, animated relayout) must
  // not trigger the OSD on that monitor: upstream only mutter's own switch
  // actions do.
  HMONITOR check_mon =
      monitor != nullptr ? monitor : winome::monitor_manager_active_monitor ();
  if (overview_is_open_for_monitor (check_mon)) {
    g_last_index_by_mon[key] = -1;  // resync after overview closes
    return;
  }

  if (cur < 0)
    return;
  auto it = g_last_index_by_mon.find (key);
  if (it == g_last_index_by_mon.end () || it->second < 0) {
    g_last_index_by_mon[key] = cur;
    return;
  }
  if (cur != it->second) {
    it->second = cur;
    g_count = count;
    g_osd_monitor = monitor;
    osd_display (cur);
  }
}

// --- window ---------------------------------------------------------------------

static void
on_osd_realize (GtkWidget *widget, gpointer user_data)
{
  (void)user_data;
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (widget));
  if (surface == nullptr)
    return;
  g_hwnd = static_cast<HWND> (gdk_win32_surface_get_handle (surface));
  LONG_PTR ex = GetWindowLongPtrW (g_hwnd, GWL_EXSTYLE);
  LONG_PTR want = ex | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
                  WS_EX_TRANSPARENT;
  want &= ~WS_EX_APPWINDOW;
  if (want != ex)
    SetWindowLongPtrW (g_hwnd, GWL_EXSTYLE, want);
  winome::harden_shell_overlay_hwnd (g_hwnd);
  ensure_osd_subclass (g_hwnd);
  winome::vd::pin_window_async (g_hwnd);

  // Suppress Windows 11 DWM rectangular shadow, border, and corner clipping so
  // only the transparent Cairo capsule renders.
  constexpr DWORD kDwmNcPolicyAttr = 2;      // DWMWA_NCRENDERING_POLICY
  constexpr DWORD kDwmNcDisabled = 1;        // DWMNCRP_DISABLED
  constexpr DWORD kDwmCornerPrefAttr = 33;   // DWMWA_WINDOW_CORNER_PREFERENCE
  constexpr DWORD kDwmCornerDoNotRound = 1;  // DWMWCP_DONOTROUND
  constexpr DWORD kDwmBorderColorAttr = 34;  // DWMWA_BORDER_COLOR
  constexpr DWORD kDwmColorNone = 0xFFFFFFFEu;
  DwmSetWindowAttribute (g_hwnd, kDwmNcPolicyAttr, &kDwmNcDisabled,
                         sizeof (kDwmNcDisabled));
  DwmSetWindowAttribute (g_hwnd, kDwmCornerPrefAttr, &kDwmCornerDoNotRound,
                         sizeof (kDwmCornerDoNotRound));
  DwmSetWindowAttribute (g_hwnd, kDwmBorderColorAttr, &kDwmColorNone,
                         sizeof (kDwmColorNone));
}

static gboolean
osd_post_map_idle (gpointer user_data G_GNUC_UNUSED)
{
  g_osd_map_idle = 0;
  if (g_window != nullptr && gtk_widget_get_visible (g_window))
    osd_present (true);
  return G_SOURCE_REMOVE;
}

static void
on_osd_map (GtkWidget *widget, gpointer user_data)
{
  (void)widget;
  (void)user_data;
  if (g_osd_map_idle != 0)
    return;
  // GDK's first-map sequence positions the window itself; re-apply ours
  // before the frame clock repaints.
  g_osd_map_idle = g_idle_add_full (G_PRIORITY_HIGH_IDLE, osd_post_map_idle,
                                    nullptr, nullptr);
}

}  // namespace

void
ws_osd_init ()
{
  g_window = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (g_window), "WINOME Workspace OSD");
  gtk_widget_set_name (g_window, "winomeWsOsdWindow");
  gtk_window_set_decorated (GTK_WINDOW (g_window), FALSE);
  gtk_window_set_resizable (GTK_WINDOW (g_window), FALSE);
  gtk_widget_set_focusable (g_window, FALSE);
  install_bundled_fonts (g_window);

  g_signal_connect (g_window, "realize", G_CALLBACK (on_osd_realize),
                    nullptr);
  g_signal_connect (g_window, "map", G_CALLBACK (on_osd_map), nullptr);

  g_area = gtk_drawing_area_new ();
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (g_area), draw_osd,
                                  nullptr, nullptr);
  gtk_window_set_child (GTK_WINDOW (g_window), g_area);

  ws::add_change_listener (on_ws_osd_desktop_changed, nullptr);
}

void
ws_osd_shutdown ()
{
  ws::remove_change_listener (on_ws_osd_desktop_changed, nullptr);
  g_last_index_by_mon.clear ();
  g_osd_monitor = nullptr;
  g_osd_last_w = -1;
  g_osd_last_h = -1;
  g_osd_want_rect = RECT{0, 0, 0, 0};
  if (g_osd_map_idle != 0) {
    g_source_remove (g_osd_map_idle);
    g_osd_map_idle = 0;
  }
  if (g_timeout_id != 0) {
    g_source_remove (g_timeout_id);
    g_timeout_id = 0;
  }
  if (g_fade_source != 0 && g_window != nullptr) {
    gtk_widget_remove_tick_callback (g_window, g_fade_source);
    g_fade_source = 0;
  }
  if (g_hwnd != nullptr)
    unsubclass_osd_hwnd (g_hwnd);
  if (g_window != nullptr) {
    gtk_window_destroy (GTK_WINDOW (g_window));
    g_window = nullptr;
    g_area = nullptr;
    g_hwnd = nullptr;
  }
}

}  // namespace winome
