// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors

#include "dev-channel.h"

#include "config.h"
#include "debug-runtime.h"
#include "perf-metrics.h"

#include <gtk/gtk.h>
#include <gdk/win32/gdkwin32.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace winome {
namespace {

void append_json_str(const std::string& s, std::string* out) {
  out->push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"':  *out += "\\\""; break;
      case '\\': *out += "\\\\"; break;
      case '\b': *out += "\\b"; break;
      case '\f': *out += "\\f"; break;
      case '\n': *out += "\\n"; break;
      case '\r': *out += "\\r"; break;
      case '\t': *out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          *out += buf;
        } else {
          out->push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out->push_back('"');
}

std::string json_err(const std::string& msg) {
  std::string out = "{\"ok\":false,\"error\":";
  append_json_str(msg, &out);
  out += "}";
  return out;
}

std::string to_lower_ascii(std::string s) {
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trim_ws(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
    ++b;
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
    --e;
  return s.substr(b, e - b);
}

std::string wide_to_utf8(const wchar_t* ws) {
  if (ws == nullptr || ws[0] == L'\0')
    return {};
  int need = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
  if (need <= 1)
    return {};
  std::string out(static_cast<size_t>(need - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), need, nullptr, nullptr);
  return out;
}

std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty())
    return {};
  int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (need <= 1)
    return {};
  std::wstring out(static_cast<size_t>(need - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), need);
  return out;
}

std::string arg_str(const std::map<std::string, std::string>& args,
                    const char* key,
                    const char* def = "") {
  auto it = args.find(key);
  return (it != args.end()) ? it->second : std::string(def);
}

int arg_int(const std::map<std::string, std::string>& args,
            const char* key,
            int def) {
  auto it = args.find(key);
  if (it == args.end() || it->second.empty())
    return def;
  return static_cast<int>(std::strtol(it->second.c_str(), nullptr, 0));
}

bool arg_bool(const std::map<std::string, std::string>& args,
              const char* key,
              bool def) {
  auto it = args.find(key);
  if (it == args.end())
    return def;
  const std::string v = to_lower_ascii(it->second);
  if (v == "1" || v == "true" || v == "yes" || v == "on")
    return true;
  if (v == "0" || v == "false" || v == "no" || v == "off")
    return false;
  return def;
}

uintptr_t arg_uptr(const std::map<std::string, std::string>& args,
                   const char* key) {
  auto it = args.find(key);
  if (it == args.end() || it->second.empty())
    return 0;
  return static_cast<uintptr_t>(std::strtoull(it->second.c_str(), nullptr, 0));
}

bool is_extended_vk(unsigned vk) {
  switch (vk) {
    case VK_LWIN:
    case VK_RWIN:
    case VK_APPS:
    case VK_RCONTROL:
    case VK_RMENU:
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_SNAPSHOT:
    case VK_DIVIDE:
      return true;
    default:
      return false;
  }
}

bool send_vk_event(unsigned vk, bool up, DWORD* out_gle = nullptr) {
  INPUT in{};
  in.type = INPUT_KEYBOARD;
  in.ki.wVk = static_cast<WORD>(vk);
  in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
  in.ki.dwFlags = (is_extended_vk(vk) ? KEYEVENTF_EXTENDEDKEY : 0u) |
                  (up ? KEYEVENTF_KEYUP : 0u);
  in.ki.dwExtraInfo = 0;
  SetLastError(0);
  const UINT sent = SendInput(1, &in, sizeof(INPUT));
  if (out_gle != nullptr)
    *out_gle = GetLastError();
  return sent == 1;
}

bool send_mouse_abs_move(int x, int y, DWORD* out_gle = nullptr) {
  const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int vw = std::max(GetSystemMetrics(SM_CXVIRTUALSCREEN), 1);
  const int vh = std::max(GetSystemMetrics(SM_CYVIRTUALSCREEN), 1);
  const LONG nx = static_cast<LONG>(
      std::lround((static_cast<double>(x - vx) * 65535.0) /
                  std::max(vw - 1, 1)));
  const LONG ny = static_cast<LONG>(
      std::lround((static_cast<double>(y - vy) * 65535.0) /
                  std::max(vh - 1, 1)));
  INPUT in{};
  in.type = INPUT_MOUSE;
  in.mi.dx = nx;
  in.mi.dy = ny;
  in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
  SetLastError(0);
  const UINT sent = SendInput(1, &in, sizeof(INPUT));
  if (out_gle != nullptr)
    *out_gle = GetLastError();
  // Also fallback to SetCursorPos when SendInput is not blocked so cursor moves reliably.
  if (sent == 0)
    return SetCursorPos(x, y) != FALSE;
  return true;
}

bool send_mouse_button(const std::string& button, bool down, DWORD* out_gle = nullptr) {
  const std::string b = to_lower_ascii(button);
  DWORD flag = 0;
  if (b == "right" || b == "r")
    flag = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
  else if (b == "middle" || b == "m")
    flag = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
  else
    flag = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;

  INPUT in{};
  in.type = INPUT_MOUSE;
  in.mi.dwFlags = flag;
  SetLastError(0);
  const UINT sent = SendInput(1, &in, sizeof(INPUT));
  if (out_gle != nullptr)
    *out_gle = GetLastError();
  return sent == 1;
}

// --- GTK Main Thread Synchronizer -------------------------------------------

GtkCssProvider* g_live_debug_css_provider = nullptr;

bool run_on_gtk_main_sync(std::function<void()> fn, int timeout_ms = 5000) {
  if (!gtk_is_initialized())
    return false;
  if (g_main_context_is_owner(g_main_context_default())) {
    fn();
    return true;
  }
  struct SyncTask {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    std::function<void()> fn;
  };
  auto st = std::make_shared<SyncTask>();
  st->fn = std::move(fn);
  auto* holder = new std::shared_ptr<SyncTask>(st);
  g_main_context_invoke_full(
      g_main_context_default(), G_PRIORITY_HIGH_IDLE,
      +[](gpointer ud) -> gboolean {
        auto* h = static_cast<std::shared_ptr<SyncTask>*>(ud);
        auto task = *h;
        delete h;
        if (task && task->fn)
          task->fn();
        if (task) {
          std::lock_guard<std::mutex> lk(task->mu);
          task->done = true;
          task->cv.notify_all();
        }
        return G_SOURCE_REMOVE;
      },
      holder, nullptr);

  std::unique_lock<std::mutex> lk(st->mu);
  return st->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                         [&] { return st->done; });
}

