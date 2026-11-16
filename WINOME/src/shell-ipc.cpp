// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Bidirectional Named Pipe IPC server and JSON dispatcher for WINOME shell.

#include "shell-ipc.h"

#include "config.h"
#include "perf-metrics.h"

#include <windows.h>
#include <glib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace winome {
namespace {

constexpr size_t kMaxIpcPayloadBytes = 65536;

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

void skip_ws(const std::string& s, size_t* pos) {
  while (*pos < s.size() &&
         std::isspace(static_cast<unsigned char>(s[*pos]))) {
    ++(*pos);
  }
}

bool parse_json_str(const std::string& s, size_t* pos, std::string* out) {
  skip_ws(s, pos);
  if (*pos >= s.size() || s[*pos] != '"')
    return false;
  ++(*pos);
  std::string res;
  while (*pos < s.size()) {
    char ch = s[(*pos)++];
    if (ch == '"') {
      *out = std::move(res);
      return true;
    }
    if (ch == '\\') {
      if (*pos >= s.size())
        return false;
      char esc = s[(*pos)++];
      switch (esc) {
        case '"':  res.push_back('"'); break;
        case '\\': res.push_back('\\'); break;
        case '/':  res.push_back('/'); break;
        case 'b':  res.push_back('\b'); break;
        case 'f':  res.push_back('\f'); break;
        case 'n':  res.push_back('\n'); break;
        case 'r':  res.push_back('\r'); break;
        case 't':  res.push_back('\t'); break;
        default:   res.push_back(esc); break;
      }
    } else {
      res.push_back(ch);
    }
  }
  return false;
}

bool parse_json_int(const std::string& s, size_t* pos, int* out) {
  skip_ws(s, pos);
  size_t start = *pos;
  if (*pos < s.size() && (s[*pos] == '-' || s[*pos] == '+'))
    ++(*pos);
  size_t d0 = *pos;
  while (*pos < s.size() && std::isdigit(static_cast<unsigned char>(s[*pos])))
    ++(*pos);
  if (*pos == d0)
    return false;
  *out = std::atoi(s.substr(start, *pos - start).c_str());
  return true;
}

bool skip_val(const std::string& s, size_t* pos) {
  skip_ws(s, pos);
  if (*pos >= s.size())
    return false;
  char c = s[*pos];
  if (c == '"') {
    std::string dummy;
    return parse_json_str(s, pos, &dummy);
  }
  if (c == '{' || c == '[') {
    char open = c;
    char close = (c == '{') ? '}' : ']';
    int depth = 1;
    ++(*pos);
    while (*pos < s.size() && depth > 0) {
      if (s[*pos] == '"') {
        std::string dummy;
        if (!parse_json_str(s, pos, &dummy))
          return false;
        continue;
      }
      if (s[*pos] == open)
        ++depth;
      else if (s[*pos] == close)
        --depth;
      ++(*pos);
    }
    return depth == 0;
  }
  while (*pos < s.size() && s[*pos] != ',' && s[*pos] != '}' &&
         !std::isspace(static_cast<unsigned char>(s[*pos]))) {
    ++(*pos);
  }
  return true;
}

struct ParsedRequest {
  std::string cmd;
  std::string page;
  std::string title;
  std::string body;
  std::string app;
  bool has_index = false;
  int index = -1;
};

bool parse_flat_request(const std::string& json, ParsedRequest* out) {
  if (out == nullptr)
    return false;
  *out = ParsedRequest{};
  size_t pos = 0;
  skip_ws(json, &pos);
  if (pos >= json.size() || json[pos] != '{')
    return false;
  ++pos;
  skip_ws(json, &pos);
  if (pos < json.size() && json[pos] == '}')
    return true;

  for (;;) {
    std::string key;
    if (!parse_json_str(json, &pos, &key))
      return false;
    skip_ws(json, &pos);
    if (pos >= json.size() || json[pos] != ':')
      return false;
    ++pos;

    bool ok = true;
    if (key == "cmd") {
      ok = parse_json_str(json, &pos, &out->cmd);
    } else if (key == "page") {
      ok = parse_json_str(json, &pos, &out->page);
    } else if (key == "title") {
      ok = parse_json_str(json, &pos, &out->title);
    } else if (key == "body") {
      ok = parse_json_str(json, &pos, &out->body);
    } else if (key == "app") {
      ok = parse_json_str(json, &pos, &out->app);
    } else if (key == "index") {
      ok = parse_json_int(json, &pos, &out->index);
      out->has_index = ok;
    } else {
      ok = skip_val(json, &pos);
    }
    if (!ok)
      return false;

    skip_ws(json, &pos);
    if (pos < json.size() && json[pos] == ',') {
      ++pos;
      continue;
    }
    if (pos < json.size() && json[pos] == '}') {
      return true;
    }
    return false;
  }
}

std::string json_error(const std::string& msg) {
  std::string out = "{\"ok\":false,\"error\":";
  append_json_escaped(msg, &out);
  out += "}";
  return out;
}

std::string format_guid_string(const GUID& g) {
  char buf[48];
  std::snprintf(buf, sizeof(buf),
                "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                static_cast<unsigned long>(g.Data1), g.Data2, g.Data3,
                g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
  return buf;
}

std::string build_status_json(const IpcCallbacks& cbs) {
  const StartupTimings st = perf_get_startup_timings();
  const ProcessTelemetry tel = perf_sample_process_telemetry();
  const FrameTimeTracker::Stats frames = perf_get_overview_frame_stats();
  const auto caps = capability_snapshot_all();
  const vd::DesktopSnapshot ds = vd::cached_snapshot();
  const bool ov_open = cbs.overview_is_open ? cbs.overview_is_open() : false;

  char num[256];
  std::string out = "{\"ok\":true,\"result\":{\"version\":\"0.1.0\",";
  out += ov_open ? "\"overview_open\":true," : "\"overview_open\":false,";

  std::snprintf(num, sizeof(num),
                "\"desktops\":{\"valid\":%s,\"count\":%d,\"active\":%d},",
                ds.valid ? "true" : "false", ds.count, ds.active_index);
  out += num;

  std::snprintf(
      num, sizeof(num),
      "\"startup\":{\"total_ms\":%.1f,\"fontmap_ms\":%.1f,\"panel_ms\":%.1f,"
      "\"overview_init_ms\":%.1f,\"ipc_ms\":%.1f,"
      "\"fonts_loaded_files\":%d,\"fonts_family_count\":%d},",
      st.total_startup_ms, st.fontmap_ms, st.panel_ms, st.overview_init_ms,
      st.ipc_ms, st.fonts_loaded_files, st.fonts_family_count);
  out += num;

  std::snprintf(
      num, sizeof(num),
      "\"telemetry\":{\"working_set_bytes\":%llu,\"private_bytes\":%llu,"
      "\"gdi_objects\":%lu,\"user_objects\":%lu},",
      static_cast<unsigned long long>(tel.working_set_bytes),
      static_cast<unsigned long long>(tel.private_bytes),
      static_cast<unsigned long>(tel.gdi_objects),
      static_cast<unsigned long>(tel.user_objects));
  out += num;

  std::snprintf(
      num, sizeof(num),
      "\"overview_frames\":{\"frames\":%llu,\"dropped\":%llu,"
      "\"avg_ms\":%.2f,\"p95_ms\":%.2f,\"max_ms\":%.2f},",
      static_cast<unsigned long long>(frames.frames),
      static_cast<unsigned long long>(frames.dropped_frames),
      frames.avg_frame_ms, frames.p95_frame_ms, frames.max_frame_ms);
  out += num;

  out += "\"capabilities\":[";
  for (size_t i = 0; i < caps.size(); ++i) {
    if (i > 0)
      out += ",";
    const CapabilityStatus& c = caps[i];
    out += "{\"name\":";
    append_json_escaped(c.name ? c.name : "", &out);
    out += ",\"state\":";
    append_json_escaped(capability_state_label(c.state), &out);
    std::snprintf(num, sizeof(num), ",\"failures\":%lu,\"detail\":",
                  static_cast<unsigned long>(c.failure_count));
    out += num;
    append_json_escaped(c.detail, &out);
    out += "}";
  }
  out += "]}}";
  return out;
}

struct ServerState {
  std::mutex mu;
  IpcCallbacks cbs;
  std::thread thread;
  HANDLE stop_event = nullptr;
  HANDLE ready_event = nullptr;
  std::atomic<bool> running{false};
};

ServerState g_ipc;

bool read_pipe_message(HANDLE pipe, HANDLE stop_event, std::string* out) {
  out->clear();
  HANDLE read_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (read_ev == nullptr)
    return false;

  char buf[4096];
  bool done = false;
  while (!done && out->size() < kMaxIpcPayloadBytes) {
    ResetEvent(read_ev);
    OVERLAPPED ov{};
    ov.hEvent = read_ev;
    DWORD got = 0;
    BOOL ok = ReadFile(pipe, buf, sizeof(buf), &got, &ov);
    if (!ok) {
      DWORD err = GetLastError();
      if (err == ERROR_IO_PENDING) {
        HANDLE waits[2] = {read_ev, stop_event};
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, 2000);
        if (w != WAIT_OBJECT_0) {
          CancelIo(pipe);
          CloseHandle(read_ev);
          return false;
        }
        if (!GetOverlappedResult(pipe, &ov, &got, FALSE)) {
          err = GetLastError();
          if (err == ERROR_BROKEN_PIPE)
            break;
          CloseHandle(read_ev);
          return !out->empty();
        }
      } else if (err == ERROR_BROKEN_PIPE) {
        break;
      } else {
        CloseHandle(read_ev);
        return false;
      }
    }
    if (got == 0)
      break;
    for (DWORD i = 0; i < got; ++i) {
      if (buf[i] == '\n') {
        done = true;
        break;
      }
      out->push_back(buf[i]);
    }
  }
  CloseHandle(read_ev);
  return !out->empty();
}

void write_pipe_reply(HANDLE pipe, const std::string& payload) {
  std::string framed = payload;
  if (framed.empty() || framed.back() != '\n')
    framed.push_back('\n');

  HANDLE write_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (write_ev == nullptr)
    return;
  OVERLAPPED ov{};
  ov.hEvent = write_ev;
  DWORD written = 0;
  BOOL ok = WriteFile(pipe, framed.data(),
                      static_cast<DWORD>(framed.size()), &written, &ov);
  if (!ok && GetLastError() == ERROR_IO_PENDING) {
    if (WaitForSingleObject(write_ev, 1500) == WAIT_OBJECT_0)
      GetOverlappedResult(pipe, &ov, &written, FALSE);
    else
      CancelIo(pipe);
  }
  CloseHandle(write_ev);
  FlushFileBuffers(pipe);
}

HANDLE create_pipe_instance() {
  return CreateNamedPipeW(
      kShellPipeName,
      PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      4,
      kMaxIpcPayloadBytes,
      kMaxIpcPayloadBytes,
      1000,
      nullptr);
}

bool start_connect(HANDLE pipe, HANDLE ev, OVERLAPPED* ov, bool* out_pending) {
  ResetEvent(ev);
  *ov = OVERLAPPED{};
  ov->hEvent = ev;
  BOOL connected = ConnectNamedPipe(pipe, ov);
  if (connected) {
    *out_pending = false;
    return true;
  }
  const DWORD err = GetLastError();
  if (err == ERROR_IO_PENDING) {
    *out_pending = true;
    return true;
  }
  if (err == ERROR_PIPE_CONNECTED) {
    *out_pending = false;
    return true;
  }
  *out_pending = false;
  return false;
}

struct PipeSlot {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  HANDLE ev = nullptr;
  OVERLAPPED ov{};
  bool armed = false;
  bool pending = false;
};

void server_thread_main() {
  PipeSlot slots[2]{};
  slots[0].ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  slots[1].ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (slots[0].ev == nullptr || slots[1].ev == nullptr) {
    if (slots[0].ev != nullptr)
      CloseHandle(slots[0].ev);
    if (slots[1].ev != nullptr)
      CloseHandle(slots[1].ev);
    capability_set(SubsystemId::IpcServer, CapabilityState::Tripped,
                   "Failed to create IPC overlapped event");
    if (g_ipc.ready_event != nullptr)
      SetEvent(g_ipc.ready_event);
    return;
  }

  int cur_idx = 0;
  bool signaled_ready = false;

  while (g_ipc.running.load(std::memory_order_acquire)) {
    PipeSlot& s = slots[cur_idx];
    PipeSlot& next = slots[1 - cur_idx];

    if (s.pipe == INVALID_HANDLE_VALUE) {
      s.pipe = create_pipe_instance();
      s.armed = false;
      s.pending = false;
      if (s.pipe == INVALID_HANDLE_VALUE) {
        capability_record_failure(SubsystemId::IpcServer,
                                  "CreateNamedPipeW failed");
        if (WaitForSingleObject(g_ipc.stop_event, 500) == WAIT_OBJECT_0)
          break;
        continue;
      }
    }

    if (!s.armed) {
      if (!start_connect(s.pipe, s.ev, &s.ov, &s.pending)) {
        CloseHandle(s.pipe);
        s.pipe = INVALID_HANDLE_VALUE;
        if (WaitForSingleObject(g_ipc.stop_event, 50) == WAIT_OBJECT_0)
          break;
        continue;
      }
      s.armed = true;
    }

    capability_set(SubsystemId::IpcServer, CapabilityState::Active);
    if (!signaled_ready) {
      signaled_ready = true;
      if (g_ipc.ready_event != nullptr)
        SetEvent(g_ipc.ready_event);
    }

    if (s.pending) {
      HANDLE waits[2] = {s.ev, g_ipc.stop_event};
      DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
      if (w != WAIT_OBJECT_0)
        break;
      DWORD dummy = 0;
      if (!GetOverlappedResult(s.pipe, &s.ov, &dummy, FALSE)) {
        CancelIo(s.pipe);
        CloseHandle(s.pipe);
        s.pipe = INVALID_HANDLE_VALUE;
        s.armed = false;
        s.pending = false;
        continue;
      }
    }

    if (!g_ipc.running.load(std::memory_order_acquire))
      break;

    // Pre-create and arm the standby listening instance at a stable OVERLAPPED
    // address BEFORE replying and disconnecting s.pipe, eliminating gaps.
    if (next.pipe == INVALID_HANDLE_VALUE) {
      next.pipe = create_pipe_instance();
      next.armed = false;
      next.pending = false;
      if (next.pipe != INVALID_HANDLE_VALUE) {
        if (start_connect(next.pipe, next.ev, &next.ov, &next.pending)) {
          next.armed = true;
        } else {
          CloseHandle(next.pipe);
          next.pipe = INVALID_HANDLE_VALUE;
        }
      }
    }

    std::string req;
    if (read_pipe_message(s.pipe, g_ipc.stop_event, &req)) {
      IpcCallbacks cbs_copy;
      {
        std::lock_guard<std::mutex> lock(g_ipc.mu);
        cbs_copy = g_ipc.cbs;
      }
      const std::string reply = ipc_dispatch_request_json(req, cbs_copy);
      write_pipe_reply(s.pipe, reply);
    }
    DisconnectNamedPipe(s.pipe);
    CloseHandle(s.pipe);
    s.pipe = INVALID_HANDLE_VALUE;
    s.armed = false;
    s.pending = false;

    cur_idx = 1 - cur_idx;
  }

  for (PipeSlot& slot : slots) {
    if (slot.pipe != INVALID_HANDLE_VALUE) {
      CancelIo(slot.pipe);
      DisconnectNamedPipe(slot.pipe);
      CloseHandle(slot.pipe);
      slot.pipe = INVALID_HANDLE_VALUE;
    }
    if (slot.ev != nullptr)
      CloseHandle(slot.ev);
  }
}

}  // namespace

