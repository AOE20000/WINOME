// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors

#include "run-dialog.h"

#include "config.h"
#include "debug-runtime.h"
#include "fonts.h"
#include "host-utils.h"
#include "i18n.h"
#include "monitor-manager.h"
#include "virtual-desktop.h"

#include <dwmapi.h>
#include <gdk/win32/gdkwin32.h>
#include <gtk/gtk.h>
#include <objbase.h>
#include <shellapi.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace winome {
namespace {

constexpr DWORD kDwmNcRenderingPolicy = 2;      // DWMWA_NCRENDERING_POLICY
constexpr DWORD kDwmNcrpDisabled = 1;           // DWMNCRP_DISABLED
constexpr DWORD kDwmWindowCornerPref = 33;      // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD kDwmCornerDoNotRound = 1;       // DWMWCP_DONOTROUND
constexpr DWORD kDwmBorderColor = 34;           // DWMWA_BORDER_COLOR
constexpr DWORD kDwmColorNone = 0xFFFFFFFEu;    // suppress DWM 1px rectangular border
constexpr size_t kMaxHistoryItems = 32;

struct RunDialogState {
  RunDialogCallbacks cbs{};
  GtkWidget* window = nullptr;
  GtkWidget* entry = nullptr;
  GtkWidget* desc_label = nullptr;
  HWND hwnd = nullptr;
  HMONITOR target_monitor = nullptr;
  guint vd_guard_id = 0;
  guint post_map_idle_id = 0;
  int last_dlg_w = 0;
  int last_dlg_h = 0;
  bool vd_listener_added = false;
  std::vector<std::string> history;
  int history_cursor = -1;
  std::string draft_before_history;
  bool history_loaded = false;
  bool navigating_history = false;
  bool css_loaded = false;
  uint64_t launch_gen = 0;
};

RunDialogState g_run;

constexpr const wchar_t* kRunDlgOrigProcProp = L"WINOME_RunDlgOrigProc";

LRESULT CALLBACK run_dialog_subclass_wndproc(HWND hwnd, UINT msg,
                                             WPARAM wParam, LPARAM lParam) {
  if (msg == WM_STYLECHANGING && wParam == static_cast<WPARAM>(GWL_EXSTYLE) &&
      lParam != 0) {
    auto* ss = reinterpret_cast<STYLESTRUCT*>(lParam);
    ss->styleNew = (static_cast<LONG_PTR>(ss->styleNew) | WS_EX_TOOLWINDOW) &
                   ~WS_EX_APPWINDOW;
  }

  if (msg == WM_WINDOWPOSCHANGING && lParam != 0 &&
      g_run.target_monitor != nullptr) {
    auto* wp = reinterpret_cast<WINDOWPOS*>(lParam);
    if ((wp->flags & SWP_HIDEWINDOW) == 0) {
      MONITORINFO mi{};
      mi.cbSize = sizeof(mi);
      if (GetMonitorInfoW(g_run.target_monitor, &mi)) {
        RECT cur{};
        const bool has_cur = GetWindowRect(hwnd, &cur) != FALSE;
        const int eff_w = (wp->flags & SWP_NOSIZE) == 0
                              ? wp->cx
                              : (has_cur ? (cur.right - cur.left) : 0);
        const int eff_h = (wp->flags & SWP_NOSIZE) == 0
                              ? wp->cy
                              : (has_cur ? (cur.bottom - cur.top) : 0);
        if (eff_w > 32 && eff_h > 32) {
          const RECT want =
              compute_run_dialog_centered_rect(mi.rcMonitor, eff_w, eff_h);
          wp->x = want.left;
          wp->y = want.top;
          wp->flags &= ~SWP_NOMOVE;
        }
      }
    }
  }

  WNDPROC orig =
      reinterpret_cast<WNDPROC>(GetPropW(hwnd, kRunDlgOrigProcProp));
  if (msg == WM_NCDESTROY) {
    RemovePropW(hwnd, kRunDlgOrigProcProp);
    if (orig != nullptr) {
      if (GetWindowLongPtrW(hwnd, GWLP_WNDPROC) ==
          reinterpret_cast<LONG_PTR>(run_dialog_subclass_wndproc)) {
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(orig));
      }
      return CallWindowProcW(orig, hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  }
  return orig != nullptr ? CallWindowProcW(orig, hwnd, msg, wParam, lParam)
                         : DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ensure_run_dialog_subclass(HWND hwnd) {
  if (hwnd == nullptr || !IsWindow(hwnd))
    return;
  if (GetPropW(hwnd, kRunDlgOrigProcProp) != nullptr)
    return;
  WNDPROC cur =
      reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
  if (cur != nullptr && cur != run_dialog_subclass_wndproc) {
    SetPropW(hwnd, kRunDlgOrigProcProp, reinterpret_cast<HANDLE>(cur));
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(run_dialog_subclass_wndproc));
  }
}

void unsubclass_run_dialog_hwnd(HWND hwnd) {
  if (hwnd == nullptr || !IsWindow(hwnd))
    return;
  WNDPROC orig =
      reinterpret_cast<WNDPROC>(GetPropW(hwnd, kRunDlgOrigProcProp));
  WNDPROC cur =
      reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
  if (orig != nullptr && cur == run_dialog_subclass_wndproc)
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
  RemovePropW(hwnd, kRunDlgOrigProcProp);
}

void keep_run_dialog_visible_on_active_vd(int reported_active_index,
                                          bool restore_focus) {
  if (g_run.window == nullptr || !gtk_widget_get_visible(g_run.window))
    return;
  GdkSurface* surf = gtk_native_get_surface(GTK_NATIVE(g_run.window));
  HWND hwnd = surf != nullptr
                  ? static_cast<HWND>(gdk_win32_surface_get_handle(surf))
                  : g_run.hwnd;
  if (hwnd == nullptr || !IsWindow(hwnd))
    return;

  LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  LONG_PTR want = (ex | WS_EX_TOOLWINDOW | WS_EX_TOPMOST) & ~WS_EX_APPWINDOW;
  if (want != ex)
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, want);

  vd::pin_window_async(hwnd);

  DWORD cloaked = 0;
  DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
  if ((cloaked & 0x2u /* DWM_CLOAKED_SHELL */) != 0) {
    int target_vd = reported_active_index;
    if (target_vd < 0) {
      const auto snap = vd::cached_snapshot();
      target_vd = snap.valid ? snap.active_index : vd::current_index();
    }
    if (target_vd >= 0) {
      vd::move_window_to_desktop(hwnd, target_vd);
      vd::pin_window_async(hwnd);
    }
  }

  SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
  if (restore_focus) {
    SetForegroundWindow(hwnd);
    gtk_window_present(GTK_WINDOW(g_run.window));
    if (g_run.entry != nullptr)
      gtk_widget_grab_focus(g_run.entry);
  }
}

gboolean on_run_dialog_vd_guard_timeout(gpointer user_data G_GNUC_UNUSED) {
  g_run.vd_guard_id = 0;
  keep_run_dialog_visible_on_active_vd(-1, true);
  return G_SOURCE_REMOVE;
}

void on_run_dialog_vd_changed(int count G_GNUC_UNUSED,
                              int active_index,
                              void* user_data G_GNUC_UNUSED) {
  keep_run_dialog_visible_on_active_vd(active_index, true);
  if (g_run.vd_guard_id != 0) {
    g_source_remove(g_run.vd_guard_id);
    g_run.vd_guard_id = 0;
  }
  if (g_run.window != nullptr) {
    g_run.vd_guard_id =
        g_timeout_add(90, on_run_dialog_vd_guard_timeout, nullptr);
  }
}

std::string trim_copy(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
    ++b;
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
    --e;
  return s.substr(b, e - b);
}

std::wstring utf8_to_wide(const std::string& utf8) {
  if (utf8.empty())
    return {};
  int need = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                 static_cast<int>(utf8.size()), nullptr, 0);
  if (need <= 0)
    return {};
  std::wstring out(static_cast<size_t>(need), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                      static_cast<int>(utf8.size()), out.data(), need);
  return out;
}

