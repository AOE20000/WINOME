// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Out-of-process stdio JSON plugin host with Win32 Job Object isolation,
// capability ACLs, rate limiting, and crash-loop circuit breaker.

#include "plugin-host.h"

#include "config.h"
#include "debug-runtime.h"
#include "host-utils.h"
#include "i18n.h"

#include <windows.h>
#include <glib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace winome {
namespace {

constexpr size_t kMaxPluginLineBytes = 65536;
constexpr DWORD kStopGraceMs = 350;

int64_t monotonic_ms() {
  return g_get_monotonic_time() / 1000;
}

std::string utf8_from_wide(const std::wstring& w) {
  if (w.empty())
    return {};
  int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                 static_cast<int>(w.size()),
                                 nullptr, 0, nullptr, nullptr);
  if (need <= 0)
    return {};
  std::string out(static_cast<size_t>(need), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                      &out[0], need, nullptr, nullptr);
  return out;
}

std::wstring wide_from_utf8(const std::string& s) {
  if (s.empty())
    return {};
  int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                 static_cast<int>(s.size()), nullptr, 0);
  if (need <= 0)
    return {};
  std::wstring out(static_cast<size_t>(need), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                      &out[0], need);
  return out;
}

std::string sanitize_utf8(const std::string& s) {
  if (s.empty())
    return {};
  if (g_utf8_validate(s.c_str(), static_cast<gssize>(s.size()), nullptr))
    return s;
  gchar* clean = g_utf8_make_valid(s.c_str(), static_cast<gssize>(s.size()));
  std::string out = clean ? clean : "";
  g_free(clean);
  return out;
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

void skip_ws(const std::string& s, size_t* pos) {
  while (*pos < s.size() &&
         std::isspace(static_cast<unsigned char>(s[*pos]))) {
    ++(*pos);
  }
}

bool hex4_to_u16(const std::string& s, size_t pos, uint16_t* out) {
  if (pos + 4 > s.size())
    return false;
  uint16_t val = 0;
  for (size_t i = 0; i < 4; ++i) {
    char c = s[pos + i];
    uint16_t d = 0;
    if (c >= '0' && c <= '9') d = static_cast<uint16_t>(c - '0');
    else if (c >= 'a' && c <= 'f') d = static_cast<uint16_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = static_cast<uint16_t>(c - 'A' + 10);
    else return false;
    val = static_cast<uint16_t>((val << 4) | d);
  }
  *out = val;
  return true;
}

void append_utf8_codepoint(uint32_t cp, std::string* out) {
  if (cp <= 0x7F) {
    out->push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out->push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out->push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0x10FFFF) {
    out->push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
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
        case 'u': {
          uint16_t w1 = 0;
          if (!hex4_to_u16(s, *pos, &w1))
            return false;
          *pos += 4;
          uint32_t cp = w1;
          if (w1 >= 0xD800 && w1 <= 0xDBFF &&
              *pos + 6 <= s.size() && s[*pos] == '\\' && s[*pos + 1] == 'u') {
            uint16_t w2 = 0;
            if (hex4_to_u16(s, *pos + 2, &w2) && w2 >= 0xDC00 && w2 <= 0xDFFF) {
              *pos += 6;
              cp = 0x10000 + (((static_cast<uint32_t>(w1 - 0xD800)) << 10) |
                              static_cast<uint32_t>(w2 - 0xDC00));
            }
          }
          append_utf8_codepoint(cp, &res);
          break;
        }
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
         s[*pos] != ']' && !std::isspace(static_cast<unsigned char>(s[*pos]))) {
    ++(*pos);
  }
  return true;
}

bool parse_json_str_array(const std::string& s, size_t* pos,
                          std::vector<std::string>* out) {
  skip_ws(s, pos);
  if (*pos >= s.size() || s[*pos] != '[')
    return false;
  ++(*pos);
  std::vector<std::string> items;
  while (*pos < s.size()) {
    skip_ws(s, pos);
    if (*pos >= s.size())
      return false;
    if (s[*pos] == ']') {
      ++(*pos);
      *out = std::move(items);
      return true;
    }
    if (s[*pos] == ',') {
      ++(*pos);
      continue;
    }
    std::string val;
    if (!parse_json_str(s, pos, &val))
      return false;
    items.push_back(std::move(val));
  }
  return false;
}

bool is_safe_plugin_id(const std::string& id) {
  if (id.empty() || id.size() > 64)
    return false;
  if (id == "." || id == ".." || id.find("..") != std::string::npos)
    return false;
  for (unsigned char ch : id) {
    if (!(std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-'))
      return false;
  }
  return true;
}

bool has_path_traversal(const std::string& path) {
  if (path.find('\0') != std::string::npos)
    return true;
  if (path == "..")
    return true;
  if (path.find("../") != std::string::npos ||
      path.find("..\\") != std::string::npos ||
      path.find("/..") != std::string::npos ||
      path.find("\\..") != std::string::npos) {
    return true;
  }
  return false;
}

std::wstring quote_win32_arg(const std::wstring& arg) {
  if (arg.empty())
    return L"\"\"";
  bool need_quotes = false;
  for (wchar_t c : arg) {
    if (c == L' ' || c == L'\t' || c == L'"') {
      need_quotes = true;
      break;
    }
  }
  if (!need_quotes)
    return arg;

  std::wstring out = L"\"";
  size_t bs = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') {
      ++bs;
    } else if (c == L'"') {
      out.append(bs * 2 + 1, L'\\');
      out.push_back(L'"');
      bs = 0;
    } else {
      out.append(bs, L'\\');
      out.push_back(c);
      bs = 0;
    }
  }
  out.append(bs * 2, L'\\');
  out.push_back(L'"');
  return out;
}

std::wstring resolve_exec_path(const std::wstring& plugin_dir,
                               const std::string& exec_utf8) {
  const std::wstring raw = wide_from_utf8(exec_utf8);
  if (raw.empty())
    return {};
  const bool is_abs = (raw.size() >= 2 && raw[1] == L':') ||
                      raw[0] == L'\\' || raw[0] == L'/';
  if (!is_abs && !plugin_dir.empty()) {
    const std::wstring candidate = plugin_dir + L"\\" + raw;
    const DWORD attrs = GetFileAttributesW(candidate.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      return candidate;
    }
  }
  return raw;
}

struct ParsedPluginMsg {
  std::string type;
  std::string slot;
  std::string icon;
  std::string text;
  std::string label;
  std::string tooltip;
  std::string title;
  std::string subtitle;
  std::string body;
  std::string message;
  std::string level;
  std::string app_name;
  bool active = false;
  bool has_active = false;
};

bool parse_plugin_message(const std::string& line, ParsedPluginMsg* out) {
  *out = ParsedPluginMsg{};
  size_t pos = 0;
  skip_ws(line, &pos);
  if (pos >= line.size() || line[pos] != '{')
    return false;
  ++pos;
  while (pos < line.size()) {
    skip_ws(line, &pos);
    if (pos >= line.size())
      return false;
    if (line[pos] == '}') {
      return !out->type.empty();
    }
    if (line[pos] == ',') {
      ++pos;
      continue;
    }
    std::string key;
    if (!parse_json_str(line, &pos, &key))
      return false;
    skip_ws(line, &pos);
    if (pos >= line.size() || line[pos] != ':')
      return false;
    ++pos;

    auto assign_str = [&](std::string* dst) {
      std::string v;
      if (!parse_json_str(line, &pos, &v))
        return false;
      *dst = sanitize_utf8(v);
      return true;
    };

    if (key == "type") {
      if (!assign_str(&out->type)) return false;
    } else if (key == "slot") {
      if (!assign_str(&out->slot)) return false;
    } else if (key == "icon") {
      if (!assign_str(&out->icon)) return false;
    } else if (key == "text") {
      if (!assign_str(&out->text)) return false;
    } else if (key == "label") {
      if (!assign_str(&out->label)) return false;
    } else if (key == "tooltip") {
      if (!assign_str(&out->tooltip)) return false;
    } else if (key == "title") {
      if (!assign_str(&out->title)) return false;
    } else if (key == "subtitle") {
      if (!assign_str(&out->subtitle)) return false;
    } else if (key == "body") {
      if (!assign_str(&out->body)) return false;
    } else if (key == "message") {
      if (!assign_str(&out->message)) return false;
    } else if (key == "level") {
      if (!assign_str(&out->level)) return false;
    } else if (key == "app_name") {
      if (!assign_str(&out->app_name)) return false;
    } else if (key == "active") {
      bool b = false;
      if (!parse_json_bool(line, &pos, &b)) return false;
      out->active = b;
      out->has_active = true;
    } else {
      if (!skip_val(line, &pos)) return false;
    }
  }
  return false;
}

struct PluginRun : std::enable_shared_from_this<PluginRun> {
  std::string plugin_id;
  uint64_t generation = 0;

  std::mutex job_mutex;
  HANDLE hJob = nullptr;
  HANDLE hProcess = nullptr;
  HANDLE hProcessExitedEvent = nullptr;
  DWORD pid = 0;

  std::mutex stdin_mutex;
  HANDLE stdin_w = nullptr;
  HANDLE stdout_r = nullptr;
  HANDLE stderr_r = nullptr;

  std::atomic<bool> stop_requested{false};
  std::condition_variable stdin_cv;
  std::deque<std::string> stdin_queue;
  bool stdin_closed = false;
  std::thread stdin_thread;
  std::mutex wait_state_mutex;
  std::thread stdout_thread;
  std::thread stderr_thread;
  std::thread wait_thread;

  static constexpr size_t kMaxStdinQueue = 128;

  ~PluginRun() {
    if (hProcessExitedEvent != nullptr) {
      CloseHandle(hProcessExitedEvent);
      hProcessExitedEvent = nullptr;
    }
  }

  // Non-blocking enqueue so a hung plugin filling its 4 KB pipe buffer can
  // never freeze the GTK main thread or deadlock stdout_thread.
  bool write_line(const std::string& line) {
    std::string buf = line;
    if (buf.empty() || buf.back() != '\n')
      buf.push_back('\n');
    {
      std::lock_guard<std::mutex> lock(stdin_mutex);
      if (stdin_closed || stdin_w == nullptr || stdin_queue.size() >= kMaxStdinQueue)
        return false;
      stdin_queue.push_back(std::move(buf));
    }
    stdin_cv.notify_one();
    return true;
  }

  void close_stdin_locked() {
    stdin_closed = true;
    stdin_cv.notify_all();
  }

  void stdin_writer_loop() {
    for (;;) {
      std::string msg;
      HANDLE pipe = nullptr;
      {
        std::unique_lock<std::mutex> lock(stdin_mutex);
        stdin_cv.wait(lock, [this] {
          return stdin_closed || !stdin_queue.empty();
        });
        if (!stdin_queue.empty()) {
          msg = std::move(stdin_queue.front());
          stdin_queue.pop_front();
          pipe = stdin_w;
        } else if (stdin_closed) {
          if (stdin_w != nullptr) {
            CloseHandle(stdin_w);
            stdin_w = nullptr;
          }
          break;
        }
      }
      if (pipe == nullptr)
        break;
      const char* ptr = msg.data();
      size_t rem = msg.size();
      bool ok = true;
      while (rem > 0) {
        DWORD written = 0;
        if (!WriteFile(pipe, ptr, static_cast<DWORD>(rem), &written, nullptr) ||
            written == 0) {
          ok = false;
          break;
        }
        ptr += written;
        rem -= written;
      }
      if (!ok) {
        std::lock_guard<std::mutex> lock(stdin_mutex);
        stdin_closed = true;
        stdin_queue.clear();
        if (stdin_w != nullptr) {
          CloseHandle(stdin_w);
          stdin_w = nullptr;
        }
        break;
      }
    }
  }
};

struct PluginRecord {
  PluginManifest manifest;
  PluginState state = PluginState::Stopped;
  PluginCrashTracker crash_tracker;
  PluginRateLimiter rate_limiter;
  std::shared_ptr<PluginRun> current_run;
  uint64_t generation = 0;
  DWORD pid = 0;
  int crash_count = 0;
  int64_t last_msg_ms = 0;
  std::string last_error;
  std::vector<std::string> owned_topbar_keys;
  std::vector<std::string> owned_quicktoggle_keys;
};

std::mutex g_mu;
std::vector<PluginRecord> g_plugins;
PluginHostCallbacks g_cbs {};
std::atomic<uint64_t> g_epoch{1};
bool g_initialized = false;
bool g_shutting_down = false;
std::mutex g_worker_mu;
std::condition_variable g_worker_cv;
int g_active_workers = 0;

struct ActiveWorkerGuard {
  ~ActiveWorkerGuard() {
    std::lock_guard<std::mutex> lock(g_worker_mu);
    if (g_active_workers > 0)
      --g_active_workers;
    g_worker_cv.notify_all();
  }
};

bool is_disabled_in_config(const std::string& id) {
  const auto& dis = get_config().disabled_plugins;
  return std::find(dis.begin(), dis.end(), id) != dis.end();
}

void set_disabled_in_config(const std::string& id, bool disabled) {
  WinomeConfig cfg = get_config();
  auto it = std::find(cfg.disabled_plugins.begin(),
                      cfg.disabled_plugins.end(), id);
  if (disabled) {
    if (it == cfg.disabled_plugins.end()) {
      cfg.disabled_plugins.push_back(id);
      update_config(cfg);
    }
  } else {
    if (it != cfg.disabled_plugins.end()) {
      cfg.disabled_plugins.erase(it);
      update_config(cfg);
    }
  }
}

std::string make_slot_key(const std::string& plugin_id,
                          const std::string& slot) {
  return slot.empty() ? plugin_id : (plugin_id + ":" + slot);
}

void track_key(std::vector<std::string>* vec, const std::string& key) {
  if (std::find(vec->begin(), vec->end(), key) == vec->end())
    vec->push_back(key);
}

void untrack_key(std::vector<std::string>* vec, const std::string& key) {
  vec->erase(std::remove(vec->begin(), vec->end(), key), vec->end());
}

// Schedule UI callbacks on the GLib main loop safely from worker threads.
void schedule_plugins_changed() {
  const uint64_t epoch = g_epoch.load();
  auto fn = g_cbs.plugins_changed;
  if (fn == nullptr)
    return;
  g_idle_add(
      +[](gpointer p) -> gboolean {
        const auto ep = reinterpret_cast<uintptr_t>(p);
        if (ep == static_cast<uintptr_t>(g_epoch.load()) &&
            !g_shutting_down && g_cbs.plugins_changed != nullptr) {
          g_cbs.plugins_changed();
        }
        return G_SOURCE_REMOVE;
      },
      reinterpret_cast<gpointer>(static_cast<uintptr_t>(epoch)));
}

void schedule_topbar_upsert(const PluginTopBarSpec& spec) {
  if (g_cbs.topbar_upsert == nullptr)
    return;
  struct Payload {
    uint64_t epoch;
    PluginTopBarSpec spec;
  };
  auto* pay = new Payload{g_epoch.load(), spec};
  g_idle_add(
      +[](gpointer p) -> gboolean {
        std::unique_ptr<Payload> d(static_cast<Payload*>(p));
        if (d->epoch == g_epoch.load() && !g_shutting_down &&
            g_cbs.topbar_upsert != nullptr) {
          g_cbs.topbar_upsert(d->spec);
        }
        return G_SOURCE_REMOVE;
      },
      pay);
}

void schedule_topbar_remove(const std::string& plugin_id,
                            const std::string& key) {
  if (g_cbs.topbar_remove == nullptr)
    return;
  struct Payload {
    uint64_t epoch;
    std::string plugin_id;
    std::string key;
  };
  auto* pay = new Payload{g_epoch.load(), plugin_id, key};
  g_idle_add(
      +[](gpointer p) -> gboolean {
        std::unique_ptr<Payload> d(static_cast<Payload*>(p));
        if (d->epoch == g_epoch.load() && !g_shutting_down &&
            g_cbs.topbar_remove != nullptr) {
          g_cbs.topbar_remove(d->plugin_id, d->key);
        }
        return G_SOURCE_REMOVE;
      },
      pay);
}

void schedule_quicktoggle_upsert(const PluginQuickToggleSpec& spec) {
  if (g_cbs.quicktoggle_upsert == nullptr)
    return;
  struct Payload {
    uint64_t epoch;
    PluginQuickToggleSpec spec;
  };
  auto* pay = new Payload{g_epoch.load(), spec};
  g_idle_add(
      +[](gpointer p) -> gboolean {
        std::unique_ptr<Payload> d(static_cast<Payload*>(p));
        if (d->epoch == g_epoch.load() && !g_shutting_down &&
            g_cbs.quicktoggle_upsert != nullptr) {
          g_cbs.quicktoggle_upsert(d->spec);
        }
        return G_SOURCE_REMOVE;
      },
      pay);
}

void schedule_quicktoggle_remove(const std::string& plugin_id,
                                 const std::string& key) {
  if (g_cbs.quicktoggle_remove == nullptr)
    return;
  struct Payload {
    uint64_t epoch;
    std::string plugin_id;
    std::string key;
  };
  auto* pay = new Payload{g_epoch.load(), plugin_id, key};
  g_idle_add(
      +[](gpointer p) -> gboolean {
        std::unique_ptr<Payload> d(static_cast<Payload*>(p));
        if (d->epoch == g_epoch.load() && !g_shutting_down &&
            g_cbs.quicktoggle_remove != nullptr) {
          g_cbs.quicktoggle_remove(d->plugin_id, d->key);
        }
        return G_SOURCE_REMOVE;
      },
      pay);
}

void schedule_notify(const std::string& title,
                     const std::string& body,
                     const std::string& app_name) {
  if (g_cbs.notify == nullptr)
    return;
  struct Payload {
    uint64_t epoch;
    std::string title;
    std::string body;
    std::string app_name;
  };
  auto* pay = new Payload{g_epoch.load(), title, body, app_name};
  g_idle_add(
      +[](gpointer p) -> gboolean {
        std::unique_ptr<Payload> d(static_cast<Payload*>(p));
        if (d->epoch == g_epoch.load() && !g_shutting_down &&
            g_cbs.notify != nullptr) {
          g_cbs.notify(d->title, d->body, d->app_name);
        }
        return G_SOURCE_REMOVE;
      },
      pay);
}

void emit_clear_slots_async(const std::string& plugin_id,
                            const std::vector<std::string>& tb_keys,
                            const std::vector<std::string>& qs_keys) {
  for (const auto& k : tb_keys)
    schedule_topbar_remove(plugin_id, k);
  for (const auto& k : qs_keys)
    schedule_quicktoggle_remove(plugin_id, k);
}

void send_hello_to_run(const std::shared_ptr<PluginRun>& run,
                       const PluginManifest& m) {
  std::string msg = "{\"type\":\"hello\",\"protocol\":1,\"plugin_id\":";
  append_json_escaped(m.id, &msg);
  msg += ",\"developer_mode\":";
  msg += (get_config().developer_mode ? "true" : "false");
  msg += ",\"capabilities\":[";
  for (size_t i = 0; i < m.capabilities.size(); ++i) {
    if (i > 0) msg.push_back(',');
    append_json_escaped(m.capabilities[i], &msg);
  }
  msg += "]}";
  run->write_line(msg);
}

void send_error_to_run(const std::shared_ptr<PluginRun>& run,
                       const std::string& code,
                       const std::string& op) {
  std::string msg = "{\"type\":\"error\",\"code\":";
  append_json_escaped(code, &msg);
  msg += ",\"op\":";
  append_json_escaped(op, &msg);
  msg.push_back('}');
  run->write_line(msg);
}

void handle_plugin_line(const std::shared_ptr<PluginRun>& run,
                        const std::string& line) {
  if (line.empty())
    return;
  if (line.size() > kMaxPluginLineBytes) {
    debug_append_log(("plugin:" + run->plugin_id).c_str(),
                     "dropped oversized message (>64KB)");
    send_error_to_run(run, "payload_too_large", "line");
    return;
  }

  ParsedPluginMsg msg;
  if (!parse_plugin_message(line, &msg)) {
    debug_append_log(("plugin:" + run->plugin_id).c_str(),
                     "invalid json line: " + sanitize_utf8(line.substr(0, 120)));
    return;
  }

  const int64_t now = monotonic_ms();
  const std::string log_tag = "plugin:" + run->plugin_id;

  bool rate_limited = false;
  bool cap_denied = false;
  bool state_changed = false;
  std::string denied_op;

  PluginTopBarSpec tb_upsert {};
  bool do_tb_upsert = false;
  std::string tb_remove_key;
  bool do_tb_remove = false;

  PluginQuickToggleSpec qs_upsert {};
  bool do_qs_upsert = false;
  std::string qs_remove_key;
  bool do_qs_remove = false;

  std::string notify_title;
  std::string notify_body;
  std::string notify_app;
  bool do_notify = false;

  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_shutting_down)
      return;
    PluginRecord* rec = nullptr;
    for (auto& p : g_plugins) {
      if (p.manifest.id == run->plugin_id) {
        rec = &p;
        break;
      }
    }
    if (rec == nullptr || rec->generation != run->generation)
      return;

    if (!rec->rate_limiter.allow(now)) {
      rate_limited = true;
    } else {
      rec->last_msg_ms = now;
      if (msg.type == "ready") {
        if (rec->state == PluginState::Starting) {
          rec->state = PluginState::Running;
          rec->last_error.clear();
          state_changed = true;
        }
      } else if (msg.type == "pong") {
        // Last message timestamp updated above.
      } else if (msg.type == "log") {
        const std::string txt = msg.message.empty() ? msg.text : msg.message;
        debug_append_log(log_tag.c_str(),
                         (msg.level.empty() ? "" : "[" + msg.level + "] ") + txt);
      } else if (msg.type == "topbar.upsert") {
        if (!plugin_has_capability(rec->manifest, "topbar")) {
          cap_denied = true;
          denied_op = msg.type;
          rec->last_error = "capability denied: topbar";
        } else {
          if (rec->state == PluginState::Starting) {
            rec->state = PluginState::Running;
            state_changed = true;
          }
          tb_upsert.plugin_id = rec->manifest.id;
          tb_upsert.slot = msg.slot;
          tb_upsert.key = make_slot_key(rec->manifest.id, msg.slot);
          tb_upsert.icon = msg.icon;
          tb_upsert.text = msg.text.empty() ? msg.label : msg.text;
          tb_upsert.tooltip = msg.tooltip;
          track_key(&rec->owned_topbar_keys, tb_upsert.key);
          do_tb_upsert = true;
        }
      } else if (msg.type == "topbar.remove") {
        if (!plugin_has_capability(rec->manifest, "topbar")) {
          cap_denied = true;
          denied_op = msg.type;
        } else {
          tb_remove_key = make_slot_key(rec->manifest.id, msg.slot);
          untrack_key(&rec->owned_topbar_keys, tb_remove_key);
          do_tb_remove = true;
        }
      } else if (msg.type == "quicksettings.upsert") {
        if (!plugin_has_capability(rec->manifest, "quicksettings")) {
          cap_denied = true;
          denied_op = msg.type;
          rec->last_error = "capability denied: quicksettings";
        } else {
          if (rec->state == PluginState::Starting) {
            rec->state = PluginState::Running;
            state_changed = true;
          }
          qs_upsert.plugin_id = rec->manifest.id;
          qs_upsert.slot = msg.slot;
          qs_upsert.key = make_slot_key(rec->manifest.id, msg.slot);
          qs_upsert.icon = msg.icon.empty() ? "application-x-addon-symbolic" : msg.icon;
          qs_upsert.title = msg.title.empty() ? rec->manifest.name : msg.title;
          qs_upsert.subtitle = msg.subtitle;
          qs_upsert.active = msg.has_active ? msg.active : false;
          track_key(&rec->owned_quicktoggle_keys, qs_upsert.key);
          do_qs_upsert = true;
        }
      } else if (msg.type == "quicksettings.remove") {
        if (!plugin_has_capability(rec->manifest, "quicksettings")) {
          cap_denied = true;
          denied_op = msg.type;
        } else {
          qs_remove_key = make_slot_key(rec->manifest.id, msg.slot);
          untrack_key(&rec->owned_quicktoggle_keys, qs_remove_key);
          do_qs_remove = true;
        }
      } else if (msg.type == "notify") {
        if (!plugin_has_capability(rec->manifest, "notify")) {
          cap_denied = true;
          denied_op = msg.type;
          rec->last_error = "capability denied: notify";
        } else {
          notify_title = msg.title.empty() ? rec->manifest.name : msg.title;
          notify_body = msg.body.empty() ? msg.text : msg.body;
          notify_app = msg.app_name.empty() ? rec->manifest.name : msg.app_name;
          do_notify = true;
        }
      } else {
        debug_append_log(log_tag.c_str(), "unknown msg type: " + msg.type);
      }
    }
  }

  if (rate_limited) {
    debug_append_log(log_tag.c_str(), "rate limit exceeded (>30 msg/s), message dropped");
    send_error_to_run(run, "rate_limited", msg.type);
    return;
  }
  if (cap_denied) {
    debug_append_log(log_tag.c_str(), "capability denied for op: " + denied_op);
    send_error_to_run(run, "capability_denied", denied_op);
  }
  if (do_tb_upsert) schedule_topbar_upsert(tb_upsert);
  if (do_tb_remove) schedule_topbar_remove(run->plugin_id, tb_remove_key);
  if (do_qs_upsert) schedule_quicktoggle_upsert(qs_upsert);
  if (do_qs_remove) schedule_quicktoggle_remove(run->plugin_id, qs_remove_key);
  if (do_notify) schedule_notify(notify_title, notify_body, notify_app);
  if (state_changed) schedule_plugins_changed();
}