std::string ipc_dispatch_request_json(const std::string& req_json,
                                      const IpcCallbacks& cbs) {
  ParsedRequest req;
  if (!parse_flat_request(req_json, &req) || req.cmd.empty())
    return json_error("invalid JSON request or missing cmd");

  if (req.cmd == "ping") {
    return "{\"ok\":true,\"result\":{\"pong\":true,\"version\":\"0.1.0\"}}";
  }
  if (req.cmd == "status") {
    return build_status_json(cbs);
  }
  if (req.cmd == "overview.open") {
    if (cbs.overview_open)
      cbs.overview_open();
    return "{\"ok\":true}";
  }
  if (req.cmd == "overview.close") {
    if (cbs.overview_close)
      cbs.overview_close();
    return "{\"ok\":true}";
  }
  if (req.cmd == "overview.toggle") {
    if (cbs.overview_toggle)
      cbs.overview_toggle();
    return "{\"ok\":true}";
  }
  if (req.cmd == "overview.app_grid") {
    if (cbs.overview_app_grid)
      cbs.overview_app_grid();
    return "{\"ok\":true}";
  }
  if (req.cmd == "settings.open") {
    if (cbs.settings_open)
      cbs.settings_open(req.page);
    return "{\"ok\":true}";
  }
  if (req.cmd == "config.get") {
    std::string out = "{\"ok\":true,\"result\":";
    out += serialize_config(get_config());
    if (!out.empty() && out.back() == '\n')
      out.pop_back();
    out += "}";
    return out;
  }
  if (req.cmd == "config.reload") {
    if (cbs.config_reload)
      cbs.config_reload();
    else
      reload_config();
    return "{\"ok\":true}";
  }
  if (req.cmd == "desktop.list") {
    std::vector<vd::DesktopInfo> list =
        cbs.desktop_list ? cbs.desktop_list() : std::vector<vd::DesktopInfo>{};
    const int active = cbs.desktop_active ? cbs.desktop_active() : -1;
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "{\"ok\":true,\"result\":{\"count\":%d,\"active\":%d,\"desktops\":[",
                  static_cast<int>(list.size()), active);
    std::string out = buf;
    for (size_t i = 0; i < list.size(); ++i) {
      if (i > 0)
        out += ",";
      std::snprintf(buf, sizeof(buf), "{\"index\":%d,\"id\":", list[i].index);
      out += buf;
      append_json_escaped(format_guid_string(list[i].id), &out);
      out += "}";
    }
    out += "]}}";
    return out;
  }
  if (req.cmd == "desktop.switch") {
    if (!req.has_index || req.index < 0)
      return json_error("desktop.switch requires non-negative index");
    const bool ok = cbs.desktop_switch ? cbs.desktop_switch(req.index) : false;
    return ok ? "{\"ok\":true}" : json_error("desktop switch failed");
  }
  if (req.cmd == "notify") {
    if (req.title.empty())
      return json_error("notify requires non-empty title");
    if (cbs.notify)
      cbs.notify(req.title, req.body, req.app);
    return "{\"ok\":true}";
  }
  if (req.cmd == "quit") {
    if (cbs.quit)
      cbs.quit();
    return "{\"ok\":true}";
  }

  return json_error("unknown command");
}

