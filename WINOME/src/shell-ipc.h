// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Bidirectional Named Pipe IPC server (\\.\pipe\WINOME_Shell_IPC) and
// lightweight JSON command dispatcher for WINOME shell control & diagnostics.

#pragma once

#include "virtual-desktop.h"

#include <string>
#include <vector>

namespace winome {

inline constexpr const wchar_t* kShellPipeName = L"\\\\.\\pipe\\WINOME_Shell_IPC";

struct IpcCallbacks {
  void (*overview_open)() = nullptr;
  void (*overview_close)() = nullptr;
  void (*overview_toggle)() = nullptr;
  void (*overview_app_grid)() = nullptr;
  bool (*overview_is_open)() = nullptr;
  void (*settings_open)(const std::string& page) = nullptr;
  void (*config_reload)() = nullptr;
  bool (*desktop_switch)(int index) = nullptr;
  std::vector<vd::DesktopInfo> (*desktop_list)() = nullptr;
  int (*desktop_active)() = nullptr;
  void (*notify)(const std::string& title,
                 const std::string& body,
                 const std::string& app) = nullptr;
  void (*quit)() = nullptr;
};

// Pure JSON command router tested independently of Named Pipe I/O.
std::string ipc_dispatch_request_json(const std::string& req_json,
                                      const IpcCallbacks& cbs);

// Starts the Named Pipe listener thread; invokes cbs on incoming commands.
bool ipc_server_start(const IpcCallbacks& cbs);
void ipc_server_stop();

// Client helper for --status / CLI tooling: sends one request and reads reply.
bool ipc_query_running_shell(const std::string& req_json,
                             std::string* out_resp,
                             int timeout_ms = 2000);

}  // namespace winome
