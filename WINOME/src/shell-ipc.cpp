// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Bidirectional Named Pipe IPC server and JSON dispatcher for WINOME shell.

#include "shell-ipc.h"

#include "app-index.h"
#include "config.h"
#include "debug-runtime.h"
#include "perf-metrics.h"
#include "systray.h"
#include "winome-version.h"

#include <windows.h>
#include <sddl.h>
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

constexpr size_t kMaxIpcPayloadBytes = 262144;

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
  std::string name;
  std::string value;
  std::string tag;
  bool has_value = false;
  bool has_index = false;
  int index = -1;
  int count = 0;
};

bool parse_json_scalar_as_string(const std::string& s, size_t* pos, std::string* out) {
  skip_ws(s, pos);
  if (*pos >= s.size())
    return false;
  if (s[*pos] == '"')
    return parse_json_str(s, pos, out);
  if (s.compare(*pos, 4, "true") == 0) {
    *pos += 4;
    *out = "true";
    return true;
  }
  if (s.compare(*pos, 5, "false") == 0) {
    *pos += 5;
    *out = "false";
    return true;
  }
  size_t start = *pos;
  if (s[*pos] == '-' || s[*pos] == '+')
    ++(*pos);
  size_t digits = *pos;
  while (*pos < s.size() && std::isdigit(static_cast<unsigned char>(s[*pos])))
    ++(*pos);
  if (*pos == digits)
    return false;
  *out = s.substr(start, *pos - start);
  return true;
}

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
    } else if (key == "name") {
      ok = parse_json_str(json, &pos, &out->name);
    } else if (key == "tag") {
      ok = parse_json_str(json, &pos, &out->tag);
    } else if (key == "value") {
      ok = parse_json_scalar_as_string(json, &pos, &out->value);
      out->has_value = ok;
    } else if (key == "index") {
      ok = parse_json_int(json, &pos, &out->index);
      out->has_index = ok;
    } else if (key == "count") {
      ok = parse_json_int(json, &pos, &out->count);
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
  const DebugFlags df = debug_get_flags();

  char num[256];
  std::string out = std::string("{\"ok\":true,\"result\":{\"version\":\"") +
                    WINOME_VERSION + "\",";
  out += ov_open ? "\"overview_open\":true," : "\"overview_open\":false,";
  out += is_safe_mode() ? "\"safe_mode\":true," : "\"safe_mode\":false,";
  out += get_config().developer_mode ? "\"developer_mode\":true,"
                                     : "\"developer_mode\":false,";
  out += "\"log_seq\":" + std::to_string(debug_latest_log_seq()) + ",";
  out += "\"debug_flags\":" + debug_flags_to_json(df) + ",";

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

class PipeSecurity {
 public:
  PipeSecurity() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
      return;
    DWORD need = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &need);
    if (need > 0 && need < 4096) {
      std::vector<BYTE> buf(need);
      if (GetTokenInformation(tok, TokenUser, buf.data(), need, &need)) {
        auto* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
        LPWSTR sid_str = nullptr;
        if (ConvertSidToStringSidW(tu->User.Sid, &sid_str) && sid_str != nullptr) {
          const std::wstring sddl = ipc_build_user_pipe_sddl(sid_str);
          if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                  sddl.c_str(), SDDL_REVISION_1, &sd_full_, nullptr)) {
            sa_full_.nLength = sizeof(sa_full_);
            sa_full_.lpSecurityDescriptor = sd_full_;
            sa_full_.bInheritHandle = FALSE;
          }
          const std::wstring dacl_only =
              std::wstring(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;") + sid_str + L")";
          if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                  dacl_only.c_str(), SDDL_REVISION_1, &sd_dacl_, nullptr)) {
            sa_dacl_.nLength = sizeof(sa_dacl_);
            sa_dacl_.lpSecurityDescriptor = sd_dacl_;
            sa_dacl_.bInheritHandle = FALSE;
          }
          LocalFree(sid_str);
        }
      }
    }
    CloseHandle(tok);
  }

  ~PipeSecurity() {
    if (sd_full_ != nullptr)
      LocalFree(sd_full_);
    if (sd_dacl_ != nullptr)
      LocalFree(sd_dacl_);
  }

  LPSECURITY_ATTRIBUTES primary_sa() {
    if (sd_full_ != nullptr)
      return &sa_full_;
    if (sd_dacl_ != nullptr)
      return &sa_dacl_;
    return nullptr;
  }

  LPSECURITY_ATTRIBUTES fallback_sa() {
    if (sd_dacl_ != nullptr)
      return &sa_dacl_;
    return nullptr;
  }

 private:
  PSECURITY_DESCRIPTOR sd_full_ = nullptr;
  PSECURITY_DESCRIPTOR sd_dacl_ = nullptr;
  SECURITY_ATTRIBUTES sa_full_{};
  SECURITY_ATTRIBUTES sa_dacl_{};
};