GtkWidget* resolve_gtk_toplevel(const std::string& selector) {
  GListModel* toplevels = gtk_window_get_toplevels();
  if (toplevels == nullptr)
    return nullptr;
  const guint n = g_list_model_get_n_items(toplevels);
  if (n == 0)
    return nullptr;

  bool is_num = !selector.empty();
  for (char c : selector) {
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      is_num = false;
      break;
    }
  }
  if (is_num) {
    guint idx = static_cast<guint>(std::atoi(selector.c_str()));
    if (idx < n) {
      gpointer obj = g_list_model_get_item(toplevels, idx);
      GtkWidget* w = GTK_IS_WIDGET(obj) ? GTK_WIDGET(obj) : nullptr;
      if (obj != nullptr)
        g_object_unref(obj);
      return w;
    }
    return nullptr;
  }

  const std::string want = to_lower_ascii(selector);
  for (guint i = 0; i < n; ++i) {
    gpointer obj = g_list_model_get_item(toplevels, i);
    if (obj == nullptr)
      continue;
    GtkWidget* w = GTK_IS_WIDGET(obj) ? GTK_WIDGET(obj) : nullptr;
    bool match = false;
    if (w != nullptr) {
      if (want.empty()) {
        match = (i == 0);
      } else {
        const char* nm = gtk_widget_get_name(w);
        const char* ttl = GTK_IS_WINDOW(w) ? gtk_window_get_title(GTK_WINDOW(w)) : nullptr;
        if (nm != nullptr && to_lower_ascii(nm).find(want) != std::string::npos)
          match = true;
        else if (ttl != nullptr && to_lower_ascii(ttl).find(want) != std::string::npos)
          match = true;
      }
    }
    g_object_unref(obj);
    if (match)
      return w;
  }
  return nullptr;
}

void dump_gtk_widget_json(GtkWidget* w,
                          GtkWidget* root,
                          int depth,
                          int max_depth,
                          int* node_budget,
                          std::string* out) {
  if (w == nullptr || *node_budget <= 0) {
    *out += "null";
    return;
  }
  --(*node_budget);
  const char* type_name = G_OBJECT_TYPE_NAME(w);
  const char* name = gtk_widget_get_name(w);
  const bool vis = gtk_widget_get_visible(w) != FALSE;
  const bool mapped = gtk_widget_get_mapped(w) != FALSE;
  const int width = gtk_widget_get_width(w);
  const int height = gtk_widget_get_height(w);
  graphene_rect_t bounds{};
  bool has_bounds = (root != nullptr &&
                     gtk_widget_compute_bounds(w, root, &bounds) != FALSE);

  *out += "{\"type\":";
  append_json_str(type_name ? type_name : "?", out);
  *out += ",\"name\":";
  append_json_str(name ? name : "", out);
  char geom[160];
  std::snprintf(
      geom, sizeof(geom),
      ",\"visible\":%s,\"mapped\":%s,\"w\":%d,\"h\":%d,\"x\":%.1f,\"y\":%.1f",
      vis ? "true" : "false", mapped ? "true" : "false", width, height,
      has_bounds ? bounds.origin.x : 0.0f, has_bounds ? bounds.origin.y : 0.0f);
  *out += geom;

  char** classes = gtk_widget_get_css_classes(w);
  *out += ",\"classes\":[";
  if (classes != nullptr) {
    for (int i = 0; classes[i] != nullptr; ++i) {
      if (i > 0)
        out->push_back(',');
      append_json_str(classes[i], out);
    }
    g_strfreev(classes);
  }
  *out += "]";

  if (GTK_IS_LABEL(w)) {
    const char* txt = gtk_label_get_text(GTK_LABEL(w));
    if (txt != nullptr && txt[0] != '\0') {
      *out += ",\"text\":";
      append_json_str(txt, out);
    }
  } else if (GTK_IS_BUTTON(w)) {
    const char* lbl = gtk_button_get_label(GTK_BUTTON(w));
    if (lbl != nullptr && lbl[0] != '\0') {
      *out += ",\"label\":";
      append_json_str(lbl, out);
    }
  } else if (GTK_IS_EDITABLE(w)) {
    const char* txt = gtk_editable_get_text(GTK_EDITABLE(w));
    if (txt != nullptr && txt[0] != '\0') {
      *out += ",\"text\":";
      append_json_str(txt, out);
    }
  }

  if (depth < max_depth && *node_budget > 0) {
    GtkWidget* ch = gtk_widget_get_first_child(w);
    if (ch != nullptr) {
      *out += ",\"children\":[";
      bool first = true;
      while (ch != nullptr && *node_budget > 0) {
        if (!first)
          out->push_back(',');
        first = false;
        dump_gtk_widget_json(ch, root, depth + 1, max_depth, node_budget, out);
        ch = gtk_widget_get_next_sibling(ch);
      }
      *out += "]";
    }
  }
  *out += "}";
}

GtkWidget* find_gtk_widget_recursive(GtkWidget* w,
                                     const std::string& want_name,
                                     const std::string& want_class,
                                     const std::string& want_text) {
  if (w == nullptr)
    return nullptr;
  bool ok = true;
  if (!want_name.empty()) {
    const char* nm = gtk_widget_get_name(w);
    if (nm == nullptr || to_lower_ascii(nm) != want_name)
      ok = false;
  }
  if (ok && !want_class.empty()) {
    if (!gtk_widget_has_css_class(w, want_class.c_str()))
      ok = false;
  }
  if (ok && !want_text.empty()) {
    std::string found_txt;
    if (GTK_IS_LABEL(w)) {
      const char* t = gtk_label_get_text(GTK_LABEL(w));
      if (t != nullptr) found_txt = t;
    } else if (GTK_IS_BUTTON(w)) {
      const char* t = gtk_button_get_label(GTK_BUTTON(w));
      if (t != nullptr) found_txt = t;
    } else if (GTK_IS_EDITABLE(w)) {
      const char* t = gtk_editable_get_text(GTK_EDITABLE(w));
      if (t != nullptr) found_txt = t;
    }
    if (to_lower_ascii(found_txt).find(want_text) == std::string::npos)
      ok = false;
  }
  if (ok && (!want_name.empty() || !want_class.empty() || !want_text.empty()))
    return w;

  for (GtkWidget* ch = gtk_widget_get_first_child(w); ch != nullptr;
       ch = gtk_widget_get_next_sibling(ch)) {
    if (GtkWidget* hit = find_gtk_widget_recursive(ch, want_name, want_class, want_text))
      return hit;
  }
  return nullptr;
}