bool ipc_server_start(const IpcCallbacks& cbs) {
  HANDLE ready_ev = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_ipc.mu);
    if (g_ipc.running.load(std::memory_order_acquire))
      return true;
    g_ipc.cbs = cbs;
    g_ipc.stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_ipc.ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_ipc.stop_event == nullptr || g_ipc.ready_event == nullptr) {
      if (g_ipc.stop_event != nullptr) {
        CloseHandle(g_ipc.stop_event);
        g_ipc.stop_event = nullptr;
      }
      if (g_ipc.ready_event != nullptr) {
        CloseHandle(g_ipc.ready_event);
        g_ipc.ready_event = nullptr;
      }
      capability_set(SubsystemId::IpcServer, CapabilityState::Tripped,
                     "CreateEventW failed for IPC stop/ready event");
      return false;
    }
    ready_ev = g_ipc.ready_event;
    g_ipc.running.store(true, std::memory_order_release);
    g_ipc.thread = std::thread(server_thread_main);
  }
  if (ready_ev != nullptr)
    WaitForSingleObject(ready_ev, 1000);
  return true;
}

void ipc_server_stop() {
  std::thread to_join;
  HANDLE stop_ev = nullptr;
  HANDLE ready_ev = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_ipc.mu);
    if (!g_ipc.running.exchange(false, std::memory_order_acq_rel))
      return;
    to_join = std::move(g_ipc.thread);
    stop_ev = g_ipc.stop_event;
    g_ipc.stop_event = nullptr;
    ready_ev = g_ipc.ready_event;
    g_ipc.ready_event = nullptr;
  }
  if (stop_ev != nullptr)
    SetEvent(stop_ev);
  if (to_join.joinable())
    to_join.join();
  if (stop_ev != nullptr)
    CloseHandle(stop_ev);
  if (ready_ev != nullptr)
    CloseHandle(ready_ev);
}