void read_pipe_lines(HANDLE pipe, bool is_stderr,
                     std::shared_ptr<PluginRun> run) {
  std::string buf;
  char chunk[4096];
  bool overflow = false;
  const std::string tag = "plugin:" + run->plugin_id;

  for (;;) {
    DWORD got = 0;
    if (!ReadFile(pipe, chunk, sizeof(chunk), &got, nullptr) || got == 0)
      break;
    for (DWORD i = 0; i < got; ++i) {
      char c = chunk[i];
      if (c == '\r')
        continue;
      if (c == '\n') {
        if (overflow) {
          debug_append_log(tag.c_str(), "oversized stream line truncated");
          overflow = false;
        } else if (!buf.empty()) {
          if (is_stderr)
            debug_append_log(tag.c_str(), "[stderr] " + sanitize_utf8(buf));
          else
            handle_plugin_line(run, buf);
        }
        buf.clear();
      } else if (!overflow) {
        if (buf.size() < kMaxPluginLineBytes + 16)
          buf.push_back(c);
        else
          overflow = true;
      }
    }
  }

  if (!overflow && !buf.empty()) {
    if (is_stderr)
      debug_append_log(tag.c_str(), "[stderr] " + sanitize_utf8(buf));
    else
      handle_plugin_line(run, buf);
  }
}

