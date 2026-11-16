// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// In-host GNOME Looking Glass style runtime control & debugging console.
// Activated strictly via Settings -> Developer Mode or `Alt+F2` -> `lg` when
// Developer Mode is enabled (zero dedicated global hotkey).

#pragma once

#include <string>

namespace winome {

struct LookingGlassCallbacks {
  std::string (*dispatch_cmd)(const std::string& json_req) = nullptr;
  void (*restack_windows)() = nullptr;
  bool (*reload_theme)() = nullptr;
  std::string (*plugin_list_json)() = nullptr;
  bool (*plugin_action)(const std::string& op,
                        const std::string& id,
                        std::string* out_err) = nullptr;
};

void looking_glass_init(const LookingGlassCallbacks& cbs);
bool open_looking_glass();
void close_looking_glass();
bool looking_glass_is_open();
void refresh_looking_glass_plugins();

}  // namespace winome