// --- Core Single-Op Executor ------------------------------------------------

bool exec_dev_op_inner(const DevOpRequest& req,
                       const DevShellOpCallback& shell_cb,
                       std::string* out_data_json,
                       std::string* out_err) {
  const std::string op = to_lower_ascii(req.op);
  *out_data_json = "{}";
  out_err->clear();

  // 1. Sleep
  if (op == "sleep") {
    const int ms = std::clamp(arg_int(req.args, "ms", 100), 0, 30000);
    if (ms > 0)
      Sleep(static_cast<DWORD>(ms));
    *out_data_json = "{\"slept_ms\":" + std::to_string(ms) + "}";
    return true;
  }

  // 2. OS Keyboard Input
  if (op == "key.tap" || op == "key.down" || op == "key.up") {
    const std::string k = arg_str(req.args, "key");
    const unsigned vk = dev_resolve_vk(k);
    if (vk == 0) {
      *out_err = "unknown key: " + k;
      return false;
    }
    DWORD gle_d = 0, gle_u = 0;
    bool ok_d = true, ok_u = true;
    if (op == "key.down") {
      ok_d = send_vk_event(vk, false, &gle_d);
    } else if (op == "key.up") {
      ok_u = send_vk_event(vk, true, &gle_u);
    } else {
      const int hold = std::clamp(arg_int(req.args, "hold_ms", 25), 5, 1000);
      ok_d = send_vk_event(vk, false, &gle_d);
      Sleep(static_cast<DWORD>(hold));
      ok_u = send_vk_event(vk, true, &gle_u);
    }
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "{\"vk\":%u,\"sent_down\":%s,\"sent_up\":%s,\"gle_down\":%lu,\"gle_up\":%lu}",
                  vk, ok_d ? "true" : "false", ok_u ? "true" : "false",
                  static_cast<unsigned long>(gle_d),
                  static_cast<unsigned long>(gle_u));
    *out_data_json = buf;
    g_print("[dev-op] %s key=%s vk=0x%x ok=%d/%d gle=%lu/%lu\n",
            op.c_str(), k.c_str(), vk, ok_d ? 1 : 0, ok_u ? 1 : 0,
            static_cast<unsigned long>(gle_d),
            static_cast<unsigned long>(gle_u));
    return ok_d && ok_u;
  }

  if (op == "key.combo") {
    const std::string keys_str = arg_str(req.args, "keys");
    if (keys_str.empty()) {
      *out_err = "key.combo requires keys (e.g. Win+Tab, Alt+F2)";
      return false;
    }
    std::vector<unsigned> vks;
    size_t pos = 0;
    while (pos < keys_str.size()) {
      size_t plus = keys_str.find('+', pos);
      const size_t end = (plus == std::string::npos) ? keys_str.size() : plus;
      const std::string tok = trim_ws(keys_str.substr(pos, end - pos));
      if (!tok.empty()) {
        unsigned vk = dev_resolve_vk(tok);
        if (vk == 0) {
          *out_err = "unknown key in combo: " + tok;
          return false;
        }
        vks.push_back(vk);
      }
      if (plus == std::string::npos)
        break;
      pos = plus + 1;
    }
    if (vks.empty()) {
      *out_err = "empty key.combo";
      return false;
    }
    const int hold = std::clamp(arg_int(req.args, "hold_ms", 30), 5, 1000);
    int sent_ok = 0;
    DWORD last_gle = 0;
    for (unsigned vk : vks) {
      if (send_vk_event(vk, false, &last_gle))
        ++sent_ok;
    }
    Sleep(static_cast<DWORD>(hold));
    for (auto it = vks.rbegin(); it != vks.rend(); ++it) {
      if (send_vk_event(*it, true, &last_gle))
        ++sent_ok;
    }
    const int total = static_cast<int>(vks.size() * 2);
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "{\"events_sent\":%d,\"events_total\":%d,\"gle\":%lu}",
                  sent_ok, total, static_cast<unsigned long>(last_gle));
    *out_data_json = buf;
    g_print("[dev-op] key.combo keys=%s sent=%d/%d gle=%lu\n",
            keys_str.c_str(), sent_ok, total,
            static_cast<unsigned long>(last_gle));
    return sent_ok == total;
  }

  if (op == "type") {
    const std::string text = arg_str(req.args, "text");
    std::wstring w = utf8_to_wide(text);
    std::vector<INPUT> inputs;
    inputs.reserve(w.size() * 2);
    for (wchar_t ch : w) {
      INPUT d{};
      d.type = INPUT_KEYBOARD;
      d.ki.wScan = static_cast<WORD>(ch);
      d.ki.dwFlags = KEYEVENTF_UNICODE;
      INPUT u = d;
      u.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
      inputs.push_back(d);
      inputs.push_back(u);
    }
    UINT sent = 0;
    DWORD gle = 0;
    if (!inputs.empty()) {
      SetLastError(0);
      sent = SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
      gle = GetLastError();
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "{\"chars\":%zu,\"events_sent\":%u,\"gle\":%lu}",
                  w.size(), sent, static_cast<unsigned long>(gle));
    *out_data_json = buf;
    return inputs.empty() || sent == inputs.size();
  }

  // 3. OS Mouse Input
  if (op == "mouse.pos") {
    POINT pt{};
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "{\"x\":%ld,\"y\":%ld,\"monitor\":\"0x%llX\"}",
                  pt.x, pt.y,
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mon)));
    *out_data_json = buf;
    return true;
  }

  if (op == "mouse.move") {
    int x = arg_int(req.args, "x", 0);
    int y = arg_int(req.args, "y", 0);
    if (arg_bool(req.args, "rel", false)) {
      POINT cur{};
      GetCursorPos(&cur);
      x += cur.x;
      y += cur.y;
    }
    DWORD gle = 0;
    bool ok = send_mouse_abs_move(x, y, &gle);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "{\"x\":%d,\"y\":%d,\"gle\":%lu}",
                  x, y, static_cast<unsigned long>(gle));
    *out_data_json = buf;
    return ok;
  }

  if (op == "mouse.click") {
    if (req.args.count("x") && req.args.count("y"))
      send_mouse_abs_move(arg_int(req.args, "x", 0), arg_int(req.args, "y", 0));
    const std::string btn = arg_str(req.args, "button", "left");
    const int clicks = arg_bool(req.args, "double", false) ? 2 : 1;
    DWORD gle = 0;
    bool ok = true;
    for (int i = 0; i < clicks; ++i) {
      ok = send_mouse_button(btn, true, &gle) && ok;
      Sleep(15);
      ok = send_mouse_button(btn, false, &gle) && ok;
      if (i + 1 < clicks)
        Sleep(40);
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "{\"clicks\":%d,\"gle\":%lu}",
                  clicks, static_cast<unsigned long>(gle));
    *out_data_json = buf;
    return ok;
  }

  if (op == "mouse.drag") {
    const int x1 = arg_int(req.args, "x1", 0);
    const int y1 = arg_int(req.args, "y1", 0);
    const int x2 = arg_int(req.args, "x2", 0);
    const int y2 = arg_int(req.args, "y2", 0);
    const int steps = std::clamp(arg_int(req.args, "steps", 16), 1, 200);
    const int dur = std::clamp(arg_int(req.args, "duration_ms", 240), 10, 5000);
    const std::string btn = arg_str(req.args, "button", "left");
    DWORD gle = 0;
    send_mouse_abs_move(x1, y1, &gle);
    Sleep(20);
    bool ok = send_mouse_button(btn, true, &gle);
    const int step_ms = std::max(1, dur / steps);
    for (int i = 1; i <= steps; ++i) {
      const double f = static_cast<double>(i) / steps;
      const int cx = static_cast<int>(std::lround(x1 + (x2 - x1) * f));
      const int cy = static_cast<int>(std::lround(y1 + (y2 - y1) * f));
      send_mouse_abs_move(cx, cy, &gle);
      Sleep(static_cast<DWORD>(step_ms));
    }
    ok = send_mouse_button(btn, false, &gle) && ok;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "{\"steps\":%d,\"gle\":%lu}",
                  steps, static_cast<unsigned long>(gle));
    *out_data_json = buf;
    return ok;
  }

  if (op == "mouse.wheel") {
    if (req.args.count("x") && req.args.count("y"))
      send_mouse_abs_move(arg_int(req.args, "x", 0), arg_int(req.args, "y", 0));
    const int delta = arg_int(req.args, "delta", -120);
    const bool horiz = arg_bool(req.args, "horiz", false);
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = horiz ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    in.mi.mouseData = static_cast<DWORD>(delta);
    SetLastError(0);
    const UINT sent = SendInput(1, &in, sizeof(INPUT));
    const DWORD gle = GetLastError();
    char buf[96];
    std::snprintf(buf, sizeof(buf), "{\"sent\":%u,\"gle\":%lu}",
                  sent, static_cast<unsigned long>(gle));
    *out_data_json = buf;
    return sent == 1;
  }

  // 4. Win32 Window Inspection & Direct Manipulation
  if (op == "win.list") {
    *out_data_json = debug_windows_to_json(debug_inspect_windows());
    return true;
  }

  if (op == "win.fg") {
    HWND fg = GetForegroundWindow();
    if (fg == nullptr) {
      *out_data_json = "{\"hwnd\":null}";
      return true;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    wchar_t cls[128] = {};
    wchar_t title[256] = {};
    GetClassNameW(fg, cls, 128);
    GetWindowTextW(fg, title, 256);
    RECT rc{};
    GetWindowRect(fg, &rc);
    char head[192];
    std::snprintf(head, sizeof(head),
                  "{\"hwnd\":\"0x%llX\",\"pid\":%lu,\"rect\":[%ld,%ld,%ld,%ld],\"class\":",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(fg)),
                  static_cast<unsigned long>(pid),
                  rc.left, rc.top, rc.right, rc.bottom);
    std::string out = head;
    append_json_str(wide_to_utf8(cls), &out);
    out += ",\"title\":";
    append_json_str(wide_to_utf8(title), &out);
    out += "}";
    *out_data_json = std::move(out);
    return true;
  }

  if (op == "win.focus" || op == "win.move" || op == "win.state" || op == "win.msg") {
    HWND hwnd = reinterpret_cast<HWND>(arg_uptr(req.args, "hwnd"));
    if (hwnd == nullptr || !IsWindow(hwnd)) {
      *out_err = "invalid or missing hwnd";
      return false;
    }
    if (op == "win.focus") {
      if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);
      AllowSetForegroundWindow(ASFW_ANY);
      BOOL ok = SetForegroundWindow(hwnd);
      *out_data_json = std::string("{\"focused\":") + (ok ? "true" : "false") + "}";
      return true;
    }
    if (op == "win.move") {
      RECT cur{};
      GetWindowRect(hwnd, &cur);
      const int x = arg_int(req.args, "x", cur.left);
      const int y = arg_int(req.args, "y", cur.top);
      const int w = arg_int(req.args, "w", cur.right - cur.left);
      const int h = arg_int(req.args, "h", cur.bottom - cur.top);
      BOOL ok = SetWindowPos(hwnd, nullptr, x, y, w, h,
                             SWP_NOZORDER | SWP_NOACTIVATE);
      *out_data_json = std::string("{\"moved\":") + (ok ? "true" : "false") + "}";
      return ok != FALSE;
    }
    if (op == "win.state") {
      const std::string act = to_lower_ascii(arg_str(req.args, "action"));
      if (act == "minimize") ShowWindowAsync(hwnd, SW_MINIMIZE);
      else if (act == "maximize") ShowWindowAsync(hwnd, SW_MAXIMIZE);
      else if (act == "restore") ShowWindowAsync(hwnd, SW_RESTORE);
      else if (act == "hide") ShowWindowAsync(hwnd, SW_HIDE);
      else if (act == "show") ShowWindowAsync(hwnd, SW_SHOW);
      else if (act == "close") PostMessageW(hwnd, WM_CLOSE, 0, 0);
      else {
        *out_err = "unknown win.state action: " + act;
        return false;
      }
      *out_data_json = "{\"action\":" + std::string("\"") + act + "\"}";
      return true;
    }
    if (op == "win.msg") {
      const UINT msg = static_cast<UINT>(arg_int(req.args, "msg", 0));
      const WPARAM wp = static_cast<WPARAM>(arg_uptr(req.args, "wparam"));
      const LPARAM lp = static_cast<LPARAM>(arg_uptr(req.args, "lparam"));
      const bool post = arg_bool(req.args, "post", true);
      if (post) {
        BOOL ok = PostMessageW(hwnd, msg, wp, lp);
        *out_data_json = std::string("{\"posted\":") + (ok ? "true" : "false") + "}";
        return ok != FALSE;
      }
      DWORD_PTR res = 0;
      BOOL ok = SendMessageTimeoutW(hwnd, msg, wp, lp, SMTO_ABORTIFHUNG, 1000, &res);
      char buf[96];
      std::snprintf(buf, sizeof(buf), "{\"sent\":%s,\"result\":%llu}",
                    ok ? "true" : "false",
                    static_cast<unsigned long long>(res));
      *out_data_json = buf;
      return ok != FALSE;
    }
  }

  // 5. GTK4 Live Introspection & Direct Mutation
  if (op == "gtk.windows") {
    if (!gtk_is_initialized()) {
      *out_err = "GTK is not initialized";
      return false;
    }
    std::string json;
    if (!run_on_gtk_main_sync([&] {
          GListModel* toplevels = gtk_window_get_toplevels();
          const guint n = toplevels != nullptr ? g_list_model_get_n_items(toplevels) : 0;
          json = "[";
          for (guint i = 0; i < n; ++i) {
            if (i > 0)
              json.push_back(',');
            gpointer obj = g_list_model_get_item(toplevels, i);
            GtkWidget* w = GTK_IS_WIDGET(obj) ? GTK_WIDGET(obj) : nullptr;
            if (w == nullptr) {
              json += "null";
              if (obj != nullptr) g_object_unref(obj);
              continue;
            }
            const char* nm = gtk_widget_get_name(w);
            const char* ttl = GTK_IS_WINDOW(w) ? gtk_window_get_title(GTK_WINDOW(w)) : "";
            GskRenderer* r = GTK_IS_NATIVE(w) ? gtk_native_get_renderer(GTK_NATIVE(w)) : nullptr;
            const char* rname = r != nullptr ? G_OBJECT_TYPE_NAME(r) : "(none)";
            GdkSurface* surf = GTK_IS_NATIVE(w) ? gtk_native_get_surface(GTK_NATIVE(w)) : nullptr;
            HWND hwnd = surf != nullptr
                            ? static_cast<HWND>(gdk_win32_surface_get_handle(surf))
                            : nullptr;
            char head[256];
            std::snprintf(
                head, sizeof(head),
                "{\"index\":%u,\"visible\":%s,\"mapped\":%s,\"w\":%d,\"h\":%d,"
                "\"scale\":%d,\"hwnd\":\"0x%llX\",\"renderer\":",
                i,
                gtk_widget_get_visible(w) ? "true" : "false",
                gtk_widget_get_mapped(w) ? "true" : "false",
                gtk_widget_get_width(w), gtk_widget_get_height(w),
                gtk_widget_get_scale_factor(w),
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)));
            json += head;
            append_json_str(rname, &json);
            json += ",\"name\":";
            append_json_str(nm ? nm : "", &json);
            json += ",\"title\":";
            append_json_str(ttl ? ttl : "", &json);
            json += "}";
            g_object_unref(obj);
          }
          json += "]";
        })) {
      *out_err = "GTK main-thread dispatch timed out";
      return false;
    }
    *out_data_json = std::move(json);
    return true;
  }

  if (op == "gtk.tree") {
    if (!gtk_is_initialized()) {
      *out_err = "GTK is not initialized";
      return false;
    }
    const std::string win_sel = arg_str(req.args, "window", "0");
    const int max_depth = std::clamp(arg_int(req.args, "max_depth", 8), 1, 20);
    std::string json;
    bool found = false;
    if (!run_on_gtk_main_sync([&] {
          GtkWidget* root = resolve_gtk_toplevel(win_sel);
          if (root == nullptr)
            return;
          found = true;
          int budget = 400;
          dump_gtk_widget_json(root, root, 0, max_depth, &budget, &json);
        })) {
      *out_err = "GTK main-thread dispatch timed out";
      return false;
    }
    if (!found) {
      *out_err = "GTK window not found: " + win_sel;
      return false;
    }
    *out_data_json = std::move(json);
    return true;
  }

  if (op == "gtk.css") {
    if (!gtk_is_initialized()) {
      *out_err = "GTK is not initialized";
      return false;
    }
    const bool clear = arg_bool(req.args, "clear", false);
    const std::string css = arg_str(req.args, "css");
    if (!run_on_gtk_main_sync([&] {
          GdkDisplay* disp = gdk_display_get_default();
          if (disp == nullptr)
            return;
          if (g_live_debug_css_provider != nullptr) {
            gtk_style_context_remove_provider_for_display(
                disp, GTK_STYLE_PROVIDER(g_live_debug_css_provider));
            g_clear_object(&g_live_debug_css_provider);
          }
          if (!clear && !css.empty()) {
            g_live_debug_css_provider = gtk_css_provider_new();
            gtk_css_provider_load_from_string(g_live_debug_css_provider, css.c_str());
            gtk_style_context_add_provider_for_display(
                disp, GTK_STYLE_PROVIDER(g_live_debug_css_provider),
                GTK_STYLE_PROVIDER_PRIORITY_USER);
          }
        })) {
      *out_err = "GTK main-thread dispatch timed out";
      return false;
    }
    *out_data_json = clear ? "{\"cleared\":true}" : "{\"applied\":true}";
    return true;
  }

  if (op == "gtk.click" || op == "gtk.prop") {
    if (!gtk_is_initialized()) {
      *out_err = "GTK is not initialized";
      return false;
    }
    const std::string win_sel = arg_str(req.args, "window", "0");
    const std::string want_name = to_lower_ascii(arg_str(req.args, "name"));
    const std::string want_class = arg_str(req.args, "class");
    const std::string want_text = to_lower_ascii(arg_str(req.args, "text"));
    const std::string prop = to_lower_ascii(arg_str(req.args, "prop"));
    const bool has_val = req.args.count("value") > 0;
    const std::string val = arg_str(req.args, "value");
    std::string res_json;
    std::string err;
    if (!run_on_gtk_main_sync([&] {
          GtkWidget* root = resolve_gtk_toplevel(win_sel);
          if (root == nullptr) {
            err = "GTK window not found: " + win_sel;
            return;
          }
          GtkWidget* target =
              (want_name.empty() && want_class.empty() && want_text.empty())
                  ? root
                  : find_gtk_widget_recursive(root, want_name, want_class, want_text);
          if (target == nullptr) {
            err = "matching GTK widget not found";
            return;
          }
          if (op == "gtk.click") {
            if (GTK_IS_BUTTON(target)) {
              g_signal_emit_by_name(target, "clicked");
              res_json = "{\"emitted\":\"clicked\"}";
            } else {
              gboolean ok = gtk_widget_activate(target);
              res_json = std::string("{\"activated\":") + (ok ? "true" : "false") + "}";
            }
          } else {
            if (prop == "visible") {
              if (has_val) gtk_widget_set_visible(target, arg_bool(req.args, "value", true));
              res_json = std::string("{\"visible\":") +
                         (gtk_widget_get_visible(target) ? "true" : "false") + "}";
            } else if (prop == "opacity") {
              if (has_val) gtk_widget_set_opacity(target, std::atof(val.c_str()));
              char b[64];
              std::snprintf(b, sizeof(b), "{\"opacity\":%.3f}",
                            gtk_widget_get_opacity(target));
              res_json = b;
            } else if (prop == "sensitive") {
              if (has_val) gtk_widget_set_sensitive(target, arg_bool(req.args, "value", true));
              res_json = std::string("{\"sensitive\":") +
                         (gtk_widget_get_sensitive(target) ? "true" : "false") + "}";
            } else if (prop == "text" && GTK_IS_EDITABLE(target)) {
              if (has_val) gtk_editable_set_text(GTK_EDITABLE(target), val.c_str());
              res_json = "{\"text\":";
              const char* t = gtk_editable_get_text(GTK_EDITABLE(target));
              append_json_str(t ? t : "", &res_json);
              res_json += "}";
            } else {
              err = "unsupported gtk.prop: " + prop;
            }
          }
        })) {
      *out_err = "GTK main-thread dispatch timed out";
      return false;
    }
    if (!err.empty()) {
      *out_err = err;
      return false;
    }
    *out_data_json = std::move(res_json);
    return true;
  }

  // 6. Telemetry, Logs, Debug Flags
  if (op == "perf.snapshot") {
    const ProcessTelemetry mem = perf_sample_process_telemetry();
    const StartupTimings st = perf_get_startup_timings();
    const FrameTimeTracker::Stats ov = perf_get_overview_frame_stats();
    char buf[768];
    std::snprintf(
        buf, sizeof(buf),
        "{\"memory\":{\"working_set_mb\":%.2f,\"private_mb\":%.2f,"
        "\"gdi\":%lu,\"user\":%lu},"
        "\"startup\":{\"total_ms\":%.1f,\"pre_gtk_ms\":%.1f,\"gtk_init_ms\":%.1f,"
        "\"fontmap_ms\":%.1f,\"panel_ms\":%.1f,\"overview_ms\":%.1f,"
        "\"activate_ms\":%.1f,\"idle_wait_ms\":%.1f},"
        "\"overview_frames\":{\"frames\":%llu,\"dropped\":%llu,"
        "\"avg_ms\":%.2f,\"p95_ms\":%.2f,\"max_ms\":%.2f}}",
        mem.working_set_bytes / (1024.0 * 1024.0),
        mem.private_bytes / (1024.0 * 1024.0),
        static_cast<unsigned long>(mem.gdi_objects),
        static_cast<unsigned long>(mem.user_objects),
        st.total_startup_ms, st.pre_gtk_ms, st.gtk_init_ms,
        st.fontmap_ms, st.panel_ms, st.overview_init_ms,
        st.gtk_activate_ms, st.idle_wait_ms,
        static_cast<unsigned long long>(ov.frames),
        static_cast<unsigned long long>(ov.dropped_frames),
        ov.avg_frame_ms, ov.p95_frame_ms, ov.max_frame_ms);
    *out_data_json = buf;
    return true;
  }

  if (op == "log.tail") {
    const int count = std::clamp(arg_int(req.args, "count", 40), 1, 512);
    const std::string tag = arg_str(req.args, "tag");
    const uint64_t latest = debug_latest_log_seq();
    const uint64_t since =
        latest > static_cast<uint64_t>(count * 4)
            ? latest - static_cast<uint64_t>(count * 4)
            : 0;
    const auto logs = debug_get_logs_since(since, tag, static_cast<size_t>(count));
    std::string out = "[";
    for (size_t i = 0; i < logs.size(); ++i) {
      if (i > 0)
        out.push_back(',');
      char head[96];
      std::snprintf(head, sizeof(head), "{\"seq\":%llu,\"tag\":",
                    static_cast<unsigned long long>(logs[i].seq));
      out += head;
      append_json_str(logs[i].tag, &out);
      out += ",\"text\":";
      append_json_str(logs[i].text, &out);
      out += "}";
    }
    out += "]";
    *out_data_json = std::move(out);
    return true;
  }

  if (op == "log.clear") {
    debug_clear_logs();
    *out_data_json = "{\"cleared\":true}";
    return true;
  }

  if (op == "shell.flag") {
    const std::string name = arg_str(req.args, "name");
    const bool val = arg_bool(req.args, "value", true);
    if (!debug_set_flag_by_name(name, val)) {
      *out_err = "unknown debug flag: " + name;
      return false;
    }
    *out_data_json = debug_flags_to_json(debug_get_flags());
    return true;
  }

  // 7. WINOME Shell-specific operations delegated to host callback.
  if (op.rfind("shell.", 0) == 0) {
    if (!shell_cb) {
      *out_err = "shell operation callback not registered: " + op;
      return false;
    }
    return shell_cb(op, req.args, out_data_json, out_err);
  }

  *out_err = "unknown dev op: " + req.op;
  return false;
}