std::string wide_to_utf8(const std::wstring& w) {
  if (w.empty())
    return {};
  int need = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                 static_cast<int>(w.size()), nullptr, 0,
                                 nullptr, nullptr);
  if (need <= 0)
    return {};
  std::string out(static_cast<size_t>(need), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(),
                      static_cast<int>(w.size()), out.data(), need,
                      nullptr, nullptr);
  return out;
}

std::string format_win32_error_utf8(DWORD err) {
  if (err == 0)
    return "Failed to launch command";
  wchar_t* msg_buf = nullptr;
  const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                      FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD n = FormatMessageW(flags, nullptr, err, 0,
                                 reinterpret_cast<LPWSTR>(&msg_buf), 0, nullptr);
  std::string out;
  if (n > 0 && msg_buf != nullptr) {
    out = wide_to_utf8(std::wstring(msg_buf, n));
    LocalFree(msg_buf);
  }
  while (!out.empty() && (out.back() == '\r' || out.back() == '\n' ||
                          out.back() == ' ')) {
    out.pop_back();
  }
  if (out.empty()) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Win32 error %lu",
                  static_cast<unsigned long>(err));
    return buf;
  }
  return out;
}

void ensure_run_css() {
  if (g_run.css_loaded)
    return;
  g_run.css_loaded = true;
  static const char* kCss = R"(
#winomeRunDialogWindow {
  background-color: transparent;
  background-image: none;
  box-shadow: none;
  color: #ffffff;
  font: 11pt "Cantarell";
}
.run-dialog-error-text {
  color: #ff7b72;
}
)";
  GtkCssProvider* p = gtk_css_provider_new();
  gtk_css_provider_load_from_string(p, kCss);
  if (GdkDisplay* disp = gdk_display_get_default()) {
    gtk_style_context_add_provider_for_display(
        disp, GTK_STYLE_PROVIDER(p),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  }
  g_object_unref(p);
}

