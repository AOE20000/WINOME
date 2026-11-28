// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// GNOME-style top panel (port of gnome-shell's js/ui/panel.js).

#pragma once

#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace winome {

// Port of upstream gnome-shell js/ui/panel.js WorkspaceDot / WorkspaceIndicators
// continuous geometry and opacity math (unit-tested in winome-tests).
struct WorkspaceDotMetric {
  double slot_x = 0.0;
  double slot_w = 0.0;
  double draw_w = 0.0;
  double draw_h = 0.0;
  double opacity = 0.5;
  double expansion = 0.0;
};

inline double
workspace_dot_width_multiplier (int count)
{
  if (count <= 2)
    return 3.625;
  if (count <= 5)
    return 3.25;
  return 2.75;
}

inline std::vector<WorkspaceDotMetric>
compute_workspace_dot_metrics (int count, double active_pos, int dot_size,
                               int spacing, double *out_total_width = nullptr)
{
  std::vector<WorkspaceDotMetric> out;
  if (count < 1)
    count = 1;
  if (dot_size < 1)
    dot_size = 8;
  if (spacing < 0)
    spacing = 0;

  const double pos = std::clamp (active_pos, 0.0, static_cast<double> (count - 1));
  const double mult = workspace_dot_width_multiplier (count);
  constexpr double kInactiveScale = 0.75;
  constexpr double kInactiveOpacity = 0.50;

  out.reserve (static_cast<size_t> (count));
  double x = 0.0;
  for (int i = 0; i < count; ++i) {
    const double dist = std::fabs (static_cast<double> (i) - pos);
    const double exp = std::clamp (1.0 - dist, 0.0, 1.0);
    const double slot_w = static_cast<double> (dot_size) * (1.0 + (mult - 1.0) * exp);
    const double scale = kInactiveScale + (1.0 - kInactiveScale) * exp;
    const double opacity = kInactiveOpacity + (1.0 - kInactiveOpacity) * exp;

    WorkspaceDotMetric m;
    m.slot_x = x;
    m.slot_w = slot_w;
    m.draw_w = slot_w * scale;
    m.draw_h = static_cast<double> (dot_size) * scale;
    m.opacity = opacity;
    m.expansion = exp;
    out.push_back (m);

    x += slot_w;
    if (i + 1 < count)
      x += static_cast<double> (spacing);
  }
  if (out_total_width != nullptr)
    *out_total_width = x;
  return out;
}

// Compute exact interpolated top-bar backdrop color (#000000 <-> #222226) and
// #panelActivities pill highlight alpha matching upstream GNOME _panel.scss:
//   desktop rest=0.00, hover=0.17; overview rest=0.28, hover=0.32.
inline void
compute_panel_overview_colors (double overview_progress,
                               double hover_progress,
                               bool pressed,
                               double *out_bg_r,
                               double *out_bg_g,
                               double *out_bg_b,
                               double *out_activities_alpha)
{
  const double p = std::clamp (overview_progress, 0.0, 1.0);
  const double h = std::clamp (hover_progress, 0.0, 1.0);
  if (out_bg_r != nullptr)
    *out_bg_r = (34.0 / 255.0) * p;
  if (out_bg_g != nullptr)
    *out_bg_g = (34.0 / 255.0) * p;
  if (out_bg_b != nullptr)
    *out_bg_b = (38.0 / 255.0) * p;
  if (out_activities_alpha != nullptr) {
    const double rest_a = 0.28 * p;
    const double hover_a = 0.17 + (0.32 - 0.17) * p;
    double alpha = rest_a + (hover_a - rest_a) * h;
    if (pressed)
      alpha = std::max (alpha, 0.28);
    *out_activities_alpha = std::clamp (alpha, 0.0, 1.0);
  }
}

// Decide whether overview.cpp's anim_tick should push a new panel overview
// progress sample on this frame.
// - On non-blocking composition renderers (default GskCairoRenderer +
//   DirectComposition), push every frame where progress changes (> 1e-4)
//   so the top bar backdrop (#000000 <-> #222226) and #panelActivities
//   pill fade smoothly at 60fps in lockstep with the overview.
// - Only when a blocking Win32 GL swap renderer (GskGLRenderer /
//   GskNglRenderer) is explicitly active do we coarsen intermediate updates
//   to avoid serializing two wglSwapBuffers VSync waits per frame.
inline bool
should_push_panel_overview_progress (double next_progress,
                                     double current_progress,
                                     bool first_frame,
                                     double norm_t,
                                     bool blocking_gl_swap)
{
  const double delta = std::fabs (next_progress - current_progress);
  if (delta <= 1e-4)
    return norm_t >= 1.0 && next_progress != current_progress;
  if (!blocking_gl_swap)
    return true;
  const bool endpoint = (first_frame || norm_t <= 0.0 || norm_t >= 1.0);
  return endpoint || delta >= 0.45;
}

