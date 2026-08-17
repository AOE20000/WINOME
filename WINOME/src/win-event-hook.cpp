// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// WinEventHook that keeps the native Windows UI hidden when Explorer
// recreates it.

#include "win-event-hook.h"

#include <windows.h>
#include <psapi.h>

#include <string>
#include <thread>

#include "native-taskbar.h"

namespace winome {

namespace {

// Windows class names used by the shell surfaces WINOME suppresses. These
// are documented Win32 constants, not project-specific strings.
constexpr wchar_t kTaskbarClass[] = L"Shell_TrayWnd";
constexpr wchar_t kSecondaryTaskbarClass[] = L"Shell_SecondaryTrayWnd";
constexpr wchar_t kNotificationCenterClass[] = L"Windows.UI.Core.CoreWindow";
constexpr wchar_t kXamlIslandClass[] = L"XamlExplorerHostIslandWindow";
constexpr wchar_t kContentBridgeClass[] =
    L"Windows.UI.Composition.DesktopWindowContentBridge";
constexpr wchar_t kInputSiteClass[] = L"Windows.UI.Input.InputSite.WindowClass";
constexpr wchar_t kShellExperienceHost[] = L"ShellExperienceHost.exe";
constexpr int kClassBuffer = 256;
constexpr int kPathBuffer = MAX_PATH;

std::wstring window_class(HWND hwnd) {
  wchar_t buffer[kClassBuffer] = {0};
  int len = GetClassNameW(hwnd, buffer, kClassBuffer);
  return std::wstring(buffer, static_cast<size_t>(len > 0 ? len : 0));
}

std::wstring window_title(HWND hwnd) {
  wchar_t buffer[kClassBuffer] = {0};
  int len = GetWindowTextW(hwnd, buffer, kClassBuffer);
  return std::wstring(buffer, static_cast<size_t>(len > 0 ? len : 0));
}

// Returns true if the executable image backing @hwnd is named @name.
bool belongs_to_process(HWND hwnd, const wchar_t* name) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == 0)
    return false;

  HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (process == nullptr)
    return false;

  wchar_t path[kPathBuffer] = {0};
  DWORD path_len = kPathBuffer;
  bool ok = QueryFullProcessImageNameW(process, 0, path, &path_len) != 0;
  CloseHandle(process);
  if (!ok)
    return false;

  std::wstring full(path);
  size_t sep = full.find_last_of(L'\\');
  std::wstring base = (sep != std::wstring::npos)
                          ? full.substr(sep + 1)
                          : full;
  return base == name;
}

// True when @hwnd covers its entire monitor: the Alt-Tab/Task View switcher
// is a fullscreen XAML island that must NOT be suppressed like the small
// Explorer flyouts.
bool covers_its_monitor(HWND hwnd) {
  RECT r;
  if (!GetWindowRect(hwnd, &r))
    return false;

  HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {sizeof(mi)};
  if (!GetMonitorInfo(mon, &mi))
    return false;

  constexpr int kTolerance = 8;
  return r.left <= mi.rcMonitor.left + kTolerance &&
         r.top <= mi.rcMonitor.top + kTolerance &&
         r.right >= mi.rcMonitor.right - kTolerance &&
         r.bottom >= mi.rcMonitor.bottom - kTolerance;
}

// Hides @hwnd and, if it is an XAML island, its input/content bridge
// children as well, so Explorer cannot show a partially-recreated flyout.
void suppress_window(HWND hwnd) {
  wchar_t cls[kClassBuffer] = {0};
  if (GetClassNameW(hwnd, cls, kClassBuffer) == 0)
    return;

  if (wcscmp(cls, kXamlIslandClass) != 0 ||
      !window_title(hwnd).empty()) {
    ShowWindowAsync(hwnd, SW_HIDE);
    return;
  }

  HWND content = FindWindowExW(hwnd, nullptr, kContentBridgeClass, nullptr);
  while (content != nullptr) {
    HWND input = FindWindowExW(content, nullptr, kInputSiteClass, nullptr);
    if (input != nullptr)
      ShowWindowAsync(input, SW_HIDE);
    ShowWindowAsync(content, SW_HIDE);
    content = FindWindowExW(hwnd, content, kContentBridgeClass, nullptr);
  }
  ShowWindowAsync(hwnd, SW_HIDE);
}

void on_window_event(DWORD event, HWND hwnd) {
  switch (event) {
    case EVENT_OBJECT_UNCLOAKED: {
      // Notification center / notification previews reappear uncloaked.
      std::wstring cls = window_class(hwnd);
      if (cls == kNotificationCenterClass &&
          belongs_to_process(hwnd, kShellExperienceHost)) {
        ShowWindowAsync(hwnd, SW_HIDE);
      }
      break;
    }
    case EVENT_OBJECT_SHOW:
    case EVENT_OBJECT_CREATE: {
      std::wstring cls = window_class(hwnd);
      HWND parent = GetParent(hwnd);
      std::wstring parent_class =
          parent != nullptr ? window_class(parent) : std::wstring();

      bool is_taskbar = cls == kTaskbarClass ||
                        cls == kSecondaryTaskbarClass ||
                        parent_class == kTaskbarClass ||
                        parent_class == kSecondaryTaskbarClass;
      if (is_taskbar) {
        NativeTaskbar::hide();
        break;
      }

      // Suppress XAML islands (flyouts, notification center) EXCEPT the
      // fullscreen Alt-Tab/Task View switcher, which must keep working.
      if (cls == kXamlIslandClass && !covers_its_monitor(hwnd))
        suppress_window(hwnd);
      break;
    }
    default:
      break;
  }
}

void CALLBACK win_event_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                             LONG id_object, LONG id_child, DWORD thread,
                             DWORD time) {
  (void)hook;
  (void)id_child;
  (void)thread;
  (void)time;

  // Only react to whole-window events; id_child/thread-level events are
  // irrelevant to the shell surfaces we hide.
  if (id_object != OBJID_WINDOW || hwnd == nullptr)
    return;

  on_window_event(event, hwnd);
}

void hook_thread_main() {
  // Register each event class of interest separately rather than hooking the
  // entire EVENT_MIN..EVENT_MAX range: we only need creation/show/uncloak
  // notifications for top-level shell windows.
  HWINEVENTHOOK hooks[] = {
      SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE, nullptr,
                      win_event_proc, 0, 0,
                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS),
      SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr,
                      win_event_proc, 0, 0,
                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS),
      SetWinEventHook(EVENT_OBJECT_UNCLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr,
                      win_event_proc, 0, 0,
                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS),
  };

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  for (HWINEVENTHOOK hook : hooks) {
    if (hook != nullptr)
      UnhookWinEvent(hook);
  }
}

}  // namespace

void start_win_event_hook() {
  std::thread(hook_thread_main).detach();
}

}  // namespace winome