void append_logs_json(const std::vector<DebugLogEntry>& logs,
                      int64_t base_us,
                      std::string* out) {
  out->push_back('[');
  for (size_t i = 0; i < logs.size(); ++i) {
    if (i > 0)
      out->push_back(',');
    const double rel_ms =
        static_cast<double>(logs[i].monotonic_us - base_us) / 1000.0;
    char head[128];
    std::snprintf(head, sizeof(head), "{\"seq\":%llu,\"t_ms\":%.2f,\"tag\":",
                  static_cast<unsigned long long>(logs[i].seq), rel_ms);
    *out += head;
    append_json_str(logs[i].tag, out);
    *out += ",\"text\":";
    append_json_str(logs[i].text, out);
    out->push_back('}');
  }
  out->push_back(']');
}

}  // namespace

unsigned dev_resolve_vk(const std::string& key_name) {
  const std::string k = to_lower_ascii(trim_ws(key_name));
  if (k.empty())
    return 0;
  if (k.size() == 1) {
    unsigned char c = static_cast<unsigned char>(k[0]);
    if (c >= 'a' && c <= 'z')
      return static_cast<unsigned>('A' + (c - 'a'));
    if (c >= '0' && c <= '9')
      return static_cast<unsigned>(c);
  }
  if (k.rfind("0x", 0) == 0) {
    unsigned v = static_cast<unsigned>(std::strtoul(k.c_str(), nullptr, 16));
    return (v > 0 && v <= 0xFE) ? v : 0;
  }
  if (k == "win" || k == "lwin" || k == "super" || k == "meta") return VK_LWIN;
  if (k == "rwin") return VK_RWIN;
  if (k == "esc" || k == "escape") return VK_ESCAPE;
  if (k == "enter" || k == "return") return VK_RETURN;
  if (k == "tab") return VK_TAB;
  if (k == "space") return VK_SPACE;
  if (k == "backspace" || k == "bksp") return VK_BACK;
  if (k == "del" || k == "delete") return VK_DELETE;
  if (k == "ins" || k == "insert") return VK_INSERT;
  if (k == "home") return VK_HOME;
  if (k == "end") return VK_END;
  if (k == "pgup" || k == "pageup") return VK_PRIOR;
  if (k == "pgdn" || k == "pagedown") return VK_NEXT;
  if (k == "left") return VK_LEFT;
  if (k == "right") return VK_RIGHT;
  if (k == "up") return VK_UP;
  if (k == "down") return VK_DOWN;
  if (k == "ctrl" || k == "lctrl" || k == "control") return VK_LCONTROL;
  if (k == "rctrl") return VK_RCONTROL;
  if (k == "alt" || k == "lalt" || k == "menu") return VK_LMENU;
  if (k == "ralt") return VK_RMENU;
  if (k == "shift" || k == "lshift") return VK_LSHIFT;
  if (k == "rshift") return VK_RSHIFT;
  if (k == "prtsc" || k == "printscreen") return VK_SNAPSHOT;
  if (k.size() >= 2 && k[0] == 'f' && std::isdigit(static_cast<unsigned char>(k[1]))) {
    int fnum = std::atoi(k.c_str() + 1);
    if (fnum >= 1 && fnum <= 24)
      return static_cast<unsigned>(VK_F1 + (fnum - 1));
  }
  return 0;
}

