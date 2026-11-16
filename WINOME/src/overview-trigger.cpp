// SPDX-License-Identifier: GPL-2.0-only
// Origin: WinOverview <https://github.com/valinet/WinOverview> (GPL-2.0-or-later)
// Modified for WINOME, 2026-08-16. Ported daemon trigger into the host.
// 2026-08-18: the overview is rendered in-host (overview.cpp); the launcher
// injection, cross-process messaging and the close-notification event are
// gone — opening/closing is now a direct in-process call.
//
// Hotkey handling:
//   - Win (pressed and released alone)      -> toggle the Activities overview
//   - Double Win (quick second press)       -> transition smoothly into App Grid
//   - Win+Tab                               -> open the Start menu
//   - Win+PageUp / Win+PageDown             -> switch virtual workspace prev/next
//   - Win+Shift+PageUp / Win+Shift+PageDown -> move active window to workspace
//   - Win+Ctrl+Left/Right/D/F4              -> forwarded + triggers VD sync
//   - Win+<anything else> (E, R, ...)       -> forwarded unchanged to Windows
//   - Esc                                   -> close the overview
// The Win keydown is swallowed so the system does not open the Start menu;
// when another key follows, the Win keydown is re-injected (tagged) so the
// combination still reaches the system, and a tagged Win keyup releases it.

#include "overview-trigger.h"
#include "config.h"
#include "debug-runtime.h"
#include "overview.h"
#include "run-dialog.h"
#include "virtual-desktop.h"

#include <windows.h>
#include <glib.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cwchar>
#include <mutex>
#include <queue>
#include <thread>

namespace winome {

void enqueue_desktop_hotkey(int delta, bool move_fg);

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

namespace {

// Marks synthetic Win keypresses this hook injects (via dwExtraInfo) so they
// are passed through to the system instead of being re-processed by the hook.
constexpr ULONG_PTR kSyntheticWinExtra = 0x574E4F31;  // "WNO1"

std::atomic<bool> g_running{false};
std::atomic<uint64_t> g_state_seq{0};
BOOL g_win_down = FALSE;
BOOL g_win_alone = FALSE;
BOOL g_win_injected = FALSE;
BOOL g_alt_f2_down = FALSE;
HHOOK g_keyboard_hook = nullptr;
std::atomic<gint64> g_last_win_press_us{0};

// State-change callback, marshaled to the main thread.
OverviewStateCallback g_state_cb = nullptr;
void *g_state_cb_data = nullptr;

struct OverviewStateData {
  gboolean active;
  uint64_t seq;
};

static gboolean
emit_overview_state (gpointer data)
{
  OverviewStateData *d = static_cast<OverviewStateData *> (data);
  if (d->seq == g_state_seq.load (std::memory_order_acquire) &&
      g_state_cb != nullptr) {
    g_state_cb (d->active != FALSE, g_state_cb_data);
  }
  g_free (d);
  return G_SOURCE_REMOVE;
}

// Notify the state-change callback on the main thread. start_overview /
// close_overview can be called from the hook thread (Win key) or the main
// thread (Activities button, overview clicks), so always marshal through the
// default context and drop out-of-order stale notifications by sequence id.
static void
notify_overview_state (bool active, uint64_t seq)
{
  OverviewStateData *d = g_new (OverviewStateData, 1);
  d->active = active;
  d->seq = seq;
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

}  // namespace

void open_start_menu() {
  inject_win_key(false);
  inject_win_key(true);
}

namespace {

struct HotkeyDesktopTask {
  int delta = 0;
  bool move_fg = false;
};

struct HotkeyWorker {
  std::mutex mu;
  std::condition_variable cv;
  std::queue<HotkeyDesktopTask> queue;
  bool stop = false;
  std::thread thread;
};

HotkeyWorker g_hotkey_worker;

bool is_eligible_window_for_move(HWND hwnd) {
  if (hwnd == nullptr || !IsWindow(hwnd) || !IsWindowVisible(hwnd))
    return false;

  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == 0 || pid == GetCurrentProcessId())
    return false;

  const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  if ((style & WS_CHILD) != 0 || (ex_style & WS_EX_TOOLWINDOW) != 0)
    return false;