void on_run_unexpected_exit(const std::string& plugin_id,
                            uint64_t generation,
                            DWORD exit_code);

void wait_worker(std::shared_ptr<PluginRun> run) {
  ActiveWorkerGuard worker_guard;
  WaitForSingleObject(run->hProcess, INFINITE);
  DWORD exit_code = 0;
  GetExitCodeProcess(run->hProcess, &exit_code);
  SetEvent(run->hProcessExitedEvent);

  {
    std::lock_guard<std::mutex> jlock(run->job_mutex);
    if (run->hJob != nullptr) {
      TerminateJobObject(run->hJob, 0);
      CloseHandle(run->hJob);
      run->hJob = nullptr;
    }
  }
  {
    std::lock_guard<std::mutex> slock(run->stdin_mutex);
    run->close_stdin_locked();
  }

  if (run->stdin_thread.joinable())
    run->stdin_thread.join();
  if (run->stdout_thread.joinable())
    run->stdout_thread.join();
  if (run->stderr_thread.joinable())
    run->stderr_thread.join();

  if (run->stdout_r != nullptr) {
    CloseHandle(run->stdout_r);
    run->stdout_r = nullptr;
  }
  if (run->stderr_r != nullptr) {
    CloseHandle(run->stderr_r);
    run->stderr_r = nullptr;
  }
  if (run->hProcess != nullptr) {
    CloseHandle(run->hProcess);
    run->hProcess = nullptr;
  }

  if (!run->stop_requested.load())
    on_run_unexpected_exit(run->plugin_id, run->generation, exit_code);

  // If no external stopper moved wait_thread to join it, detach before returning
  // so destroying the last shared_ptr<PluginRun> never destroys a joinable thread.
  {
    std::lock_guard<std::mutex> wlock(run->wait_state_mutex);
    if (run->wait_thread.joinable())
      run->wait_thread.detach();
  }
}