bool ipc_query_running_shell(const std::string& req_json,
                             std::string* out_resp,
                             int timeout_ms) {
  if (out_resp == nullptr)
    return false;
  out_resp->clear();

  const DWORD total_wait = static_cast<DWORD>(std::max(100, timeout_ms));
  const ULONGLONG deadline = GetTickCount64() + total_wait;
  HANDLE pipe = INVALID_HANDLE_VALUE;
  for (;;) {
    pipe = CreateFileW(kShellPipeName, GENERIC_READ | GENERIC_WRITE,
                       0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE)
      break;

    const DWORD err = GetLastError();
    if (err != ERROR_PIPE_BUSY)
      return false;

    const ULONGLONG now = GetTickCount64();
    if (now >= deadline)
      return false;
    const DWORD rem = static_cast<DWORD>(deadline - now);
    WaitNamedPipeW(kShellPipeName, std::min<DWORD>(rem, 100));
  }

  std::string framed = req_json;
  if (framed.empty() || framed.back() != '\n')
    framed.push_back('\n');

  DWORD written = 0;
  BOOL write_ok = WriteFile(pipe, framed.data(),
                            static_cast<DWORD>(framed.size()), &written, nullptr);
  if (!write_ok || written != framed.size()) {
    CloseHandle(pipe);
    return false;
  }

  char buf[4096];
  while (out_resp->size() < kMaxIpcPayloadBytes) {
    DWORD got = 0;
    BOOL rok = ReadFile(pipe, buf, sizeof(buf), &got, nullptr);
    if (!rok || got == 0)
      break;
    bool hit_nl = false;
    for (DWORD i = 0; i < got; ++i) {
      if (buf[i] == '\n') {
        hit_nl = true;
        break;
      }
      out_resp->push_back(buf[i]);
    }
    if (hit_nl)
      break;
  }

  CloseHandle(pipe);
  return !out_resp->empty();
}

}  // namespace winome
