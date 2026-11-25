// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Multi-monitor enumeration, display/DPI/wallpaper/session event watcher,
// and secondary panel lifecycle manager.

#pragma once

#include <gtk/gtk.h>
#include <windows.h>

#include <string>
#include <vector>

namespace winome {

struct MonitorDescriptor {
  HMONITOR hmonitor = nullptr;
  RECT monitor_rect{};
  RECT work_rect{};
  UINT dpi_x = 96;
  UINT dpi_y = 96;
  bool is_primary = false;
  std::wstring device_name;
};

struct MonitorRuntimeCallbacks {
  int (*panel_height)() = nullptr;
  GtkWidget* (*create_secondary_panel)() = nullptr;
  void (*register_panel)(HWND hwnd) = nullptr;
  void (*unregister_panel)(HWND hwnd) = nullptr;
  bool (*is_panel_hidden_for_monitor)(HMONITOR mon) = nullptr;
  void (*pin_window)(HWND hwnd) = nullptr;
  void (*on_session_lock_changed)(bool locked) = nullptr;
  void (*on_display_changed)(const RECT& primary_monitor_rect) = nullptr;
  void (*on_wallpaper_changed)() = nullptr;
  void (*on_theme_changed)() = nullptr;
  void (*on_workarea_changed)() = nullptr;
};

// Enumerate active display monitors (primary monitor sorted first).
std::vector<MonitorDescriptor> enumerate_monitors();

// Compute monitor work area with the top panel strip reserved.
RECT compute_panel_work_area(const RECT& monitor_rect, int panel_height);

// Compare two monitor topologies for geometry/DPI/primary changes.
bool same_monitor_topology(const std::vector<MonitorDescriptor>& a,
                           const std::vector<MonitorDescriptor>& b);

// Pure helper: find index of monitor containing (x, y), or -1 if none.
inline int select_monitor_index_for_point(const std::vector<RECT>& rects,
                                          int x,
                                          int y) {
  for (size_t i = 0; i < rects.size(); ++i) {
    const RECT& r = rects[i];
    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom)
      return static_cast<int>(i);
  }
  return -1;
}

// Pure helper: compute popover top-left inside a target monitor's coordinates
// (supports negative or extended positive virtual-screen offsets).
inline POINT clamp_popover_origin_to_monitor(const RECT& mon_rect,
                                             int panel_bottom,
                                             const RECT& anchor_rect,
                                             int width,
                                             int height,
                                             bool centered,
                                             int top_gap,
                                             int side_gap,
                                             int screen_gap) {
  const int raw_x = centered
                        ? ((anchor_rect.left + anchor_rect.right) / 2 - width / 2)
                        : (mon_rect.right - width - side_gap);
  const int raw_y = panel_bottom + top_gap;

  const int x_min = mon_rect.left + screen_gap;
  const int x_max = (mon_rect.right - width - screen_gap > x_min)
                        ? (mon_rect.right - width - screen_gap)
                        : x_min;
  const int y_min = panel_bottom + top_gap;
  const int y_max = (mon_rect.bottom - height - screen_gap > y_min)
                        ? (mon_rect.bottom - height - screen_gap)
                        : y_min;

  POINT out{};
  out.x = raw_x < x_min ? x_min : (raw_x > x_max ? x_max : raw_x);
  out.y = raw_y < y_min ? y_min : (raw_y > y_max ? y_max : raw_y);
  return out;
}

// Pure helper: center a dialog/overlay inside a monitor work area or bounds.
inline POINT center_rect_in_bounds(const RECT& bounds,
                                   int width,
                                   int height,
                                   double y_fraction = 0.5) {
  const int bw = bounds.right - bounds.left;
  const int bh = bounds.bottom - bounds.top;
  POINT out{};
  out.x = bounds.left + (bw - width) / 2;
  out.y = bounds.top + static_cast<int>((bh - height) * y_fraction);
  return out;
}

// Primary monitor handle (from primary panel HWND or origin fallback).
HMONITOR monitor_manager_primary_monitor();

// Active monitor handle (GNOME layoutManager.currentMonitor: pointer monitor,
// falling back to foreground window monitor, then primary monitor).
HMONITOR monitor_manager_active_monitor();

// Active monitor work area (falls back to SPI_GETWORKAREA if unavailable).
RECT monitor_manager_active_workarea();

// Panel HWND (primary or secondary) hosted on @mon, or nullptr if none.
HWND monitor_manager_panel_for_monitor(HMONITOR mon);

// True if @hwnd is the primary or any managed secondary panel window.
bool monitor_manager_is_panel_hwnd(HWND hwnd);

// Pure helper: center a newly launched window onto @dst_work while preserving
// its intended size (clamped to @dst_work). Used by winome-tests.
inline RECT compute_launched_window_rect(const RECT& src_rect,
                                         const RECT& dst_work) {
  const int work_w =
      (dst_work.right > dst_work.left) ? (dst_work.right - dst_work.left) : 800;
  const int work_h =
      (dst_work.bottom > dst_work.top) ? (dst_work.bottom - dst_work.top) : 600;
  int w = src_rect.right - src_rect.left;
  int h = src_rect.bottom - src_rect.top;
  if (w <= 0)
    w = (work_w * 3) / 4;
  if (h <= 0)
    h = (work_h * 3) / 4;
  if (w > work_w)
    w = work_w;
  if (h > work_h)
    h = work_h;
  RECT out{};
  out.left = dst_work.left + (work_w - w) / 2;
  out.top = dst_work.top + (work_h - h) / 2;
  out.right = out.left + w;
  out.bottom = out.top + h;
  return out;
}

// Resolve the HMONITOR containing @widget (falls back to active monitor).
HMONITOR monitor_manager_monitor_for_widget(GtkWidget* widget);

// Watch briefly for a newly opened app window and relocate it to @target_monitor
// if the OS/app placed its initial window on another display.
void watch_launched_window_for_monitor(HMONITOR target_monitor);

// Start display/DPI/wallpaper/session watcher and secondary panels.
void monitor_manager_init(HWND primary_panel_hwnd,
                          const MonitorRuntimeCallbacks& callbacks);

// Re-sync primary & secondary panel placements with current displays/config.
void monitor_manager_sync();

// Destroy secondary panels, unhook session notifications, and close watcher.
void monitor_manager_shutdown();

}  // namespace winome
