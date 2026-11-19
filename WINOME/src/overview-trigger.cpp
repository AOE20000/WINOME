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
#include "app-grid.h"
#include "config.h"
#include "debug-runtime.h"
#include "overview-dash.h"
#include "overview.h"
#include "run-dialog.h"
#include "shell-panel.h"
#include "virtual-desktop.h"

#include <gtk/gtk.h>
#include <gdk/win32/gdkwin32.h>
#include <windows.h>
#include <glib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cwchar>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace winome {

void enqueue_desktop_hotkey(int delta, bool move_fg);

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

namespace {

// Marks synthetic Win keypresses this hook injects (via dwExtraInfo) so they
// are passed through to the system instead of being re-processed by the hook.
constexpr ULONG_PTR kSyntheticWinExtra = 0x574E4F31;  // "WNO1"
constexpr size_t kMaxCachedPanels = 8;

std::mutex g_state_mu;
std::atomic<bool> g_running{false};
std::atomic<uint64_t> g_state_seq{0};

// Hook-thread-only keyboard tracking state (plus WinEvent desktop-switch reset).
BOOL g_win_down = FALSE;
WORD g_win_vk = VK_LWIN;
BOOL g_win_alone = FALSE;
BOOL g_win_injected = FALSE;
ULONGLONG g_win_inject_tick = 0;
BOOL g_alt_f2_down = FALSE;
BOOL g_esc_swallowed = FALSE;
HHOOK g_keyboard_hook = nullptr;
HWINEVENTHOOK g_desktop_switch_hook = nullptr;
std::atomic<gint64> g_last_win_press_us{0};

// Lock-free cached panel HWNDs populated on the GTK main thread so the hook can
// distinguish panel windows (no Esc handler) from overview/popover/dialogs
// without blocking on cross-thread SendMessage/GetWindowTextW.
std::array<std::atomic<HWND>, kMaxCachedPanels> g_cached_panel_hwnds{};

// State-change callback & serialized main-thread event queue, guarded by g_state_mu.
OverviewStateCallback g_state_cb = nullptr;
void *g_state_cb_data = nullptr;

struct OverviewStateData {
  gboolean active;
  uint64_t seq;
};

std::queue<OverviewStateData> g_pending_events;
bool g_dispatch_scheduled = false;

void
refresh_cached_window_handles ()
{
  if (!gtk_is_initialized ())
    return;

  std::array<HWND, kMaxCachedPanels> panels{};
  size_t n_panels = 0;

  GListModel *toplevels = gtk_window_get_toplevels ();
  if (toplevels != nullptr) {
    const guint count = g_list_model_get_n_items (toplevels);
    for (guint i = 0; i < count; ++i) {
      gpointer obj = g_list_model_get_item (toplevels, i);
      if (obj == nullptr)
        continue;
      if (GTK_IS_WINDOW (obj)) {
        GtkWindow *win = GTK_WINDOW (obj);
        const char *title = gtk_window_get_title (win);
        if (g_strcmp0 (title, "WINOME Shell") == 0 ||
            g_strcmp0 (title, "WINOME Secondary Panel") == 0) {
          GdkSurface *surf = gtk_native_get_surface (GTK_NATIVE (win));
          HWND hwnd = surf != nullptr
              ? static_cast<HWND> (gdk_win32_surface_get_handle (surf))
              : nullptr;
          if (hwnd != nullptr && n_panels < kMaxCachedPanels)
            panels[n_panels++] = hwnd;
        }
      }
      g_object_unref (obj);
    }
  }

  for (size_t i = 0; i < kMaxCachedPanels; ++i)
    g_cached_panel_hwnds[i].store (panels[i], std::memory_order_release);
}

gboolean
refresh_cached_handles_idle (gpointer user_data G_GNUC_UNUSED)
{
  refresh_cached_window_handles ();
  return G_SOURCE_REMOVE;
}

bool
is_cached_panel_hwnd (HWND hwnd)
{
  if (hwnd == nullptr)
    return false;
  for (size_t i = 0; i < kMaxCachedPanels; ++i) {
    HWND p = g_cached_panel_hwnds[i].load (std::memory_order_acquire);
    if (p == hwnd)
      return true;
  }
  return false;
}

bool
should_intercept_overview_esc ()
{
  HWND fg = GetForegroundWindow ();
  if (fg == nullptr)
    return true;
  DWORD fg_pid = 0;
  GetWindowThreadProcessId (fg, &fg_pid);
  if (fg_pid != GetCurrentProcessId ())
    return true;
  // Clicking the panel Activities button leaves keyboard focus on the panel,
  // which has no Esc controller; route Esc to our layered overview handler.
  return is_cached_panel_hwnd (fg);
}

bool
has_visible_popover_child (GtkWidget *parent)
{
  if (parent == nullptr || !gtk_widget_get_visible (parent))
    return false;
  std::vector<GtkWidget *> stack;
  stack.push_back (parent);
  while (!stack.empty ()) {
    GtkWidget *cur = stack.back ();
    stack.pop_back ();
    for (GtkWidget *c = gtk_widget_get_first_child (cur); c != nullptr;
         c = gtk_widget_get_next_sibling (c)) {
      if (!gtk_widget_get_visible (c))
        continue;
      if (GTK_IS_POPOVER (c))
        return true;
      stack.push_back (c);
    }
  }
  return false;
}

gboolean
handle_unfocused_esc_idle (gpointer user_data G_GNUC_UNUSED)
{
  if (!overview_active ())
    return G_SOURCE_REMOVE;

  // 1. Panel popover (Quick Settings / Calendar / Tray) floating over overview.
  if (winome_shell_panel_top_popover_hwnd () != nullptr) {
    winome_shell_panel_close_popovers ();
    return G_SOURCE_REMOVE;
  }

  if (!gtk_is_initialized ()) {
    close_overview ("esc");
    return G_SOURCE_REMOVE;
  }

  // Locate the live WINOME Overview window and its children on the main thread.
  GtkWindow *ov_win = nullptr;
  GListModel *toplevels = gtk_window_get_toplevels ();
  if (toplevels != nullptr) {
    const guint count = g_list_model_get_n_items (toplevels);
    for (guint i = 0; i < count; ++i) {
      gpointer obj = g_list_model_get_item (toplevels, i);
      if (obj == nullptr)
        continue;
      if (GTK_IS_WINDOW (obj) &&
          g_strcmp0 (gtk_window_get_title (GTK_WINDOW (obj)),
                     "WINOME Overview") == 0) {
        ov_win = GTK_WINDOW (obj);
        g_object_unref (obj);
        break;
      }
      g_object_unref (obj);
    }
  }

  if (ov_win == nullptr || !gtk_widget_get_visible (GTK_WIDGET (ov_win))) {
    close_overview ("esc");
    return G_SOURCE_REMOVE;
  }

  GtkWidget *root = gtk_window_get_child (ov_win);
  GtkWidget *search_entry = nullptr;
  GtkWidget *app_grid = nullptr;
  GtkWidget *dash = nullptr;
  if (root != nullptr) {
    for (GtkWidget *c = gtk_widget_get_first_child (root); c != nullptr;
         c = gtk_widget_get_next_sibling (c)) {
      const char *tname = G_OBJECT_TYPE_NAME (c);
      if (GTK_IS_ENTRY (c))
        search_entry = c;
      else if (g_strcmp0 (tname, "WinomeAppGrid") == 0)
        app_grid = c;
      else if (g_strcmp0 (tname, "WinomeDash") == 0)
        dash = c;
    }
  }

  // 2. Context menu popovers owned by Dash or App Grid.
  if (has_visible_popover_child (dash)) {
    overview_dash_close_menu (dash);
    return G_SOURCE_REMOVE;
  }
  if (has_visible_popover_child (app_grid)) {
    app_grid_close_menu (app_grid);
    return G_SOURCE_REMOVE;
  }

  // 3. Clear non-empty search query before leaving App Grid or closing overview.
  if (search_entry != nullptr) {
    const char *txt = gtk_editable_get_text (GTK_EDITABLE (search_entry));
    if (txt != nullptr && txt[0] != '\0') {
      gtk_editable_set_text (GTK_EDITABLE (search_entry), "");
      return G_SOURCE_REMOVE;
    }
  }

  // 4. In APP_GRID state (or transitioning/pending APP_GRID), close open
  //    folder first, otherwise step back to WINDOW_PICKER.
  if (!overview_can_shift_to_app_grid ()) {
    if (app_grid != nullptr && app_grid_close_open_folder (app_grid))
      return G_SOURCE_REMOVE;
    overview_toggle_app_grid ();
    return G_SOURCE_REMOVE;
  }

  // 5. Base WINDOW_PICKER state: close the overview.
  close_overview ("esc");
  return G_SOURCE_REMOVE;
}

static gboolean
dispatch_overview_events_idle (gpointer data G_GNUC_UNUSED)
{
  for (;;) {
    OverviewStateData ev{};
    OverviewStateCallback cb = nullptr;
    void *cb_data = nullptr;
    {
      std::lock_guard<std::mutex> lk (g_state_mu);
      if (g_pending_events.empty ()) {
        g_dispatch_scheduled = false;
        return G_SOURCE_REMOVE;
      }
      ev = g_pending_events.front ();
      g_pending_events.pop ();
      cb = g_state_cb;
      cb_data = g_state_cb_data;
    }

    if (ev.active != FALSE)
      overview_show_async ();
    else
      overview_hide_async ();

    refresh_cached_window_handles ();
    if (cb != nullptr)
      cb (ev.active != FALSE, cb_data);
  }
}

// Schedule asynchronous FIFO dispatch onto the default GMainContext outside
// g_state_mu. Using g_idle_add_full avoids synchronous same-thread re-entry
// (which g_main_context_invoke would perform on the main thread) while
// g_pending_events preserves exact state-transition ordering across threads.
static void
schedule_overview_dispatch ()
{
  g_idle_add_full (G_PRIORITY_DEFAULT, dispatch_overview_events_idle,
                   nullptr, nullptr);
}

// Inject a Win keypress (keydown or keyup) into the input stream, tagged so
// LowLevelKeyboardProc ignores it. Used to (a) re-inject the consumed Win
// keydown so Win+<key> combinations still reach the system, and (b) open the
// Start menu with a bare Win press.
void inject_win_key(bool key_up, WORD vk = VK_LWIN) {
  INPUT input = {};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = vk;
  input.ki.dwExtraInfo = kSyntheticWinExtra;
  input.ki.dwFlags = key_up ? KEYEVENTF_KEYUP : 0;
  SendInput(1, &input, sizeof(INPUT));
}

void reset_win_key_state() {
  if (g_win_injected) {
    inject_win_key(true, g_win_vk);
    g_win_injected = FALSE;
  }
  g_win_down = FALSE;
  g_win_alone = FALSE;
  g_win_inject_tick = 0;
  g_alt_f2_down = FALSE;
  g_esc_swallowed = FALSE;
}

void CALLBACK on_desktop_switch_event(HWINEVENTHOOK hook G_GNUC_UNUSED,
                                      DWORD event,
                                      HWND hwnd G_GNUC_UNUSED,
                                      LONG id_object G_GNUC_UNUSED,
                                      LONG id_child G_GNUC_UNUSED,
                                      DWORD event_thread G_GNUC_UNUSED,
                                      DWORD event_time G_GNUC_UNUSED) {
  if (event == EVENT_SYSTEM_DESKTOPSWITCH)
    reset_win_key_state();
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

std::mutex g_trigger_mu;
std::condition_variable g_trigger_cv;
std::thread g_trigger_thread;
std::atomic<DWORD> g_trigger_tid{0};
bool g_trigger_ready = false;
bool g_trigger_stop_requested = false;

void trigger_thread_main() {
  MSG dummy;
  PeekMessageW(&dummy, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

  {
    std::lock_guard<std::mutex> lock(g_trigger_mu);
    if (g_trigger_stop_requested) {
      g_trigger_tid.store(0, std::memory_order_release);
      g_trigger_ready = true;
      g_trigger_cv.notify_all();
      return;
    }
    g_trigger_tid.store(GetCurrentThreadId(), std::memory_order_release);
    g_trigger_ready = true;
    g_trigger_cv.notify_all();
  }

  g_keyboard_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                      nullptr, 0);
  g_desktop_switch_hook = SetWinEventHook(
      EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH, nullptr,
      on_desktop_switch_event, 0, 0, WINEVENT_OUTOFCONTEXT);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  if (g_desktop_switch_hook != nullptr) {
    UnhookWinEvent(g_desktop_switch_hook);
    g_desktop_switch_hook = nullptr;
  }
  if (g_keyboard_hook != nullptr) {
    UnhookWindowsHookEx(g_keyboard_hook);
    g_keyboard_hook = nullptr;
  }
  reset_win_key_state();
  {
    std::lock_guard<std::mutex> lock(g_trigger_mu);
    g_trigger_tid.store(0, std::memory_order_release);
    g_trigger_ready = true;
    g_trigger_cv.notify_all();
  }
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

    // Fallback recovery: if a Win+key combo already injected Win down and
    // Windows later reports that Win key up (e.g. after secure desktop /
    // lock screen where the Win keyup bypassed this hook), clear stale state
    // before evaluating the incoming non-Win key.
    if (g_win_down && g_win_injected &&
        state->vkCode != VK_LWIN && state->vkCode != VK_RWIN) {
      const ULONGLONG now_tick = GetTickCount64();
      if ((now_tick - g_win_inject_tick) > 30 &&
          (GetAsyncKeyState(g_win_vk) & 0x8000) == 0) {
        g_win_down = FALSE;
        g_win_alone = FALSE;
        g_win_injected = FALSE;
        g_win_inject_tick = 0;
      }
    }

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
          g_win_vk = static_cast<WORD>(state->vkCode);
          g_win_alone = TRUE;
          g_win_injected = FALSE;
          g_win_inject_tick = 0;
        } else if (state->vkCode != g_win_vk) {
          g_win_alone = FALSE;
        }
        return 1;
      }
      if (up) {
        if (!g_win_down)
          return CallNextHookEx(nullptr, nCode, wParam, lParam);
        if (state->vkCode != g_win_vk)
          return 1;
        g_win_down = FALSE;
        const bool bare = g_win_alone;
        g_win_alone = FALSE;
        g_win_inject_tick = 0;
        if (g_win_injected) {
          // Release the synthetic Win keydown that enabled the Win+<key> combo.
          inject_win_key(true, g_win_vk);
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
        inject_win_key(false, g_win_vk);
        g_win_injected = TRUE;
        g_win_inject_tick = GetTickCount64();
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
    } else if (state->vkCode == VK_ESCAPE) {
      if (up) {
        if (g_esc_swallowed) {
          g_esc_swallowed = FALSE;
          return 1;
        }
      } else if (down && !g_win_down) {
        if (!g_running.load(std::memory_order_acquire)) {
          g_esc_swallowed = FALSE;
        } else if (should_intercept_overview_esc()) {
          // Overview is WS_EX_NOACTIVATE (or the top panel holds focus), so
          // GTK never receives VK_ESCAPE directly. Swallow it and run the
          // layered Esc dismissal on the GTK main thread.
          if (!g_esc_swallowed) {
            g_esc_swallowed = TRUE;
            g_main_context_invoke(nullptr, handle_unfocused_esc_idle, nullptr);
          }
          return 1;
        }
      }
    }
  }
  // Everything else (Alt+Tab, Ctrl+Esc, print-screen, ...) is passed through
  // untouched: the hook only ever consumes Win, Win+Tab, Win(+Shift)+PageUp/Dn,
  // Alt+F2, and Esc-while-open.
  return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void start_overview_trigger() {
  start_hotkey_worker();
  g_main_context_invoke(nullptr, refresh_cached_handles_idle, nullptr);
  std::unique_lock<std::mutex> lock(g_trigger_mu);
  if (!g_trigger_thread.joinable()) {
    g_trigger_stop_requested = false;
    g_trigger_ready = false;
    g_trigger_thread = std::thread(trigger_thread_main);
    g_trigger_cv.wait(lock, [] { return g_trigger_ready; });
  }
}

void stop_overview_trigger() {
  std::thread to_join;
  {
    std::unique_lock<std::mutex> lock(g_trigger_mu);
    g_trigger_stop_requested = true;
    if (g_trigger_thread.joinable()) {
      g_trigger_cv.wait(lock, [] { return g_trigger_ready; });
      DWORD tid = g_trigger_tid.exchange(0, std::memory_order_acq_rel);
      if (tid != 0)
        PostThreadMessageW(tid, WM_QUIT, 0, 0);
      to_join = std::move(g_trigger_thread);
    }
  }
  if (to_join.joinable())
    to_join.join();
  stop_hotkey_worker();
}

namespace {

// Must be called with g_state_mu held.
uint64_t open_overview_locked(bool *out_schedule) {
  if (g_running.load(std::memory_order_acquire))
    return 0;
  g_running.store(true, std::memory_order_release);
  const uint64_t seq = g_state_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
  g_pending_events.push(OverviewStateData{TRUE, seq});
  if (!g_dispatch_scheduled) {
    g_dispatch_scheduled = true;
    *out_schedule = true;
  }
  return seq;
}

// Must be called with g_state_mu held.
uint64_t close_overview_locked(bool *out_schedule) {
  if (!g_running.load(std::memory_order_acquire))
    return 0;
  g_running.store(false, std::memory_order_release);
  const uint64_t seq = g_state_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
  g_last_win_press_us.store(0, std::memory_order_release);
  g_pending_events.push(OverviewStateData{FALSE, seq});
  if (!g_dispatch_scheduled) {
    g_dispatch_scheduled = true;
    *out_schedule = true;
  }
  return seq;
}

}  // namespace

void open_overview(const char *source) {
  uint64_t seq = 0;
  bool schedule = false;
  {
    std::lock_guard<std::mutex> lk(g_state_mu);
    seq = open_overview_locked(&schedule);
  }
  if (schedule)
    schedule_overview_dispatch();
  if (seq != 0)
    g_print("[trig] open_overview (%s)\n", source != nullptr ? source : "?");
}

void close_overview(const char *source) {
  uint64_t seq = 0;
  bool schedule = false;
  {
    std::lock_guard<std::mutex> lk(g_state_mu);
    seq = close_overview_locked(&schedule);
  }
  if (schedule)
    schedule_overview_dispatch();
  if (seq != 0)
    g_print("[trig] close_overview (%s)\n", source != nullptr ? source : "?");
}

void toggle_overview(const char *source) {
  uint64_t seq = 0;
  bool opened = false;
  bool schedule = false;
  {
    std::lock_guard<std::mutex> lk(g_state_mu);
    if (!g_running.load(std::memory_order_acquire)) {
      seq = open_overview_locked(&schedule);
      opened = (seq != 0);
    } else {
      seq = close_overview_locked(&schedule);
      opened = false;
    }
  }
  if (schedule)
    schedule_overview_dispatch();
  if (seq != 0) {
    g_print("[trig] %s (%s)\n",
            opened ? "open_overview" : "close_overview",
            source != nullptr ? source : "?");
  }
}

bool overview_active() {
  return g_running.load(std::memory_order_acquire);
}

void set_overview_change_callback(OverviewStateCallback callback,
                                  void *user_data) {
  std::lock_guard<std::mutex> lk(g_state_mu);
  g_state_cb = callback;
  g_state_cb_data = user_data;
}

}  // namespace winome