std::wstring history_file_path() {
  std::wstring dir = app_data_dir();
  if (dir.empty())
    return {};
  return dir + L"\\run-history.txt";
}

void load_history_once() {
  if (g_run.history_loaded)
    return;
  g_run.history_loaded = true;
  const std::wstring path = history_file_path();
  if (path.empty())
    return;
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE)
    return;
  LARGE_INTEGER sz{};
  if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart <= 131072) {
    std::string raw(static_cast<size_t>(sz.QuadPart), '\0');
    DWORD got = 0;
    if (ReadFile(f, raw.data(), static_cast<DWORD>(sz.QuadPart), &got, nullptr)) {
      raw.resize(got);
      size_t pos = 0;
      while (pos < raw.size()) {
        size_t nl = raw.find('\n', pos);
        std::string line =
            (nl == std::string::npos) ? raw.substr(pos)
                                      : raw.substr(pos, nl - pos);
        line = trim_copy(line);
        if (!line.empty())
          g_run.history = run_history_add_item(std::move(g_run.history), line, kMaxHistoryItems);
        if (nl == std::string::npos)
          break;
        pos = nl + 1;
      }
    }
  }
  CloseHandle(f);
}

void save_history() {
  const std::wstring path = history_file_path();
  if (path.empty())
    return;
  std::string payload;
  for (const auto& item : g_run.history) {
    payload += item;
    payload.push_back('\n');
  }
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE)
    return;
  DWORD w = 0;
  WriteFile(f, payload.data(), static_cast<DWORD>(payload.size()), &w, nullptr);
  CloseHandle(f);
}

std::string default_hint_text() {
  return tr("Press ESC to close", "按 ESC 键关闭");
}

void set_description_status(const std::string& text, bool is_error) {
  if (g_run.desc_label == nullptr)
    return;
  gtk_label_set_text(GTK_LABEL(g_run.desc_label), text.c_str());
  if (is_error)
    gtk_widget_add_css_class(g_run.desc_label, "run-dialog-error-text");
  else
    gtk_widget_remove_css_class(g_run.desc_label, "run-dialog-error-text");
}

struct PathExecCache {
  std::mutex mu;
  std::vector<std::string> execs;
  int64_t updated_us = 0;
  std::atomic<bool> scanning{false};
};

PathExecCache g_path_cache;

void refresh_path_exec_cache_async() {
  const int64_t now_us = g_get_monotonic_time();
  {
    std::lock_guard<std::mutex> lk(g_path_cache.mu);
    if (g_path_cache.updated_us != 0 &&
        (now_us - g_path_cache.updated_us) < 60LL * 1000 * 1000) {
      return;
    }
  }
  bool expected = false;
  if (!g_path_cache.scanning.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) {
    return;
  }
  std::thread([] {
    std::vector<std::string> found;
    found.reserve(256);
    wchar_t path_buf[32768] = {};
    const DWORD plen = GetEnvironmentVariableW(L"PATH", path_buf, 32768);
    if (plen > 0 && plen < 32768) {
      std::wstring path_env(path_buf, plen);
      size_t start = 0;
      int scanned_dirs = 0;
      while (start < path_env.size() && scanned_dirs < 32) {
        size_t semi = path_env.find(L';', start);
        std::wstring dir = (semi == std::string::npos)
                               ? path_env.substr(start)
                               : path_env.substr(start, semi - start);
        if (!dir.empty()) {
          ++scanned_dirs;
          if (dir.back() != L'\\' && dir.back() != L'/')
            dir.push_back(L'\\');
          std::wstring pattern = dir + L"*";
          WIN32_FIND_DATAW fd{};
          HANDLE hfind = FindFirstFileW(pattern.c_str(), &fd);
          if (hfind != INVALID_HANDLE_VALUE) {
            do {
              if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                continue;
              const std::wstring name(fd.cFileName);
              const size_t dot = name.rfind(L'.');
              if (dot != std::wstring::npos) {
                std::wstring ext = name.substr(dot);
                for (wchar_t& ch : ext)
                  ch = static_cast<wchar_t>(std::towlower(ch));
                if (ext == L".exe" || ext == L".cmd" || ext == L".bat" ||
                    ext == L".com") {
                  found.push_back(wide_to_utf8(name));
                }
              }
            } while (FindNextFileW(hfind, &fd) && found.size() < 4096);
            FindClose(hfind);
          }
        }
        if (semi == std::string::npos)
          break;
        start = semi + 1;
      }
    }
    {
      std::lock_guard<std::mutex> lk(g_path_cache.mu);
      g_path_cache.execs = std::move(found);
      g_path_cache.updated_us = g_get_monotonic_time();
    }
    g_path_cache.scanning.store(false, std::memory_order_release);
  }).detach();
}

