// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Un-elevated runtime control & live debugging CLI for WINOME.
// Connects directly to \\.\pipe\WINOME_Shell_IPC without requiring a UAC
// elevation prompt so developers and scripts can inspect/control the shell.

#include "ctl-protocol.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\WINOME_Shell_IPC";
constexpr size_t kMaxPayloadBytes = 4 * 1024 * 1024;

bool query_pipe(const std::string& req_json,
                std::string* out_resp,
                int timeout_ms = 2000,
                bool* out_timed_out = nullptr,
                DWORD* out_last_err = nullptr,
                bool* out_busy_timed_out = nullptr) {
  if (out_timed_out != nullptr)
    *out_timed_out = false;
  if (out_last_err != nullptr)
    *out_last_err = 0;
  if (out_busy_timed_out != nullptr)
    *out_busy_timed_out = false;
  if (out_resp == nullptr)
    return false;
  out_resp->clear();

  const ULONGLONG connect_deadline =
      GetTickCount64() + static_cast<ULONGLONG>(std::max(100, timeout_ms));
  HANDLE pipe = INVALID_HANDLE_VALUE;
  for (;;) {
    SetLastError(0);
    pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE,
                       0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE)
      break;
    const DWORD err = GetLastError();
    if (err != ERROR_PIPE_BUSY) {
      if (out_last_err != nullptr)
        *out_last_err = err;
      return false;
    }
    const ULONGLONG now = GetTickCount64();
    if (now >= connect_deadline) {
      if (out_last_err != nullptr)
        *out_last_err = err;
      if (out_busy_timed_out != nullptr)
        *out_busy_timed_out = true;
      return false;
    }
    WaitNamedPipeW(kPipeName,
                   std::min<DWORD>(static_cast<DWORD>(connect_deadline - now), 100));
  }

  std::string framed = req_json;
  if (framed.empty() || framed.back() != '\n')
    framed.push_back('\n');

  DWORD written = 0;
  SetLastError(0);
  BOOL wok = WriteFile(pipe, framed.data(), static_cast<DWORD>(framed.size()),
                       &written, nullptr);
  if (!wok || written != framed.size()) {
    if (out_last_err != nullptr)
      *out_last_err = GetLastError();
    CloseHandle(pipe);
    return false;
  }

  // Bound waiting on server reply so IPC stalls never leave CLI silent forever.
  const ULONGLONG reply_deadline =
      GetTickCount64() + static_cast<ULONGLONG>(std::max(2000, timeout_ms * 2));
  char buf[4096];
  bool hit_nl = false;
  while (out_resp->size() < kMaxPayloadBytes && !hit_nl) {
    DWORD avail = 0;
    SetLastError(0);
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) {
      const DWORD err = GetLastError();
      if (out_resp->empty() && out_last_err != nullptr)
        *out_last_err = err;
      if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED)
        break;
      break;
    }
    if (avail == 0) {
      if (GetTickCount64() >= reply_deadline) {
        if (out_timed_out != nullptr)
          *out_timed_out = true;
        break;
      }
      Sleep(10);
      continue;
    }
    const DWORD want = std::min<DWORD>(sizeof(buf), avail);
    DWORD got = 0;
    SetLastError(0);
    BOOL rok = ReadFile(pipe, buf, want, &got, nullptr);
    if (!rok || got == 0) {
      if (!rok && out_resp->empty() && out_last_err != nullptr)
        *out_last_err = GetLastError();
      break;
    }
    for (DWORD i = 0; i < got; ++i) {
      if (buf[i] == '\n') {
        hit_nl = true;
        break;
      }
      out_resp->push_back(buf[i]);
    }
  }

  CloseHandle(pipe);
  return !out_resp->empty();
}

void print_usage() {
  std::puts(
      "Usage: winome-ctl <command> [args...]\n"
      "\n"
      "Commands:\n"
      "  ping                                  Verify connection to WINOME IPC server\n"
      "  status [--watch [ms]]                 Print runtime telemetry & circuit breakers\n"
      "  logs [--tail N] [--follow] [--tag T]  Fetch or stream live shell logs\n"
      "  overview <open|close|toggle|grid>     Control Activities Overview state\n"
      "  desktop <list|create|switch I|remove I> Query or manage virtual desktops\n"
      "  inspect <windows|panel|tray|appgrid>  Inspect windows, panel, tray, or app grid\n"
      "  flags [get | set <name> <true|false>] Inspect or toggle debug trace flags\n"
      "  subsystem <trip|recover> <Name>       Test/recover subsystem circuit breakers\n"
      "  config <get|reload|set <k> <v>>       Query, reload, or mutate live config\n"
      "  plugins <list|start|stop|restart|enable|disable|reset <id>>\n"
      "  theme reload                          Hot-reload GNOME CSS / StEngine metrics\n"
      "  run <command...>                      Execute command via WINOME shell host\n"
      "  settings [page]                       Open Settings (diagnostics, appearance, ...)\n"
      "  lg                                    Open Looking Glass console (requires Dev Mode)\n"
      "  notify <title> [body]                 Trigger shell toast notification\n"
      "  test trigger <act> [--wait ms] [--tag T]\n"
      "                                        [Dev] Trigger single action & return logs\n"
      "  test run (--steps CSV | --preset P) [--gap ms] [--settle ms] [--repeat N] [--tag T]\n"
      "                                        [Dev] Run action sequence & return aggregated report\n"
      "  test begin [--tag T] / test end       [Dev] Aggregate logs across multiple triggers\n"
      "  dev help                              [Dev] List all universal debug channel ops\n"
      "  dev op <op> [k=v ...] [--wait ms] [--logs] [--tag T]\n"
      "                                        [Dev] Execute any input/win/gtk/shell/perf op\n"
      "  dev batch (--ops \"op1; op2\" | --json '[...]') [--logs] [--stop-on-error] [--tag T]\n"
      "                                        [Dev] Execute multi-step universal op pipeline\n"
      "  cmd <json|cmd_name>                   Send raw JSON request to IPC server\n"
      "  quit                                  Gracefully exit WINOME\n");
}

