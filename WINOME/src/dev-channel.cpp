// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors

#include "dev-channel.h"

#include "config.h"
#include "debug-runtime.h"
#include "perf-metrics.h"

#include <gtk/gtk.h>
#include <gdk/win32/gdkwin32.h>
#include <dwmapi.h>
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

static std::string g_live_debug_css_text;

HWND gtk_widget_hwnd(GtkWidget* w) {
  if (w == nullptr || !GTK_IS_NATIVE(w))
    return nullptr;
  GdkSurface* surf = gtk_native_get_surface(GTK_NATIVE(w));
  if (surf == nullptr)
    return nullptr;
  return static_cast<HWND>(gdk_win32_surface_get_handle(surf));
}

bool append_win32_hwnd_state_fields(HWND hwnd, std::string* out) {
  if (hwnd == nullptr || !IsWindow(hwnd)) {
    *out += "\"win32_visible\":false,\"cloaked\":false,\"topmost\":false,"
            "\"minimized\":false,\"on_screen\":false";
    return false;
  }
  const bool w32_vis = IsWindowVisible(hwnd) != FALSE;
  BOOL cloaked_val = FALSE;
  (void)DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked_val, sizeof(cloaked_val));
  const bool cloaked = cloaked_val != FALSE;
  const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  const bool topmost = (ex & WS_EX_TOPMOST) != 0;
  const bool minimized = IsIconic(hwnd) != FALSE;
  RECT rc{};
  const bool has_rect = GetWindowRect(hwnd, &rc) != FALSE;
  const int rw = has_rect ? std::max(0L, rc.right - rc.left) : 0;
  const int rh = has_rect ? std::max(0L, rc.bottom - rc.top) : 0;
  const bool on_screen = w32_vis && !cloaked && !minimized && rw > 0 && rh > 0;
  char buf[256];
  std::snprintf(
      buf, sizeof(buf),
      "\"win32_visible\":%s,\"cloaked\":%s,\"topmost\":%s,\"minimized\":%s,"
      "\"on_screen\":%s,\"exstyle\":\"0x%llX\","
      "\"win32_rect\":{\"x\":%ld,\"y\":%ld,\"w\":%d,\"h\":%d}",
      w32_vis ? "true" : "false",
      cloaked ? "true" : "false",
      topmost ? "true" : "false",
      minimized ? "true" : "false",
      on_screen ? "true" : "false",
      static_cast<unsigned long long>(ex),
      has_rect ? rc.left : 0L,
      has_rect ? rc.top : 0L,
      rw, rh);
  *out += buf;
  return on_screen;
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

  // Support hex HWND matching (e.g. "0x120A4").
  if (selector.size() > 2 && selector[0] == '0' &&
      (selector[1] == 'x' || selector[1] == 'X')) {
    char* endp = nullptr;
    const uintptr_t want_hwnd =
        static_cast<uintptr_t>(std::strtoull(selector.c_str(), &endp, 16));
    if (want_hwnd != 0 && endp != nullptr && *endp == '\0') {
      for (guint i = 0; i < n; ++i) {
        gpointer obj = g_list_model_get_item(toplevels, i);
        GtkWidget* w = GTK_IS_WIDGET(obj) ? GTK_WIDGET(obj) : nullptr;
        const HWND hwnd = gtk_widget_hwnd(w);
        if (obj != nullptr)
          g_object_unref(obj);
        if (hwnd != nullptr && reinterpret_cast<uintptr_t>(hwnd) == want_hwnd)
          return w;
      }
      return nullptr;
    }
  }

  // For name/title selectors, prefer visible+mapped windows over hidden ones.
  const std::string want = to_lower_ascii(selector);
  GtkWidget* best = nullptr;
  int best_score = -1;
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
    int score = -1;
    if (match) {
      const bool vis = gtk_widget_get_visible(w) != FALSE;
      const bool mapped = gtk_widget_get_mapped(w) != FALSE;
      score = (vis && mapped) ? 2 : (vis ? 1 : 0);
    }
    g_object_unref(obj);
    if (score > best_score) {
      best = w;
      best_score = score;
      if (best_score == 2)
        return best;
    }
  }
  return best;
}