  wchar_t cls[128] = {};
  if (GetClassNameW(hwnd, cls, 128) > 0) {
    if (std::wcscmp(cls, L"Shell_TrayWnd") == 0 ||
        std::wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
        std::wcscmp(cls, L"Progman") == 0 ||
        std::wcscmp(cls, L"WorkerW") == 0) {
      return false;
    }
  }
  return true;
}

void hotkey_worker_main() {
  for (;;) {
    HotkeyDesktopTask task;
    {
      std::unique_lock<std::mutex> lk(g_hotkey_worker.mu);
      g_hotkey_worker.cv.wait(lk, [] {
        return g_hotkey_worker.stop || !g_hotkey_worker.queue.empty();
      });
      if (g_hotkey_worker.stop && g_hotkey_worker.queue.empty())
        break;
      task = g_hotkey_worker.queue.front();
      g_hotkey_worker.queue.pop();
    }

    const int count = vd::count();
    const int cur = vd::current_index();
    if (count <= 0 || cur < 0)
      continue;

    const int target = std::clamp(cur + task.delta, 0, count - 1);
    if (target == cur)
      continue;

    if (task.move_fg) {
      HWND fg = GetForegroundWindow();
      if (is_eligible_window_for_move(fg))
        vd::move_window_to_desktop(fg, target);
    }

    if (vd::switch_to(target))
      vd::notify_change_hint_async();
  }
}

void start_hotkey_worker() {
  {
    std::lock_guard<std::mutex> lk(g_hotkey_worker.mu);
    g_hotkey_worker.stop = false;
  }
  if (!g_hotkey_worker.thread.joinable())
    g_hotkey_worker.thread = std::thread(hotkey_worker_main);
}

void stop_hotkey_worker() {
  {
    std::lock_guard<std::mutex> lk(g_hotkey_worker.mu);
    g_hotkey_worker.stop = true;
    while (!g_hotkey_worker.queue.empty())
      g_hotkey_worker.queue.pop();
  }
  g_hotkey_worker.cv.notify_all();
  if (g_hotkey_worker.thread.joinable())
    g_hotkey_worker.thread.join();
}

std::thread g_trigger_thread;
std::atomic<DWORD> g_trigger_tid{0};

void trigger_thread_main() {
  MSG dummy;
  PeekMessageW(&dummy, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
  g_trigger_tid.store(GetCurrentThreadId(), std::memory_order_release);

  g_keyboard_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                      nullptr, 0);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  if (g_keyboard_hook != nullptr) {
    UnhookWindowsHookEx(g_keyboard_hook);
    g_keyboard_hook = nullptr;
  }
  g_trigger_tid.store(0, std::memory_order_release);
}

}  // namespace

void enqueue_desktop_hotkey(int delta, bool move_fg) {
  std::lock_guard<std::mutex> lk(g_hotkey_worker.mu);
  if (g_hotkey_worker.stop || g_hotkey_worker.queue.size() >= 8)
    return;
  g_hotkey_worker.queue.push(HotkeyDesktopTask{delta, move_fg});
  g_hotkey_worker.cv.notify_one();
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
        if (!g_win_down && !get_config().win_key_overview)
          return CallNextHookEx(nullptr, nCode, wParam, lParam);
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
        if (!g_win_down)
          return CallNextHookEx(nullptr, nCode, wParam, lParam);
        g_win_down = FALSE;
        const bool bare = g_win_alone;
        g_win_alone = FALSE;
        if (g_win_injected) {
          // Release the synthetic Win keydown that enabled the Win+<key> combo.
          inject_win_key(true);
          g_win_injected = FALSE;
        }
        if (bare) {
          // A bare Win press opens the Activities overview; a rapid second Win
          // press (within 550ms while opening or in WINDOW_PICKER) interrupts
          // smoothly into the App Grid like GNOME Shell's overlay-key flow.
          gint64 now = g_get_monotonic_time();
          if (!g_running.load(std::memory_order_acquire)) {
            g_last_win_press_us.store(now, std::memory_order_release);
            open_overview("win-key");
          } else {
            gint64 prev =
                g_last_win_press_us.exchange(0, std::memory_order_acq_rel);
            if (prev > 0 && (now - prev) <= 550000 &&
                overview_can_shift_to_app_grid()) {
              g_print("[trig] double win-key -> app-grid\n");
              overview_shift_to_app_grid_async();
            } else {
              close_overview("win-key");
            }
          }
        }
        return 1;
      }
    }

    if (down && g_win_down) {
      // A key pressed while Win is held: this is a combo, not a bare Win.
      g_win_alone = FALSE;
      const bool ctrl_held = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
      const bool shift_held = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
      const bool alt_held =
          (wParam == WM_SYSKEYDOWN) ||
          ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);

      // Only a plain Win+Tab (no Alt) is remapped. With Alt held, Tab arrives
      // as WM_SYSKEYDOWN (Win+Alt+Tab = Task View); that and every other
      // unrelated hotkey fall through to CallNextHookEx unchanged below.
      if (state->vkCode == VK_TAB && wParam == WM_KEYDOWN) {
        // Win+Tab normally opens Task View; open the Start menu instead.
        open_start_menu();
        return 1;
      }

      // GNOME-style workspace switching & active window move:
      //   Win+PageUp / Win+PageDown         -> switch workspace prev/next
      //   Win+Shift+PageUp / Win+Shift+PgDn -> move foreground window & follow
      // Dispatched via HotkeyWorker so WH_KEYBOARD_LL never blocks on Explorer COM.
      if (!ctrl_held && !alt_held &&
          (state->vkCode == VK_PRIOR || state->vkCode == VK_NEXT)) {
        const int delta = (state->vkCode == VK_PRIOR) ? -1 : 1;
        enqueue_desktop_hotkey(delta, shift_held);
        return 1;
      }

      // Trigger immediate observer refresh on native Windows VD hotkeys.
      if (ctrl_held && !alt_held &&
          (state->vkCode == VK_LEFT || state->vkCode == VK_RIGHT ||
           state->vkCode == 'D' || state->vkCode == VK_F4)) {
        vd::notify_change_hint_async();
      }

      if (!g_win_injected) {
        // Re-inject the swallowed Win keydown so the system processes Win+key
        // (Win+E, Win+R, ...) as usual.
        inject_win_key(false);
        g_win_injected = TRUE;
      }
    } else if (state->vkCode == VK_F2) {
      if (up) {
        g_alt_f2_down = FALSE;
      } else if (down && !g_win_down) {
        const bool alt_held =
            (wParam == WM_SYSKEYDOWN) ||
            ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
        const bool ctrl_held = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift_held = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (alt_held && !ctrl_held && !shift_held) {
          if (!g_alt_f2_down) {
            g_alt_f2_down = TRUE;
            if (debug_get_flags().hotkey_trace)
              debug_append_log("trig", "[trig] Alt+F2 -> toggle RunDialog");
            run_dialog_toggle_async();
          }
          return 1;
        }
      }
    } else if (up && state->vkCode == VK_ESCAPE && g_running) {
      close_overview("esc");
    }
  }
  // Everything else (Alt+Tab, Ctrl+Esc, print-screen, ...) is passed through
  // untouched: the hook only ever consumes Win, Win+Tab, Win(+Shift)+PageUp/Dn,
  // Alt+F2, and Esc-while-open.
  return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void start_overview_trigger() {
  start_hotkey_worker();
  if (!g_trigger_thread.joinable())
    g_trigger_thread = std::thread(trigger_thread_main);
}

