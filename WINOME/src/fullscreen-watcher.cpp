// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Auto-hide the top panel while a genuine fullscreen window or the lock
// screen is active. GNOME keeps the panel over maximized windows, so
// maximized windows (IsZoomed) never hide it; a window is considered
// fullscreen only when it covers its entire monitor.

#include "fullscreen-watcher.h"

#include <windows.h>
#include <glib.h>

#include <string>

namespace winome {

namespace {

constexpr wchar_t kOverviewClass[] = L"ActivitiesOverviewWindowClassFull";

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
  std::wstring base = (sep != std::wstring::npos) ? full.substr(sep + 1)
                                                  : full;
  return base == name;
}

// True when @hwnd covers its entire monitor (within a small tolerance),
// i.e. it is a genuine fullscreen window.
bool covers_monitor(HWND hwnd) {
  RECT r;
  if (!GetWindowRect(hwnd, &r))
    return false;

  HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {sizeof(mi)};
  if (!GetMonitorInfo(mon, &mi))
    return false;

  constexpr int kTolerance = 6;
  return r.left <= mi.rcMonitor.left + kTolerance &&
         r.top <= mi.rcMonitor.top + kTolerance &&
         r.right >= mi.rcMonitor.right - kTolerance &&
         r.bottom >= mi.rcMonitor.bottom - kTolerance;
}

// The Explorer desktop surfaces cover the whole monitor but are not
// fullscreen apps: clicking the desktop foregrounds a WorkerW/Progman window
// and must not hide the panel.
bool is_desktop_window(HWND hwnd) {
  wchar_t cls[256] = {0};
  if (GetClassNameW(hwnd, cls, 256) == 0)
    return false;
  return wcscmp(cls, L"Progman") == 0 ||
         wcscmp(cls, L"WorkerW") == 0 ||
         wcscmp(cls, L"SHELLDLL_DefView") == 0 ||
         wcscmp(cls, L"SysListView32") == 0;
}

// Should the panel be hidden while @fg is the foreground window?
bool should_hide_panel(HWND fg) {
  if (fg == nullptr)
    return false;

  // Our own windows (the panel itself, popovers) never hide the panel.
  if (belongs_to_process(fg, L"winome.exe"))
    return false;

  // The desktop is not a fullscreen app.
  if (is_desktop_window(fg))
    return false;

  // The overview and the Alt-Tab/Task View switcher are fullscreen overlays,
  // but the panel must stay visible above them.
  wchar_t cls[256] = {0};
  if (GetClassNameW(fg, cls, 256) > 0 &&
      (wcscmp(cls, kOverviewClass) == 0 ||
       wcscmp(cls, L"XamlExplorerHostIslandWindow") == 0))
    return false;

  // Lock screen: hide the panel so it does not float over it.
  if (belongs_to_process(fg, L"LogonUI.exe"))
    return true;

  // Maximized windows are not fullscreen (GNOME keeps the panel visible).
  if (IsZoomed(fg))
    return false;

  return covers_monitor(fg);
}

gboolean fullscreen_tick(gpointer user_data) {
  HWND panel = static_cast<HWND>(user_data);
  static bool hidden = false;

  bool hide = should_hide_panel(GetForegroundWindow());
  if (hide != hidden) {
    hidden = hide;
    ShowWindow(panel, hide ? SW_HIDE : SW_SHOWNOACTIVATE);
  }
  return G_SOURCE_CONTINUE;
}

}  // namespace

void start_fullscreen_watcher(HWND panel_hwnd) {
  // Re-evaluate immediately so a fullscreen app already focused hides the
  // panel without waiting for the first timeout.
  fullscreen_tick(static_cast<gpointer>(panel_hwnd));
  g_timeout_add(400, fullscreen_tick, static_cast<gpointer>(panel_hwnd));
}

}  // namespace winome
