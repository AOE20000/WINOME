// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Auto-hide the top panel while a genuine fullscreen window or the lock
// screen is active. GNOME keeps the panel over maximized windows, so
// maximized windows (IsZoomed) never hide it; a window is considered
// fullscreen only when it covers its entire monitor.

#include "fullscreen-watcher.h"
#include "overview-trigger.h"
#include "overview.h"

#include <windows.h>
#include <glib.h>

#include <string>

namespace winome {

namespace {

// Whether the panel is currently hidden by a fullscreen window / lock
// screen. Read by panel_hidden() so the hot corner can stay inactive while
// the panel is not visible.
bool g_panel_hidden = false;

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

  // The panel always stays visible over the Activities overview (the
  // overview window itself does not take focus, so @fg may still be a
  // fullscreen app).
  if (winome::overview_active())
    return false;

  // Our own windows (the panel itself, popovers, the overview) never hide
  // the panel.
  if (belongs_to_process(fg, L"winome.exe"))
    return false;

  // The desktop is not a fullscreen app.
  if (is_desktop_window(fg))
    return false;

  // The Alt-Tab/Task View switcher is a fullscreen overlay, but the panel
  // must stay visible above it.
  wchar_t cls[256] = {0};
  if (GetClassNameW(fg, cls, 256) > 0 &&
      wcscmp(cls, L"XamlExplorerHostIslandWindow") == 0)
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

  // GDK re-applies its own extended styles on configure events, which would
  // drop WS_EX_TOOLWINDOW (needed to keep the panel out of Alt-Tab and the
  // taskbar). Re-assert it every tick.
  LONG_PTR ex = GetWindowLongPtrW(panel, GWL_EXSTYLE);
  if ((ex & WS_EX_TOOLWINDOW) == 0)
    SetWindowLongPtrW(panel, GWL_EXSTYLE, ex | WS_EX_TOOLWINDOW);

  // Same idea for z-order: GDK/Windows reshuffle the topmost band, which
  // can leave the overview above the panel or a popover buried under the
  // overview. The restack checks the chain first and only re-asserts
  // popover > panel > overview when it actually drifted.
  winome::overview_restack ();

  // Hiding the taskbar makes the shell asynchronously recompute the work area
  // to the full screen, which can revert the panel-strip reservation set at
  // startup. Re-assert it, but only when it currently differs, so stable
  // states do not trigger needless relayouts.
  RECT panel_rect;
  if (GetWindowRect(panel, &panel_rect)) {
    HMONITOR mon = MonitorFromWindow(panel, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    RECT current;
    if (mon != nullptr && GetMonitorInfo(mon, &mi) &&
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &current, 0)) {
      RECT work = mi.rcMonitor;
      work.top += (panel_rect.bottom - panel_rect.top);
      if (current.top != work.top || current.left != work.left ||
          current.right != work.right || current.bottom != work.bottom)
        SystemParametersInfoW(SPI_SETWORKAREA, 0, &work, 0);
    }
  }

  bool hide = should_hide_panel(GetForegroundWindow());
  if (hide != g_panel_hidden) {
    g_panel_hidden = hide;
    ShowWindow(panel, hide ? SW_HIDE : SW_SHOWNOACTIVATE);
  }
  return G_SOURCE_CONTINUE;
}

}  // namespace

bool
panel_hidden (void)
{
  return g_panel_hidden;
}

void start_fullscreen_watcher(HWND panel_hwnd) {
  // Re-evaluate immediately so a fullscreen app already focused hides the
  // panel without waiting for the first timeout.
  fullscreen_tick(static_cast<gpointer>(panel_hwnd));
  g_timeout_add(400, fullscreen_tick, static_cast<gpointer>(panel_hwnd));
}

}  // namespace winome