HANDLE create_pipe_instance(PipeSecurity* sec) {
  auto make_pipe = [](LPSECURITY_ATTRIBUTES sa) {
    return CreateNamedPipeW(
        kShellPipeName,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        4,
        kMaxIpcPayloadBytes,
        kMaxIpcPayloadBytes,
        1000,
        sa);
  };

  if (sec != nullptr && sec->primary_sa() != nullptr) {
    HANDLE h = make_pipe(sec->primary_sa());
    if (h != INVALID_HANDLE_VALUE)
      return h;
    if (sec->fallback_sa() != nullptr) {
      h = make_pipe(sec->fallback_sa());
      if (h != INVALID_HANDLE_VALUE)
        return h;
    }
  }
  return make_pipe(nullptr);
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
  PipeSecurity pipe_sec;
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
      s.pipe = create_pipe_instance(&pipe_sec);
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
      next.pipe = create_pipe_instance(&pipe_sec);
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

bool find_subsystem_by_name(const std::string& name, SubsystemId* out_id) {
  if (out_id == nullptr || name.empty())
    return false;
  for (size_t i = 0; i < static_cast<size_t>(SubsystemId::Count); ++i) {
    auto id = static_cast<SubsystemId>(i);
    const char* sname = capability_name(id);
    if (sname != nullptr && _stricmp(sname, name.c_str()) == 0) {
      *out_id = id;
      return true;
    }
  }
  return false;
}

bool apply_config_kv_default(const std::string& key, const std::string& val) {
  auto parse_bool_str = [&](bool* b) {
    if (val == "true" || val == "1" || val == "on") {
      *b = true;
      return true;
    }
    if (val == "false" || val == "0" || val == "off") {
      *b = false;
      return true;
    }
    return false;
  };

  WinomeConfig cfg = get_config();
  bool ok = false;
  if (key == "developer_mode")
    ok = parse_bool_str(&cfg.developer_mode);
  else if (key == "hot_corner_enabled")
    ok = parse_bool_str(&cfg.hot_corner_enabled);
  else if (key == "win_key_overview")
    ok = parse_bool_str(&cfg.win_key_overview);
  else if (key == "overview_window_blacklist")
    ok = parse_bool_str(&cfg.overview_window_blacklist);
  else if (key == "multi_monitor_panels")
    ok = parse_bool_str(&cfg.multi_monitor_panels);
  else if (key == "end_session_confirm")
    ok = parse_bool_str(&cfg.end_session_confirm);
  else if (key == "unify_dark_titlebars")
    ok = parse_bool_str(&cfg.unify_dark_titlebars);
  else if (key == "unify_rounded_corners")
    ok = parse_bool_str(&cfg.unify_rounded_corners);
  else if (key == "clock_show_seconds")
    ok = parse_bool_str(&cfg.clock_show_seconds);
  else if (key == "show_battery_percentage")
    ok = parse_bool_str(&cfg.show_battery_percentage);
  else if (key == "hot_corner_cooldown_ms") {
    cfg.hot_corner_cooldown_ms = std::atoi(val.c_str());
    ok = true;
  } else if (key == "language") {
    cfg.language = val;
    ok = true;
  } else if (key == "clock_format") {
    cfg.clock_format = val;
    ok = true;
  }
  if (!ok)
    return false;
  update_config(cfg);
  return true;
}

}  // namespace

std::wstring ipc_build_user_pipe_sddl(const wchar_t* user_sid_str) {
  if (user_sid_str == nullptr || user_sid_str[0] == L'\0')
    return {};
  std::wstring sddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;";
  sddl += user_sid_str;
  sddl += L")S:(ML;;NW;;;ME)";
  return sddl;
}

