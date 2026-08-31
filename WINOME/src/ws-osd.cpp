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
// The popup is detected by polling the virtual-desktop index (400ms);
// switches made while the overview is open are handled by the overview's
// own animated relayout and never trigger the OSD.

#include "ws-osd.h"
#include "overview.h"
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
constexpr int kPollMs = 400;       // desktop-switch poll interval

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
guint g_poll_tick = 0;

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
  double r = height / 2.0;
  cairo_new_sub_path (cr);
  cairo_arc (cr, width - r, r, r, -G_PI_2, G_PI_2);
  cairo_arc (cr, r, r, r, G_PI_2, 3 * G_PI_2);
  cairo_close_path (cr);
  cairo_set_source_rgba (cr, kOsdBgR, kOsdBgG, kOsdBgB, 1.0 * f);
  cairo_fill_preserve (cr);
  cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.02 * f);
  cairo_set_line_width (cr, 1.0);
  cairo_stroke (cr);

  // One 32px unit per workspace; dots grow via the padding/margin pairs
  // from the stylesheet (drawn centered on the unit here).
  double x = kPaddingX + kUnit / 2.0;
  double cy = kPaddingY + kUnit / 2.0;
  for (int i = 0; i < g_count; ++i) {
    bool active = i == g_active;
    double dot = (active ? kDotActive : kDotInactive) / 2.0;
    double alpha = (active ? 1.0 : 0.5) * f;
    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, alpha);
    cairo_arc (cr, x, cy, dot, 0.0, 2.0 * G_PI);
    cairo_fill (cr);
    x += kUnit + kSpacing;
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

  int n = std::max (g_count, 1);
  int w = (int)(2 * kPaddingX + n * kUnit + (n - 1) * kSpacing + 0.5);
  int h = (int)(2 * kPaddingY + kUnit + 0.5);
  gtk_widget_set_size_request (g_area, w, h);

  if (g_hwnd == nullptr || !IsWindow (g_hwnd))
    return;

  POINT origin = {0, 0};
  HMONITOR mon = MonitorFromPoint (origin, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {sizeof (mi)};
  if (!GetMonitorInfoW (mon, &mi))
    return;

  int scale = gtk_widget_get_scale_factor (g_window);
  int pw = w * scale, ph = h * scale;
  int margin = (int)(kMarginBottom * scale);
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

// --- poll -----------------------------------------------------------------------

static gboolean
ws_osd_poll (gpointer user_data)
{
  (void)user_data;

  // Slow channel: refresh the workspace count (COM enumeration) only every
  // ~5s — the fast channel below queries the current index every tick.
  if ((g_poll_tick++ % 12) == 0 || g_count == 0)
    g_count = (int)vd::desktops ().size ();

  if (g_count <= 1)
    return G_SOURCE_CONTINUE;

  // Switches made from the overview (pill clicks, animated relayout) must
  // not trigger the OSD: upstream only mutter's own switch actions do.
  if (overview_is_open () != 0) {
    g_last_index = -1;  // resync on the next poll after it closes
    return G_SOURCE_CONTINUE;
  }

  int cur = vd::current_index ();
  if (cur < 0)
    return G_SOURCE_CONTINUE;
  if (g_last_index < 0) {
    g_last_index = cur;
    return G_SOURCE_CONTINUE;
  }
  if (cur != g_last_index) {
    g_last_index = cur;
    osd_display (cur);
  }
  return G_SOURCE_CONTINUE;
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

// The first virtual-desktop enumeration runs late: on systems where the
// explorer-side RPC would deadlock (non-admin), vd::desktops() blocks until
// the module's 8s timeout auto-disables it — keep that away from the
// startup path.
static gboolean
ws_osd_start_polling (gpointer user_data)
{
  (void)user_data;
  g_timeout_add (kPollMs, ws_osd_poll, nullptr);
  return G_SOURCE_REMOVE;
}

}  // namespace

void
ws_osd_init ()
{
  g_window = gtk_window_new ();
  gtk_window_set_decorated (GTK_WINDOW (g_window), FALSE);
  gtk_window_set_resizable (GTK_WINDOW (g_window), FALSE);

  g_signal_connect (g_window, "realize", G_CALLBACK (on_osd_realize),
                    nullptr);
  g_signal_connect (g_window, "map", G_CALLBACK (on_osd_map), nullptr);

  g_area = gtk_drawing_area_new ();
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (g_area), draw_osd,
                                  nullptr, nullptr);
  gtk_window_set_child (GTK_WINDOW (g_window), g_area);

  g_timeout_add (2000, ws_osd_start_polling, nullptr);
}

}  // namespace winome