std::vector<std::string> collect_completion_candidates(const std::string& prefix) {
  std::vector<std::string> cands;
  if (get_config().developer_mode) {
    static const char* const kInternals[] = {"lg", "rt", "r", "restart", "debugexit"};
    for (const char* cmd : kInternals) {
      if (std::string(cmd).compare(0, prefix.size(), prefix) == 0)
        cands.emplace_back(cmd);
    }
  }
  for (const auto& h : g_run.history) {
    if (h.compare(0, prefix.size(), prefix) == 0)
      cands.push_back(h);
  }
  refresh_path_exec_cache_async();
  std::lock_guard<std::mutex> lk(g_path_cache.mu);
  std::string lprefix = prefix;
  for (char& c : lprefix)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const auto& name : g_path_cache.execs) {
    if (name.size() >= lprefix.size()) {
      bool match = true;
      for (size_t i = 0; i < lprefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(name[i])) != lprefix[i]) {
          match = false;
          break;
        }
      }
      if (match) {
        cands.push_back(name);
        if (cands.size() >= 200)
          break;
      }
    }
  }
  return cands;
}

void position_run_dialog(GtkWidget* dlg, int dlg_w, int dlg_h) {
  GdkSurface* surf = gtk_native_get_surface(GTK_NATIVE(dlg));
  if (surf == nullptr)
    return;
  HWND hwnd = static_cast<HWND>(gdk_win32_surface_get_handle(surf));
  if (hwnd == nullptr)
    return;

  if (g_run.target_monitor == nullptr)
    g_run.target_monitor = monitor_manager_active_monitor();
  g_run.last_dlg_w = dlg_w;
  g_run.last_dlg_h = dlg_h;
  g_run.hwnd = hwnd;
  ensure_run_dialog_subclass(hwnd);
  harden_shell_overlay_hwnd(hwnd);

  DwmSetWindowAttribute(hwnd, kDwmNcRenderingPolicy, &kDwmNcrpDisabled,
                        sizeof(kDwmNcrpDisabled));
  DwmSetWindowAttribute(hwnd, kDwmWindowCornerPref, &kDwmCornerDoNotRound,
                        sizeof(kDwmCornerDoNotRound));
  DwmSetWindowAttribute(hwnd, kDwmBorderColor, &kDwmColorNone,
                        sizeof(kDwmColorNone));

  LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                    (ex | WS_EX_TOOLWINDOW | WS_EX_TOPMOST) & ~WS_EX_APPWINDOW);

  RECT mon_rect = monitor_manager_active_workarea();
  if (g_run.target_monitor != nullptr) {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(g_run.target_monitor, &mi))
      mon_rect = mi.rcMonitor;
  }

  RECT wr{};
  GetWindowRect(hwnd, &wr);
  const int scale = std::max(1, gtk_widget_get_scale_factor(dlg));
  const int w = (wr.right - wr.left) > 32 ? (wr.right - wr.left) : dlg_w * scale;
  const int h = (wr.bottom - wr.top) > 32 ? (wr.bottom - wr.top) : dlg_h * scale;
  const RECT want = compute_run_dialog_centered_rect(mon_rect, w, h);
  SetWindowPos(hwnd, HWND_TOPMOST, want.left, want.top, 0, 0,
               SWP_NOSIZE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
  vd::pin_window_async(hwnd);
}

gboolean on_run_post_map_idle(gpointer ud G_GNUC_UNUSED) {
  g_run.post_map_idle_id = 0;
  if (g_run.window != nullptr && gtk_widget_get_visible(g_run.window)) {
    position_run_dialog(g_run.window, g_run.last_dlg_w, g_run.last_dlg_h);
    gtk_window_present(GTK_WINDOW(g_run.window));
    if (g_run.entry != nullptr)
      gtk_widget_grab_focus(g_run.entry);
  }
  return G_SOURCE_REMOVE;
}

void on_run_map(GtkWidget* w G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  if (g_run.post_map_idle_id != 0)
    return;
  g_run.post_map_idle_id =
      g_idle_add_full(G_PRIORITY_HIGH_IDLE, on_run_post_map_idle, nullptr, nullptr);
}

void on_entry_changed(GtkEditable* ed G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  if (!g_run.navigating_history)
    g_run.history_cursor = -1;
  set_description_status(default_hint_text(), false);
}

}  // namespace