void stop_run_blocking(std::shared_ptr<PluginRun> run) {
  if (!run)
    return;
  run->stop_requested.store(true);
  run->write_line("{\"type\":\"shutdown\"}");
  {
    std::lock_guard<std::mutex> slock(run->stdin_mutex);
    run->close_stdin_locked();
  }

  if (run->hProcessExitedEvent != nullptr &&
      WaitForSingleObject(run->hProcessExitedEvent, kStopGraceMs) == WAIT_TIMEOUT) {
    std::lock_guard<std::mutex> jlock(run->job_mutex);
    if (run->hJob != nullptr) {
      TerminateJobObject(run->hJob, 0);
      CloseHandle(run->hJob);
      run->hJob = nullptr;
    }
  }
  std::thread t;
  {
    std::lock_guard<std::mutex> wlock(run->wait_state_mutex);
    t = std::move(run->wait_thread);
  }
  if (t.joinable())
    t.join();
}

bool launch_plugin_locked(PluginRecord* rec, std::string* out_err) {
  if (rec->manifest.exec.empty()) {
    if (out_err) *out_err = "empty exec path";
    return false;
  }

  SECURITY_ATTRIBUTES sa {};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE stdin_r = nullptr, stdin_w = nullptr;
  HANDLE stdout_r = nullptr, stdout_w = nullptr;
  HANDLE stderr_r = nullptr, stderr_w = nullptr;

  auto close_pipe_pair = [](HANDLE* a, HANDLE* b) {
    if (*a != nullptr) { CloseHandle(*a); *a = nullptr; }
    if (*b != nullptr) { CloseHandle(*b); *b = nullptr; }
  };

  if (!CreatePipe(&stdin_r, &stdin_w, &sa, 0) ||
      !CreatePipe(&stdout_r, &stdout_w, &sa, 0) ||
      !CreatePipe(&stderr_r, &stderr_w, &sa, 0)) {
    close_pipe_pair(&stdin_r, &stdin_w);
    close_pipe_pair(&stdout_r, &stdout_w);
    close_pipe_pair(&stderr_r, &stderr_w);
    if (out_err) *out_err = "CreatePipe failed";
    return false;
  }

  SetHandleInformation(stdin_w, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(stdout_r, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(stderr_r, HANDLE_FLAG_INHERIT, 0);

  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job != nullptr) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info {};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (rec->manifest.max_memory_mb > 0) {
      info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
      info.ProcessMemoryLimit =
          static_cast<SIZE_T>(rec->manifest.max_memory_mb) * 1024ULL * 1024ULL;
    }
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &info, sizeof(info))) {
      info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      info.ProcessMemoryLimit = 0;
      SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                              &info, sizeof(info));
    }
  }

  const std::wstring exe_w = resolve_exec_path(rec->manifest.dir, rec->manifest.exec);
  std::wstring cmdline = quote_win32_arg(exe_w);
  for (const auto& a : rec->manifest.args) {
    cmdline.push_back(L' ');
    cmdline += quote_win32_arg(wide_from_utf8(a));
  }
  std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
  cmd_buf.push_back(L'\0');

  STARTUPINFOW si {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = stdin_r;
  si.hStdOutput = stdout_w;
  si.hStdError = stderr_w;

  PROCESS_INFORMATION pi {};
  const wchar_t* cwd = rec->manifest.dir.empty() ? nullptr : rec->manifest.dir.c_str();
  BOOL ok = CreateProcessW(
      nullptr,
      cmd_buf.data(),
      nullptr,
      nullptr,
      TRUE,
      CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
      nullptr,
      cwd,
      &si,
      &pi);

  // Close child ends immediately in parent process.
  CloseHandle(stdin_r);
  CloseHandle(stdout_w);
  CloseHandle(stderr_w);

  if (!ok) {
    DWORD gle = GetLastError();
    if (job != nullptr) CloseHandle(job);
    CloseHandle(stdin_w);
    CloseHandle(stdout_r);
    CloseHandle(stderr_r);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "CreateProcessW failed (err=%lu)",
                  static_cast<unsigned long>(gle));
    rec->last_error = buf;
    rec->state = PluginState::Stopped;
    if (out_err) *out_err = buf;
    return false;
  }

  if (job != nullptr)
    AssignProcessToJobObject(job, pi.hProcess);

  HANDLE exited_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);

  rec->generation += 1;
  rec->pid = pi.dwProcessId;
  rec->state = PluginState::Starting;
  rec->rate_limiter.reset();
  rec->last_error.clear();

  auto run = std::make_shared<PluginRun>();
  run->plugin_id = rec->manifest.id;
  run->generation = rec->generation;
  run->hJob = job;
  run->hProcess = pi.hProcess;
  run->hProcessExitedEvent = exited_ev;
  run->pid = pi.dwProcessId;
  run->stdin_w = stdin_w;
  run->stdout_r = stdout_r;
  run->stderr_r = stderr_r;
  rec->current_run = run;

  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);

  send_hello_to_run(run, rec->manifest);

  {
    std::lock_guard<std::mutex> wlock(g_worker_mu);
    ++g_active_workers;
  }
  {
    std::lock_guard<std::mutex> lock_ws(run->wait_state_mutex);
    run->stdin_thread = std::thread([run]() { run->stdin_writer_loop(); });
    run->stdout_thread = std::thread(read_pipe_lines, run->stdout_r, false, run);
    run->stderr_thread = std::thread(read_pipe_lines, run->stderr_r, true, run);
    run->wait_thread = std::thread(wait_worker, run);
  }

  debug_append_log(("plugin:" + rec->manifest.id).c_str(),
                   "started pid=" + std::to_string(run->pid));
  return true;
}

