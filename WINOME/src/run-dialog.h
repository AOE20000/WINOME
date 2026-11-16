// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// GNOME-style Alt+F2 "Run a Command" modal dialog (.run-dialog, port of
// gnome-shell's js/ui/runDialog.js), integrated with Developer Mode internal
// commands (lg / rt / r / debugexit).

#pragma once

#include <string>
#include <vector>

namespace winome {

enum class RunInternalAction {
  None = 0,
  OpenLookingGlass,
  ReloadTheme,
  RestartShell,
  DebugExit,
};

struct RunCommandParts {
  std::string executable;
  std::string arguments;
};

struct RunDialogCallbacks {
  bool (*open_looking_glass)() = nullptr;
  bool (*reload_theme)() = nullptr;
  void (*restart_shell)() = nullptr;
  void (*quit_shell)() = nullptr;
};

// Pure helpers covered independently by unit tests.
bool split_run_command(const std::string& raw, RunCommandParts* out);
RunInternalAction classify_run_internal_command(const std::string& trimmed,
                                                 bool developer_mode);
std::vector<std::string> run_history_add_item(std::vector<std::string> history,
                                              const std::string& cmd,
                                              size_t max_items = 32);
std::string run_complete_prefix(const std::string& prefix,
                                const std::vector<std::string>& candidates);

// Execute a command string (handles Developer Mode internal commands when
// unlocked, otherwise launches via ShellExecuteExW / terminal wrapper).
bool execute_run_command_string(const std::string& raw_cmd,
                                bool in_terminal,
                                const RunDialogCallbacks& cbs,
                                std::string* out_err);

void run_dialog_init(const RunDialogCallbacks& cbs);
void run_dialog_open();
void run_dialog_close();
void run_dialog_toggle();
void run_dialog_toggle_async();

}  // namespace winome
