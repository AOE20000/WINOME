// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Lightweight JSON config serializer/parser and atomic file persistence.

#include "config.h"
#include "host-utils.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace winome {
namespace {

struct ListenerEntry {
  ConfigListener fn;
  void* user_data;
};

WinomeConfig g_config;
bool g_loaded = false;
std::vector<ListenerEntry> g_listeners;

std::wstring config_file_path() {
  wchar_t override_dir[MAX_PATH] = {};
  DWORD len = GetEnvironmentVariableW(L"WINOME_CONFIG_DIR", override_dir, MAX_PATH);
  if (len > 0 && len < MAX_PATH) {
    CreateDirectoryW(override_dir, nullptr);
    return std::wstring(override_dir) + L"\\config.json";
  }

  std::wstring dir = app_data_dir();
  if (dir.empty())
    return {};
  return dir + L"\\config.json";
}

void append_escaped_json_string(const std::string& s, std::string* out) {
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
          snprintf(buf, sizeof(buf), "\\u%04x", ch);
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
         std::isspace(static_cast<unsigned char>(s[*pos])))
    ++(*pos);
}

bool parse_json_string(const std::string& s, size_t* pos, std::string* out) {
  skip_ws(s, pos);
  if (*pos >= s.size() || s[*pos] != '"')
    return false;
  ++(*pos);
  std::string result;
  while (*pos < s.size()) {
    char ch = s[(*pos)++];
    if (ch == '"') {
      *out = std::move(result);
      return true;
    }
    if (ch == '\\') {
      if (*pos >= s.size())
        return false;
      char esc = s[(*pos)++];
      switch (esc) {
        case '"':  result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case '/':  result.push_back('/'); break;
        case 'b':  result.push_back('\b'); break;
        case 'f':  result.push_back('\f'); break;
        case 'n':  result.push_back('\n'); break;
        case 'r':  result.push_back('\r'); break;
        case 't':  result.push_back('\t'); break;
        case 'u':
          if (*pos + 4 <= s.size()) {
            unsigned int code = 0;
            bool ok = true;
            for (int i = 0; i < 4; ++i) {
              char h = s[*pos + static_cast<size_t>(i)];
              code <<= 4;
              if (h >= '0' && h <= '9') code |= static_cast<unsigned int>(h - '0');
              else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned int>(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned int>(h - 'A' + 10);
              else { ok = false; break; }
            }
            if (!ok)
              return false;
            *pos += 4;
            if (code < 0x80)
              result.push_back(static_cast<char>(code));
            else if (code < 0x800) {
              result.push_back(static_cast<char>(0xC0 | (code >> 6)));
              result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            } else {
              result.push_back(static_cast<char>(0xE0 | (code >> 12)));
              result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
              result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
          } else {
            return false;
          }
          break;
        default:
          return false;
      }
    } else {
      result.push_back(ch);
    }
  }
  return false;
}

bool parse_json_bool(const std::string& s, size_t* pos, bool* out) {
  skip_ws(s, pos);
  if (s.compare(*pos, 4, "true") == 0) {
    *pos += 4;
    *out = true;
    return true;
  }
  if (s.compare(*pos, 5, "false") == 0) {
    *pos += 5;
    *out = false;
    return true;
  }
  return false;
}

bool parse_json_int(const std::string& s, size_t* pos, int* out) {
  skip_ws(s, pos);
  size_t start = *pos;
  if (*pos < s.size() && (s[*pos] == '-' || s[*pos] == '+'))
    ++(*pos);
  size_t digits_start = *pos;
  while (*pos < s.size() && std::isdigit(static_cast<unsigned char>(s[*pos])))
    ++(*pos);
  if (*pos == digits_start)
    return false;
  *out = std::atoi(s.substr(start, *pos - start).c_str());
  return true;
}

bool parse_json_string_array(const std::string& s, size_t* pos,
                             std::vector<std::string>* out) {
  skip_ws(s, pos);
  if (*pos >= s.size() || s[*pos] != '[')
    return false;
  ++(*pos);
  std::vector<std::string> items;
  skip_ws(s, pos);
  if (*pos < s.size() && s[*pos] == ']') {
    ++(*pos);
    *out = std::move(items);
    return true;
  }
  for (;;) {
    std::string item;
    if (!parse_json_string(s, pos, &item))
      return false;
    items.push_back(std::move(item));
    skip_ws(s, pos);
    if (*pos < s.size() && s[*pos] == ',') {
      ++(*pos);
      continue;
    }
    if (*pos < s.size() && s[*pos] == ']') {
      ++(*pos);
      *out = std::move(items);
      return true;
    }
    return false;
  }
}

bool skip_json_value(const std::string& s, size_t* pos);

bool skip_json_container(const std::string& s, size_t* pos,
                         char open_ch, char close_ch) {
  if (*pos >= s.size() || s[*pos] != open_ch)
    return false;
  ++(*pos);
  int depth = 1;
  while (*pos < s.size() && depth > 0) {
    if (s[*pos] == '"') {
      std::string dummy;
      if (!parse_json_string(s, pos, &dummy))
        return false;
      continue;
    }
    if (s[*pos] == open_ch)
      ++depth;
    else if (s[*pos] == close_ch)
      --depth;
    ++(*pos);
  }
  return depth == 0;
}

bool skip_json_value(const std::string& s, size_t* pos) {
  skip_ws(s, pos);
  if (*pos >= s.size())
    return false;
  char ch = s[*pos];
  if (ch == '"') {
    std::string dummy;
    return parse_json_string(s, pos, &dummy);
  }
  if (ch == '{')
    return skip_json_container(s, pos, '{', '}');
  if (ch == '[')
    return skip_json_container(s, pos, '[', ']');
  if (ch == 't' || ch == 'f') {
    bool dummy = false;
    return parse_json_bool(s, pos, &dummy);
  }
  if (s.compare(*pos, 4, "null") == 0) {
    *pos += 4;
    return true;
  }
  while (*pos < s.size()) {
    char c = s[*pos];
    if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' ||
        c == '.' || c == 'e' || c == 'E') {
      ++(*pos);
    } else {
      break;
    }
  }
  return true;
}

void sanitize_config(WinomeConfig* cfg) {
  if (cfg->language != "auto" && cfg->language != "zh" && cfg->language != "en")
    cfg->language = "auto";
  if (cfg->clock_format != "auto" && cfg->clock_format != "24h" &&
      cfg->clock_format != "12h")
    cfg->clock_format = "auto";
  cfg->hot_corner_cooldown_ms = std::clamp(cfg->hot_corner_cooldown_ms, 100, 5000);
}

bool read_file_utf8(const std::wstring& path, std::string* out) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;

  LARGE_INTEGER sz{};
  bool ok = false;
  if (GetFileSizeEx(file, &sz) && sz.QuadPart >= 0 && sz.QuadPart <= 1048576) {
    std::string buf(static_cast<size_t>(sz.QuadPart), '\0');
    DWORD read_bytes = 0;
    if (sz.QuadPart == 0 ||
        (ReadFile(file, buf.data(), static_cast<DWORD>(sz.QuadPart),
                  &read_bytes, nullptr) &&
         read_bytes == static_cast<DWORD>(sz.QuadPart))) {
      *out = std::move(buf);
      ok = true;
    }
  }
  CloseHandle(file);
  return ok;
}

