// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Safe Windows System Tray integration (classic ToolbarWindow32 + Win11 UIA).

#pragma once

#include <gtk/gtk.h>

#include <cstdint>
#include <string>
#include <vector>

namespace winome {

struct TrayIconEntry {
  uint64_t id = 0;
  std::string title;
  std::string app_hint;
  bool is_uia = false;
  std::vector<int> uia_runtime_id;
  uintptr_t target_hwnd = 0;
  uint32_t callback_msg = 0;
  uint32_t u_id = 0;
  uint32_t version = 0;
  int icon_width = 0;
  int icon_height = 0;
  std::vector<uint8_t> rgba_pixels;
};

using SystrayChangedCallback = void (*)(void* user_data);

// Lifecycle and async discovery on a dedicated MTA worker thread.
void systray_init();
void systray_shutdown();
void systray_refresh_async(bool force = false);

// Return a snapshot of discovered third-party system tray icons.
std::vector<TrayIconEntry> systray_get_items();

// Forward a primary or secondary click safely to the tray item.
void systray_activate_item(uint64_t id,
                           bool right_click,
                           int screen_x,
                           int screen_y);

// Ask Explorer to toggle its native notification overflow popup.
void systray_toggle_native_overflow();

// Subscribe/unsubscribe to snapshot changes (invoked on GTK main loop).
void systray_add_listener(SystrayChangedCallback cb, void* user_data);
void systray_remove_listener(SystrayChangedCallback cb, void* user_data);

// Create a GdkTexture from an entry's RGBA buffer, or nullptr if absent.
GdkTexture* systray_create_texture(const TrayIconEntry& entry);

// Pure filtering & icon-mapping helpers exposed for unit testing.
bool is_system_builtin_tray_item(const std::string& title,
                                 const std::string& app_hint);
const char* tray_fallback_icon_name(const std::string& title,
                                    const std::string& app_hint);

}  // namespace winome