void on_run_unexpected_exit(const std::string& plugin_id,
                            uint64_t generation,
                            DWORD exit_code) {
  std::vector<std::string> tb_clear;
  std::vector<std::string> qs_clear;
  bool tripped = false;
  bool should_restart = false;
  int backoff_ms = 500;
  uint64_t epoch = 0;

  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_shutting_down)
      return;
    PluginRecord* rec = nullptr;
    for (auto& p : g_plugins) {
      if (p.manifest.id == plugin_id) {
        rec = &p;
        break;
      }
    }
    if (rec == nullptr || rec->generation != generation)
      return;

    rec->current_run.reset();
    rec->pid = 0;
    tb_clear.swap(rec->owned_topbar_keys);
    qs_clear.swap(rec->owned_quicktoggle_keys);

    if (rec->state == PluginState::Disabled) {
      schedule_plugins_changed();
      emit_clear_slots_async(plugin_id, tb_clear, qs_clear);
      return;
    }

    const int64_t now = monotonic_ms();
    tripped = rec->crash_tracker.record_crash(now);
    rec->crash_count = rec->crash_tracker.recent_count(now);

    if (tripped) {
      rec->state = PluginState::Tripped;
      rec->last_error = "tripped after 3 crashes in 60s (exit=" +
                        std::to_string(static_cast<unsigned long>(exit_code)) + ")";
    } else {
      rec->state = PluginState::Stopped;
      rec->last_error = "exited unexpectedly (code=" +
                        std::to_string(static_cast<unsigned long>(exit_code)) + ")";
      should_restart = true;
      if (rec->crash_count >= 2)
        backoff_ms = 1200;
      epoch = g_epoch.load();
    }
  }

  emit_clear_slots_async(plugin_id, tb_clear, qs_clear);
  debug_append_log(("plugin:" + plugin_id).c_str(),
                   tripped ? "circuit breaker tripped (3 crashes in 60s)"
                           : "unexpected exit; scheduling restart");

  if (tripped) {
    schedule_notify(
        tr("Plugin Tripped", "插件已熔断"),
        plugin_id + tr(" crashed 3 times within 60s and was stopped.",
                       " 在 60 秒内连续崩溃 3 次，已被安全熔断。"),
        "WINOME Plugins");
  } else if (should_restart) {
    struct RestartReq {
      uint64_t epoch;
      std::string plugin_id;
      uint64_t generation;
    };
    auto* req = new RestartReq{epoch, plugin_id, generation};
    g_timeout_add(
        static_cast<guint>(backoff_ms),
        +[](gpointer p) -> gboolean {
          std::unique_ptr<RestartReq> r(static_cast<RestartReq*>(p));
          if (r->epoch != g_epoch.load() || g_shutting_down)
            return G_SOURCE_REMOVE;
          std::shared_ptr<PluginRun> dummy;
          bool ok = false;
          {
            std::lock_guard<std::mutex> lock(g_mu);
            if (!g_shutting_down) {
              for (auto& rec : g_plugins) {
                if (rec.manifest.id == r->plugin_id &&
                    rec.generation == r->generation &&
                    rec.state == PluginState::Stopped &&
                    rec.manifest.enabled &&
                    !is_disabled_in_config(rec.manifest.id)) {
                  std::string err;
                  ok = launch_plugin_locked(&rec, &err);
                  break;
                }
              }
            }
          }
          if (ok)
            schedule_plugins_changed();
          return G_SOURCE_REMOVE;
        },
        req);
  }
  schedule_plugins_changed();
}

