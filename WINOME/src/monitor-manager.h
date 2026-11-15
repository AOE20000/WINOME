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

// Start display/DPI/wallpaper/session watcher and secondary panels.
void monitor_manager_init(HWND primary_panel_hwnd,
                          const MonitorRuntimeCallbacks& callbacks);

// Re-sync primary & secondary panel placements with current displays/config.
void monitor_manager_sync();

// Destroy secondary panels, unhook session notifications, and close watcher.
void monitor_manager_shutdown();

}  // namespace winome
