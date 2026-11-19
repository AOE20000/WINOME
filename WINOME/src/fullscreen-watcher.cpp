// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Event-driven per-monitor fullscreen detection & lock-screen panel hiding.

#include "fullscreen-watcher.h"
#include "host-utils.h"
#include "hot-corner.h"
#include "overview-trigger.h"
#include "overview.h"

#include <windows.h>
#include <dwmapi.h>
#include <glib.h>

#include <algorithm>
#include <string>
#include <vector>

namespace winome {
namespace {

struct PanelTrackEntry {
  HWND hwnd = nullptr;
  bool is_primary = false;
  bool hidden = false;
};

std::vector<PanelTrackEntry> g_panels;
bool g_session_locked = false;
guint g_tick_source = 0;
HWINEVENTHOOK g_fg_hook = nullptr;
HWINEVENTHOOK g_loc_hook = nullptr;
HWINEVENTHOOK g_state_hook = nullptr;

bool belongs_to_process(HWND hwnd, const wchar_t* name) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == 0)
    return false;

  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (process == nullptr)
    return false;

  wchar_t path[MAX_PATH] = {0};
  DWORD len = MAX_PATH;
  bool ok = QueryFullProcessImageNameW(process, 0, path, &len) != 0;
  CloseHandle(process);
  if (!ok)
    return false;

  std::wstring full(path);
  size_t sep = full.find_last_of(L'\\');
  std::wstring base = (sep != std::wstring::npos) ? full.substr(sep + 1) : full;
  return _wcsicmp(base.c_str(), name) == 0;
}

bool covers_monitor(HWND hwnd, HMONITOR* out_mon) {
  RECT r{};
  if (!GetWindowRect(hwnd, &r))
    return false;

  HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(mon, &mi))
    return false;

  constexpr int kTolerance = 6;
  const bool full = r.left <= mi.rcMonitor.left + kTolerance &&
                    r.top <= mi.rcMonitor.top + kTolerance &&
                    r.right >= mi.rcMonitor.right - kTolerance &&
                    r.bottom >= mi.rcMonitor.bottom - kTolerance;
  if (full && out_mon != nullptr)
    *out_mon = mon;
  return full;
}

bool is_desktop_window(HWND hwnd) {
  wchar_t cls[256] = {0};
  if (GetClassNameW(hwnd, cls, 256) == 0)
    return false;
  return wcscmp(cls, L"Progman") == 0 ||
         wcscmp(cls, L"WorkerW") == 0 ||
         wcscmp(cls, L"SHELLDLL_DefView") == 0 ||
         wcscmp(cls, L"SysListView32") == 0 ||
         wcscmp(cls, L"Shell_TrayWnd") == 0 ||
         wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
         wcscmp(cls, L"XamlExplorerHostIslandWindow") == 0 ||
         wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0 ||
         wcscmp(cls, L"EdgeUiInputTopWndClass") == 0 ||
         wcscmp(cls, L"EdgeUiInputWndClass") == 0 ||
         wcscmp(cls, L"DummyDWMListenerWindow") == 0 ||
         wcscmp(cls, L"ForegroundStaging") == 0;
}

// Compute which monitor (if any) is currently covered by a fullscreen foreground
// window. When @all_locked is true every panel must hide regardless of monitor.
void detect_fullscreen_state(bool* all_locked, HMONITOR* fullscreen_mon) {
  *all_locked = g_session_locked;
  *fullscreen_mon = nullptr;

  if (g_session_locked)
    return;

  // Overview always keeps top panels visible.
  if (winome::overview_active())
    return;

  HWND fg = GetForegroundWindow();
  if (fg == nullptr)
    return;

  DWORD fg_pid = 0;
  GetWindowThreadProcessId(fg, &fg_pid);
  if (fg_pid == GetCurrentProcessId())
    return;

  if (belongs_to_process(fg, L"LogonUI.exe")) {
    *all_locked = true;
    return;
  }

  if (!IsWindowVisible(fg) || IsIconic(fg))
    return;

  BOOL cloaked = FALSE;
  if (SUCCEEDED(DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloaked,
                                      sizeof(cloaked))) &&
      cloaked != FALSE) {
    return;
  }

  if (is_desktop_window(fg))
    return;

  if (belongs_to_process(fg, L"winome.exe"))
    return;

  LONG_PTR style = GetWindowLongPtrW(fg, GWL_STYLE);
  LONG_PTR ex_style = GetWindowLongPtrW(fg, GWL_EXSTYLE);
  if ((style & WS_CHILD) != 0 ||
      (ex_style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0) {
    return;
  }

  if (winome::is_overview_default_blacklisted_hwnd(fg, true))
    return;

  if (IsZoomed(fg))
    return;

  HMONITOR covered = nullptr;
  if (covers_monitor(fg, &covered))
    *fullscreen_mon = covered;
}

void reassert_primary_workarea_and_styles() {
  for (const auto& entry : g_panels) {
    if (!IsWindow(entry.hwnd))
      continue;

    LONG_PTR ex = GetWindowLongPtrW(entry.hwnd, GWL_EXSTYLE);
    LONG_PTR want = (ex & ~WS_EX_APPWINDOW) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (ex != want)
      SetWindowLongPtrW(entry.hwnd, GWL_EXSTYLE, want);

    if (entry.is_primary) {
      RECT panel_rect{};
      if (GetWindowRect(entry.hwnd, &panel_rect)) {
        HMONITOR mon = MonitorFromWindow(entry.hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        RECT current{};
        if (mon != nullptr && GetMonitorInfoW(mon, &mi) &&
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &current, 0)) {
          RECT work = mi.rcMonitor;
          work.top += (panel_rect.bottom - panel_rect.top);
          if (current.top != work.top || current.left != work.left ||
              current.right != work.right || current.bottom != work.bottom) {
            SystemParametersInfoW(SPI_SETWORKAREA, 0, &work, 0);
          }
        }
      }
    }
  }
  winome::overview_restack();
}