std::string read_file_utf8(const std::wstring& path) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE)
    return {};
  LARGE_INTEGER sz {};
  std::string out;
  if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart <= 1048576) {
    out.resize(static_cast<size_t>(sz.QuadPart));
    DWORD got = 0;
    if (!ReadFile(f, &out[0], static_cast<DWORD>(out.size()), &got, nullptr))
      out.clear();
    else
      out.resize(got);
  }
  CloseHandle(f);
  return out;
}

}  // namespace

PluginRateLimiter::PluginRateLimiter(int max_messages, int64_t window_ms)
    : max_messages_(max_messages > 0 ? max_messages : 30),
      window_ms_(window_ms > 0 ? window_ms : 1000) {}

bool PluginRateLimiter::allow(int64_t now_ms) {
  if (window_start_ms_ == 0 || (now_ms - window_start_ms_) >= window_ms_) {
    window_start_ms_ = now_ms;
    count_in_window_ = 0;
  }
  if (count_in_window_ >= max_messages_) {
    ++dropped_;
    return false;
  }
  ++count_in_window_;
  return true;
}

void PluginRateLimiter::reset() {
  window_start_ms_ = 0;
  count_in_window_ = 0;
  dropped_ = 0;
}

PluginCrashTracker::PluginCrashTracker(int threshold, int64_t window_ms)
    : threshold_(threshold > 0 ? threshold : 3),
      window_ms_(window_ms > 0 ? window_ms : 60000) {}

bool PluginCrashTracker::record_crash(int64_t now_ms) {
  timestamps_.push_back(now_ms);
  const int count = recent_count(now_ms);
  return count >= threshold_;
}

int PluginCrashTracker::recent_count(int64_t now_ms) {
  timestamps_.erase(
      std::remove_if(timestamps_.begin(), timestamps_.end(),
                     [&](int64_t ts) { return (now_ms - ts) > window_ms_; }),
      timestamps_.end());
  return static_cast<int>(timestamps_.size());
}

void PluginCrashTracker::reset() {
  timestamps_.clear();
}

const char* plugin_state_name(PluginState s) {
  switch (s) {
    case PluginState::Stopped:  return "Stopped";
    case PluginState::Starting: return "Starting";
    case PluginState::Running:  return "Running";
    case PluginState::Tripped:  return "Tripped";
    case PluginState::Disabled: return "Disabled";
  }
  return "Unknown";
}

bool plugin_has_capability(const PluginManifest& m, const char* cap) {
  if (cap == nullptr)
    return false;
  for (const auto& c : m.capabilities) {
    if (c == cap)
      return true;
  }
  return false;
}

bool parse_plugin_manifest(const std::string& json_text,
                           const std::wstring& plugin_dir,
                           PluginManifest* out,
                           std::string* out_err) {
  if (out == nullptr)
    return false;
  *out = PluginManifest{};
  out->dir = plugin_dir;

  size_t pos = 0;
  skip_ws(json_text, &pos);
  if (pos >= json_text.size() || json_text[pos] != '{') {
    if (out_err) *out_err = "manifest root must be a JSON object";
    return false;
  }
  ++pos;

  while (pos < json_text.size()) {
    skip_ws(json_text, &pos);
    if (pos >= json_text.size()) {
      if (out_err) *out_err = "unterminated manifest JSON";
      return false;
    }
    if (json_text[pos] == '}') {
      break;
    }
    if (json_text[pos] == ',') {
      ++pos;
      continue;
    }

    std::string key;
    if (!parse_json_str(json_text, &pos, &key)) {
      if (out_err) *out_err = "invalid manifest key";
      return false;
    }
    skip_ws(json_text, &pos);
    if (pos >= json_text.size() || json_text[pos] != ':') {
      if (out_err) *out_err = "expected ':' after key";
      return false;
    }
    ++pos;

    if (key == "id") {
      if (!parse_json_str(json_text, &pos, &out->id)) {
        if (out_err) *out_err = "invalid id string";
        return false;
      }
    } else if (key == "name") {
      if (!parse_json_str(json_text, &pos, &out->name)) {
        if (out_err) *out_err = "invalid name string";
        return false;
      }
    } else if (key == "version") {
      if (!parse_json_str(json_text, &pos, &out->version)) {
        if (out_err) *out_err = "invalid version string";
        return false;
      }
    } else if (key == "exec") {
      if (!parse_json_str(json_text, &pos, &out->exec)) {
        if (out_err) *out_err = "invalid exec string";
        return false;
      }
    } else if (key == "args") {
      if (!parse_json_str_array(json_text, &pos, &out->args)) {
        if (out_err) *out_err = "invalid args array";
        return false;
      }
    } else if (key == "capabilities") {
      if (!parse_json_str_array(json_text, &pos, &out->capabilities)) {
        if (out_err) *out_err = "invalid capabilities array";
        return false;
      }
    } else if (key == "max_memory_mb") {
      int mem = 64;
      if (!parse_json_int(json_text, &pos, &mem)) {
        if (out_err) *out_err = "invalid max_memory_mb integer";
        return false;
      }
      out->max_memory_mb = std::clamp(mem, 8, 1024);
    } else if (key == "enabled") {
      if (!parse_json_bool(json_text, &pos, &out->enabled)) {
        if (out_err) *out_err = "invalid enabled boolean";
        return false;
      }
    } else {
      if (!skip_val(json_text, &pos)) {
        if (out_err) *out_err = "invalid field value";
        return false;
      }
    }
  }

  if (!is_safe_plugin_id(out->id)) {
    if (out_err) *out_err = "missing or invalid plugin id";
    return false;
  }
  if (out->exec.empty() || has_path_traversal(out->exec)) {
    if (out_err) *out_err = "missing or unsafe exec field";
    return false;
  }
  if (out->name.empty())
    out->name = out->id;
  return true;
}

