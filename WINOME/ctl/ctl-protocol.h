// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Pure CLI-to-JSON translation helper shared between un-elevated
// winome-ctl.exe and unit tests.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace winome {
namespace ctl {

inline void append_json_str(const std::string& s, std::string* out) {
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

// Map Win32 Named Pipe failure codes and timeout flags to distinct IPC error keys
// so callers can distinguish sandbox access denial from "shell not running".
inline const char* ctl_classify_pipe_error(unsigned long win32_err,
                                           bool timed_out,
                                           bool busy_timed_out = false) {
  if (timed_out)
    return "ipc_reply_timeout";
  if (busy_timed_out)
    return "ipc_pipe_busy_timeout";
  if (win32_err == 2UL || win32_err == 3UL)  // ERROR_FILE_NOT_FOUND / PATH_NOT_FOUND
    return "not_running";
  if (win32_err == 5UL || win32_err == 1314UL)  // ERROR_ACCESS_DENIED / PRIVILEGE_NOT_HELD
    return "ipc_access_denied";
  return "ipc_error";
}

inline bool ctl_build_request_json(const std::vector<std::string>& args,
                                   std::string* out_json,
                                   std::string* out_err) {
  if (out_json == nullptr)
    return false;
  out_json->clear();
  if (out_err != nullptr)
    out_err->clear();

  if (args.empty()) {
    if (out_err != nullptr)
      *out_err = "missing subcommand";
    return false;
  }

  const std::string& sub = args[0];
  if (sub == "ping" || sub == "status") {
    *out_json = std::string("{\"cmd\":") + "\"" + sub + "\"}";
    return true;
  }
  if (sub == "overview") {
    if (args.size() < 2) {
      if (out_err != nullptr)
        *out_err = "overview requires: open | close | toggle | grid";
      return false;
    }
    const std::string& op = args[1];
    if (op == "open" || op == "close" || op == "toggle") {
      *out_json = "{\"cmd\":\"overview." + op + "\"}";
      return true;
    }
    if (op == "grid" || op == "app_grid") {
      *out_json = "{\"cmd\":\"overview.app_grid\"}";
      return true;
    }
    if (out_err != nullptr)
      *out_err = "unknown overview action";
    return false;
  }
  if (sub == "desktop") {
    if (args.size() < 2) {
      if (out_err != nullptr)
        *out_err = "desktop requires: list | switch <idx> | create | remove <idx>";
      return false;
    }
    const std::string& op = args[1];
    if (op == "list") {
      *out_json = "{\"cmd\":\"desktop.list\"}";
      return true;
    }
    if (op == "create") {
      *out_json = "{\"cmd\":\"desktop.create\"}";
      return true;
    }
    if ((op == "switch" || op == "remove") && args.size() >= 3) {
      const int idx = std::atoi(args[2].c_str());
      *out_json = "{\"cmd\":\"desktop." + op + "\",\"index\":" + std::to_string(idx) + "}";
      return true;
    }
    if (out_err != nullptr)
      *out_err = "invalid desktop arguments";
    return false;
  }
  if (sub == "settings") {
    const std::string page = args.size() >= 2 ? args[1] : "";
    std::string req = "{\"cmd\":\"settings.open\",\"page\":";
    append_json_str(page, &req);
    req += "}";
    *out_json = std::move(req);
    return true;
  }
  if (sub == "lg" || sub == "looking-glass") {
    *out_json = "{\"cmd\":\"looking_glass.open\"}";
    return true;
  }
  if (sub == "theme" && args.size() >= 2 && args[1] == "reload") {
    *out_json = "{\"cmd\":\"theme.reload\"}";
    return true;
  }
  if (sub == "inspect") {
    const std::string target = args.size() >= 2 ? args[1] : "windows";
    if (target == "windows" || target == "panel" || target == "tray" ||
        target == "appgrid") {
      *out_json = "{\"cmd\":\"inspect." + target + "\"}";
      return true;
    }
    if (out_err != nullptr)
      *out_err = "inspect target must be windows, panel, tray, or appgrid";
    return false;
  }
  if (sub == "flags") {
    if (args.size() < 2 || args[1] == "get") {
      *out_json = "{\"cmd\":\"debug.flags.get\"}";
      return true;
    }
    if (args[1] == "set" && args.size() >= 4) {
      const bool v = (args[3] == "1" || args[3] == "true" || args[3] == "on");
      std::string req = "{\"cmd\":\"debug.flags.set\",\"name\":";
      append_json_str(args[2], &req);
      req += ",\"value\":";
      req += (v ? "true" : "false");
      req += "}";
      *out_json = std::move(req);
      return true;
    }
    if (out_err != nullptr)
      *out_err = "flags requires: get | set <flag> <true|false>";
    return false;
  }
  if (sub == "subsystem" && args.size() >= 3) {
    const std::string& op = args[1];
    if (op != "trip" && op != "recover") {
      if (out_err != nullptr)
        *out_err = "subsystem requires: trip <Name> | recover <Name>";
      return false;
    }
    std::string req = "{\"cmd\":\"subsystem." + op + "\",\"name\":";
    append_json_str(args[2], &req);
    req += "}";
    *out_json = std::move(req);
    return true;
  }
  if (sub == "config") {
    if (args.size() < 2 || args[1] == "get") {
      *out_json = "{\"cmd\":\"config.get\"}";
      return true;
    }
    if (args[1] == "reload") {
      *out_json = "{\"cmd\":\"config.reload\"}";
      return true;
    }
    if (args[1] == "set" && args.size() >= 4) {
      std::string req = "{\"cmd\":\"config.set\",\"name\":";
      append_json_str(args[2], &req);
      req += ",\"value\":";
      append_json_str(args[3], &req);
      req += "}";
      *out_json = std::move(req);
      return true;
    }
    if (out_err != nullptr)
      *out_err = "config requires: get | reload | set <key> <value>";
    return false;
  }
  if (sub == "plugins" || sub == "plugin") {
    if (args.size() < 2 || args[1] == "list") {
      *out_json = "{\"cmd\":\"plugin.list\"}";
      return true;
    }
    const std::string& op = args[1];
    if ((op == "restart" || op == "enable" || op == "disable" ||
         op == "reset" || op == "stop" || op == "start") &&
        args.size() >= 3) {
      std::string req = "{\"cmd\":\"plugin." + op + "\",\"name\":";
      append_json_str(args[2], &req);
      req += "}";
      *out_json = std::move(req);
      return true;
    }
    if (out_err != nullptr)
      *out_err = "plugins requires: list | start|stop|restart|enable|disable|reset <id>";
    return false;
  }
  if (sub == "run" && args.size() >= 2) {
    std::string cmdline;
    for (size_t i = 1; i < args.size(); ++i) {
      if (i > 1)
        cmdline.push_back(' ');
      cmdline += args[i];
    }
    std::string req = "{\"cmd\":\"run.exec\",\"name\":";
    append_json_str(cmdline, &req);
    req += "}";
    *out_json = std::move(req);
    return true;
  }
  if (sub == "notify" && args.size() >= 2) {
    std::string req = "{\"cmd\":\"notify\",\"title\":";
    append_json_str(args[1], &req);
    if (args.size() >= 3) {
      req += ",\"body\":";
      append_json_str(args[2], &req);
    }
    req += "}";
    *out_json = std::move(req);
    return true;
  }
  if (sub == "test") {
    if (args.size() < 2) {
      if (out_err != nullptr)
        *out_err = "test requires: trigger <action> | run [--steps S | --preset P] | begin | end";
      return false;
    }
    const std::string& op = args[1];
    if (op == "trigger") {
      if (args.size() < 3) {
        if (out_err != nullptr)
          *out_err = "test trigger requires <action> [--wait ms] [--tag tags]";
        return false;
      }
      const std::string& action = args[2];
      int wait_ms = 450;
      std::string tag;
      for (size_t i = 3; i < args.size(); ++i) {
        if ((args[i] == "--wait" || args[i] == "-w") && i + 1 < args.size())
          wait_ms = std::atoi(args[++i].c_str());
        else if (args[i] == "--tag" && i + 1 < args.size())
          tag = args[++i];
      }
      std::string req = "{\"cmd\":\"test.trigger\",\"action\":";
      append_json_str(action, &req);
      req += ",\"wait_ms\":" + std::to_string(wait_ms);
      if (!tag.empty()) {
        req += ",\"tag\":";
        append_json_str(tag, &req);
      }
      req += "}";
      *out_json = std::move(req);
      return true;
    }
    if (op == "run") {
      std::string steps;
      std::string preset;
      std::string tag;
      int gap_ms = -1;
      int settle_ms = 500;
      int repeat = 1;
      for (size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--steps" && i + 1 < args.size())
          steps = args[++i];
        else if (args[i] == "--preset" && i + 1 < args.size())
          preset = args[++i];
        else if (args[i] == "--gap" && i + 1 < args.size())
          gap_ms = std::atoi(args[++i].c_str());
        else if (args[i] == "--settle" && i + 1 < args.size())
          settle_ms = std::atoi(args[++i].c_str());
        else if (args[i] == "--repeat" && i + 1 < args.size())
          repeat = std::atoi(args[++i].c_str());
        else if (args[i] == "--tag" && i + 1 < args.size())
          tag = args[++i];
      }
      if (steps.empty() && preset.empty()) {
        if (out_err != nullptr)
          *out_err = "test run requires --steps <csv> or --preset <name>";
        return false;
      }
      std::string req = "{\"cmd\":\"test.run\"";
      if (!steps.empty()) {
        req += ",\"steps\":";
        append_json_str(steps, &req);
      }
      if (!preset.empty()) {
        req += ",\"preset\":";
        append_json_str(preset, &req);
      }
      if (gap_ms >= 0)
        req += ",\"gap_ms\":" + std::to_string(gap_ms);
      req += ",\"settle_ms\":" + std::to_string(settle_ms);
      req += ",\"repeat\":" + std::to_string(repeat);
      if (!tag.empty()) {
        req += ",\"tag\":";
        append_json_str(tag, &req);
      }
      req += "}";
      *out_json = std::move(req);
      return true;
    }
    if (op == "begin" || op == "end") {
      std::string tag;
      for (size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--tag" && i + 1 < args.size())
          tag = args[++i];
      }
      std::string req = "{\"cmd\":\"test." + op + "\"";
      if (!tag.empty()) {
        req += ",\"tag\":";
        append_json_str(tag, &req);
      }
      req += "}";
      *out_json = std::move(req);
      return true;
    }
    if (out_err != nullptr)
      *out_err = "unknown test subcommand (use trigger, run, begin, or end)";
    return false;
  }
  if (sub == "dev") {
    if (args.size() < 2) {
      if (out_err != nullptr)
        *out_err = "dev requires: help | op <op> [k=v ...] | batch (--ops <str> | --json <arr>)";
      return false;
    }
    const std::string& dsub = args[1];
    if (dsub == "help") {
      *out_json = "{\"cmd\":\"dev.help\"}";
      return true;
    }
    if (dsub == "op") {
      if (args.size() < 3) {
        if (out_err != nullptr)
          *out_err = "dev op requires <op> [k=v ...] [--wait ms] [--logs] [--tag T]";
        return false;
      }
      const std::string& op_name = args[2];
      int wait_ms = 0;
      bool capture_logs = false;
      std::string tag;
      std::vector<std::pair<std::string, std::string>> kv_args;
      for (size_t i = 3; i < args.size(); ++i) {
        if (args[i] == "--wait" && i + 1 < args.size()) {
          wait_ms = std::atoi(args[++i].c_str());
        } else if (args[i] == "--logs") {
          capture_logs = true;
        } else if (args[i] == "--tag" && i + 1 < args.size()) {
          tag = args[++i];
          capture_logs = true;
        } else {
          size_t eq = args[i].find('=');
          if (eq != std::string::npos && eq > 0) {
            kv_args.emplace_back(args[i].substr(0, eq), args[i].substr(eq + 1));
          } else if (kv_args.empty()) {
            kv_args.emplace_back("action", args[i]);
          }
        }
      }
      std::string req = "{\"cmd\":\"dev.op\",\"op\":";
      append_json_str(op_name, &req);
      req += ",\"args\":{";
      for (size_t i = 0; i < kv_args.size(); ++i) {
        if (i > 0)
          req.push_back(',');
        append_json_str(kv_args[i].first, &req);
        req.push_back(':');
        append_json_str(kv_args[i].second, &req);
      }
      req += "}";
      if (wait_ms > 0)
        req += ",\"wait_ms\":" + std::to_string(wait_ms);
      if (capture_logs)
        req += ",\"capture_logs\":true";
      if (!tag.empty()) {
        req += ",\"tag\":";
        append_json_str(tag, &req);
      }
      req += "}";
      *out_json = std::move(req);
      return true;
    }
    if (dsub == "batch") {
      std::string ops_shorthand;
      std::string ops_json;
      std::string tag;
      bool capture_logs = false;
      bool stop_on_error = false;
      for (size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--ops" && i + 1 < args.size()) {
          ops_shorthand = args[++i];
        } else if (args[i] == "--json" && i + 1 < args.size()) {
          ops_json = args[++i];
        } else if (args[i] == "--logs") {
          capture_logs = true;
        } else if (args[i] == "--stop-on-error") {
          stop_on_error = true;
        } else if (args[i] == "--tag" && i + 1 < args.size()) {
          tag = args[++i];
          capture_logs = true;
        } else if (ops_shorthand.empty() && ops_json.empty()) {
          ops_shorthand = args[i];
        }
      }
      if (ops_shorthand.empty() && ops_json.empty()) {
        if (out_err != nullptr)
          *out_err = "dev batch requires --ops \"op1 ...; op2 ...\" or --json '[...]'";
        return false;
      }
      std::string req = "{\"cmd\":\"dev.batch\",\"ops\":";
      if (!ops_json.empty())
        req += ops_json;
      else
        append_json_str(ops_shorthand, &req);
      if (capture_logs)
        req += ",\"capture_logs\":true";
      if (stop_on_error)
        req += ",\"stop_on_error\":true";
      if (!tag.empty()) {
        req += ",\"tag\":";
        append_json_str(tag, &req);
      }
      req += "}";
      *out_json = std::move(req);
      return true;
    }
    if (out_err != nullptr)
      *out_err = "unknown dev subcommand (use help, op, or batch)";
    return false;
  }
  if (sub == "quit") {
    *out_json = "{\"cmd\":\"quit\"}";
    return true;
  }
  if (sub == "cmd" && args.size() >= 2) {
    const std::string& raw = args[1];
    if (!raw.empty() && raw[0] == '{') {
      *out_json = raw;
      return true;
    }
    std::string req = "{\"cmd\":";
    append_json_str(raw, &req);
    req += "}";
    *out_json = std::move(req);
    return true;
  }

  if (out_err != nullptr)
    *out_err = "unknown winome-ctl subcommand: " + sub;
  return false;
}

}  // namespace ctl
}  // namespace winome