bool write_file_atomic(const std::wstring& path, const std::string& content) {
  std::wstring tmp = path + L".tmp";
  HANDLE file = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;

  DWORD written = 0;
  bool ok = WriteFile(file, content.data(),
                      static_cast<DWORD>(content.size()), &written, nullptr) &&
            written == static_cast<DWORD>(content.size());
  CloseHandle(file);
  if (!ok) {
    DeleteFileW(tmp.c_str());
    return false;
  }
  if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}

void notify_listeners() {
  std::vector<ListenerEntry> snapshot = g_listeners;
  for (const ListenerEntry& entry : snapshot) {
    if (entry.fn != nullptr)
      entry.fn(entry.user_data);
  }
}

void ensure_loaded() {
  if (g_loaded)
    return;
  g_loaded = true;
  std::wstring path = config_file_path();
  std::string content;
  if (!path.empty() && read_file_utf8(path, &content)) {
    WinomeConfig parsed;
    if (parse_config(content, &parsed)) {
      g_config = std::move(parsed);
      return;
    }
  }
  // Persist default config on first run so users can discover/edit config.json.
  if (!path.empty())
    write_file_atomic(path, serialize_config(g_config));
}

}  // namespace

std::string serialize_config(const WinomeConfig& cfg) {
  std::string out;
  out.reserve(512);
  out += "{\n";
  out += "  \"language\": ";
  append_escaped_json_string(cfg.language, &out);
  out += ",\n  \"clock_format\": ";
  append_escaped_json_string(cfg.clock_format, &out);
  out += ",\n  \"clock_show_seconds\": ";
  out += cfg.clock_show_seconds ? "true" : "false";
  out += ",\n  \"show_battery_percentage\": ";
  out += cfg.show_battery_percentage ? "true" : "false";
  out += ",\n  \"hot_corner_enabled\": ";
  out += cfg.hot_corner_enabled ? "true" : "false";
  out += ",\n  \"hot_corner_cooldown_ms\": ";
  out += std::to_string(cfg.hot_corner_cooldown_ms);
  out += ",\n  \"win_key_overview\": ";
  out += cfg.win_key_overview ? "true" : "false";
  out += ",\n  \"multi_monitor_panels\": ";
  out += cfg.multi_monitor_panels ? "true" : "false";
  out += ",\n  \"end_session_confirm\": ";
  out += cfg.end_session_confirm ? "true" : "false";
  out += ",\n  \"unify_dark_titlebars\": ";
  out += cfg.unify_dark_titlebars ? "true" : "false";
  out += ",\n  \"unify_rounded_corners\": ";
  out += cfg.unify_rounded_corners ? "true" : "false";
  out += ",\n  \"pinned_apps\": [";
  for (size_t i = 0; i < cfg.pinned_apps.size(); ++i) {
    if (i > 0)
      out += ", ";
    append_escaped_json_string(cfg.pinned_apps[i], &out);
  }
  out += "]\n}\n";
  return out;
}

