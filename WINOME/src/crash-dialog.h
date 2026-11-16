// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Crash recovery persistence helpers and pure-Win32 post-crash operation UI
// (zero GTK4/OpenGL dependency so recovery UI always displays reliably).

#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace winome {

struct RecoverySnapshot {
  bool valid = false;
  int32_t taskbar_state = 2;  // ABS_ALWAYSONTOP by default
  RECT primary_work{};
};

struct CrashState {
  std::string reason;
  uint32_t exit_code = 0;
  uint32_t pid = 0;
  std::string timestamp_utc;
  std::string minidump_path;
  std::string log_tail;
  bool taskbar_restored = false;
  bool safe_mode = false;
};

enum class CrashDialogAction {
  ExitKeepTaskbar = 0,
  RestartNormal,
  RestartSafeMode,
};

using CrashRestoreCallback = bool (*)(void* user_data);

struct CrashRecoveryDialogInput {
  CrashState state;
  std::wstring data_dir;
  bool zh = true;
  CrashRestoreCallback on_restore_now = nullptr;
  void* user_data = nullptr;
};

// Pure snapshot/crash-state serializers & parsers (covered by winome-tests).
std::string serialize_recovery_snapshot(const RecoverySnapshot& snap);
bool parse_recovery_snapshot(const std::string& raw, RecoverySnapshot* out);

std::string serialize_crash_state(const CrashState& state);
bool parse_crash_state(const std::string& json, CrashState* out);

// Allocation-minimal writer safe inside SEH / signal / terminate handlers.
void write_crash_state_emergency_file(const wchar_t* path,
                                      const char* reason,
                                      uint32_t code,
                                      uint32_t pid,
                                      const char* minidump_utf8,
                                      bool safe_mode);

std::string read_log_tail_utf8(const std::wstring& log_path,
                               size_t max_bytes = 3500);

// Blocks until the user chooses an action or closes the recovery window.
CrashDialogAction show_crash_recovery_dialog(
    const CrashRecoveryDialogInput& input);

}  // namespace winome