std::wstring plugin_host_directory() {
  wchar_t override_dir[MAX_PATH] = {};
  DWORD len = GetEnvironmentVariableW(L"WINOME_PLUGINS_DIR",
                                      override_dir, MAX_PATH);
  if (len > 0 && len < MAX_PATH)
    return std::wstring(override_dir);
  const std::wstring base = app_data_dir();
  if (base.empty())
    return {};
  return base + L"\\plugins";
}

void plugin_scan_directory(const std::wstring& root_dir,
                           std::vector<PluginManifest>* out_manifests,
                           std::vector<std::string>* out_errors) {
  if (out_manifests == nullptr)
    return;
  out_manifests->clear();
  if (root_dir.empty())
    return;

  CreateDirectoryW(root_dir.c_str(), nullptr);
  WIN32_FIND_DATAW fd {};
  HANDLE hFind = FindFirstFileW((root_dir + L"\\*").c_str(), &fd);
  if (hFind == INVALID_HANDLE_VALUE)
    return;

  do {
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
      continue;
    const std::wstring entry = fd.cFileName;
    if (entry == L"." || entry == L"..")
      continue;

    const std::wstring pdir = root_dir + L"\\" + entry;
    const std::wstring mpath = pdir + L"\\manifest.json";
    if (GetFileAttributesW(mpath.c_str()) == INVALID_FILE_ATTRIBUTES)
      continue;

    const std::string raw = read_file_utf8(mpath);
    PluginManifest m;
    std::string err;
    if (!parse_plugin_manifest(raw, pdir, &m, &err)) {
      if (out_errors)
        out_errors->push_back(utf8_from_wide(entry) + ": " + err);
      continue;
    }

    bool dup = false;
    for (const auto& existing : *out_manifests) {
      if (existing.id == m.id) {
        dup = true;
        break;
      }
    }
    if (!dup)
      out_manifests->push_back(std::move(m));
    else if (out_errors)
      out_errors->push_back(m.id + ": duplicate plugin id skipped");
  } while (FindNextFileW(hFind, &fd));

  FindClose(hFind);
  std::sort(out_manifests->begin(), out_manifests->end(),
            [](const PluginManifest& a, const PluginManifest& b) {
              return a.id < b.id;
            });
}

void plugin_host_init(const PluginHostCallbacks& cbs) {
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_shutting_down = false;
    g_epoch.fetch_add(1);
    g_cbs = cbs;
    g_initialized = true;
  }

  std::string err;
  plugin_host_reload(&err);
}

void plugin_host_shutdown() {
  std::vector<std::shared_ptr<PluginRun>> to_stop;
  std::vector<std::pair<std::string, std::string>> tb_clear;
  std::vector<std::pair<std::string, std::string>> qs_clear;
  PluginHostCallbacks cbs {};

  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_initialized && g_plugins.empty())
      return;
    g_shutting_down = true;
    g_epoch.fetch_add(1);
    cbs = g_cbs;
    for (auto& rec : g_plugins) {
      if (rec.current_run) {
        to_stop.push_back(std::move(rec.current_run));
        rec.current_run.reset();
      }
      rec.pid = 0;
      rec.state = PluginState::Stopped;
      for (const auto& k : rec.owned_topbar_keys)
        tb_clear.emplace_back(rec.manifest.id, k);
      for (const auto& k : rec.owned_quicktoggle_keys)
        qs_clear.emplace_back(rec.manifest.id, k);
      rec.owned_topbar_keys.clear();
      rec.owned_quicktoggle_keys.clear();
    }
    g_plugins.clear();
    g_initialized = false;
  }

  for (auto& run : to_stop)
    stop_run_blocking(run);

  {
    std::unique_lock<std::mutex> wlock(g_worker_mu);
    g_worker_cv.wait(wlock, [] { return g_active_workers == 0; });
  }

  for (const auto& kv : tb_clear) {
    if (cbs.topbar_remove)
      cbs.topbar_remove(kv.first, kv.second);
  }
  for (const auto& kv : qs_clear) {
    if (cbs.quicktoggle_remove)
      cbs.quicktoggle_remove(kv.first, kv.second);
  }
}

bool plugin_host_reload(std::string* out_err) {
  if (out_err) out_err->clear();

  std::vector<PluginManifest> discovered;
  std::vector<std::string> scan_errors;
  plugin_scan_directory(plugin_host_directory(), &discovered, &scan_errors);
  for (const auto& se : scan_errors)
    debug_append_log("plugin", "manifest scan error: " + se);

  std::vector<std::shared_ptr<PluginRun>> old_runs;
  std::vector<std::pair<std::string, std::string>> old_tb;
  std::vector<std::pair<std::string, std::string>> old_qs;

  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_shutting_down) {
      if (out_err) *out_err = "plugin host shutting down";
      return false;
    }
    for (auto& rec : g_plugins) {
      if (rec.current_run) {
        old_runs.push_back(std::move(rec.current_run));
        rec.current_run.reset();
      }
      rec.generation += 1;
      for (const auto& k : rec.owned_topbar_keys)
        old_tb.emplace_back(rec.manifest.id, k);
      for (const auto& k : rec.owned_quicktoggle_keys)
        old_qs.emplace_back(rec.manifest.id, k);
    }
    g_plugins.clear();
  }

  for (auto& r : old_runs)
    stop_run_blocking(r);
  for (const auto& kv : old_tb)
    schedule_topbar_remove(kv.first, kv.second);
  for (const auto& kv : old_qs)
    schedule_quicktoggle_remove(kv.first, kv.second);

  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_shutting_down)
      return false;
    const bool safe = is_safe_mode();
    for (auto& m : discovered) {
      PluginRecord rec;
      rec.manifest = std::move(m);
      const bool disabled = !rec.manifest.enabled ||
                            is_disabled_in_config(rec.manifest.id);
      if (safe) {
        rec.state = PluginState::Disabled;
        rec.last_error = "disabled in safe mode";
      } else if (disabled) {
        rec.state = PluginState::Disabled;
      } else {
        std::string launch_err;
        launch_plugin_locked(&rec, &launch_err);
      }
      g_plugins.push_back(std::move(rec));
    }
  }

  schedule_plugins_changed();
  return true;
}