std::string ipc_dispatch_request_json(const std::string& req_json,
                                      const IpcCallbacks& cbs) {
  ParsedRequest req;
  if (!parse_flat_request(req_json, &req) || req.cmd.empty())
    return json_error("invalid JSON request or missing cmd");

  if (debug_get_flags().ipc_trace && req.cmd != "logs.tail")
    debug_append_log("ipc", std::string("[ipc] cmd=") + req.cmd);

  if (req.cmd == "ping") {
    return std::string("{\"ok\":true,\"result\":{\"pong\":true,\"version\":\"") +
           WINOME_VERSION + "\"}}";
  }
  if (req.cmd == "status") {
    return build_status_json(cbs);
  }
  if (req.cmd == "logs.tail") {
    const uint64_t after_seq =
        req.index > 0 ? static_cast<uint64_t>(req.index) : 0;
    const size_t limit =
        req.count > 0 ? static_cast<size_t>(std::min(req.count, 512)) : 50;
    const auto entries = debug_get_logs_since(after_seq, req.tag, limit);
    return std::string("{\"ok\":true,\"result\":") +
           debug_logs_to_json(entries) + "}";
  }
  if (req.cmd == "logs.clear") {
    debug_clear_logs();
    return "{\"ok\":true}";
  }
  if (req.cmd == "debug.flags.get") {
    return std::string("{\"ok\":true,\"result\":") +
           debug_flags_to_json(debug_get_flags()) + "}";
  }
  if (req.cmd == "debug.flags.set") {
    if (req.name.empty() || !req.has_value)
      return json_error("debug.flags.set requires name and value");
    const bool b =
        (req.value == "true" || req.value == "1" || req.value == "on");
    if (!debug_set_flag_by_name(req.name, b))
      return json_error("unknown debug flag");
    return std::string("{\"ok\":true,\"result\":") +
           debug_flags_to_json(debug_get_flags()) + "}";
  }
  if (req.cmd == "subsystem.trip" || req.cmd == "subsystem.recover") {
    SubsystemId id{};
    if (!find_subsystem_by_name(req.name, &id))
      return json_error("unknown subsystem name");
    if (req.cmd == "subsystem.trip")
      capability_record_failure(id, "manual-ipc-trip", 1);
    else
      capability_set(id, CapabilityState::Active, "manual-ipc-recover");
    const CapabilityStatus st = capability_get(id);
    std::string out = "{\"ok\":true,\"result\":{\"name\":";
    append_json_escaped(st.name ? st.name : "", &out);
    out += ",\"state\":";
    append_json_escaped(capability_state_label(st.state), &out);
    out += ",\"detail\":";
    append_json_escaped(st.detail, &out);
    out += "}}";
    return out;
  }
  if (req.cmd == "inspect.windows") {
    const auto wins = debug_inspect_windows();
    return std::string("{\"ok\":true,\"result\":") +
           debug_windows_to_json(wins) + "}";
  }
  if (req.cmd == "inspect.tray") {
    return std::string("{\"ok\":true,\"result\":") +
           systray_debug_inspect_json() + "}";
  }
  if (req.cmd == "inspect.appgrid") {
    return std::string("{\"ok\":true,\"result\":") +
           app_index_debug_dump_json() + "}";
  }
  if (req.cmd == "inspect.panel") {
    if (cbs.inspect_panel_json)
      return std::string("{\"ok\":true,\"result\":") +
             cbs.inspect_panel_json() + "}";
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "{\"ok\":true,\"result\":{\"safe_mode\":%s,\"developer_mode\":%s}}",
                  is_safe_mode() ? "true" : "false",
                  get_config().developer_mode ? "true" : "false");
    return buf;
  }
  if (req.cmd == "theme.reload") {
    const bool ok = cbs.theme_reload ? cbs.theme_reload() : false;
    return ok ? "{\"ok\":true}" : json_error("theme reload unavailable");
  }
  if (req.cmd == "looking_glass.open") {
    const bool ok = cbs.looking_glass_open ? cbs.looking_glass_open() : false;
    return ok ? "{\"ok\":true}"
              : json_error("enable developer_mode in Settings first");
  }
  if (req.cmd == "looking_glass.close") {
    const bool ok = cbs.looking_glass_close ? cbs.looking_glass_close() : false;
    return ok ? "{\"ok\":true}" : "{\"ok\":true}";
  }
  if (req.cmd == "run.exec") {
    if (req.name.empty())
      return json_error("run.exec requires non-empty name command");
    std::string err;
    const bool ok = cbs.run_exec ? cbs.run_exec(req.name, &err) : false;
    if (ok)
      return "{\"ok\":true}";
    return json_error(err.empty() ? "command execution unavailable" : err);
  }
  if (req.cmd == "plugin.list") {
    const std::string body =
        cbs.plugin_list_json ? cbs.plugin_list_json() : "{\"plugins\":[]}";
    return std::string("{\"ok\":true,\"result\":") + body + "}";
  }
  if (req.cmd == "plugin.start" || req.cmd == "plugin.stop" ||
      req.cmd == "plugin.restart" || req.cmd == "plugin.enable" ||
      req.cmd == "plugin.disable" || req.cmd == "plugin.reset") {
    if (req.name.empty())
      return json_error("plugin command requires plugin id in name");
    const std::string op = req.cmd.substr(7);
    std::string err;
    const bool ok =
        cbs.plugin_action ? cbs.plugin_action(op, req.name, &err) : false;
    return ok ? "{\"ok\":true}"
              : json_error(err.empty() ? "plugin action unavailable" : err);
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
  if (req.cmd == "config.set") {
    if (req.name.empty() || !req.has_value)
      return json_error("config.set requires name and value");
    const bool ok = cbs.config_set ? cbs.config_set(req.name, req.value)
                                   : apply_config_kv_default(req.name, req.value);
    return ok ? "{\"ok\":true}" : json_error("invalid config key or value");
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
  if (req.cmd == "desktop.create") {
    const bool ok = cbs.desktop_create ? cbs.desktop_create() : false;
    return ok ? "{\"ok\":true}" : json_error("desktop create failed");
  }
  if (req.cmd == "desktop.remove") {
    if (!req.has_index || req.index < 0)
      return json_error("desktop.remove requires non-negative index");
    const bool ok = cbs.desktop_remove ? cbs.desktop_remove(req.index) : false;
    return ok ? "{\"ok\":true}" : json_error("desktop remove failed");
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
