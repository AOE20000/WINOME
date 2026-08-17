// SPDX-License-Identifier: GPL-2.0-only
// Origin: WinOverview <https://github.com/valinet/WinOverview> (GPL-2.0-or-later)
// Modified for WINOME, 2026-08-16. Ported daemon trigger into the host;
// hotkey changed to Win+W.
//
// The overview renders inside RuntimeBroker.exe via an injected DLL. The
// daemon (here: the host) listens for the Win+W hotkey and starts
// WinOverviewLauncher.exe inside explorer.exe via WinExec, so the launcher
// runs with ordinary (non-elevated) privileges.

#include "overview-trigger.h"

#include <windows.h>
#include <tlhelp32.h>
#include <shlwapi.h>

#include <string>
#include <thread>
#include <vector>

namespace winome {

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

namespace {

// Matches WinOverviewLibrary/constants.h
constexpr wchar_t kOverviewClassName[] = L"ActivitiesOverviewWindowClassFull";
constexpr UINT kWMAskMouse = WM_USER + 0x0002;

BOOL g_running = FALSE;
BOOL g_win_down = FALSE;
HHOOK g_keyboard_hook = nullptr;
HHOOK g_mouse_hook = nullptr;

typedef NTSTATUS(WINAPI* NtUserBuildHwndListFn)(
    HDESK in_hDesk, HWND in_hWndNext, BOOL in_EnumChildren,
    BOOL in_RemoveImmersive, DWORD in_ThreadID, UINT in_Max,
    HWND* out_List, UINT* out_Cnt);

NtUserBuildHwndListFn g_build_hwnd_list = nullptr;

void build_hwnd_list(std::vector<HWND>* out) {
  HMODULE win32u = LoadLibraryW(L"win32u.dll");
  if (win32u == nullptr)
    return;
  if (g_build_hwnd_list == nullptr)
    g_build_hwnd_list = reinterpret_cast<NtUserBuildHwndListFn>(
        GetProcAddress(win32u, "NtUserBuildHwndList"));
  if (g_build_hwnd_list == nullptr)
    return;

  UINT max = 512;
  for (;;) {
    std::vector<HWND> list(max);
    UINT count = 0;
    NTSTATUS status = g_build_hwnd_list(nullptr, nullptr, FALSE, FALSE, 0,
                                        max, list.data(), &count);
    if (status == 0) {
      for (UINT i = 0; i < count; ++i)
        if (IsWindow(list[i]))
          out->push_back(list[i]);
      break;
    }
    if (status != 0xC0000023 /* STATUS_BUFFER_TOO_SMALL */ || count <= max)
      break;
    max = count + 16;
  }
}

std::wstring get_class(HWND hwnd) {
  wchar_t buffer[200] = {0};
  GetClassNameW(hwnd, buffer, 200);
  return buffer;
}

void send_message_to_overview(UINT message, WPARAM wparam, LPARAM lparam) {
  std::vector<HWND> windows;
  build_hwnd_list(&windows);
  for (HWND hwnd : windows) {
    if (get_class(hwnd) == kOverviewClassName)
      SendMessageW(hwnd, message, wparam, lparam);
  }
}

// Start WinOverviewLauncher.exe inside explorer.exe so the overview runs at
// ordinary integrity level (RuntimeBroker cannot be launched from an elevated
// process directly).
DWORD WINAPI run(LPVOID) {
  char launcher_path[_MAX_PATH + 16] = {0};

  PROCESSENTRY32 entry{};
  entry.dwSize = sizeof(entry);

  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    g_running = FALSE;
    return 1;
  }

  if (Process32First(snapshot, &entry)) {
    do {
      if (wcscmp(entry.szExeFile, L"explorer.exe") != 0)
        continue;

      HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, entry.th32ProcessID);
      if (process == nullptr)
        continue;

      char module_path[_MAX_PATH];
      GetModuleFileNameA(GetModuleHandle(nullptr), module_path, _MAX_PATH);
      // Host lives in build/src/winome.exe; the launcher + DLL are in
      // build/overview/. Remove the file name, then the src/ directory.
      PathRemoveFileSpecA(module_path);   // .../build/src/winome.exe -> .../build/src
      PathRemoveFileSpecA(module_path);   // .../build/src -> .../build
      launcher_path[0] = '\"';
      strcat_s(launcher_path, module_path);
      strcat_s(launcher_path, "\\overview\\WinOverviewLauncher.exe\"");

      void* remote = VirtualAllocEx(process, nullptr, sizeof(launcher_path),
                                    MEM_COMMIT, PAGE_READWRITE);
      if (remote != nullptr) {
        WriteProcessMemory(process, remote, launcher_path,
                           sizeof(launcher_path), nullptr);

        FARPROC win_exec = GetProcAddress(GetModuleHandleW(L"kernel32"),
                                          "WinExec");
        HANDLE thread = CreateRemoteThread(process, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(win_exec), remote,
            0, nullptr);
        if (thread != nullptr) {
          WaitForSingleObject(thread, INFINITE);
          CloseHandle(thread);
        }
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
      }
      CloseHandle(process);
      break;
    } while (Process32Next(snapshot, &entry));
  }

  CloseHandle(snapshot);

  g_running = FALSE;
  return 0;
}

void start_overview() {
  g_running = TRUE;
  CreateThread(nullptr, 0, run, nullptr, 0, nullptr);
  g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, nullptr, 0);
}

void close_overview() {
  send_message_to_overview(WM_CLOSE, 0, 0);
  if (g_mouse_hook != nullptr) {
    UnhookWindowsHookEx(g_mouse_hook);
    g_mouse_hook = nullptr;
  }
  g_running = FALSE;
}

void trigger_thread_main() {
  g_keyboard_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                      nullptr, 0);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

}  // namespace

// Low-level mouse hook: forward mouse-up events to the overview window so it
// can decide whether the click was on blank space (close) or a thumbnail.
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {  if (g_running) {
    if (wParam == WM_LBUTTONUP || wParam == WM_RBUTTONUP) {
      MSLLHOOKSTRUCT* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
      send_message_to_overview(kWMAskMouse, TRUE,
                               MAKELPARAM(info->pt.x, info->pt.y));
    }
  }
  return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode == HC_ACTION) {
    KBDLLHOOKSTRUCT* state = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

    if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
      if (state->vkCode == VK_LWIN || state->vkCode == VK_RWIN) {
        g_win_down = TRUE;
      } else if (state->vkCode == 'W' && g_win_down) {
        // Win+W toggles the overview.
        if (!g_running)
          start_overview();
        else
          close_overview();
      } else {
        g_win_down = FALSE;
      }
    } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
      if (state->vkCode == VK_LWIN || state->vkCode == VK_RWIN) {
        g_win_down = FALSE;
      } else if (state->vkCode == VK_ESCAPE && g_running) {
        close_overview();
      }
    }
  }
  return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void start_overview_trigger() {
  std::thread(trigger_thread_main).detach();
}

void toggle_overview() {
  if (!g_running)
    start_overview();
  else
    close_overview();
}

}  // namespace winome