bool dev_parse_op_shorthand(const std::string& text,
                            DevOpRequest* out,
                            std::string* out_err) {
  if (out == nullptr)
    return false;
  out->op.clear();
  out->args.clear();
  if (out_err != nullptr)
    out_err->clear();

  // Tokenize respecting single/double quotes.
  std::vector<std::string> tokens;
  std::string cur;
  char quote = '\0';
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (quote != '\0') {
      if (c == '\\' && i + 1 < text.size()) {
        cur.push_back(text[++i]);
      } else if (c == quote) {
        quote = '\0';
      } else {
        cur.push_back(c);
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!cur.empty()) {
        tokens.push_back(std::move(cur));
        cur.clear();
      }
      continue;
    }
    cur.push_back(c);
  }
  if (!cur.empty())
    tokens.push_back(std::move(cur));
  if (tokens.empty()) {
    if (out_err != nullptr)
      *out_err = "empty operation";
    return false;
  }

  out->op = tokens[0];
  for (size_t i = 1; i < tokens.size(); ++i) {
    const std::string& tok = tokens[i];
    size_t eq = tok.find('=');
    if (eq != std::string::npos && eq > 0) {
      out->args[tok.substr(0, eq)] = tok.substr(eq + 1);
    } else if (i == 1) {
      // Friendly positional default for common single-arg ops.
      const std::string lop = to_lower_ascii(out->op);
      if (lop == "sleep") out->args["ms"] = tok;
      else if (lop == "key.tap" || lop == "key.down" || lop == "key.up") out->args["key"] = tok;
      else if (lop == "key.combo") out->args["keys"] = tok;
      else if (lop == "type") out->args["text"] = tok;
      else if (lop == "win.focus") out->args["hwnd"] = tok;
      else if (lop == "gtk.tree") out->args["window"] = tok;
      else if (lop == "gtk.css") out->args["css"] = tok;
      else out->args["action"] = tok;
    }
  }
  return true;
}