void stop_overview_trigger() {
  DWORD tid = g_trigger_tid.exchange(0, std::memory_order_acq_rel);
  if (tid != 0)
    PostThreadMessageW(tid, WM_QUIT, 0, 0);
  if (g_trigger_thread.joinable())
    g_trigger_thread.join();
  stop_hotkey_worker();
}

void open_overview(const char *source) {
  bool expected = false;
  if (!g_running.compare_exchange_strong(expected, true,
                                        std::memory_order_acq_rel)) {
    return;
  }
  const uint64_t seq = g_state_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
  g_print ("[trig] open_overview (%s)\n", source != nullptr ? source : "?");
  overview_show_async();
  notify_overview_state(TRUE, seq);
}

void close_overview(const char *source) {
  bool expected = true;
  if (!g_running.compare_exchange_strong(expected, false,
                                        std::memory_order_acq_rel)) {
    return;
  }
  const uint64_t seq = g_state_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
  g_print ("[trig] close_overview (%s)\n", source != nullptr ? source : "?");
  g_last_win_press_us.store(0, std::memory_order_release);
  overview_hide_async();
  notify_overview_state(FALSE, seq);
}

void toggle_overview(const char *source) {
  if (!g_running.load(std::memory_order_acquire))
    open_overview(source);
  else
    close_overview(source);
}

bool overview_active() {
  return g_running.load(std::memory_order_acquire);
}

void set_overview_change_callback(OverviewStateCallback callback,
                                  void *user_data) {
  g_state_cb = callback;
  g_state_cb_data = user_data;
}

}  // namespace winome