bool parse_config(const std::string& json, WinomeConfig* out) {
  if (out == nullptr)
    return false;
  WinomeConfig cfg;
  size_t pos = 0;
  skip_ws(json, &pos);
  if (pos >= json.size() || json[pos] != '{')
    return false;
  ++pos;
  skip_ws(json, &pos);
  if (pos < json.size() && json[pos] == '}') {
    sanitize_config(&cfg);
    *out = std::move(cfg);
    return true;
  }

  for (;;) {
    std::string key;
    if (!parse_json_string(json, &pos, &key))
      return false;
    skip_ws(json, &pos);
    if (pos >= json.size() || json[pos] != ':')
      return false;
    ++pos;

    bool ok = true;
    if (key == "language")
      ok = parse_json_string(json, &pos, &cfg.language);
    else if (key == "clock_format")
      ok = parse_json_string(json, &pos, &cfg.clock_format);
    else if (key == "clock_show_seconds")
      ok = parse_json_bool(json, &pos, &cfg.clock_show_seconds);
    else if (key == "show_battery_percentage")
      ok = parse_json_bool(json, &pos, &cfg.show_battery_percentage);
    else if (key == "hot_corner_enabled")
      ok = parse_json_bool(json, &pos, &cfg.hot_corner_enabled);
    else if (key == "hot_corner_cooldown_ms")
      ok = parse_json_int(json, &pos, &cfg.hot_corner_cooldown_ms);
    else if (key == "win_key_overview")
      ok = parse_json_bool(json, &pos, &cfg.win_key_overview);
    else if (key == "multi_monitor_panels")
      ok = parse_json_bool(json, &pos, &cfg.multi_monitor_panels);
    else if (key == "end_session_confirm")
      ok = parse_json_bool(json, &pos, &cfg.end_session_confirm);
    else if (key == "unify_dark_titlebars")
      ok = parse_json_bool(json, &pos, &cfg.unify_dark_titlebars);
    else if (key == "unify_rounded_corners")
      ok = parse_json_bool(json, &pos, &cfg.unify_rounded_corners);
    else if (key == "pinned_apps")
      ok = parse_json_string_array(json, &pos, &cfg.pinned_apps);
    else
      ok = skip_json_value(json, &pos);

    if (!ok)
      return false;

    skip_ws(json, &pos);
    if (pos < json.size() && json[pos] == ',') {
      ++pos;
      continue;
    }
    if (pos < json.size() && json[pos] == '}') {
      ++pos;
      break;
    }
    return false;
  }

  sanitize_config(&cfg);
  *out = std::move(cfg);
  return true;
}

const WinomeConfig& get_config() {
  ensure_loaded();
  return g_config;
}

bool update_config(const WinomeConfig& cfg) {
  ensure_loaded();
  g_config = cfg;
  sanitize_config(&g_config);
  std::wstring path = config_file_path();
  bool ok = !path.empty() && write_file_atomic(path, serialize_config(g_config));
  notify_listeners();
  return ok;
}

bool reload_config() {
  g_loaded = true;
  std::wstring path = config_file_path();
  std::string content;
  if (path.empty() || !read_file_utf8(path, &content))
    return false;
  WinomeConfig parsed;
  if (!parse_config(content, &parsed))
    return false;
  g_config = std::move(parsed);
  notify_listeners();
  return true;
}

void add_config_listener(ConfigListener listener, void* user_data) {
  if (listener == nullptr)
    return;
  g_listeners.push_back(ListenerEntry{listener, user_data});
}

void remove_config_listener(ConfigListener listener, void* user_data) {
  g_listeners.erase(
      std::remove_if(g_listeners.begin(), g_listeners.end(),
                     [listener, user_data](const ListenerEntry& e) {
                       return e.fn == listener && e.user_data == user_data;
                     }),
      g_listeners.end());
}

}  // namespace winome