bool dev_parse_batch_shorthand(const std::string& text,
                               std::vector<DevOpRequest>* out_ops,
                               std::string* out_err) {
  if (out_ops == nullptr)
    return false;
  out_ops->clear();
  if (out_err != nullptr)
    out_err->clear();

  // Split on ';' outside quotes.
  std::vector<std::string> segments;
  std::string cur;
  char quote = '\0';
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (quote != '\0') {
      cur.push_back(c);
      if (c == '\\' && i + 1 < text.size())
        cur.push_back(text[++i]);
      else if (c == quote)
        quote = '\0';
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
      cur.push_back(c);
      continue;
    }
    if (c == ';') {
      const std::string t = trim_ws(cur);
      if (!t.empty())
        segments.push_back(t);
      cur.clear();
      continue;
    }
    cur.push_back(c);
  }
  if (!trim_ws(cur).empty())
    segments.push_back(trim_ws(cur));

  if (segments.empty()) {
    if (out_err != nullptr)
      *out_err = "empty batch operations";
    return false;
  }
  for (const auto& seg : segments) {
    DevOpRequest req;
    if (!dev_parse_op_shorthand(seg, &req, out_err))
      return false;
    out_ops->push_back(std::move(req));
  }
  return true;
}

std::string dev_channel_help_json() {
  return R"({"ok":true,"result":{"channel":"dev-universal","requires_developer_mode":true,"ops":{)"
         R"("input":["key.tap key=<name> [hold_ms=25]","key.down key=<name>","key.up key=<name>","key.combo keys=Win+Tab [hold_ms=30]","type text=<utf8>","mouse.pos","mouse.move x=<px> y=<px> [rel=false]","mouse.click [button=left|right|middle] [x=.. y=..] [double=false]","mouse.drag x1=.. y1=.. x2=.. y2=.. [steps=16 duration_ms=240]","mouse.wheel delta=-120 [horiz=false x=.. y=..]"],)"
         R"("window":["win.list","win.fg","win.focus hwnd=0x..","win.move hwnd=0x.. [x=.. y=.. w=.. h=..]","win.state hwnd=0x.. action=minimize|maximize|restore|show|hide|close","win.msg hwnd=0x.. msg=<num> [wparam=0 lparam=0 post=true]"],)"
         R"("gtk":["gtk.windows","gtk.tree [window=0|<name>] [max_depth=8]","gtk.css css=<rules> | clear=true","gtk.click [window=0] [name=.. class=.. text=..]","gtk.prop [window=0] [name=.. class=..] prop=visible|opacity|sensitive|text [value=..]"],)"
         R"("shell":["shell.overview action=open|close|toggle|grid|refresh|restack|invalidate_wallpaper|reposition","shell.panel action=reload_theme|close_popovers|inspect","shell.ws action=list|next|prev|switch|create|remove [index=N]","shell.dialog target=settings|looking_glass|run action=open|close|toggle [page=..]","shell.cache action=icon_clear|icon_trim|app_refresh|brightness_invalidate|systray_refresh [keep=64]","shell.config action=get|set|reload [key=.. value=..]","shell.flag name=zorder_trace|hotkey_trace|ipc_trace|frame_overlay value=true|false","shell.fullscreen action=eval","shell.win_tap"],)"
         R"("telemetry":["perf.snapshot","log.tail [count=40 tag=..]","log.clear","sleep ms=<N>"]}}})";
}