bool execute_run_command_with_monitor(const std::string& raw_cmd,
                                      bool in_terminal,
                                      const RunDialogCallbacks& cbs,
                                      HMONITOR target_mon,
                                      std::string* out_err);

namespace {

void submit_run_command(const std::string& raw, bool in_terminal) {
  const std::string trimmed = trim_copy(raw);
  if (trimmed.empty()) {
    run_dialog_close();
    return;
  }

  load_history_once();
  g_run.history = run_history_add_item(std::move(g_run.history), trimmed, kMaxHistoryItems);
  save_history();

  // Internal commands (lg/rt/r/debugexit) always invoke GTK UI callbacks and
  // must run on the GTK main thread regardless of Ctrl+Enter (in_terminal).
  if (classify_run_internal_command(trimmed, get_config().developer_mode) !=
      RunInternalAction::None) {
    std::string err;
    if (execute_run_command_with_monitor(trimmed, false, g_run.cbs, nullptr, &err))
      run_dialog_close();
    else
      set_description_status(err.empty() ? "Command failed" : err, true);
    return;
  }

  // Resolve target monitor & arm launch watcher on the GTK main thread before
  // spawning the worker thread (GTK/GDK and monitor-manager are main-thread only).
  HMONITOR target_mon = monitor_manager_monitor_for_widget(g_run.window);
  watch_launched_window_for_monitor(target_mon);

  const uint64_t gen = ++g_run.launch_gen;
  if (g_run.entry != nullptr)
    gtk_widget_set_sensitive(g_run.entry, FALSE);
  set_description_status(tr("Launching command...", "正在执行命令..."), false);

  const RunDialogCallbacks cbs_copy = g_run.cbs;
  std::thread([trimmed, in_terminal, cbs_copy, target_mon, gen]() {
    HRESULT hr = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::string err;
    const bool ok = execute_run_command_with_monitor(
        trimmed, in_terminal, cbs_copy, target_mon, &err);
    if (SUCCEEDED(hr))
      CoUninitialize();
    struct Result {
      uint64_t gen;
      bool ok;
      std::string err;
    };
    auto* res = new Result{gen, ok, std::move(err)};
    g_idle_add(
        +[](gpointer p) -> gboolean {
          std::unique_ptr<Result> r(static_cast<Result*>(p));
          if (g_run.window == nullptr || r->gen != g_run.launch_gen)
            return G_SOURCE_REMOVE;
          if (g_run.entry != nullptr)
            gtk_widget_set_sensitive(g_run.entry, TRUE);
          if (r->ok) {
            run_dialog_close();
          } else {
            set_description_status(
                r->err.empty() ? "Command failed" : r->err, true);
            if (g_run.entry != nullptr)
              gtk_widget_grab_focus(g_run.entry);
          }
          return G_SOURCE_REMOVE;
        },
        res);
  }).detach();
}

void on_entry_activate(GtkEntry* entry, gpointer ud G_GNUC_UNUSED) {
  submit_run_command(gtk_editable_get_text(GTK_EDITABLE(entry)), false);
}

gboolean on_run_key_pressed(GtkEventControllerKey* ctrl G_GNUC_UNUSED,
                            guint keyval,
                            guint keycode G_GNUC_UNUSED,
                            GdkModifierType state,
                            gpointer ud G_GNUC_UNUSED) {
  if (keyval == GDK_KEY_Escape) {
    ++g_run.launch_gen;
    run_dialog_close();
    return TRUE;
  }
  if (g_run.entry == nullptr)
    return FALSE;

  if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter ||
      keyval == GDK_KEY_ISO_Enter) {
    if ((state & GDK_CONTROL_MASK) != 0) {
      submit_run_command(gtk_editable_get_text(GTK_EDITABLE(g_run.entry)), true);
      return TRUE;
    }
    return FALSE;
  }

  if (keyval == GDK_KEY_Tab) {
    const std::string cur = gtk_editable_get_text(GTK_EDITABLE(g_run.entry));
    if (!cur.empty() && cur.find(' ') == std::string::npos) {
      load_history_once();
      const auto cands = collect_completion_candidates(cur);
      const std::string suffix = run_complete_prefix(cur, cands);
      if (!suffix.empty()) {
        const std::string next = cur + suffix;
        gtk_editable_set_text(GTK_EDITABLE(g_run.entry), next.c_str());
        gtk_editable_set_position(GTK_EDITABLE(g_run.entry), -1);
      }
    }
    return TRUE;
  }