void dump_gtk_widget_json(GtkWidget* w,
                          GtkWidget* root,
                          int depth,
                          int max_depth,
                          int* node_budget,
                          bool include_measure,
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
  const bool sensitive = gtk_widget_get_sensitive(w) != FALSE;
  const double opacity = gtk_widget_get_opacity(w);
  const int width = gtk_widget_get_width(w);
  const int height = gtk_widget_get_height(w);
  graphene_rect_t bounds{};
  bool has_bounds = (root != nullptr &&
                     gtk_widget_compute_bounds(w, root, &bounds) != FALSE);

  *out += "{\"type\":";
  append_json_str(type_name ? type_name : "?", out);
  *out += ",\"name\":";
  append_json_str(name ? name : "", out);
  char geom[224];
  std::snprintf(
      geom, sizeof(geom),
      ",\"visible\":%s,\"mapped\":%s,\"sensitive\":%s,\"opacity\":%.3f,"
      "\"w\":%d,\"h\":%d,\"x\":%.1f,\"y\":%.1f",
      vis ? "true" : "false", mapped ? "true" : "false",
      sensitive ? "true" : "false", opacity,
      width, height,
      has_bounds ? bounds.origin.x : 0.0f, has_bounds ? bounds.origin.y : 0.0f);
  *out += geom;

  if (include_measure) {
    int min_w = 0, nat_w = 0, min_h = 0, nat_h = 0;
    gtk_widget_measure(w, GTK_ORIENTATION_HORIZONTAL, -1,
                       &min_w, &nat_w, nullptr, nullptr);
    gtk_widget_measure(w, GTK_ORIENTATION_VERTICAL, std::max(nat_w, 1),
                       &min_h, &nat_h, nullptr, nullptr);
    char m[128];
    std::snprintf(m, sizeof(m),
                  ",\"min_w\":%d,\"nat_w\":%d,\"min_h\":%d,\"nat_h\":%d",
                  min_w, nat_w, min_h, nat_h);
    *out += m;
  }

  if (GTK_IS_WINDOW(w)) {
    const HWND hwnd = gtk_widget_hwnd(w);
    char hbuf[48];
    std::snprintf(hbuf, sizeof(hbuf), ",\"hwnd\":\"0x%llX\",",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)));
    *out += hbuf;
    append_win32_hwnd_state_fields(hwnd, out);
  }

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
        dump_gtk_widget_json(ch, root, depth + 1, max_depth, node_budget,
                             include_measure, out);
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
            HWND hwnd = gtk_widget_hwnd(w);
            char head[256];
            std::snprintf(
                head, sizeof(head),
                "{\"index\":%u,\"visible\":%s,\"mapped\":%s,\"sensitive\":%s,"
                "\"opacity\":%.3f,\"w\":%d,\"h\":%d,"
                "\"scale\":%d,\"hwnd\":\"0x%llX\",",
                i,
                gtk_widget_get_visible(w) ? "true" : "false",
                gtk_widget_get_mapped(w) ? "true" : "false",
                gtk_widget_get_sensitive(w) ? "true" : "false",
                gtk_widget_get_opacity(w),
                gtk_widget_get_width(w), gtk_widget_get_height(w),
                gtk_widget_get_scale_factor(w),
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)));
            json += head;
            append_win32_hwnd_state_fields(hwnd, &json);
            json += ",\"renderer\":";
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
    const bool include_measure = arg_bool(req.args, "measure", false);
    std::string json;
    bool found = false;
    if (!run_on_gtk_main_sync([&] {
          GtkWidget* root = resolve_gtk_toplevel(win_sel);
          if (root == nullptr)
            return;
          found = true;
          int budget = 400;
          dump_gtk_widget_json(root, root, 0, max_depth, &budget,
                               include_measure, &json);
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
    const bool clear_flag = arg_bool(req.args, "clear", false);
    std::string action = to_lower_ascii(arg_str(req.args, "action"));
    if (clear_flag)
      action = "clear";
    else if (action.empty())
      action = "replace";
    const std::string css = arg_str(req.args, "css");
    if (action != "replace" && action != "append" &&
        action != "list" && action != "clear") {
      *out_err = "unknown gtk.css action: " + action;
      return false;
    }
    std::string res_json;
    if (!run_on_gtk_main_sync([&] {
          GdkDisplay* disp = gdk_display_get_default();
          if (action == "list") {
            char head[96];
            std::snprintf(head, sizeof(head),
                          "{\"action\":\"list\",\"active\":%s,\"bytes\":%zu,\"css\":",
                          g_live_debug_css_provider != nullptr ? "true" : "false",
                          g_live_debug_css_text.size());
            res_json = head;
            append_json_str(g_live_debug_css_text, &res_json);
            res_json += "}";
            return;
          }
          if (action == "clear") {
            g_live_debug_css_text.clear();
          } else if (action == "append") {
            if (!css.empty()) {
              if (!g_live_debug_css_text.empty() && g_live_debug_css_text.back() != '\n')
                g_live_debug_css_text.push_back('\n');
              g_live_debug_css_text += css;
            }
          } else {
            g_live_debug_css_text = css;
          }

          if (disp != nullptr) {
            if (g_live_debug_css_provider != nullptr) {
              gtk_style_context_remove_provider_for_display(
                  disp, GTK_STYLE_PROVIDER(g_live_debug_css_provider));
              g_clear_object(&g_live_debug_css_provider);
            }
            if (!g_live_debug_css_text.empty()) {
              g_live_debug_css_provider = gtk_css_provider_new();
              gtk_css_provider_load_from_string(g_live_debug_css_provider,
                                                g_live_debug_css_text.c_str());
              gtk_style_context_add_provider_for_display(
                  disp, GTK_STYLE_PROVIDER(g_live_debug_css_provider),
                  GTK_STYLE_PROVIDER_PRIORITY_USER);
            }
          }
          char buf[128];
          std::snprintf(buf, sizeof(buf),
                        "{\"action\":\"%s\",\"%s\":true,\"bytes\":%zu}",
                        action.c_str(),
                        g_live_debug_css_text.empty() ? "cleared" : "applied",
                        g_live_debug_css_text.size());
          res_json = buf;
        })) {
      *out_err = "GTK main-thread dispatch timed out";
      return false;
    }
    *out_data_json = std::move(res_json);
    return true;
  }

  if (op == "gtk.node_at") {
    if (!gtk_is_initialized()) {
      *out_err = "GTK is not initialized";
      return false;
    }
    const std::string win_sel = arg_str(req.args, "window", "0");
    const double req_x = std::atof(arg_str(req.args, "x", "0").c_str());
    const double req_y = std::atof(arg_str(req.args, "y", "0").c_str());
    const bool screen_coords = arg_bool(req.args, "screen", false);
    std::string res_json;
    std::string err;
    if (!run_on_gtk_main_sync([&] {
          GtkWidget* root = resolve_gtk_toplevel(win_sel);
          if (root == nullptr) {
            err = "GTK window not found: " + win_sel;
            return;
          }
          double lx = req_x;
          double ly = req_y;
          if (screen_coords) {
            HWND hwnd = gtk_widget_hwnd(root);
            if (hwnd == nullptr || !IsWindow(hwnd)) {
              err = "target window has no realized HWND for screen coords";
              return;
            }
            POINT pt{static_cast<LONG>(std::lround(req_x)),
                     static_cast<LONG>(std::lround(req_y))};
            ScreenToClient(hwnd, &pt);
            const int scale = std::max(gtk_widget_get_scale_factor(root), 1);
            lx = static_cast<double>(pt.x) / static_cast<double>(scale);
            ly = static_cast<double>(pt.y) / static_cast<double>(scale);
          }
          GtkWidget* picked = gtk_widget_pick(root, lx, ly, GTK_PICK_DEFAULT);
          if (picked == nullptr) {
            char b[128];
            std::snprintf(b, sizeof(b),
                          "{\"hit\":false,\"lx\":%.1f,\"ly\":%.1f,\"chain\":[]}",
                          lx, ly);
            res_json = b;
            return;
          }
          std::vector<GtkWidget*> chain;
          for (GtkWidget* cur = picked; cur != nullptr; cur = gtk_widget_get_parent(cur))
            chain.push_back(cur);
          std::reverse(chain.begin(), chain.end());

          auto append_node_brief = [&](GtkWidget* nw, bool include_meas) {
            const char* tn = G_OBJECT_TYPE_NAME(nw);
            const char* nm = gtk_widget_get_name(nw);
            const char* cn = gtk_widget_get_css_name(nw);
            graphene_rect_t b{};
            const bool hb = gtk_widget_compute_bounds(nw, root, &b) != FALSE;
            res_json += "{\"type\":";
            append_json_str(tn ? tn : "?", &res_json);
            res_json += ",\"css_name\":";
            append_json_str(cn ? cn : "", &res_json);
            res_json += ",\"name\":";
            append_json_str(nm ? nm : "", &res_json);
            char g[192];
            std::snprintf(g, sizeof(g),
                          ",\"visible\":%s,\"sensitive\":%s,\"w\":%d,\"h\":%d,\"x\":%.1f,\"y\":%.1f",
                          gtk_widget_get_visible(nw) ? "true" : "false",
                          gtk_widget_get_sensitive(nw) ? "true" : "false",
                          gtk_widget_get_width(nw), gtk_widget_get_height(nw),
                          hb ? b.origin.x : 0.0f, hb ? b.origin.y : 0.0f);
            res_json += g;
            if (include_meas) {
              int mw = 0, nw_sz = 0, mh = 0, nh = 0;
              gtk_widget_measure(nw, GTK_ORIENTATION_HORIZONTAL, -1, &mw, &nw_sz, nullptr, nullptr);
              gtk_widget_measure(nw, GTK_ORIENTATION_VERTICAL, std::max(nw_sz, 1), &mh, &nh, nullptr, nullptr);
              char ms[96];
              std::snprintf(ms, sizeof(ms),
                            ",\"min_w\":%d,\"nat_w\":%d,\"min_h\":%d,\"nat_h\":%d",
                            mw, nw_sz, mh, nh);
              res_json += ms;
            }
            char** cls = gtk_widget_get_css_classes(nw);
            res_json += ",\"classes\":[";
            if (cls != nullptr) {
              for (int i = 0; cls[i] != nullptr; ++i) {
                if (i > 0) res_json.push_back(',');
                append_json_str(cls[i], &res_json);
              }
              g_strfreev(cls);
            }
            res_json += "]}";
          };

          char hd[96];
          std::snprintf(hd, sizeof(hd),
                        "{\"hit\":true,\"lx\":%.1f,\"ly\":%.1f,\"target\":",
                        lx, ly);
          res_json = hd;
          append_node_brief(picked, true);
          res_json += ",\"chain\":[";
          for (size_t i = 0; i < chain.size(); ++i) {
            if (i > 0) res_json.push_back(',');
            append_node_brief(chain[i], false);
          }
          res_json += "]}";
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

  if (op == "gtk.redraw") {
    if (!gtk_is_initialized()) {
      *out_err = "GTK is not initialized";
      return false;
    }
    const std::string win_sel = arg_str(req.args, "window", "all");
    std::string res_json;
    std::string err;
    if (!run_on_gtk_main_sync([&] {
          auto redraw_widget = [](GtkWidget* w) {
            gtk_widget_queue_allocate(w);
            gtk_widget_queue_draw(w);
            if (GTK_IS_WINDOW(w)) {
              if (GtkWidget* ch = gtk_window_get_child(GTK_WINDOW(w)))
                gtk_widget_queue_draw(ch);
            }
            HWND hwnd = gtk_widget_hwnd(w);
            if (hwnd != nullptr && IsWindow(hwnd))
              InvalidateRect(hwnd, nullptr, TRUE);
          };
          std::vector<std::string> hwnds;
          if (to_lower_ascii(win_sel) == "all") {
            GListModel* toplevels = gtk_window_get_toplevels();
            const guint n = toplevels != nullptr ? g_list_model_get_n_items(toplevels) : 0;
            for (guint i = 0; i < n; ++i) {
              gpointer obj = g_list_model_get_item(toplevels, i);
              GtkWidget* w = GTK_IS_WIDGET(obj) ? GTK_WIDGET(obj) : nullptr;
              if (w != nullptr && gtk_widget_get_visible(w)) {
                redraw_widget(w);
                char hb[32];
                std::snprintf(hb, sizeof(hb), "0x%llX",
                              static_cast<unsigned long long>(
                                  reinterpret_cast<uintptr_t>(gtk_widget_hwnd(w))));
                hwnds.emplace_back(hb);
              }
              if (obj != nullptr)
                g_object_unref(obj);
            }
          } else {
            GtkWidget* root = resolve_gtk_toplevel(win_sel);
            if (root == nullptr) {
              err = "GTK window not found: " + win_sel;
              return;
            }
            redraw_widget(root);
            char hb[32];
            std::snprintf(hb, sizeof(hb), "0x%llX",
                          static_cast<unsigned long long>(
                              reinterpret_cast<uintptr_t>(gtk_widget_hwnd(root))));
            hwnds.emplace_back(hb);
          }
          res_json = "{\"redrawn\":" + std::to_string(hwnds.size()) + ",\"windows\":[";
          for (size_t i = 0; i < hwnds.size(); ++i) {
            if (i > 0) res_json.push_back(',');
            append_json_str(hwnds[i], &res_json);
          }
          res_json += "]}";
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
    const std::string fallback_str = to_lower_ascii(arg_str(req.args, "fallback"));
    const bool fallback_mouse =
        (fallback_str == "mouse") || arg_bool(req.args, "fallback", false);
    std::string res_json;
    std::string err;
    bool need_mouse_fallback = false;
    int click_screen_x = 0;
    int click_screen_y = 0;
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
            const bool vis = gtk_widget_get_visible(target) != FALSE;
            const bool sens = gtk_widget_get_sensitive(target) != FALSE;
            if (!vis || !sens) {
              res_json = std::string("{\"activated\":false,\"reason\":\"") +
                         (!vis ? "widget_not_visible" : "widget_not_sensitive") + "\"}";
              return;
            }
            if (GTK_IS_BUTTON(target)) {
              g_signal_emit_by_name(target, "clicked");
              res_json = "{\"activated\":true,\"emitted\":\"clicked\"}";
            } else {
              gboolean ok = gtk_widget_activate(target);
              if (ok) {
                res_json = "{\"activated\":true}";
              } else if (fallback_mouse) {
                HWND hwnd = gtk_widget_hwnd(root);
                graphene_rect_t bounds{};
                if (hwnd == nullptr || !IsWindow(hwnd) ||
                    !gtk_widget_compute_bounds(target, root, &bounds)) {
                  res_json = "{\"activated\":false,\"reason\":\"mouse_fallback_unavailable\"}";
                  return;
                }
                POINT pt{0, 0};
                ClientToScreen(hwnd, &pt);
                const int scale = std::max(gtk_widget_get_scale_factor(root), 1);
                click_screen_x = pt.x + static_cast<int>(std::lround(
                    (bounds.origin.x + bounds.size.width * 0.5) * scale));
                click_screen_y = pt.y + static_cast<int>(std::lround(
                    (bounds.origin.y + bounds.size.height * 0.5) * scale));
                need_mouse_fallback = true;
              } else {
                res_json = "{\"activated\":false,\"reason\":\"widget_not_activatable_use_fallback_mouse\"}";
              }
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
            } else if (prop == "text" && GTK_IS_LABEL(target)) {
              if (has_val) gtk_label_set_text(GTK_LABEL(target), val.c_str());
              res_json = "{\"text\":";
              const char* t = gtk_label_get_text(GTK_LABEL(target));
              append_json_str(t ? t : "", &res_json);
              res_json += "}";
            } else if (prop == "classes") {
              if (has_val && !val.empty()) {
                if (val[0] == '+' && val.size() > 1)
                  gtk_widget_add_css_class(target, val.c_str() + 1);
                else if (val[0] == '-' && val.size() > 1)
                  gtk_widget_remove_css_class(target, val.c_str() + 1);
              }
              char** cls = gtk_widget_get_css_classes(target);
              res_json = "{\"classes\":[";
              if (cls != nullptr) {
                for (int i = 0; cls[i] != nullptr; ++i) {
                  if (i > 0) res_json.push_back(',');
                  append_json_str(cls[i], &res_json);
                }
                g_strfreev(cls);
              }
              res_json += "]}";
            } else if (prop == "min_size" || prop == "measure") {
              int min_w = 0, nat_w = 0, min_h = 0, nat_h = 0;
              gtk_widget_measure(target, GTK_ORIENTATION_HORIZONTAL, -1,
                                 &min_w, &nat_w, nullptr, nullptr);
              gtk_widget_measure(target, GTK_ORIENTATION_VERTICAL, std::max(nat_w, 1),
                                 &min_h, &nat_h, nullptr, nullptr);
              char b[160];
              std::snprintf(b, sizeof(b),
                            "{\"min_w\":%d,\"nat_w\":%d,\"min_h\":%d,\"nat_h\":%d,\"w\":%d,\"h\":%d}",
                            min_w, nat_w, min_h, nat_h,
                            gtk_widget_get_width(target), gtk_widget_get_height(target));
              res_json = b;
            } else if (prop == "bounds") {
              graphene_rect_t b{};
              const bool hb = gtk_widget_compute_bounds(target, root, &b) != FALSE;
              char buf[128];
              std::snprintf(buf, sizeof(buf),
                            "{\"x\":%.1f,\"y\":%.1f,\"w\":%d,\"h\":%d}",
                            hb ? b.origin.x : 0.0f, hb ? b.origin.y : 0.0f,
                            gtk_widget_get_width(target), gtk_widget_get_height(target));
              res_json = buf;
            } else if (prop == "css_name") {
              const char* cn = gtk_widget_get_css_name(target);
              res_json = "{\"css_name\":";
              append_json_str(cn ? cn : "", &res_json);
              res_json += "}";
            } else if (prop == "margins" || prop == "margin_top" ||
                       prop == "margin_bottom" || prop == "margin_start" ||
                       prop == "margin_end") {
              char buf[128];
              std::snprintf(buf, sizeof(buf),
                            "{\"margin_top\":%d,\"margin_bottom\":%d,"
                            "\"margin_start\":%d,\"margin_end\":%d}",
                            gtk_widget_get_margin_top(target),
                            gtk_widget_get_margin_bottom(target),
                            gtk_widget_get_margin_start(target),
                            gtk_widget_get_margin_end(target));
              res_json = buf;
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
    if (need_mouse_fallback) {
      DWORD gle = 0;
      send_mouse_abs_move(click_screen_x, click_screen_y, &gle);
      Sleep(5);
      const bool d_ok = send_mouse_button("left", true, &gle);
      Sleep(15);
      const bool u_ok = send_mouse_button("left", false, &gle);
      char buf[160];
      std::snprintf(buf, sizeof(buf),
                    "{\"activated\":%s,\"fallback\":\"mouse\",\"x\":%d,\"y\":%d}",
                    (d_ok && u_ok) ? "true" : "false",
                    click_screen_x, click_screen_y);
      *out_data_json = buf;
      return d_ok && u_ok;
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
      else if (lop == "gtk.tree" || lop == "gtk.redraw") out->args["window"] = tok;
      else if (lop == "gtk.css") {
        const std::string ltok = to_lower_ascii(tok);
        if (ltok == "list" || ltok == "clear" || ltok == "append" || ltok == "replace")
          out->args["action"] = ltok;
        else
          out->args["css"] = tok;
      }
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
         R"("gtk":["gtk.windows","gtk.tree [window=0|<name>|0x<hwnd>] [max_depth=8] [measure=false]","gtk.css [action=replace|append|list|clear] [css=<rules>] [clear=true]","gtk.click [window=0] [name=.. class=.. text=..] [fallback=mouse]","gtk.prop [window=0] [name=.. class=..] prop=visible|opacity|sensitive|text|classes|min_size|bounds|css_name|margins [value=..]","gtk.node_at x=<px> y=<px> [window=0] [screen=false]","gtk.redraw [window=all|0|<name>|0x<hwnd>]"],)"
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