std::string dev_channel_exec_op(const DevOpRequest& req,
                                int wait_ms,
                                bool capture_logs,
                                const std::string& tag_filter,
                                const DevShellOpCallback& shell_cb) {
  if (!get_config().developer_mode)
    return json_err("developer_mode is disabled");
  if (req.op.empty())
    return json_err("missing op");

  const int64_t t0 = g_get_monotonic_time();
  const uint64_t seq0 = debug_latest_log_seq();
  std::string data_json;
  std::string err;
  const bool ok = exec_dev_op_inner(req, shell_cb, &data_json, &err);
  const int clamped_wait = std::clamp(wait_ms, 0, 15000);
  if (ok && clamped_wait > 0)
    Sleep(static_cast<DWORD>(clamped_wait));
  const double elapsed_ms =
      static_cast<double>(g_get_monotonic_time() - t0) / 1000.0;

  if (!ok) {
    std::string out = "{\"ok\":false,\"op\":";
    append_json_str(req.op, &out);
    out += ",\"error\":";
    append_json_str(err.empty() ? "operation failed" : err, &out);
    out += ",\"data\":" + (data_json.empty() ? "{}" : data_json);
    if (capture_logs) {
      const auto logs = debug_get_logs_since(seq0, tag_filter, 256);
      out += ",\"logs\":";
      append_logs_json(logs, t0, &out);
    }
    out += "}";
    return out;
  }

  char meta[128];
  std::snprintf(meta, sizeof(meta), ",\"elapsed_ms\":%.2f,\"data\":", elapsed_ms);
  std::string out = "{\"ok\":true,\"result\":{\"op\":";
  append_json_str(req.op, &out);
  out += meta;
  out += data_json.empty() ? "{}" : data_json;
  if (capture_logs) {
    const auto logs = debug_get_logs_since(seq0, tag_filter, 512);
    out += ",\"logs\":";
    append_logs_json(logs, t0, &out);
  }
  out += "}}";
  return out;
}

