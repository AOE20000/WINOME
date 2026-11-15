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
#include "overview.h"
#include "st-engine.h"
#include "virtual-desktop.h"

#include <gtk/gtk.h>
#include <gdk/win32/gdkwin32.h>

#include <windows.h>

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
int g_last_index = -1;    // last seen desktop (change detection)
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
  if (g_count <= 0 || width <= 0 || height <= 0)
    return;

  // Everything fades together (upstream fades the whole actor's opacity).
  double f = g_fade;
  if (f <= 0.0)
    return;

  cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint (cr);
  cairo_set_operator (cr, CAIRO_OPERATOR_OVER);

  // Capsule panel ($forced_circular_radius 999px clamps to height/2).
  const OsdMetrics &m = osd_metrics ();
  double r = height / 2.0;
  cairo_new_sub_path (cr);
  cairo_arc (cr, width - r, r, r, -G_PI_2, G_PI_2);
  cairo_arc (cr, r, r, r, G_PI_2, 3 * G_PI_2);
  cairo_close_path (cr);
  cairo_set_source_rgba (cr, m.bg_r, m.bg_g, m.bg_b, 1.0 * f);
  cairo_fill_preserve (cr);
  cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.02 * f);
  cairo_set_line_width (cr, 1.0);
  cairo_stroke (cr);

  // One 32px unit per workspace; dots grow via the padding/margin pairs
  // from the stylesheet (drawn centered on the unit here).
  double x = m.padding_x + m.unit / 2.0;
  double cy = m.padding_y + m.unit / 2.0;
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
  gtk_widget_queue_draw (g_area);
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
  g_fade_source =
      gtk_widget_add_tick_callback (g_window, fade_tick, nullptr, nullptr);
}

// (Re)position the window for the current workspace count and show it.
static void
osd_present (void)
{
  if (g_window == nullptr)
    return;

  const OsdMetrics &m = osd_metrics ();
  int n = std::max (g_count, 1);
  int w = (int)(2 * m.padding_x + n * m.unit + (n - 1) * m.spacing + 0.5);
  int h = (int)(2 * m.padding_y + m.unit + 0.5);
  gtk_widget_set_size_request (g_area, w, h);

  if (g_hwnd == nullptr || !IsWindow (g_hwnd))
    return;

  POINT origin = {0, 0};
  HMONITOR mon = MonitorFromPoint (origin, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {};
  mi.cbSize = sizeof (mi);
  if (!GetMonitorInfoW (mon, &mi))
    return;

  int scale = gtk_widget_get_scale_factor (g_window);
  int pw = w * scale, ph = h * scale;
  int margin = (int)(m.margin_bottom * scale);
  int x = mi.rcMonitor.left + (mi.rcMonitor.right - mi.rcMonitor.left) / 2 -
          pw / 2;
  int y = mi.rcMonitor.bottom - margin - ph;

  SetWindowPos (g_hwnd, HWND_TOPMOST, x, y, pw, ph,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
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

  if (g_window != nullptr && !gtk_widget_get_visible (g_window))
    gtk_widget_set_visible (g_window, TRUE);
  osd_present ();

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
on_ws_osd_desktop_changed (int count, int cur, void *user_data G_GNUC_UNUSED)
{
  g_count = count;
  if (g_count <= 1) {
    g_last_index = cur;
    return;
  }

  // Switches made from the overview (pill clicks, animated relayout) must
  // not trigger the OSD: upstream only mutter's own switch actions do.
  if (overview_is_open () != 0) {
    g_last_index = -1;  // resync after overview closes
    return;
  }

  if (cur < 0)
    return;
  if (g_last_index < 0) {
    g_last_index = cur;
    return;
  }
  if (cur != g_last_index) {
    g_last_index = cur;
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
  LONG_PTR want = ex | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
  want &= ~WS_EX_APPWINDOW;
  if (want != ex)
    SetWindowLongPtrW (g_hwnd, GWL_EXSTYLE, want);
}

static void
on_osd_map (GtkWidget *widget, gpointer user_data)
{
  (void)widget;
  (void)user_data;
  // GDK's first-map sequence positions the window itself; re-apply ours
  // right after the map (same pattern as the overview chrome windows).
  g_idle_add (+[] (gpointer) -> gboolean {
    osd_present ();
    return G_SOURCE_REMOVE;
  }, nullptr);
}

}  // namespace

void
ws_osd_init ()
{
  g_window = gtk_window_new ();
  gtk_window_set_decorated (GTK_WINDOW (g_window), FALSE);
  gtk_window_set_resizable (GTK_WINDOW (g_window), FALSE);
  install_bundled_fonts (g_window);

  g_signal_connect (g_window, "realize", G_CALLBACK (on_osd_realize),
                    nullptr);
  g_signal_connect (g_window, "map", G_CALLBACK (on_osd_map), nullptr);

  g_area = gtk_drawing_area_new ();
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (g_area), draw_osd,
                                  nullptr, nullptr);
  gtk_window_set_child (GTK_WINDOW (g_window), g_area);

  vd::add_change_listener (on_ws_osd_desktop_changed, nullptr);
}

}  // namespace winome