  if (keyval == GDK_KEY_Up || keyval == GDK_KEY_Down) {
    load_history_once();
    if (g_run.history.empty())
      return TRUE;
    const int n = static_cast<int>(g_run.history.size());
    if (g_run.history_cursor < 0)
      g_run.draft_before_history = gtk_editable_get_text(GTK_EDITABLE(g_run.entry));

    if (keyval == GDK_KEY_Up) {
      if (g_run.history_cursor < 0)
        g_run.history_cursor = n - 1;
      else if (g_run.history_cursor > 0)
        --g_run.history_cursor;
    } else {
      if (g_run.history_cursor >= 0 && g_run.history_cursor + 1 < n) {
        ++g_run.history_cursor;
      } else {
        g_run.history_cursor = -1;
      }
    }

    g_run.navigating_history = true;
    if (g_run.history_cursor >= 0) {
      gtk_editable_set_text(GTK_EDITABLE(g_run.entry),
                            g_run.history[g_run.history_cursor].c_str());
    } else {
      gtk_editable_set_text(GTK_EDITABLE(g_run.entry),
                            g_run.draft_before_history.c_str());
    }
    gtk_editable_set_position(GTK_EDITABLE(g_run.entry), -1);
    g_run.navigating_history = false;
    return TRUE;
  }

  return FALSE;
}

void on_run_destroy(GtkWidget* w G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  if (g_run.vd_listener_added) {
    vd::remove_change_listener(on_run_dialog_vd_changed, nullptr);
    g_run.vd_listener_added = false;
  }
  if (g_run.vd_guard_id != 0) {
    g_source_remove(g_run.vd_guard_id);
    g_run.vd_guard_id = 0;
  }
  if (g_run.post_map_idle_id != 0) {
    g_source_remove(g_run.post_map_idle_id);
    g_run.post_map_idle_id = 0;
  }
  if (g_run.hwnd != nullptr) {
    unsubclass_run_dialog_hwnd(g_run.hwnd);
    g_run.hwnd = nullptr;
  }
  g_run.target_monitor = nullptr;
  g_run.last_dlg_w = 0;
  g_run.last_dlg_h = 0;
  g_run.window = nullptr;
  g_run.entry = nullptr;
  g_run.desc_label = nullptr;
  g_run.history_cursor = -1;
}

gboolean idle_toggle_run_dialog(gpointer ud G_GNUC_UNUSED) {
  run_dialog_toggle();
  return G_SOURCE_REMOVE;
}

}  // namespace

bool split_run_command(const std::string& raw, RunCommandParts* out) {
  if (out == nullptr)
    return false;
  out->executable.clear();
  out->arguments.clear();

  const std::string trimmed = trim_copy(raw);
  if (trimmed.empty())
    return false;

  if (trimmed[0] == '"') {
    const size_t close = trimmed.find('"', 1);
    if (close == std::string::npos) {
      out->executable = trimmed.substr(1);
      return !out->executable.empty();
    }
    out->executable = trimmed.substr(1, close - 1);
    if (close + 1 < trimmed.size())
      out->arguments = trim_copy(trimmed.substr(close + 1));
    return !out->executable.empty();
  }

  const size_t sp = trimmed.find_first_of(" \t");
  if (sp == std::string::npos) {
    out->executable = trimmed;
    return true;
  }
  out->executable = trimmed.substr(0, sp);
  out->arguments = trim_copy(trimmed.substr(sp + 1));
  return true;
}

RunInternalAction classify_run_internal_command(const std::string& trimmed,
                                                 bool developer_mode) {
  if (!developer_mode)
    return RunInternalAction::None;
  if (trimmed == "lg")
    return RunInternalAction::OpenLookingGlass;
  if (trimmed == "rt")
    return RunInternalAction::ReloadTheme;
  if (trimmed == "r" || trimmed == "restart")
    return RunInternalAction::RestartShell;
  if (trimmed == "debugexit")
    return RunInternalAction::DebugExit;
  return RunInternalAction::None;
}

std::vector<std::string> run_history_add_item(std::vector<std::string> history,
                                              const std::string& cmd,
                                              size_t max_items) {
  const std::string trimmed = trim_copy(cmd);
  if (trimmed.empty())
    return history;

  history.erase(std::remove(history.begin(), history.end(), trimmed),
                history.end());
  history.push_back(trimmed);
  if (max_items > 0 && history.size() > max_items) {
    history.erase(history.begin(),
                  history.begin() +
                      static_cast<ptrdiff_t>(history.size() - max_items));
  }
  return history;
}

std::string run_complete_prefix(const std::string& prefix,
                                const std::vector<std::string>& candidates) {
  if (candidates.empty())
    return {};
  std::string common;
  bool any = false;
  for (const auto& c : candidates) {
    if (c.size() <= prefix.size())
      continue;
    bool match = true;
    for (size_t i = 0; i < prefix.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(c[i])) !=
          std::tolower(static_cast<unsigned char>(prefix[i]))) {
        match = false;
        break;
      }
    }
    if (!match)
      continue;
    if (!any) {
      common = c;
      any = true;
    } else {
      size_t k = prefix.size();
      while (k < common.size() && k < c.size() &&
             std::tolower(static_cast<unsigned char>(common[k])) ==
                 std::tolower(static_cast<unsigned char>(c[k]))) {
        ++k;
      }
      common.resize(k);
    }
  }
  if (!any || common.size() <= prefix.size())
    return {};
  return common.substr(prefix.size());
}

