// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// GNOME-style Background Apps tracker merging Windows System Tray apps and
// windowless installed user applications.

#pragma once

#include "app-index.h"
#include "systray.h"

#include <gtk/gtk.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace winome {

enum class BackgroundAppKind : uint8_t {
  TrayApp = 0,
  BackgroundApp = 1,
};

struct BackgroundAppEntry {
  BackgroundAppKind kind = BackgroundAppKind::BackgroundApp;
  std::string key;
  std::string title;
  std::string detail;
  std::string tooltip;
  std::string app_id;
  std::string exe_lower;
  uint64_t tray_id = 0;
  uint32_t pid = 0;
  uintptr_t hwnd = 0;
  std::vector<uint32_t> pids;
  int icon_width = 0;
  int icon_height = 0;
  std::vector<uint8_t> rgba_pixels;
  std::string fallback_icon_name;
};

struct RawProcessInfo {
  uint32_t pid = 0;
  std::string exe_base_lower;
};

struct TrayOwnerMeta {
  uint32_t pid = 0;
  std::string exe_base_lower;
};

using BackgroundAppsChangedCallback = void (*)(void* user_data);

// Lifecycle and async discovery on a worker thread (never blocks GTK main loop).
void background_apps_init();
void background_apps_shutdown();
void background_apps_refresh_async(bool force = false);
std::vector<BackgroundAppEntry> background_apps_get_snapshot();

// Listeners are always invoked on the GTK main thread.
void background_apps_add_listener(BackgroundAppsChangedCallback cb,
                                  void* user_data);
void background_apps_remove_listener(BackgroundAppsChangedCallback cb,
                                     void* user_data);

// Texture helper for rows (tray RGBA buffer or matched app icon).
GdkTexture* background_apps_create_icon_texture(const BackgroundAppEntry& entry,
                                                int px);

// Primary click (activate/restore) or secondary click (tray context menu).
void background_apps_activate(const BackgroundAppEntry& entry,
                              bool secondary_menu,
                              int screen_x,
                              int screen_y);

// Graceful WM_CLOSE -> delayed TerminateProcess fallback on a worker thread.
void background_apps_quit_async(const BackgroundAppEntry& entry,
                                void (*on_done)(void*) = nullptr,
                                void* user_data = nullptr);

// Pure helpers exposed for deterministic unit testing.
std::string compact_tray_title_and_detail(const std::string& raw_title,
                                          const std::string& app_hint,
                                          std::string* out_detail = nullptr);
std::string format_background_app_subtitle(BackgroundAppKind kind,
                                           const std::string& detail,
                                           bool zh);
std::string format_background_apps_toggle_title(size_t total_count, bool zh);
std::string format_background_apps_header_subtitle(size_t tray_count,
                                                   size_t bg_count,
                                                   bool zh);
bool is_ignored_background_exe(const std::string& exe_base_lower);
std::vector<BackgroundAppEntry> merge_background_and_tray_apps(
    const std::vector<TrayIconEntry>& tray_items,
    const std::unordered_map<uint64_t, TrayOwnerMeta>& tray_owners,
    const std::vector<AppEntry>& installed_apps,
    const std::vector<RawProcessInfo>& running_procs,
    const std::vector<RunningWindow>& visible_windows,
    bool include_tray,
    uint32_t self_pid);

}  // namespace winome
