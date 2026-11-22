// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Persistent JSON user configuration stored under %LOCALAPPDATA%\WINOME\config.json.

#pragma once

#include <string>
#include <vector>

namespace winome {

struct WinomeConfig {
  std::string language = "auto";      // "auto", "zh", "en"
  std::string clock_format = "auto";  // "auto", "24h", "12h"
  bool clock_show_seconds = false;
  bool show_battery_percentage = false;
  bool hot_corner_enabled = true;
  int hot_corner_cooldown_ms = 500;
  bool win_key_overview = true;
  bool overview_window_blacklist = true;
  std::string live_wallpaper_mode = "live";  // "live", "snapshot", "off"
  bool dash_isolate_workspaces = false;  // false = all workspaces, true = active workspace only
  bool multi_monitor_panels = true;
  bool end_session_confirm = true;
  bool unify_dark_titlebars = false;
  bool unify_rounded_corners = false;
  bool developer_mode = false;
  std::vector<std::string> pinned_apps;
  std::vector<std::string> disabled_plugins;
};

// Runtime-only Safe Mode flag (enabled via --safe-mode; disables third-party
// plugins and opt-in DWM window theming for emergency recovery without
// overwriting user preferences in config.json).
bool is_safe_mode();
void set_safe_mode(bool safe);

using ConfigListener = void (*)(void* user_data);

// Return the active runtime configuration (loads from disk on first access).
const WinomeConfig& get_config();

// Replace runtime configuration in memory, persist to disk, and notify
// listeners. Returns false if writing to disk failed (memory state still updates).
bool update_config(const WinomeConfig& cfg);

// Reload configuration from disk and notify listeners on change.
bool reload_config();

// Register a callback invoked (on caller thread) when configuration changes.
void add_config_listener(ConfigListener listener, void* user_data);
void remove_config_listener(ConfigListener listener, void* user_data);

// Pure serialization/parsing helpers exposed for automated unit testing.
std::string serialize_config(const WinomeConfig& cfg);
bool parse_config(const std::string& json, WinomeConfig* out);

}  // namespace winome