bool execute_run_command_with_monitor(const std::string& raw_cmd,
                                      bool in_terminal,
                                      const RunDialogCallbacks& cbs,
                                      HMONITOR target_mon,
                                      std::string* out_err) {
  if (out_err != nullptr)
    out_err->clear();

  const std::string trimmed = trim_copy(raw_cmd);
  if (trimmed.empty())
    return false;

  const auto action =
      classify_run_internal_command(trimmed, get_config().developer_mode);
  switch (action) {
    case RunInternalAction::OpenLookingGlass:
      if (cbs.open_looking_glass && cbs.open_looking_glass())
        return true;
      if (out_err != nullptr)
        *out_err = "Looking Glass is unavailable";
      return false;
    case RunInternalAction::ReloadTheme:
      if (cbs.reload_theme && cbs.reload_theme())
        return true;
      if (out_err != nullptr)
        *out_err = "Theme reload failed";
      return false;
    case RunInternalAction::RestartShell:
      if (cbs.restart_shell) {
        cbs.restart_shell();
        return true;
      }
      if (out_err != nullptr)
        *out_err = "Shell restart unavailable";
      return false;
    case RunInternalAction::DebugExit:
      if (cbs.quit_shell) {
        cbs.quit_shell();
        return true;
      }
      if (out_err != nullptr)
        *out_err = "Debug exit unavailable";
      return false;
    case RunInternalAction::None:
      break;
  }

  debug_append_log("run", std::string("[run] exec: ") + trimmed);

  if (in_terminal) {
    const std::wstring wargs = L"/k " + utf8_to_wide(trimmed);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    if (target_mon != nullptr) {
      sei.fMask |= SEE_MASK_HMONITOR;
      sei.hMonitor = target_mon;
    }
    sei.lpVerb = L"open";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = wargs.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei))
      return true;
    if (out_err != nullptr)
      *out_err = format_win32_error_utf8(GetLastError());
    return false;
  }

  RunCommandParts parts;
  if (!split_run_command(trimmed, &parts))
    return false;

  const std::wstring wfile = utf8_to_wide(parts.executable);
  const std::wstring wargs = utf8_to_wide(parts.arguments);

  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  if (target_mon != nullptr) {
    sei.fMask |= SEE_MASK_HMONITOR;
    sei.hMonitor = target_mon;
  }
  sei.lpVerb = L"open";
  sei.lpFile = wfile.c_str();
  sei.lpParameters = wargs.empty() ? nullptr : wargs.c_str();
  sei.nShow = SW_SHOWNORMAL;
  if (ShellExecuteExW(&sei))
    return true;

  const DWORD err = GetLastError();
  // Fall back to full-line path check (handles unquoted path with spaces).
  if (!parts.arguments.empty()) {
    const std::wstring wfull = utf8_to_wide(trimmed);
    if (GetFileAttributesW(wfull.c_str()) != INVALID_FILE_ATTRIBUTES) {
      SHELLEXECUTEINFOW sei2{};
      sei2.cbSize = sizeof(sei2);
      sei2.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
      if (target_mon != nullptr) {
        sei2.fMask |= SEE_MASK_HMONITOR;
        sei2.hMonitor = target_mon;
      }
      sei2.lpVerb = L"open";
      sei2.lpFile = wfull.c_str();
      sei2.nShow = SW_SHOWNORMAL;
      if (ShellExecuteExW(&sei2))
        return true;
    }
  }

  if (out_err != nullptr)
    *out_err = format_win32_error_utf8(err);
  return false;
}

bool execute_run_command_string(const std::string& raw_cmd,
                                bool in_terminal,
                                const RunDialogCallbacks& cbs,
                                std::string* out_err) {
  HMONITOR target_mon = monitor_manager_monitor_for_widget(g_run.window);
  watch_launched_window_for_monitor(target_mon);
  return execute_run_command_with_monitor(raw_cmd, in_terminal, cbs, target_mon,
                                          out_err);
}

void run_dialog_init(const RunDialogCallbacks& cbs) {
  g_run.cbs = cbs;
}

