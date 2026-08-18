// SPDX-License-Identifier: GPL-2.0-only
// Origin: WinOverview <https://github.com/valinet/WinOverview> (GPL-2.0-or-later)
// Modified for WINOME, 2026-08-16. Ported daemon trigger into the host.
//
// The overview renders inside RuntimeBroker.exe via an injected DLL. The
// daemon (here: the host) listens for the Win key and starts
// WinOverviewLauncher.exe inside explorer.exe via WinExec, so the launcher
// runs with ordinary (non-elevated) privileges.
//
// Hotkey handling:
//   - Win (pressed and released alone)      -> toggle the Activities overview
//   - Win+Tab                               -> open the Start menu
//   - Win+<anything else> (E, R, D, ...)    -> forwarded unchanged to Windows
//   - Esc                                   -> close the overview
// The Win keydown is swallowed so the system does not open the Start menu;
// when another key follows, the Win keydown is re-injected (tagged) so the
// combination still reaches the system, and a tagged Win keyup releases it.

#include "overview-trigger.h"

#include <windows.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <glib.h>

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

// Marks synthetic Win keypresses this hook injects (via dwExtraInfo) so they
// are passed through to the system instead of being re-processed by the hook.
constexpr ULONG_PTR kSyntheticWinExtra = 0x574E4F31;  // "WNO1"

BOOL g_running = FALSE;
BOOL g_win_down = FALSE;
BOOL g_win_alone = FALSE;
BOOL g_win_injected = FALSE;
HHOOK g_keyboard_hook = nullptr;
HHOOK g_mouse_hook = nullptr;

// State-change callback, marshaled to the main thread.
OverviewStateCallback g_state_cb = nullptr;
void *g_state_cb_data = nullptr;

struct OverviewStateData {
  gboolean active;
};

static gboolean
emit_overview_state (gpointer data)
{
  OverviewStateData *d = static_cast<OverviewStateData *>(data);
  if (g_state_cb != nullptr)
    g_state_cb (d->active != FALSE, g_state_cb_data);
  g_free (d);
  return G_SOURCE_REMOVE;
}

// Notify the state-change callback on the main thread. start_overview /
// close_overview can be called from the hook thread (Win key) or the main
// thread (Activities button), so always marshal through the default context.
static void
notify_overview_state (bool active)
{
  OverviewStateData *d = g_new (OverviewStateData, 1);
  d->active = active;
  g_main_context_invoke (nullptr, emit_overview_state, d);
}

// Inject a Win keypress (keydown or keyup) into the input stream, tagged so
// LowLevelKeyboardProc ignores it. Used to (a) re-inject the consumed Win
// keydown so Win+<key> combinations still reach the system, and (b) open the
// Start menu with a bare Win press.
void inject_win_key(bool key_up) {
  INPUT input = {};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = VK_LWIN;
  input.ki.dwExtraInfo = kSyntheticWinExtra;
  input.ki.dwFlags = key_up ? KEYEVENTF_KEYUP : 0;
  SendInput(1, &input, sizeof(INPUT));
}

void open_start_menu() {
  inject_win_key(false);
  inject_win_key(true);
}

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
  notify_overview_state(TRUE);
}

void close_overview() {
  send_message_to_overview(WM_CLOSE, 0, 0);
  if (g_mouse_hook != nullptr) {
    UnhookWindowsHookEx(g_mouse_hook);
    g_mouse_hook = nullptr;
  }
  g_running = FALSE;
  notify_overview_state(FALSE);
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

// True when the given screen point is over one of our own windows (the panel
// and its popovers). Clicks there are handled by the shell itself and must not
// be forwarded to the overview as blank-space clicks.
static bool
point_over_winome_window (const POINT &pt)
{
  HWND hwnd = WindowFromPoint (pt);
  if (hwnd == nullptr)
    return false;

  DWORD pid = 0;
  GetWindowThreadProcessId (hwnd, &pid);
  if (pid == 0)
    return false;

  HANDLE process = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (process == nullptr)
    return false;

  wchar_t path[MAX_PATH] = {0};
  DWORD len = MAX_PATH;
  bool ok = QueryFullProcessImageNameW (process, 0, path, &len) != 0;
  CloseHandle (process);
  if (!ok)
    return false;

  std::wstring full (path);
  size_t sep = full.find_last_of (L'\\');
  return (sep != std::wstring::npos ? full.substr (sep + 1) : full) ==
         L"winome.exe";
}

}  // namespace

