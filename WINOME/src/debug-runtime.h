// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Runtime debugging infrastructure: live log ring buffer, hot-toggleable
// diagnostic flags, Z-order/desktop window inspector, and JSON serializers.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace winome {

struct DebugLogEntry {
  uint64_t seq = 0;
  int64_t monotonic_us = 0;
  std::string tag;
  std::string text;
};

struct DebugFlags {
  bool zorder_trace = false;
  bool frame_overlay = false;
  bool hotkey_trace = false;
  bool ipc_trace = false;
};

struct InspectedWindowInfo {
  int z_index = 0;
  uintptr_t hwnd = 0;
  uint32_t pid = 0;
  int desktop_index = -1;
  bool is_own_process = false;
  bool topmost = false;
  bool cloaked = false;
  std::string class_name;
  std::string title;
  long left = 0;
  long top = 0;
  long right = 0;
  long bottom = 0;
};

// Installs GLib g_print / g_printerr capture handlers that tee output into
// the in-memory ring buffer while preserving stdout/stderr log output.
void debug_install_log_capture();

// Appends a message to the live ring buffer (auto-extracts "[tag]" prefix if
// tag is null/empty).
void debug_append_log(const char* tag, const std::string& text);

std::vector<DebugLogEntry> debug_get_logs_since(uint64_t after_seq,
                                                const std::string& tag_filter,
                                                size_t max_count);
uint64_t debug_latest_log_seq();
void debug_clear_logs();

DebugFlags debug_get_flags();
void debug_set_flags(const DebugFlags& flags);
bool debug_set_flag_by_name(const std::string& name, bool value);

// Enumerates visible top-level windows in current Z-order (topmost -> bottom).
std::vector<InspectedWindowInfo> debug_inspect_windows();

// JSON serializers for IPC / Looking Glass / winome-ctl.
std::string debug_logs_to_json(const std::vector<DebugLogEntry>& entries);
std::string debug_flags_to_json(const DebugFlags& flags);
std::string debug_windows_to_json(const std::vector<InspectedWindowInfo>& wins);

// Utility used by ring-buffer parsers & unit tests.
std::string debug_extract_log_tag(const std::string& line);

}  // namespace winome
