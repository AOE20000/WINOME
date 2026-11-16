// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Out-of-process stdio JSON plugin host with Win32 Job Object isolation,
// capability ACLs, rate limiting, and crash-loop circuit breaker.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace winome {

enum class PluginState {
  Stopped = 0,
  Starting,
  Running,
  Tripped,
  Disabled,
};

struct PluginManifest {
  std::string id;
  std::string name;
  std::string version = "0.0.0";
  std::string exec;
  std::vector<std::string> args;
  std::vector<std::string> capabilities;
  int max_memory_mb = 64;
  bool enabled = true;
  std::wstring dir;
};

struct PluginTopBarSpec {
  std::string key;        // "<plugin_id>" or "<plugin_id>:<slot>"
  std::string plugin_id;
  std::string slot;
  std::string icon;
  std::string text;
  std::string tooltip;
};

struct PluginQuickToggleSpec {
  std::string key;        // "<plugin_id>" or "<plugin_id>:<slot>"
  std::string plugin_id;
  std::string slot;
  std::string icon;
  std::string title;
  std::string subtitle;
  bool active = false;
};

struct PluginHostCallbacks {
  void (*topbar_upsert)(const PluginTopBarSpec& spec) = nullptr;
  void (*topbar_remove)(const std::string& plugin_id,
                        const std::string& key) = nullptr;
  void (*quicktoggle_upsert)(const PluginQuickToggleSpec& spec) = nullptr;
  void (*quicktoggle_remove)(const std::string& plugin_id,
                             const std::string& key) = nullptr;
  void (*notify)(const std::string& title,
                 const std::string& body,
                 const std::string& app_name) = nullptr;
  void (*plugins_changed)() = nullptr;
};

// Rate limiter bounding incoming messages per monotonic time window.
class PluginRateLimiter {
 public:
  PluginRateLimiter(int max_messages = 30, int64_t window_ms = 1000);
  bool allow(int64_t now_ms);
  void reset();
  uint64_t dropped_count() const { return dropped_; }

 private:
  int max_messages_;
  int64_t window_ms_;
  int64_t window_start_ms_ = 0;
  int count_in_window_ = 0;
  uint64_t dropped_ = 0;
};

// Crash tracker implementing N crashes in W ms circuit-breaker semantics.
class PluginCrashTracker {
 public:
  PluginCrashTracker(int threshold = 3, int64_t window_ms = 60000);
  // Record crash at now_ms; returns true when breaker should trip.
  bool record_crash(int64_t now_ms);
  int recent_count(int64_t now_ms);
  void reset();

 private:
  int threshold_;
  int64_t window_ms_;
  std::vector<int64_t> timestamps_;
};

const char* plugin_state_name(PluginState s);
bool plugin_has_capability(const PluginManifest& m, const char* cap);

bool parse_plugin_manifest(const std::string& json_text,
                           const std::wstring& plugin_dir,
                           PluginManifest* out,
                           std::string* out_err);

std::wstring plugin_host_directory();
void plugin_scan_directory(const std::wstring& root_dir,
                           std::vector<PluginManifest>* out_manifests,
                           std::vector<std::string>* out_errors);

void plugin_host_init(const PluginHostCallbacks& cbs);
void plugin_host_shutdown();
bool plugin_host_reload(std::string* out_err = nullptr);

// Control operations: start, stop, restart, enable, disable, reset, reload, ping.
bool plugin_host_action(const std::string& op,
                        const std::string& plugin_id,
                        std::string* out_err);

std::string plugin_host_list_json();

// Outbound shell -> plugin events sent over child stdin.
void plugin_host_emit_topbar_click(const std::string& plugin_id,
                                   const std::string& slot);
void plugin_host_emit_quicktoggle(const std::string& plugin_id,
                                  const std::string& slot,
                                  bool active);
void plugin_host_emit_desktop_changed(int count, int active);

}  // namespace winome