bool extract_uint64_field(const std::string& json, const char* key, uint64_t* out) {
  std::string pat = std::string("\"") + key + "\":";
  size_t pos = json.find(pat);
  if (pos == std::string::npos)
    return false;
  pos += pat.size();
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
    ++pos;
  if (pos >= json.size() || json[pos] < '0' || json[pos] > '9')
    return false;
  *out = std::strtoull(json.c_str() + pos, nullptr, 10);
  return true;
}

int handle_logs_command(const std::vector<std::string>& args) {
  bool follow = false;
  int tail = 50;
  std::string tag = "all";
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--follow" || args[i] == "-f") {
      follow = true;
    } else if (args[i] == "--tail" && i + 1 < args.size()) {
      tail = std::max(1, std::atoi(args[++i].c_str()));
    } else if (args[i] == "--tag" && i + 1 < args.size()) {
      tag = args[++i];
    }
  }

  uint64_t after_seq = 0;
  for (;;) {
    std::string req = "{\"cmd\":\"logs.tail\",\"index\":";
    req += std::to_string(after_seq);
    req += ",\"count\":";
    req += std::to_string(after_seq == 0 ? tail : 200);
    req += ",\"tag\":";
    winome::ctl::append_json_str(tag, &req);
    req += "}";

    std::string resp;
    DWORD last_err = 0;
    bool timed_out = false;
    bool busy_timed_out = false;
    if (!query_pipe(req, &resp, 2000, &timed_out, &last_err, &busy_timed_out)) {
      const char* kind =
          winome::ctl::ctl_classify_pipe_error(last_err, timed_out, busy_timed_out);
      std::fprintf(stderr, "winome-ctl: %s (gle=%lu) on %ls\n",
                   kind, static_cast<unsigned long>(last_err), kPipeName);
      return 1;
    }
    if (!follow) {
      std::printf("%s\n", resp.c_str());
      return 0;
    }

    if (resp.find("\"entries\":[]") == std::string::npos) {
      std::printf("%s\n", resp.c_str());
      std::fflush(stdout);
    }
    uint64_t next_seq = after_seq;
    if (extract_uint64_field(resp, "latest_seq", &next_seq) && next_seq > after_seq)
      after_seq = next_seq;
    Sleep(250);
  }
}

int handle_status_watch(const std::vector<std::string>& args) {
  int interval_ms = 500;
  bool watch = false;
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--watch" || args[i] == "-w") {
      watch = true;
      if (i + 1 < args.size() && args[i + 1][0] >= '0' && args[i + 1][0] <= '9')
        interval_ms = std::max(100, std::atoi(args[++i].c_str()));
    }
  }
  for (;;) {
    std::string resp;
    DWORD last_err = 0;
    bool timed_out = false;
    bool busy_timed_out = false;
    if (!query_pipe("{\"cmd\":\"status\"}", &resp, 2000,
                    &timed_out, &last_err, &busy_timed_out)) {
      const char* kind =
          winome::ctl::ctl_classify_pipe_error(last_err, timed_out, busy_timed_out);
      std::fprintf(stderr, "{\"ok\":false,\"error\":\"%s\",\"gle\":%lu}\n",
                   kind, static_cast<unsigned long>(last_err));
      return 1;
    }
    std::printf("%s\n", resp.c_str());
    std::fflush(stdout);
    if (!watch)
      return 0;
    Sleep(static_cast<DWORD>(interval_ms));
  }
}

}  // namespace

int main(int argc, char** argv) {
  SetConsoleOutputCP(CP_UTF8);
  if (argc < 2) {
    print_usage();
    return 1;
  }

  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc - 1));
  for (int i = 1; i < argc; ++i)
    args.emplace_back(argv[i]);

  if (args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
    print_usage();
    return 0;
  }
  if (args[0] == "logs")
    return handle_logs_command(args);
  if (args[0] == "status")
    return handle_status_watch(args);

  std::string req;
  std::string err;
  if (!winome::ctl::ctl_build_request_json(args, &req, &err)) {
    std::fprintf(stderr, "winome-ctl: %s\n", err.c_str());
    return 2;
  }

  std::string resp;
  bool timed_out = false;
  DWORD last_err = 0;
  bool busy_timed_out = false;
  // Multi-step developer test sequences (test run / trigger) can legitimately
  // span many seconds; scale client reply timeout accordingly.
  const int pipe_timeout_ms =
      (args[0] == "test" || args[0] == "dev") ? 60000 : 3000;
  if (!query_pipe(req, &resp, pipe_timeout_ms,
                  &timed_out, &last_err, &busy_timed_out)) {
    const char* kind =
        winome::ctl::ctl_classify_pipe_error(last_err, timed_out, busy_timed_out);
    std::fprintf(stderr, "{\"ok\":false,\"error\":\"%s\",\"gle\":%lu}\n",
                 kind, static_cast<unsigned long>(last_err));
    std::fflush(stderr);
    return 1;
  }

  std::printf("%s\n", resp.c_str());
  std::fflush(stdout);
  return (resp.find("\"ok\":true") != std::string::npos) ? 0 : 1;
}
