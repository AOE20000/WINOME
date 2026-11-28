// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Bidirectional Named Pipe IPC server (\\.\pipe\WINOME_Shell_IPC) with
// per-user SID Medium-IL DACL and extensible JSON runtime control protocol.

#pragma once

#include "dev-channel.h"
#include "virtual-desktop.h"

#include <windows.h>

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
  bool (*config_set)(const std::string& key, const std::string& val) = nullptr;
  bool (*desktop_switch)(int index) = nullptr;
  std::vector<vd::DesktopInfo> (*desktop_list)() = nullptr;
  int (*desktop_active)() = nullptr;
  bool (*desktop_create)() = nullptr;
  bool (*desktop_remove)(int index) = nullptr;
  void (*notify)(const std::string& title,
                 const std::string& body,
                 const std::string& app) = nullptr;
  bool (*theme_reload)() = nullptr;
  bool (*looking_glass_open)() = nullptr;
  bool (*looking_glass_close)() = nullptr;
  std::string (*inspect_panel_json)() = nullptr;
  DWORD (*watchdog_pid)() = nullptr;
  bool (*run_exec)(const std::string& cmdline, std::string* out_err) = nullptr;
  std::string (*plugin_list_json)() = nullptr;
  bool (*plugin_action)(const std::string& op,
                        const std::string& id,
                        std::string* out_err) = nullptr;
  // Developer-mode test harness action dispatcher (e.g. win_tap_os, win_tap,
  // esc_os, overview_open/close/toggle/grid, hot_corner, desktop_next/prev).
  bool (*dev_test_action)(const std::string& action,
                          std::string* out_err) = nullptr;
  // Universal developer-mode shell subsystem operation dispatcher (shell.*).
  DevShellOpCallback dev_shell_op;
  void (*quit)() = nullptr;
};

// Pure developer-test step parser & preset expander (unit-tested independently).
bool ipc_expand_dev_test_steps(const std::string& steps_csv,
                               const std::string& preset,
                               int gap_ms,
                               std::vector<std::string>* out_steps,
                               std::string* out_err);

// Pure SDDL builder & DACL helper tested independently of live pipes.
std::wstring ipc_build_user_pipe_sddl(const wchar_t* user_sid_str);

// Pure JSON command router tested independently of Named Pipe I/O.
std::string ipc_dispatch_request_json(const std::string& req_json,
                                      const IpcCallbacks& cbs);

// Starts the Named Pipe listener thread; invokes cbs on incoming commands.
bool ipc_server_start(const IpcCallbacks& cbs);
void ipc_server_stop();

// Client helper for --status / winome-ctl / tests.
bool ipc_query_running_shell(const std::string& req_json,
                             std::string* out_resp,
                             int timeout_ms = 2000);

}  // namespace winome