bool plugin_host_action(const std::string& op,
                        const std::string& plugin_id,
                        std::string* out_err) {
  if (out_err) out_err->clear();

  if (op == "reload")
    return plugin_host_reload(out_err);

  if (plugin_id.empty()) {
    if (out_err) *out_err = "missing plugin id";
    return false;
  }

  std::shared_ptr<PluginRun> to_stop;
  std::shared_ptr<PluginRun> ping_run;
  std::vector<std::string> tb_clear;
  std::vector<std::string> qs_clear;
  bool need_start = false;
  bool ok = true;

  {
    std::lock_guard<std::mutex> lock(g_mu);
    PluginRecord* rec = nullptr;
    for (auto& p : g_plugins) {
      if (p.manifest.id == plugin_id) {
        rec = &p;
        break;
      }
    }
    if (rec == nullptr) {
      if (out_err) *out_err = "unknown plugin: " + plugin_id;
      return false;
    }

    if (op == "ping") {
      if (rec->current_run &&
          (rec->state == PluginState::Running || rec->state == PluginState::Starting)) {
        ping_run = rec->current_run;
      } else {
        if (out_err) *out_err = "plugin is not running";
        return false;
      }
    } else if (op == "stop") {
      rec->generation += 1;
      to_stop = std::move(rec->current_run);
      rec->current_run.reset();
      rec->pid = 0;
      rec->state = PluginState::Stopped;
      tb_clear.swap(rec->owned_topbar_keys);
      qs_clear.swap(rec->owned_quicktoggle_keys);
    } else if (op == "disable") {
      set_disabled_in_config(plugin_id, true);
      rec->generation += 1;
      to_stop = std::move(rec->current_run);
      rec->current_run.reset();
      rec->pid = 0;
      rec->state = PluginState::Disabled;
      tb_clear.swap(rec->owned_topbar_keys);
      qs_clear.swap(rec->owned_quicktoggle_keys);
    } else if (op == "enable") {
      set_disabled_in_config(plugin_id, false);
      rec->crash_tracker.reset();
      rec->crash_count = 0;
      rec->last_error.clear();
      if (is_safe_mode()) {
        rec->state = PluginState::Disabled;
        rec->last_error = "safe mode active";
      } else {
        rec->generation += 1;
        to_stop = std::move(rec->current_run);
        rec->current_run.reset();
        tb_clear.swap(rec->owned_topbar_keys);
        qs_clear.swap(rec->owned_quicktoggle_keys);
        need_start = true;
      }
    } else if (op == "start" || op == "restart" || op == "reset") {
      if (is_safe_mode()) {
        if (out_err) *out_err = "cannot start plugins in safe mode";
        return false;
      }
      if (op == "reset" || op == "restart") {
        rec->crash_tracker.reset();
        rec->crash_count = 0;
        rec->last_error.clear();
      }
      if (rec->state == PluginState::Tripped && op == "start") {
        if (out_err) *out_err = "plugin tripped; use reset or restart";
        return false;
      }
      if (rec->state == PluginState::Disabled) {
        if (out_err) *out_err = "plugin disabled; use enable first";
        return false;
      }
      rec->generation += 1;
      to_stop = std::move(rec->current_run);
      rec->current_run.reset();
      rec->pid = 0;
      tb_clear.swap(rec->owned_topbar_keys);
      qs_clear.swap(rec->owned_quicktoggle_keys);
      need_start = true;
    } else {
      if (out_err) *out_err = "unsupported plugin op: " + op;
      return false;
    }
  }

  if (ping_run) {
    const bool sent = ping_run->write_line("{\"type\":\"ping\"}");
    if (!sent && out_err)
      *out_err = "failed writing ping to plugin stdin";
    return sent;
  }

  if (to_stop)
    stop_run_blocking(to_stop);
  emit_clear_slots_async(plugin_id, tb_clear, qs_clear);

  if (need_start) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_shutting_down) {
      for (auto& rec : g_plugins) {
        if (rec.manifest.id == plugin_id) {
          ok = launch_plugin_locked(&rec, out_err);
          break;
        }
      }
    }
  }

  schedule_plugins_changed();
  return ok;
}

std::string plugin_host_list_json() {
  std::lock_guard<std::mutex> lock(g_mu);
  std::string out = "{\"safe_mode\":";
  out += (is_safe_mode() ? "true" : "false");
  out += ",\"plugins_dir\":";
  append_json_escaped(utf8_from_wide(plugin_host_directory()), &out);
  out += ",\"plugins\":[";
  for (size_t i = 0; i < g_plugins.size(); ++i) {
    const auto& p = g_plugins[i];
    if (i > 0) out.push_back(',');
    out += "{\"id\":";
    append_json_escaped(p.manifest.id, &out);
    out += ",\"name\":";
    append_json_escaped(p.manifest.name, &out);
    out += ",\"version\":";
    append_json_escaped(p.manifest.version, &out);
    out += ",\"state\":";
    append_json_escaped(plugin_state_name(p.state), &out);
    out += ",\"enabled\":";
    out += ((p.manifest.enabled && !is_disabled_in_config(p.manifest.id))
                ? "true"
                : "false");
    out += ",\"pid\":" + std::to_string(static_cast<unsigned long>(p.pid));
    out += ",\"crash_count\":" + std::to_string(p.crash_count);
    out += ",\"max_memory_mb\":" + std::to_string(p.manifest.max_memory_mb);
    out += ",\"last_error\":";
    append_json_escaped(p.last_error, &out);
    out += ",\"capabilities\":[";
    for (size_t j = 0; j < p.manifest.capabilities.size(); ++j) {
      if (j > 0) out.push_back(',');
      append_json_escaped(p.manifest.capabilities[j], &out);
    }
    out += "]}";
  }
  out += "]}";
  return out;
}

void plugin_host_emit_topbar_click(const std::string& plugin_id,
                                   const std::string& slot) {
  std::shared_ptr<PluginRun> run;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (const auto& p : g_plugins) {
      if (p.manifest.id == plugin_id) {
        run = p.current_run;
        break;
      }
    }
  }
  if (!run)
    return;
  std::string msg = "{\"type\":\"event\",\"event\":\"topbar.click\",\"slot\":";
  append_json_escaped(slot, &msg);
  msg.push_back('}');
  run->write_line(msg);
}

void plugin_host_emit_quicktoggle(const std::string& plugin_id,
                                  const std::string& slot,
                                  bool active) {
  std::shared_ptr<PluginRun> run;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (const auto& p : g_plugins) {
      if (p.manifest.id == plugin_id) {
        run = p.current_run;
        break;
      }
    }
  }
  if (!run)
    return;
  std::string msg = "{\"type\":\"event\",\"event\":\"quicksettings.toggle\",\"slot\":";
  append_json_escaped(slot, &msg);
  msg += ",\"active\":";
  msg += (active ? "true}" : "false}");
  run->write_line(msg);
}

void plugin_host_emit_desktop_changed(int count, int active) {
  std::vector<std::shared_ptr<PluginRun>> targets;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (const auto& p : g_plugins) {
      if (p.current_run && plugin_has_capability(p.manifest, "desktop.read"))
        targets.push_back(p.current_run);
    }
  }
  if (targets.empty())
    return;
  const std::string msg =
      "{\"type\":\"event\",\"event\":\"desktop.changed\",\"count\":" +
      std::to_string(count) + ",\"active\":" + std::to_string(active) + "}";
  for (auto& run : targets)
    run->write_line(msg);
}

}  // namespace winome
