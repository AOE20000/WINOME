// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors

#include "debug-runtime.h"

#include "virtual-desktop.h"

#include <dwmapi.h>
#include <glib.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace winome {
namespace {

constexpr size_t kMaxLogRingEntries = 512;

struct DebugStore {
  std::mutex mu;
  std::deque<DebugLogEntry> logs;
  uint64_t next_seq = 1;
  DebugFlags flags{};
  bool handlers_installed = false;
  std::string pending_stdout;
  std::string pending_stderr;
};

DebugStore& dstore() {
  static DebugStore s;
  return s;
}

void append_json_escaped(const std::string& s, std::string* out) {
  out->push_back('"');
  for (unsigned char ch : s) {
    switch (ch) {
      case '"':  *out += "\\\""; break;
      case '\\': *out += "\\\\"; break;
      case '\b': *out += "\\b"; break;
      case '\f': *out += "\\f"; break;
      case '\n': *out += "\\n"; break;
      case '\r': *out += "\\r"; break;
      case '\t': *out += "\\t"; break;
      default:
        if (ch < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
          *out += buf;
        } else {
          out->push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  out->push_back('"');
}

void push_line_locked(DebugStore& s, const char* explicit_tag, std::string line) {
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
    line.pop_back();
  if (line.empty())
    return;

  DebugLogEntry e;
  e.seq = s.next_seq++;
  e.monotonic_us = g_get_monotonic_time();
  e.tag = (explicit_tag != nullptr && explicit_tag[0] != '\0')
              ? std::string(explicit_tag)
              : debug_extract_log_tag(line);
  e.text = std::move(line);
  s.logs.push_back(std::move(e));
  while (s.logs.size() > kMaxLogRingEntries)
    s.logs.pop_front();
}

void flush_partial_locked(DebugStore& s, std::string* pending, const char* chunk,
                          const char* default_tag) {
  if (chunk == nullptr)
    return;
  pending->append(chunk);
  size_t pos = 0;
  while (true) {
    size_t nl = pending->find('\n', pos);
    if (nl == std::string::npos)
      break;
    std::string line = pending->substr(pos, nl - pos);
    push_line_locked(s, default_tag, std::move(line));
    pos = nl + 1;
  }
  if (pos > 0)
    pending->erase(0, pos);
  if (pending->size() > 4096) {
    push_line_locked(s, default_tag, *pending);
    pending->clear();
  }
}

void glib_print_tee(const gchar* text) {
  if (text != nullptr) {
    std::fputs(text, stdout);
    std::fflush(stdout);
    auto& s = dstore();
    std::lock_guard<std::mutex> lock(s.mu);
    flush_partial_locked(s, &s.pending_stdout, text, nullptr);
  }
}

void glib_printerr_tee(const gchar* text) {
  if (text != nullptr) {
    std::fputs(text, stderr);
    std::fflush(stderr);
    auto& s = dstore();
    std::lock_guard<std::mutex> lock(s.mu);
    flush_partial_locked(s, &s.pending_stderr, text, nullptr);
  }
}

std::string wide_to_utf8(const wchar_t* w) {
  if (w == nullptr || w[0] == L'\0')
    return {};
  int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (need <= 1)
    return {};
  std::string out(static_cast<size_t>(need - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), need, nullptr, nullptr);
  return out;
}

struct EnumWinCtx {
  std::vector<InspectedWindowInfo>* out = nullptr;
  DWORD own_pid = 0;
  int next_z = 0;
};

#ifndef DWMWA_CLOAKED
constexpr DWORD kDwmCloakedAttr = 14;
#else
constexpr DWORD kDwmCloakedAttr = DWMWA_CLOAKED;
#endif

BOOL CALLBACK enum_inspect_proc(HWND hwnd, LPARAM lp) {
  auto* ctx = reinterpret_cast<EnumWinCtx*>(lp);
  if (!IsWindowVisible(hwnd))
    return TRUE;

  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  if ((style & WS_CHILD) != 0)
    return TRUE;

  wchar_t cls[128] = {};
  wchar_t title[256] = {};
  GetClassNameW(hwnd, cls, 128);
  GetWindowTextW(hwnd, title, 256);

  DWORD cloaked = 0;
  DwmGetWindowAttribute(hwnd, kDwmCloakedAttr, &cloaked, sizeof(cloaked));

  RECT rc{};
  GetWindowRect(hwnd, &rc);

  InspectedWindowInfo info;
  info.z_index = ctx->next_z++;
  info.hwnd = reinterpret_cast<uintptr_t>(hwnd);
  info.pid = static_cast<uint32_t>(pid);
  info.is_own_process = (pid == ctx->own_pid);
  info.topmost = (ex & WS_EX_TOPMOST) != 0;
  info.cloaked = (cloaked != 0);
  info.desktop_index = info.is_own_process ? -1 : vd::window_desktop_index(hwnd);
  info.class_name = wide_to_utf8(cls);
  info.title = wide_to_utf8(title);
  info.left = rc.left;
  info.top = rc.top;
  info.right = rc.right;
  info.bottom = rc.bottom;

  if (ctx->out->size() < 200)
    ctx->out->push_back(std::move(info));
  return TRUE;
}

}  // namespace

std::string debug_extract_log_tag(const std::string& line) {
  if (line.size() >= 3 && line[0] == '[') {
    const size_t close = line.find(']');
    if (close != std::string::npos && close > 1 && close <= 32)
      return line.substr(1, close - 1);
  }
  return "misc";
}

void debug_install_log_capture() {
  auto& s = dstore();
  std::lock_guard<std::mutex> lock(s.mu);
  if (s.handlers_installed)
    return;
  s.handlers_installed = true;
  g_set_print_handler(glib_print_tee);
  g_set_printerr_handler(glib_printerr_tee);
}

void debug_append_log(const char* tag, const std::string& text) {
  auto& s = dstore();
  std::lock_guard<std::mutex> lock(s.mu);
  push_line_locked(s, tag, text);
}

std::vector<DebugLogEntry> debug_get_logs_since(uint64_t after_seq,
                                                const std::string& tag_filter,
                                                size_t max_count) {
  auto& s = dstore();
  std::lock_guard<std::mutex> lock(s.mu);
  std::vector<DebugLogEntry> matched;
  for (const auto& e : s.logs) {
    if (e.seq <= after_seq)
      continue;
    if (!tag_filter.empty() && tag_filter != "all" &&
        e.tag.compare(0, tag_filter.size(), tag_filter) != 0) {
      continue;
    }
    matched.push_back(e);
  }
  if (max_count > 0 && matched.size() > max_count) {
    matched.erase(matched.begin(),
                  matched.begin() + static_cast<ptrdiff_t>(matched.size() - max_count));
  }
  return matched;
}

uint64_t debug_latest_log_seq() {
  auto& s = dstore();
  std::lock_guard<std::mutex> lock(s.mu);
  return s.next_seq - 1;
}

void debug_clear_logs() {
  auto& s = dstore();
  std::lock_guard<std::mutex> lock(s.mu);
  s.logs.clear();
  s.pending_stdout.clear();
  s.pending_stderr.clear();
}

DebugFlags debug_get_flags() {
  auto& s = dstore();
  std::lock_guard<std::mutex> lock(s.mu);
  return s.flags;
}

void debug_set_flags(const DebugFlags& flags) {
  auto& s = dstore();
  std::lock_guard<std::mutex> lock(s.mu);
  s.flags = flags;
}

bool debug_set_flag_by_name(const std::string& name, bool value) {
  auto& s = dstore();
  std::lock_guard<std::mutex> lock(s.mu);
  if (name == "zorder_trace") {
    s.flags.zorder_trace = value;
    return true;
  }
  if (name == "frame_overlay") {
    s.flags.frame_overlay = value;
    return true;
  }
  if (name == "hotkey_trace") {
    s.flags.hotkey_trace = value;
    return true;
  }
  if (name == "ipc_trace") {
    s.flags.ipc_trace = value;
    return true;
  }
  return false;
}

std::vector<InspectedWindowInfo> debug_inspect_windows() {
  std::vector<InspectedWindowInfo> wins;
  EnumWinCtx ctx;
  ctx.out = &wins;
  ctx.own_pid = GetCurrentProcessId();
  EnumWindows(enum_inspect_proc, reinterpret_cast<LPARAM>(&ctx));
  return wins;
}

std::string debug_logs_to_json(const std::vector<DebugLogEntry>& entries) {
  std::string out = "{\"latest_seq\":";
  out += std::to_string(debug_latest_log_seq());
  out += ",\"entries\":[";
  char num[128];
  for (size_t i = 0; i < entries.size(); ++i) {
    if (i > 0)
      out += ",";
    const DebugLogEntry& e = entries[i];
    std::snprintf(num, sizeof(num), "{\"seq\":%llu,\"us\":%lld,\"tag\":",
                  static_cast<unsigned long long>(e.seq),
                  static_cast<long long>(e.monotonic_us));
    out += num;
    append_json_escaped(e.tag, &out);
    out += ",\"text\":";
    append_json_escaped(e.text, &out);
    out += "}";
  }
  out += "]}";
  return out;
}

std::string debug_flags_to_json(const DebugFlags& flags) {
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "{\"zorder_trace\":%s,\"frame_overlay\":%s,"
                "\"hotkey_trace\":%s,\"ipc_trace\":%s}",
                flags.zorder_trace ? "true" : "false",
                flags.frame_overlay ? "true" : "false",
                flags.hotkey_trace ? "true" : "false",
                flags.ipc_trace ? "true" : "false");
  return buf;
}

std::string debug_windows_to_json(const std::vector<InspectedWindowInfo>& wins) {
  std::string out = "{\"count\":";
  out += std::to_string(wins.size());
  out += ",\"windows\":[";
  char num[256];
  for (size_t i = 0; i < wins.size(); ++i) {
    if (i > 0)
      out += ",";
    const auto& w = wins[i];
    std::snprintf(
        num, sizeof(num),
        "{\"z\":%d,\"hwnd\":\"0x%llX\",\"pid\":%lu,\"desktop\":%d,"
        "\"own\":%s,\"topmost\":%s,\"cloaked\":%s,"
        "\"rect\":[%ld,%ld,%ld,%ld],\"class\":",
        w.z_index, static_cast<unsigned long long>(w.hwnd),
        static_cast<unsigned long>(w.pid), w.desktop_index,
        w.is_own_process ? "true" : "false",
        w.topmost ? "true" : "false",
        w.cloaked ? "true" : "false",
        w.left, w.top, w.right, w.bottom);
    out += num;
    append_json_escaped(w.class_name, &out);
    out += ",\"title\":";
    append_json_escaped(w.title, &out);
    out += "}";
  }
  out += "]}";
  return out;
}

}  // namespace winome