void CALLBACK fg_win_event_proc(HWINEVENTHOOK hook G_GNUC_UNUSED,
                                  DWORD event,
                                  HWND hwnd,
                                  LONG id_object,
                                  LONG id_child G_GNUC_UNUSED,
                                  DWORD thread G_GNUC_UNUSED,
                                  DWORD time G_GNUC_UNUSED) {
  if (id_object != OBJID_WINDOW)
    return;
  if (event == EVENT_OBJECT_LOCATIONCHANGE && hwnd != GetForegroundWindow())
    return;
  evaluate_fullscreen_state();
}

gboolean maintenance_tick(gpointer G_GNUC_UNUSED user_data) {
  reassert_primary_workarea_and_styles();
  evaluate_fullscreen_state();
  return G_SOURCE_CONTINUE;
}

}  // namespace

void evaluate_fullscreen_state(void) {
  bool all_locked = false;
  HMONITOR fs_mon = nullptr;
  detect_fullscreen_state(&all_locked, &fs_mon);

  bool any_shown = false;
  for (auto& entry : g_panels) {
    if (!IsWindow(entry.hwnd))
      continue;
    HMONITOR pmon = MonitorFromWindow(entry.hwnd, MONITOR_DEFAULTTONEAREST);
    const bool want_hide = all_locked || (fs_mon != nullptr && pmon == fs_mon);
    if (want_hide != entry.hidden) {
      entry.hidden = want_hide;
      ShowWindow(entry.hwnd, want_hide ? SW_HIDE : SW_SHOWNOACTIVATE);
      if (!want_hide)
        any_shown = true;
      if (entry.is_primary)
        hot_corner_set_suppressed(want_hide || g_session_locked);
    }
  }
  if (any_shown)
    winome::overview_restack();
}

void register_panel_window(HWND panel_hwnd) {
  if (panel_hwnd == nullptr)
    return;
  for (const auto& entry : g_panels) {
    if (entry.hwnd == panel_hwnd)
      return;
  }
  g_panels.push_back({panel_hwnd, g_panels.empty(), false});
  evaluate_fullscreen_state();
}

void unregister_panel_window(HWND panel_hwnd) {
  g_panels.erase(
      std::remove_if(g_panels.begin(), g_panels.end(),
                     [&](const PanelTrackEntry& e) {
                       return e.hwnd == panel_hwnd;
                     }),
      g_panels.end());
}

void set_session_locked(bool locked) {
  g_session_locked = locked;
  hot_corner_set_suppressed(locked || panel_hidden());
  evaluate_fullscreen_state();
}

void reassert_panel_workarea_now(void) {
  reassert_primary_workarea_and_styles();
}

bool panel_hidden(void) {
  if (g_session_locked)
    return true;
  for (const auto& entry : g_panels) {
    if (entry.is_primary)
      return entry.hidden;
  }
  return false;
}

bool panel_hidden_for_monitor(HMONITOR mon) {
  if (g_session_locked)
    return true;
  for (const auto& entry : g_panels) {
    if (MonitorFromWindow(entry.hwnd, MONITOR_DEFAULTTONEAREST) == mon)
      return entry.hidden;
  }
  return false;
}

void start_fullscreen_watcher(HWND primary_panel_hwnd) {
  g_panels.clear();
  if (primary_panel_hwnd != nullptr)
    g_panels.push_back({primary_panel_hwnd, true, false});

  if (g_fg_hook == nullptr) {
    g_fg_hook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
        fg_win_event_proc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  }
  if (g_loc_hook == nullptr) {
    g_loc_hook = SetWinEventHook(
        EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
        fg_win_event_proc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  }
  if (g_state_hook == nullptr) {
    g_state_hook = SetWinEventHook(
        EVENT_SYSTEM_MINIMIZEEND, EVENT_SYSTEM_MINIMIZEEND, nullptr,
        fg_win_event_proc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  }

  reassert_primary_workarea_and_styles();
  evaluate_fullscreen_state();
  // Low-frequency 10s safety backstop (foreground, location, minimize, lock,
  // and SPI_SETWORKAREA events drive immediate transitions with zero 600ms churn).
  if (g_tick_source == 0)
    g_tick_source = g_timeout_add(10000, maintenance_tick, nullptr);
}

void stop_fullscreen_watcher(void) {
  if (g_tick_source != 0) {
    g_source_remove(g_tick_source);
    g_tick_source = 0;
  }
  if (g_fg_hook != nullptr) {
    UnhookWinEvent(g_fg_hook);
    g_fg_hook = nullptr;
  }
  if (g_loc_hook != nullptr) {
    UnhookWinEvent(g_loc_hook);
    g_loc_hook = nullptr;
  }
  if (g_state_hook != nullptr) {
    UnhookWinEvent(g_state_hook);
    g_state_hook = nullptr;
  }
  g_panels.clear();
  g_session_locked = false;
}

}  // namespace winome
