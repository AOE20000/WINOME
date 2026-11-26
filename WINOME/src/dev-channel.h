// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Universal Developer-Mode Debug Operation Channel (dev.help / dev.op / dev.batch).
// Strictly gated on config.developer_mode; provides direct in-process access to
// OS input simulation, Win32 window control, GTK4 live widget/CSS introspection,
// WINOME subsystem hooks, telemetry snapshots, and log-capturing batch pipelines.

#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace winome {

struct DevOpRequest {
  std::string op;
  std::map<std::string, std::string> args;
};

// Host-provided callback for WINOME shell-specific operations (shell.*).
// Returns true on success and populates *out_data_json (a JSON value/object,
// defaults to "{}" if left empty) or *out_err on failure.
using DevShellOpCallback = std::function<bool(
    const std::string& op,
    const std::map<std::string, std::string>& args,
    std::string* out_data_json,
    std::string* out_err)>;

// Pure helper: resolve a key name ("LWin", "Esc", "F2", "A", "0x5B", etc.) to a
// Windows Virtual-Key code (0 if unknown).
unsigned dev_resolve_vk(const std::string& key_name);

// Pure helper: parse a shorthand op string like "key.combo keys=Win+Tab hold_ms=30"
// or "sleep 500" into a DevOpRequest.
bool dev_parse_op_shorthand(const std::string& text,
                            DevOpRequest* out,
                            std::string* out_err = nullptr);

// Pure helper: split a ';'-delimited batch shorthand string into DevOpRequests.
bool dev_parse_batch_shorthand(const std::string& text,
                               std::vector<DevOpRequest>* out_ops,
                               std::string* out_err = nullptr);

// Returns a JSON object describing all supported universal debug operations.
std::string dev_channel_help_json();

// Executes a single developer-mode debug operation. Returns a complete JSON
// response envelope {"ok":true/false, ...}. Requires config.developer_mode.
std::string dev_channel_exec_op(const DevOpRequest& req,
                                int wait_ms,
                                bool capture_logs,
                                const std::string& tag_filter,
                                const DevShellOpCallback& shell_cb);

// Executes a sequence of developer-mode debug operations, optionally capturing
// per-step + aggregated logs, animation telemetry, and memory deltas.
std::string dev_channel_exec_batch(const std::vector<DevOpRequest>& ops,
                                   bool capture_logs,
                                   const std::string& tag_filter,
                                   bool stop_on_error,
                                   const DevShellOpCallback& shell_cb);

// Cleanup live injected debug CSS provider on shell shutdown (safe if unused).
void dev_channel_shutdown();

}  // namespace winome