void run_dialog_open() {
  if (g_run.cbs.close_popovers != nullptr)
    g_run.cbs.close_popovers();

  if (g_run.window != nullptr) {
    gtk_window_present(GTK_WINDOW(g_run.window));
    keep_run_dialog_visible_on_active_vd(-1, true);
    if (g_run.entry != nullptr) {
      gtk_editable_set_text(GTK_EDITABLE(g_run.entry), "");
      gtk_widget_grab_focus(g_run.entry);
    }
    set_description_status(default_hint_text(), false);
    return;
  }

  ensure_run_css();
  load_history_once();
  g_run.history_cursor = -1;

  GtkWidget* dlg = gtk_window_new();
  g_run.window = dlg;
  gtk_widget_set_name(dlg, "winomeRunDialogWindow");
  gtk_window_set_title(GTK_WINDOW(dlg), tr("Run a Command", "运行命令"));
  gtk_window_set_decorated(GTK_WINDOW(dlg), FALSE);
  gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
  install_bundled_fonts(dlg);

  GtkWidget* wrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_top(wrap, 28);
  gtk_widget_set_margin_bottom(wrap, 28);
  gtk_widget_set_margin_start(wrap, 28);
  gtk_widget_set_margin_end(wrap, 28);

  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(box, "modal-dialog");
  gtk_widget_add_css_class(box, "run-dialog");

  GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(content, "modal-dialog-content-box");

  GtkWidget* msg_content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_add_css_class(msg_content, "message-dialog-content");

  GtkWidget* title_lbl =
      gtk_label_new(tr("Run a Command", "运行命令"));
  gtk_widget_add_css_class(title_lbl, "message-dialog-title");
  gtk_widget_set_halign(title_lbl, GTK_ALIGN_CENTER);
  gtk_label_set_justify(GTK_LABEL(title_lbl), GTK_JUSTIFY_CENTER);
  gtk_label_set_xalign(GTK_LABEL(title_lbl), 0.5f);
  gtk_box_append(GTK_BOX(msg_content), title_lbl);

  GtkWidget* entry = gtk_entry_new();
  g_run.entry = entry;
  gtk_widget_add_css_class(entry, "run-dialog-entry");
  gtk_widget_set_hexpand(entry, TRUE);
  g_signal_connect(entry, "changed", G_CALLBACK(on_entry_changed), nullptr);
  g_signal_connect(entry, "activate", G_CALLBACK(on_entry_activate), nullptr);
  gtk_box_append(GTK_BOX(msg_content), entry);

  GtkWidget* desc = gtk_label_new(default_hint_text().c_str());
  g_run.desc_label = desc;
  gtk_widget_add_css_class(desc, "run-dialog-description");
  gtk_label_set_xalign(GTK_LABEL(desc), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(desc), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(desc), PANGO_WRAP_WORD_CHAR);
  gtk_box_append(GTK_BOX(msg_content), desc);

  gtk_box_append(GTK_BOX(content), msg_content);
  gtk_box_append(GTK_BOX(box), content);
  gtk_box_append(GTK_BOX(wrap), box);

  GtkEventController* key_ctrl = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(key_ctrl, GTK_PHASE_CAPTURE);
  g_signal_connect(key_ctrl, "key-pressed",
                   G_CALLBACK(on_run_key_pressed), nullptr);
  gtk_widget_add_controller(dlg, key_ctrl);
  g_signal_connect(dlg, "map", G_CALLBACK(on_run_map), nullptr);
  g_signal_connect(dlg, "destroy", G_CALLBACK(on_run_destroy), nullptr);

  gtk_window_set_child(GTK_WINDOW(dlg), wrap);

  int min_w = 0, nat_w = 420;
  int min_h = 0, nat_h = 160;
  gtk_widget_measure(wrap, GTK_ORIENTATION_HORIZONTAL, -1,
                     &min_w, &nat_w, nullptr, nullptr);
  const int dlg_w = std::max(nat_w, min_w);
  gtk_widget_measure(wrap, GTK_ORIENTATION_VERTICAL, dlg_w,
                     &min_h, &nat_h, nullptr, nullptr);
  const int dlg_h = std::max(nat_h, min_h);
  gtk_window_set_default_size(GTK_WINDOW(dlg), dlg_w, dlg_h);

  gtk_widget_realize(dlg);
  position_run_dialog(dlg, dlg_w, dlg_h);
  if (!g_run.vd_listener_added) {
    vd::add_change_listener(on_run_dialog_vd_changed, nullptr);
    g_run.vd_listener_added = true;
  }
  gtk_widget_set_visible(dlg, TRUE);
  gtk_window_present(GTK_WINDOW(dlg));
  gtk_widget_grab_focus(entry);
}

void run_dialog_close() {
  if (g_run.window != nullptr)
    gtk_window_destroy(GTK_WINDOW(g_run.window));
}

void run_dialog_toggle() {
  if (g_run.window != nullptr)
    run_dialog_close();
  else
    run_dialog_open();
}

void run_dialog_toggle_async() {
  g_main_context_invoke(nullptr, idle_toggle_run_dialog, nullptr);
}

}  // namespace winome