// Whether a panel popover still occupies the topmost shell band (above the
// panel and Activities overview): true while actively open OR while still
// visible during its BOXPOPOVER_ANIMATION_TIME fade-out.
inline bool
popover_occupies_top_band (bool open, bool fading_out, bool window_visible)
{
  return open || (fading_out && window_visible);
}

}  // namespace winome

// Create the full-width top panel window (Activities + clock + quick settings).
GtkWidget *winome_shell_panel_new (void);

// Create a lightweight top panel window for secondary monitors (Activities + clock).
GtkWidget *winome_shell_panel_new_secondary (void);

// Panel height in pixels, resolved from the GNOME theme (#panel { height:
// 2.2em }) using the St theme context font. Matches gnome-shell's 32px bar at
// scale 1.
int winome_shell_panel_height (void);

// HWND of the topmost open or fading-out visible popover (quick settings /
// calendar / tray), or NULL when none occupies the topmost band. Used to
// enforce the layering contract popover > panel > overview from
// overview_restack().
void *winome_shell_panel_top_popover_hwnd (void);

// Per-monitor variant: returns the topmost open or fading-out visible popover
// anchored on @hmonitor (or any matching popover when @hmonitor == nullptr).
void *winome_shell_panel_top_popover_hwnd_for_monitor (void *hmonitor);

// TRUE for a short window after a popover was dismissed by a click outside
// itself while the Activities overview was open. In GNOME that click is
// consumed by the menu grab: it closes the menu only, so the overview must
// ignore it instead of closing too (windowPreview/overview click handling).
int winome_shell_panel_popover_just_closed (void);

// Close all open panel popovers, or only popovers on @hmonitor (nullptr = all).
void winome_shell_panel_close_popovers (void);
void winome_shell_panel_close_popovers_for_monitor (void *hmonitor);
void winome_shell_panel_close_popovers_immediate (void);
void winome_shell_panel_sync_popover_size (GtkWidget *widget);

// Reload GNOME theme stylesheet, optional developer user-debug.css, and StEngine.
bool winome_shell_reload_theme (void);

// Lockstep panel overview backdrop (#000000 <-> #222226) and #panelActivities
// highlight transition driven directly by overview.cpp's anim_tick [0..1].
void winome_shell_panel_set_overview_progress (double progress);

// Per-monitor variant: updates only the panel hosted on @hmonitor (or all
// panels when @hmonitor == nullptr).
void winome_shell_panel_set_overview_progress_for_monitor (void *hmonitor,
                                                           double progress);

// Real-time continuous workspace indicator scroll position for #panelActivities
// (matching upstream panel.js WorkspaceIndicators). Pass position < 0 to release
// back to virtual-desktop observer sync.
void winome_shell_panel_set_workspace_scroll (int count, double position);
void winome_shell_panel_set_workspace_scroll_for_monitor (void *hmonitor,
                                                          int count,
                                                          double position);

// Declarative TopBar plugin UI slots (host updates only on GTK main thread).
typedef void (*WinomePluginTopBarClickCb) (const char *plugin_id,
                                           const char *slot);
void winome_shell_panel_set_plugin_click_cb (WinomePluginTopBarClickCb cb);
void winome_shell_panel_plugin_upsert (const char *key,
                                       const char *plugin_id,
                                       const char *slot,
                                       const char *icon_name,
                                       const char *text,
                                       const char *tooltip);
void winome_shell_panel_plugin_remove (const char *key);

// Request an asynchronous GTK redraw on all realized shell panels (e.g. after
// fullscreen unhide or session unlock so DComp swapchains present fresh frames).
void winome_shell_panel_queue_redraw_all (void);

// Inspect registered panel HWNDs, Win32/DWM visibility state, and open popovers.
std::string winome_shell_panel_inspect_json (void);
