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