std::string dev_channel_exec_batch(const std::vector<DevOpRequest>& ops,
                                   bool capture_logs,
                                   const std::string& tag_filter,
                                   bool stop_on_error,
                                   const DevShellOpCallback& shell_cb) {
  if (!get_config().developer_mode)
    return json_err("developer_mode is disabled");
  if (ops.empty())
    return json_err("empty batch ops");

  const int64_t t0 = g_get_monotonic_time();
  const uint64_t seq0 = debug_latest_log_seq();
  const ProcessTelemetry mem0 = perf_sample_process_telemetry();

  std::string steps_json = "[";
  bool all_ok = true;
  size_t executed = 0;

  for (size_t i = 0; i < ops.size(); ++i) {
    if (i > 0)
      steps_json.push_back(',');
    const int64_t st_t0 = g_get_monotonic_time();
    const uint64_t st_seq0 = debug_latest_log_seq();
    std::string data_json;
    std::string err;
    const bool ok = exec_dev_op_inner(ops[i], shell_cb, &data_json, &err);
    const double st_ms =
        static_cast<double>(g_get_monotonic_time() - st_t0) / 1000.0;
    ++executed;
    if (!ok)
      all_ok = false;

    char head[128];
    std::snprintf(head, sizeof(head),
                  "{\"index\":%zu,\"ok\":%s,\"elapsed_ms\":%.2f,\"op\":",
                  i, ok ? "true" : "false", st_ms);
    steps_json += head;
    append_json_str(ops[i].op, &steps_json);
    steps_json += ",\"data\":" + (data_json.empty() ? "{}" : data_json);
    if (!ok) {
      steps_json += ",\"error\":";
      append_json_str(err, &steps_json);
    }
    if (capture_logs) {
      const auto st_logs = debug_get_logs_since(st_seq0, tag_filter, 256);
      steps_json += ",\"logs\":";
      append_logs_json(st_logs, t0, &steps_json);
    }
    steps_json.push_back('}');
    if (!ok && stop_on_error)
      break;
  }
  steps_json.push_back(']');

  const ProcessTelemetry mem1 = perf_sample_process_telemetry();
  const double total_ms =
      static_cast<double>(g_get_monotonic_time() - t0) / 1000.0;
  const int64_t delta_kb =
      (static_cast<int64_t>(mem1.private_bytes) -
       static_cast<int64_t>(mem0.private_bytes)) / 1024;
  const int64_t delta_gdi =
      static_cast<int64_t>(mem1.gdi_objects) - static_cast<int64_t>(mem0.gdi_objects);
  const int64_t delta_user =
      static_cast<int64_t>(mem1.user_objects) - static_cast<int64_t>(mem0.user_objects);

  char head[256];
  std::snprintf(
      head, sizeof(head),
      "{\"ok\":%s,\"result\":{\"executed\":%zu,\"total\":%zu,"
      "\"elapsed_ms\":%.2f,\"delta_private_kb\":%lld,"
      "\"delta_gdi\":%lld,\"delta_user\":%lld,\"steps\":",
      all_ok ? "true" : "false", executed, ops.size(), total_ms,
      static_cast<long long>(delta_kb),
      static_cast<long long>(delta_gdi),
      static_cast<long long>(delta_user));
  std::string out = head;
  out += steps_json;
  if (capture_logs) {
    const auto all_logs = debug_get_logs_since(seq0, tag_filter, 1024);
    out += ",\"all_logs\":";
    append_logs_json(all_logs, t0, &out);
  }
  out += "}}";
  return out;
}

void dev_channel_shutdown() {
  if (!gtk_is_initialized())
    return;
  if (g_live_debug_css_provider != nullptr) {
    if (GdkDisplay* disp = gdk_display_get_default()) {
      gtk_style_context_remove_provider_for_display(
          disp, GTK_STYLE_PROVIDER(g_live_debug_css_provider));
    }
    g_clear_object(&g_live_debug_css_provider);
  }
}

}  // namespace winome