// Low-level mouse hook: forward mouse-up events to the overview window so it
// can decide whether the click was on blank space (close) or a thumbnail.
// Clicks over our own windows (the panel / popovers) are never forwarded: they
// are interactive over the overview and must not be treated as blank space.
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (g_running) {
    if (wParam == WM_LBUTTONUP || wParam == WM_RBUTTONUP) {
      MSLLHOOKSTRUCT* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
      if (!point_over_winome_window(info->pt))
        send_message_to_overview(kWMAskMouse, TRUE,
                                 MAKELPARAM(info->pt.x, info->pt.y));
    }
  }
  return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode == HC_ACTION) {
    KBDLLHOOKSTRUCT* state = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

    // Synthetic Win presses injected below: pass them through untouched so the
    // system sees the Start menu / Win+<key> we deliberately re-created.
    if (state->dwExtraInfo == kSyntheticWinExtra)
      return CallNextHookEx(nullptr, nCode, wParam, lParam);

    const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const bool up = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

    if (state->vkCode == VK_LWIN || state->vkCode == VK_RWIN) {
      if (down) {
        // Swallow the Win keydown so Windows does not open the Start menu.
        // If another key follows, the Win keydown is re-injected for Win+<key>.
        // Only the first keydown initializes the state: holding Win generates
        // auto-repeat keydowns that must not reset it (otherwise a Win+<key>
        // combo pressed with the key held would be mistaken for a bare Win).
        if (!g_win_down) {
          g_win_down = TRUE;
          g_win_alone = TRUE;
          g_win_injected = FALSE;
        }
        return 1;
      }
      if (up) {
        g_win_down = FALSE;
        const bool bare = g_win_alone;
        g_win_alone = FALSE;
        if (g_win_injected) {
          // Release the synthetic Win keydown that enabled the Win+<key> combo.
          inject_win_key(true);
          g_win_injected = FALSE;
        }
        if (bare) {
          // A bare Win press opens/closes the Activities overview.
          if (!g_running)
            start_overview();
          else
            close_overview();
        }
        return 1;
      }
    }

    if (down && g_win_down) {
      // A key pressed while Win is held: this is a combo, not a bare Win.
      g_win_alone = FALSE;
      // Only a plain Win+Tab (no Alt) is remapped. With Alt held, Tab arrives
      // as WM_SYSKEYDOWN (Win+Alt+Tab = Task View); that and every other
      // unrelated hotkey fall through to CallNextHookEx unchanged below.
      if (state->vkCode == VK_TAB && wParam == WM_KEYDOWN) {
        // Win+Tab normally opens Task View; open the Start menu instead.
        open_start_menu();
        return 1;
      }
      if (!g_win_injected) {
        // Re-inject the swallowed Win keydown so the system processes Win+key
        // (Win+E, Win+R, ...) as usual.
        inject_win_key(false);
        g_win_injected = TRUE;
      }
    } else if (up && state->vkCode == VK_ESCAPE && g_running) {
      close_overview();
    }
  }
  // Everything else (Alt+Tab, Ctrl+Esc, print-screen, ...) is passed through
  // untouched: the hook only ever consumes Win, Win+Tab, and Esc-while-open.
  return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void start_overview_trigger() {
  std::thread(trigger_thread_main).detach();
}

void open_overview() {
  if (!g_running)
    start_overview();
}

void toggle_overview() {
  if (!g_running)
    start_overview();
  else
    close_overview();
}

bool overview_active() {
  return g_running != FALSE;
}

void set_overview_change_callback(OverviewStateCallback callback,
                                  void *user_data) {
  g_state_cb = callback;
  g_state_cb_data = user_data;
}

}  // namespace winome
