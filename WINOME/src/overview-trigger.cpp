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
#include "overview-secondary.h"
#include "overview.h"
#include "run-dialog.h"
#include "shell-panel.h"
#include "monitor-manager.h"
#include "virtual-desktop.h"
#include "workspace-manager.h"

#include <gtk/gtk.h>
#include <gdk/win32/gdkwin32.h>
#include <windows.h>
#include <dwmapi.h>
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
ULONGLONG g_win_down_tick = 0;
ULONGLONG g_win_last_keydown_tick = 0;
ULONGLONG g_win_inject_tick = 0;
DWORD g_swallowed_combo_vk = 0;
ULONGLONG g_combo_swallow_tick = 0;
BOOL g_alt_f2_down = FALSE;
ULONGLONG g_alt_f2_tick = 0;
BOOL g_esc_swallowed = FALSE;
ULONGLONG g_esc_swallow_tick = 0;
HHOOK g_keyboard_hook = nullptr;
HWINEVENTHOOK g_desktop_switch_hook = nullptr;
ULONGLONG g_last_kbd_event_tick = 0;
bool g_kbd_swapping_hook = false;
WPARAM g_swap_last_wparam = 0;
DWORD g_swap_last_vk = 0;
DWORD g_swap_last_scan = 0;
DWORD g_swap_last_flags = 0;
DWORD g_swap_last_time = 0;
std::atomic<gint64> g_last_win_press_us{0};
std::atomic<bool> g_win_key_overview_enabled{true};

constexpr UINT kMsgRefreshKeyboardHook = WM_APP + 42;

enum class TriggerActionKind {
  BareWinUp,
  OpenStartMenu,
  WorkspaceMove,
  NotifyVdHint,
  ToggleRunDialog,
  UnfocusedEsc,
};

struct TriggerAction {
  TriggerActionKind kind = TriggerActionKind::BareWinUp;
  int64_t timestamp_us = 0;
  int delta = 0;
  bool move_fg = false;
};

AtomicTriggerRing<TriggerAction, 64> g_trigger_ring;
HANDLE g_dispatch_event = nullptr;
std::atomic<bool> g_dispatch_stop{false};
std::thread g_dispatch_thread;

inline void enqueue_trigger_action(const TriggerAction& act) {
  g_trigger_ring.try_push(act);
  if (g_dispatch_event != nullptr)
    SetEvent(g_dispatch_event);
}

// Lock-free cached panel HWNDs populated on the GTK main thread so the hook can
// distinguish panel windows (no Esc handler) from overview/popover/dialogs
// without blocking on cross-thread SendMessage/GetWindowTextW.
std::array<std::atomic<HWND>, kMaxCachedPanels> g_cached_panel_hwnds{};

// State-change callback & serialized main-thread event queue, guarded by g_state_mu.
OverviewStateCallback g_state_cb = nullptr;
void *g_state_cb_data = nullptr;

enum class OverviewQueueOp {
  Open,
  Toggle,
  CloseMainOrAll,
  CloseAll,
};

struct OverviewStateData {
  OverviewQueueOp op = OverviewQueueOp::Toggle;
  uint64_t seq = 0;
  const char *source = nullptr;
  HMONITOR monitor = nullptr;
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

  // In unlinked mode, if the cursor monitor has a Secondary Overview open
  // (or no main Overview is open at all), dismiss an open secondary Dash menu
  // first, otherwise dismiss that Secondary Overview.
  if (overview_secondary_close_open_dash_menu ())
    return G_SOURCE_REMOVE;
  if (!get_config ().multi_monitor_overview_linked) {
    HMONITOR cursor_mon = monitor_manager_active_monitor ();
    if (overview_secondary_is_open_for_monitor (cursor_mon)) {
      overview_close_secondary_for_monitor (cursor_mon);
      return G_SOURCE_REMOVE;
    }
    if (!overview_is_main_open () && overview_secondary_any_open ()) {
      overview_close_all_secondary ();
      return G_SOURCE_REMOVE;
    }
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
      else if (g_strcmp0 (tname, "WinomeAppGridClip") == 0)
        app_grid = gtk_widget_get_first_child (c);
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
      overview_restore_focus_if_active ();
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

void request_keyboard_hook_refresh_async();

static gboolean
dispatch_overview_events_idle (gpointer data G_GNUC_UNUSED)
{
  bool saw_transition = false;
  for (;;) {
    OverviewStateData ev{};
    OverviewStateCallback cb = nullptr;
    void *cb_data = nullptr;
    {
      std::lock_guard<std::mutex> lk (g_state_mu);
      if (g_pending_events.empty ()) {
        g_dispatch_scheduled = false;
        if (saw_transition)
          request_keyboard_hook_refresh_async ();
        return G_SOURCE_REMOVE;
      }
      ev = g_pending_events.front ();
      g_pending_events.pop ();
      cb = g_state_cb;
      cb_data = g_state_cb_data;
    }

    saw_transition = true;
    const bool linked = get_config ().multi_monitor_overview_linked;
    HMONITOR target =
        (ev.monitor != nullptr) ? ev.monitor : monitor_manager_active_monitor ();
    const bool main_open = overview_is_main_open ();
    HMONITOR main_mon = overview_main_monitor ();
    const bool target_has_sec = overview_secondary_is_open_for_monitor (target);
    const bool any_sec = overview_secondary_any_open ();

    OverviewDispatchAction act = OverviewDispatchAction::None;
    if (ev.op == OverviewQueueOp::CloseAll) {
      act = (main_open || any_sec) ? OverviewDispatchAction::CloseAll
                                   : OverviewDispatchAction::None;
    } else if (ev.op == OverviewQueueOp::CloseMainOrAll) {
      if (linked) {
        act = (main_open || any_sec) ? OverviewDispatchAction::CloseAll
                                     : OverviewDispatchAction::None;
      } else if (main_open) {
        act = OverviewDispatchAction::CloseMainOnly;
      } else if (target_has_sec) {
        act = OverviewDispatchAction::CloseSecondaryOnly;
      } else if (any_sec) {
        act = OverviewDispatchAction::CloseAll;
      }
    } else {
      act = decide_multi_monitor_overview_action (
          linked, ev.op == OverviewQueueOp::Toggle,
          reinterpret_cast<uintptr_t> (target), main_open,
          reinterpret_cast<uintptr_t> (main_mon), target_has_sec, any_sec);
    }

    switch (act) {
      case OverviewDispatchAction::None:
        break;
      case OverviewDispatchAction::OpenMain:
      case OverviewDispatchAction::OpenLinkedAll: {
        g_running.store (true, std::memory_order_release);
        g_print ("[trig] open_overview (%s mon=%p linked=%d)\n",
                 ev.source != nullptr ? ev.source : "?",
                 static_cast<void *> (target), linked ? 1 : 0);
        if (cb != nullptr)
          cb (true, cb_data);
        overview_set_target_monitor (target);
        overview_show ();
        refresh_cached_window_handles ();
        break;
      }
      case OverviewDispatchAction::OpenSecondary: {
        g_running.store (true, std::memory_order_release);
        g_last_win_press_us.store (0, std::memory_order_release);
        g_print ("[trig] open_secondary_overview (%s mon=%p)\n",
                 ev.source != nullptr ? ev.source : "?",
                 static_cast<void *> (target));
        if (cb != nullptr)
          cb (true, cb_data);
        overview_open_secondary_for_monitor (target);
        refresh_cached_window_handles ();
        break;
      }
      case OverviewDispatchAction::CloseMainOnly: {
        g_last_win_press_us.store (0, std::memory_order_release);
        g_print ("[trig] close_overview (%s main-only)\n",
                 ev.source != nullptr ? ev.source : "?");
        overview_hide ();
        refresh_cached_window_handles ();
        if (!any_sec) {
          g_running.store (false, std::memory_order_release);
          if (cb != nullptr)
            cb (false, cb_data);
        }
        break;
      }
      case OverviewDispatchAction::CloseSecondaryOnly: {
        g_last_win_press_us.store (0, std::memory_order_release);
        g_print ("[trig] close_secondary_overview (%s mon=%p)\n",
                 ev.source != nullptr ? ev.source : "?",
                 static_cast<void *> (target));
        overview_close_secondary_for_monitor (target);
        refresh_cached_window_handles ();
        if (!main_open && !overview_secondary_any_open ()) {
          g_running.store (false, std::memory_order_release);
          if (cb != nullptr)
            cb (false, cb_data);
        }
        break;
      }
      case OverviewDispatchAction::CloseAll: {
        g_last_win_press_us.store (0, std::memory_order_release);
        g_running.store (false, std::memory_order_release);
        g_print ("[trig] close_overview (%s all)\n",
                 ev.source != nullptr ? ev.source : "?");
        if (main_open)
          overview_hide ();
        if (should_close_secondary_on_close_all (linked, main_open, any_sec))
          overview_close_all_secondary ();
        else if (any_sec)
          overview_secondary_close_unlinked_animated ();
        refresh_cached_window_handles ();
        if (cb != nullptr)
          cb (false, cb_data);
        break;
      }
    }
  }
}

// Schedule asynchronous FIFO dispatch onto the default GMainContext outside
// g_state_mu. Using g_idle_add_full avoids synchronous same-thread re-entry
// (which g_main_context_invoke would perform on the main thread) while
// g_pending_events preserves exact state-transition ordering across threads.
static void
schedule_overview_dispatch ()
{
  g_idle_add_full (G_PRIORITY_HIGH_IDLE, dispatch_overview_events_idle,
                   nullptr, nullptr);
}

INPUT make_synthetic_key_input(WORD vk, WORD scan, DWORD flags) {
  INPUT input = {};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = vk;
  input.ki.wScan =
      scan != 0 ? scan : static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
  input.ki.dwFlags = flags;
  input.ki.dwExtraInfo = kSyntheticWinExtra;
  return input;
}

// Inject a Win keypress (keydown or keyup) into the input stream, tagged so
// LowLevelKeyboardProc ignores it.
void inject_win_key(bool key_up, WORD vk = VK_LWIN) {
  const DWORD flags = KEYEVENTF_EXTENDEDKEY | (key_up ? KEYEVENTF_KEYUP : 0);
  INPUT input = make_synthetic_key_input(vk, 0, flags);
  SendInput(1, &input, sizeof(INPUT));
}

// Atomically inject Win keydown immediately followed by the combo keydown in
// one SendInput batch so Windows processes Win+<key> (e.g. Win+V, Win+E,
// Win+R) in strict modifier-before-trigger order rather than letting the
// original key beat the injected Win keydown.
bool inject_win_with_combo_key(WORD win_vk, const KBDLLHOOKSTRUCT* state) {
  const DWORD combo_flags =
      ((state->flags & LLKHF_EXTENDED) != 0) ? KEYEVENTF_EXTENDEDKEY : 0;
  INPUT inputs[2] = {
      make_synthetic_key_input(win_vk, 0, KEYEVENTF_EXTENDEDKEY),
      make_synthetic_key_input(static_cast<WORD>(state->vkCode),
                               static_cast<WORD>(state->scanCode),
                               combo_flags),
  };
  return SendInput(2, inputs, sizeof(INPUT)) == 2;
}

void reset_win_key_state() {
  if (g_win_injected) {
    inject_win_key(true, g_win_vk);
    g_win_injected = FALSE;
  }
  g_win_down = FALSE;
  g_win_alone = FALSE;
  g_win_down_tick = 0;
  g_win_last_keydown_tick = 0;
  g_win_inject_tick = 0;
  g_swallowed_combo_vk = 0;
  g_combo_swallow_tick = 0;
  g_alt_f2_down = FALSE;
  g_alt_f2_tick = 0;
  g_esc_swallowed = FALSE;
  g_esc_swallow_tick = 0;
}

bool is_start_menu_foreground() {
  HWND fg = GetForegroundWindow();
  if (fg == nullptr)
    return false;
  const bool visible = IsWindowVisible(fg) != FALSE;
  DWORD cloaked = 0;
  const bool cloak_ok =
      SUCCEEDED(DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloaked,
                                      sizeof(cloaked)));
  wchar_t cls[128] = {};
  if (GetClassNameW(fg, cls, 128) <= 0)
    return false;
  const bool is_core_class =
      std::wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0 ||
      std::wcscmp(cls, L"XamlExplorerHostIslandWindow") == 0;
  if (!is_core_class)
    return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  if (pid == 0 || pid == GetCurrentProcessId())
    return false;
  HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (proc == nullptr)
    return false;
  wchar_t path[MAX_PATH] = {};
  DWORD len = MAX_PATH;
  BOOL ok = QueryFullProcessImageNameW(proc, 0, path, &len);
  CloseHandle(proc);
  if (!ok)
    return false;
  const wchar_t* base = std::wcsrchr(path, L'\\');
  base = (base != nullptr) ? (base + 1) : path;
  const bool is_start_exe =
      _wcsicmp(base, L"StartMenuExperienceHost.exe") == 0 ||
      _wcsicmp(base, L"SearchHost.exe") == 0 ||
      _wcsicmp(base, L"SearchApp.exe") == 0;
  return is_start_menu_window_active(visible, cloak_ok ? cloaked : 0u,
                                     is_core_class, is_start_exe);
}

bool is_current_process_elevated() {
  HANDLE tok = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
    return false;
  TOKEN_ELEVATION elev{};
  DWORD ret_len = 0;
  const BOOL ok = GetTokenInformation(tok, TokenElevation, &elev,
                                      sizeof(elev), &ret_len);
  CloseHandle(tok);
  return ok != FALSE && elev.TokenIsElevated != 0;
}

void on_trigger_config_changed(void* data G_GNUC_UNUSED) {
  g_win_key_overview_enabled.store(get_config().win_key_overview,
                                   std::memory_order_release);
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
  INPUT inputs[2] = {
      make_synthetic_key_input(VK_LWIN, 0, KEYEVENTF_EXTENDEDKEY),
      make_synthetic_key_input(VK_LWIN, 0,
                               KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP),
  };
  SendInput(2, inputs, sizeof(INPUT));
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

    HWND fg = GetForegroundWindow();
    HMONITOR target_mon = nullptr;
    if (task.move_fg && is_eligible_window_for_move(fg))
      target_mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    if (target_mon == nullptr)
      target_mon = monitor_manager_active_monitor();

    const int count = ws::count_for_monitor(target_mon);
    const int cur = ws::active_for_monitor(target_mon);
    if (count <= 0 || cur < 0)
      continue;

    const int target = std::clamp(cur + task.delta, 0, count - 1);
    if (target == cur)
      continue;

    if (task.move_fg && is_eligible_window_for_move(fg))
      ws::move_window_to_workspace(fg, target_mon, target);

    ws::switch_to(target_mon, target);
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

void dispatch_worker_main() {
  while (!g_dispatch_stop.load(std::memory_order_acquire)) {
    WaitForSingleObject(g_dispatch_event, INFINITE);
    TriggerAction act{};
    while (g_trigger_ring.try_pop(&act)) {
      switch (act.kind) {
        case TriggerActionKind::BareWinUp: {
          if (!g_running.load(std::memory_order_acquire) &&
              is_start_menu_foreground()) {
            open_start_menu();
            break;
          }
          const gint64 prev =
              g_last_win_press_us.exchange(0, std::memory_order_acq_rel);
          if (g_running.load(std::memory_order_acquire) && prev > 0 &&
              (act.timestamp_us - prev) <= 550000 &&
              overview_can_shift_to_app_grid()) {
            overview_shift_to_app_grid_async();
          } else {
            g_last_win_press_us.store(act.timestamp_us,
                                      std::memory_order_release);
            toggle_overview_for_monitor("win-key", nullptr);
          }
          break;
        }
        case TriggerActionKind::OpenStartMenu: {
          if (g_running.load(std::memory_order_acquire)) {
            g_idle_add_full(
                G_PRIORITY_HIGH_IDLE,
                +[](gpointer) -> gboolean {
                  overview_discard_restore_focus();
                  close_overview("win-tab");
                  g_idle_add(
                      +[](gpointer) -> gboolean {
                        open_start_menu();
                        return G_SOURCE_REMOVE;
                      },
                      nullptr);
                  return G_SOURCE_REMOVE;
                },
                nullptr, nullptr);
          } else {
            open_start_menu();
          }
          break;
        }
        case TriggerActionKind::WorkspaceMove:
          enqueue_desktop_hotkey(act.delta, act.move_fg);
          break;
        case TriggerActionKind::NotifyVdHint:
          vd::notify_change_hint_async();
          break;
        case TriggerActionKind::ToggleRunDialog:
          if (debug_get_flags().hotkey_trace)
            debug_append_log("trig", "[trig] Alt+F2 -> toggle RunDialog");
          run_dialog_toggle_async();
          break;
        case TriggerActionKind::UnfocusedEsc:
          g_idle_add_full(G_PRIORITY_HIGH_IDLE, handle_unfocused_esc_idle,
                          nullptr, nullptr);
          break;
      }
    }
  }
}

void start_dispatch_worker() {
  g_trigger_ring.clear();
  g_dispatch_stop.store(false, std::memory_order_release);
  if (g_dispatch_event == nullptr)
    g_dispatch_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!g_dispatch_thread.joinable() && g_dispatch_event != nullptr)
    g_dispatch_thread = std::thread(dispatch_worker_main);
}

void stop_dispatch_worker() {
  g_dispatch_stop.store(true, std::memory_order_release);
  if (g_dispatch_event != nullptr)
    SetEvent(g_dispatch_event);
  if (g_dispatch_thread.joinable())
    g_dispatch_thread.join();
  g_trigger_ring.clear();
  if (g_dispatch_event != nullptr) {
    CloseHandle(g_dispatch_event);
    g_dispatch_event = nullptr;
  }
}

void request_keyboard_hook_refresh_async() {
  const DWORD tid = g_trigger_tid.load(std::memory_order_acquire);
  if (tid != 0)
    PostThreadMessageW(tid, kMsgRefreshKeyboardHook, 0, 0);
}

void maybe_refresh_keyboard_hook(ULONGLONG now_tick,
                                 ULONGLONG* last_reinstall_tick,
                                 ULONGLONG min_spacing_ms) {
  HookWatchdogInput in{};
  in.now_tick = now_tick;
  in.win_down = g_win_down != FALSE;
  in.win_injected = g_win_injected != FALSE;
  in.win_physically_down =
      g_win_down ? ((GetAsyncKeyState(g_win_vk) & 0x8000) != 0) : false;
  in.win_last_keydown_tick = g_win_last_keydown_tick;
  in.win_inject_tick = g_win_inject_tick;
  in.swallowed_combo_vk = static_cast<unsigned>(g_swallowed_combo_vk);
  in.combo_swallow_tick = g_combo_swallow_tick;
  in.alt_f2_down = g_alt_f2_down != FALSE;
  in.alt_f2_tick = g_alt_f2_tick;
  in.esc_swallowed = g_esc_swallowed != FALSE;
  in.esc_swallow_tick = g_esc_swallow_tick;

  const HookWatchdogOutcome out = evaluate_hook_watchdog(in);
  if (out.reset_win || out.clear_combo || out.clear_alt_f2 || out.clear_esc) {
    g_print("[trig] cleared stale hook flags win=%d combo=%d f2=%d esc=%d\n",
            out.reset_win ? 1 : 0, out.clear_combo ? 1 : 0,
            out.clear_alt_f2 ? 1 : 0, out.clear_esc ? 1 : 0);
  }
  if (out.reset_win) {
    reset_win_key_state();
  } else {
    if (out.clear_combo) {
      g_swallowed_combo_vk = 0;
      g_combo_swallow_tick = 0;
    }
    if (out.clear_alt_f2) {
      g_alt_f2_down = FALSE;
      g_alt_f2_tick = 0;
    }
    if (out.clear_esc) {
      g_esc_swallowed = FALSE;
      g_esc_swallow_tick = 0;
    }
  }

  if (!out.safe_to_reinstall)
    return;
  if (!should_reinstall_ll_hook(g_keyboard_hook != nullptr, now_tick,
                                g_last_kbd_event_tick, *last_reinstall_tick,
                                4000, min_spacing_ms)) {
    return;
  }

  g_kbd_swapping_hook = true;
  HHOOK fresh = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                  GetModuleHandleW(nullptr), 0);
  if (fresh != nullptr) {
    if (g_keyboard_hook != nullptr)
      UnhookWindowsHookEx(g_keyboard_hook);
    g_keyboard_hook = fresh;
    *last_reinstall_tick = now_tick;
  } else {
    g_print("[trig] SetWindowsHookExW(WH_KEYBOARD_LL) failed gle=%lu\n",
            GetLastError());
  }
  g_kbd_swapping_hook = false;
}

void trigger_thread_main() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
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
                                      GetModuleHandleW(nullptr), 0);
  g_print("[trig] keyboard hook ready (installed=%d elevated=%d)\n",
          g_keyboard_hook != nullptr ? 1 : 0,
          is_current_process_elevated() ? 1 : 0);
  g_desktop_switch_hook = SetWinEventHook(
      EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH, nullptr,
      on_desktop_switch_event, 0, 0, WINEVENT_OUTOFCONTEXT);

  constexpr DWORD kWatchdogIntervalMs = 2500;
  ULONGLONG last_reinstall_tick = GetTickCount64();

  bool running = true;
  while (running) {
    const DWORD wait_rc = MsgWaitForMultipleObjectsEx(
        0, nullptr, kWatchdogIntervalMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        running = false;
        break;
      }
      if (msg.message == kMsgRefreshKeyboardHook) {
        maybe_refresh_keyboard_hook(GetTickCount64(), &last_reinstall_tick, 400);
        continue;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (!running)
      break;

    const ULONGLONG now_tick = GetTickCount64();
    if (wait_rc == WAIT_TIMEOUT ||
        (now_tick - last_reinstall_tick) >= kWatchdogIntervalMs) {
      maybe_refresh_keyboard_hook(now_tick, &last_reinstall_tick,
                                  kWatchdogIntervalMs);
    }
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
  if (nCode < 0)
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
  if (nCode == HC_ACTION) {
    KBDLLHOOKSTRUCT* state = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
    g_last_kbd_event_tick = GetTickCount64();
    if (g_kbd_swapping_hook && wParam == g_swap_last_wparam &&
        state->vkCode == g_swap_last_vk &&
        state->scanCode == g_swap_last_scan &&
        state->flags == g_swap_last_flags &&
        state->time == g_swap_last_time) {
      return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }
    g_swap_last_wparam = wParam;
    g_swap_last_vk = state->vkCode;
    g_swap_last_scan = state->scanCode;
    g_swap_last_flags = state->flags;
    g_swap_last_time = state->time;

    // Synthetic Win presses injected below: pass them through untouched so the
    // system sees the Start menu / Win+<key> we deliberately re-created.
    if (state->dwExtraInfo == kSyntheticWinExtra)
      return CallNextHookEx(nullptr, nCode, wParam, lParam);

    const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const bool up = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

    // Fallback recovery: if a previous Win keydown lost its matching keyup
    // (e.g. across secure desktop / UAC / elevated window), clear stale state
    // before evaluating a non-Win key.
    // Note: while !g_win_injected, Win keydown was swallowed by this hook so
    // GetAsyncKeyState(g_win_vk) is ALWAYS 0 even while physically held — only
    // query GetAsyncKeyState once Win has actually been injected!
    if (g_win_down && state->vkCode != VK_LWIN && state->vkCode != VK_RWIN) {
      const ULONGLONG now_tick = GetTickCount64();
      if (g_win_injected) {
        if ((now_tick - g_win_inject_tick) > 40 &&
            (GetAsyncKeyState(g_win_vk) & 0x8000) == 0) {
          reset_win_key_state();
        }
      } else if ((now_tick - g_win_last_keydown_tick) > 2500) {
        reset_win_key_state();
      }
    }

    if (up && g_swallowed_combo_vk != 0 &&
        state->vkCode == g_swallowed_combo_vk) {
      g_swallowed_combo_vk = 0;
      g_combo_swallow_tick = 0;
      return 1;
    }

    if (state->vkCode == VK_LWIN || state->vkCode == VK_RWIN) {
      if (down) {
        if (!g_win_down &&
            !g_win_key_overview_enabled.load(std::memory_order_acquire))
          return CallNextHookEx(nullptr, nCode, wParam, lParam);
        const ULONGLONG now_tick = GetTickCount64();
        // If a previous Win keydown got stuck without a keyup (e.g. UAC),
        // reset before starting the new press. When g_win_injected is true and
        // Win is still physically reported down, keep tracking it normally.
        if (g_win_down &&
            (g_win_injected
                 ? ((GetAsyncKeyState(g_win_vk) & 0x8000) == 0)
                 : ((!g_win_alone && (now_tick - g_win_down_tick) > 2000) ||
                    (now_tick - g_win_last_keydown_tick) > 2500))) {
          reset_win_key_state();
        }
        // Swallow the Win keydown so Windows does not open the Start menu.
        // Only the first keydown initializes state; typematic auto-repeats keep
        // updating g_win_last_keydown_tick while physically held.
        if (!g_win_down) {
          g_win_down = TRUE;
          g_win_vk = static_cast<WORD>(state->vkCode);
          g_win_alone = TRUE;
          g_win_injected = FALSE;
          g_win_down_tick = now_tick;
          g_win_last_keydown_tick = now_tick;
          g_win_inject_tick = 0;
        } else {
          if (state->vkCode != g_win_vk)
            g_win_alone = FALSE;
          else
            g_win_last_keydown_tick = now_tick;
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
        g_win_down_tick = 0;
        g_win_last_keydown_tick = 0;
        g_win_inject_tick = 0;
        if (g_win_injected) {
          // Release the synthetic Win keydown that enabled the Win+<key> combo.
          inject_win_key(true, g_win_vk);
          g_win_injected = FALSE;
        }
        if (bare) {
          TriggerAction act{};
          act.kind = TriggerActionKind::BareWinUp;
          act.timestamp_us = g_get_monotonic_time();
          enqueue_trigger_action(act);
        }
        return 1;
      }
    }

    if (down && !g_win_down) {
      g_swallowed_combo_vk = 0;
      g_combo_swallow_tick = 0;
    }

    if (down && g_win_down) {
      // A key pressed while Win is held: this is a combo, not a bare Win.
      g_win_alone = FALSE;
      const bool ctrl_held = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
      const bool shift_held = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
      const bool alt_held =
          (wParam == WM_SYSKEYDOWN) ||
          ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);

      const WinKeyComboAction action = classify_win_key_combo(
          state->vkCode, wParam == WM_SYSKEYDOWN,
          ctrl_held, shift_held, alt_held, g_win_injected != FALSE);

      switch (action) {
        case WinKeyComboAction::ModifierPassthrough:
          // Let Ctrl/Shift/Alt pass through without prematurely injecting Win
          // so Win+Shift+PageUp never leaks Win and Win+Shift+S / Win+Ctrl+Arrows
          // atomically deliver Win right before the non-modifier trigger key.
          return CallNextHookEx(nullptr, nCode, wParam, lParam);

        case WinKeyComboAction::StartMenu:
          // Win+Tab opens the Start menu instead of Task View (ignore auto-repeat).
          if (g_swallowed_combo_vk != VK_TAB) {
            g_swallowed_combo_vk = VK_TAB;
            g_combo_swallow_tick = GetTickCount64();
            TriggerAction act{};
            act.kind = TriggerActionKind::OpenStartMenu;
            enqueue_trigger_action(act);
          }
          return 1;

        case WinKeyComboAction::WorkspacePrev:
        case WinKeyComboAction::WorkspaceNext: {
          // GNOME-style workspace switching & active window move via worker.
          g_swallowed_combo_vk = state->vkCode;
          g_combo_swallow_tick = GetTickCount64();
          TriggerAction act{};
          act.kind = TriggerActionKind::WorkspaceMove;
          act.delta = (action == WinKeyComboAction::WorkspacePrev) ? -1 : 1;
          act.move_fg = shift_held;
          enqueue_trigger_action(act);
          return 1;
        }

        case WinKeyComboAction::PassthroughWithVdHint: {
          // In Independent per-monitor workspace mode, intercept
          // Win+Ctrl+Left/Right so switching stays per-monitor instead of
          // jumping Windows onto the hidden parking desktop.
          if (!g_win_injected &&
              (state->vkCode == VK_LEFT || state->vkCode == VK_RIGHT) &&
              ws::is_independent_active()) {
            g_swallowed_combo_vk = state->vkCode;
            g_combo_swallow_tick = GetTickCount64();
            TriggerAction act{};
            act.kind = TriggerActionKind::WorkspaceMove;
            act.delta = (state->vkCode == VK_LEFT) ? -1 : 1;
            act.move_fg = false;
            enqueue_trigger_action(act);
            return 1;
          }
          TriggerAction act{};
          act.kind = TriggerActionKind::NotifyVdHint;
          enqueue_trigger_action(act);
          [[fallthrough]];
        }

        case WinKeyComboAction::PassthroughCombo:
          if (!g_win_injected) {
            // Atomically inject [Win down, key down] in strict order and swallow
            // the original keydown so shortcuts like Win+V, Win+E, Win+R reach
            // Windows with Win already active.
            if (inject_win_with_combo_key(g_win_vk, state)) {
              g_win_injected = TRUE;
              g_win_inject_tick = GetTickCount64();
              return 1;
            }
          }
          break;
      }
    } else if (state->vkCode == VK_F2) {
      if (up) {
        g_alt_f2_down = FALSE;
        g_alt_f2_tick = 0;
      } else if (down && !g_win_down) {
        const bool alt_held =
            (wParam == WM_SYSKEYDOWN) ||
            ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
        const bool ctrl_held = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift_held = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (alt_held && !ctrl_held && !shift_held) {
          if (!g_alt_f2_down) {
            g_alt_f2_down = TRUE;
            g_alt_f2_tick = GetTickCount64();
            TriggerAction act{};
            act.kind = TriggerActionKind::ToggleRunDialog;
            enqueue_trigger_action(act);
          }
          return 1;
        }
      }
    } else if (state->vkCode == VK_ESCAPE) {
      if (up) {
        if (g_esc_swallowed) {
          g_esc_swallowed = FALSE;
          g_esc_swallow_tick = 0;
          return 1;
        }
      } else if (down && !g_win_down) {
        if (!g_running.load(std::memory_order_acquire)) {
          g_esc_swallowed = FALSE;
          g_esc_swallow_tick = 0;
        } else if (should_intercept_overview_esc()) {
          // Overview is WS_EX_NOACTIVATE (or the top panel holds focus), so
          // GTK never receives VK_ESCAPE directly. Swallow it and run the
          // layered Esc dismissal on the GTK main thread via dispatch worker.
          if (!g_esc_swallowed) {
            g_esc_swallowed = TRUE;
            g_esc_swallow_tick = GetTickCount64();
            TriggerAction act{};
            act.kind = TriggerActionKind::UnfocusedEsc;
            enqueue_trigger_action(act);
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
  g_win_key_overview_enabled.store(get_config().win_key_overview,
                                   std::memory_order_release);
  add_config_listener(on_trigger_config_changed, nullptr);
  start_hotkey_worker();
  start_dispatch_worker();
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
  remove_config_listener(on_trigger_config_changed, nullptr);
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
  stop_dispatch_worker();
  stop_hotkey_worker();
}

namespace {

// Must be called with g_state_mu held.
uint64_t enqueue_overview_op_locked(OverviewQueueOp op,
                                    const char *source,
                                    HMONITOR monitor,
                                    bool *out_schedule) {
  const uint64_t seq = g_state_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
  g_pending_events.push(OverviewStateData{
      op, seq, source != nullptr ? source : "?", monitor});
  if (!g_dispatch_scheduled) {
    g_dispatch_scheduled = true;
    *out_schedule = true;
  }
  return seq;
}

}  // namespace

void open_overview_for_monitor(const char *source, HMONITOR monitor) {
  bool schedule = false;
  {
    std::lock_guard<std::mutex> lk(g_state_mu);
    enqueue_overview_op_locked(OverviewQueueOp::Open, source, monitor,
                               &schedule);
  }
  if (schedule)
    schedule_overview_dispatch();
}

void open_overview(const char *source) {
  open_overview_for_monitor(source, nullptr);
}

void close_overview(const char *source) {
  bool schedule = false;
  {
    std::lock_guard<std::mutex> lk(g_state_mu);
    enqueue_overview_op_locked(OverviewQueueOp::CloseMainOrAll, source, nullptr,
                               &schedule);
  }
  if (schedule)
    schedule_overview_dispatch();
}

void toggle_overview_for_monitor(const char *source, HMONITOR monitor) {
  bool schedule = false;
  {
    std::lock_guard<std::mutex> lk(g_state_mu);
    enqueue_overview_op_locked(OverviewQueueOp::Toggle, source, monitor,
                               &schedule);
  }
  if (schedule)
    schedule_overview_dispatch();
}

void toggle_overview(const char *source) {
  toggle_overview_for_monitor(source, nullptr);
}

bool overview_active() {
  return g_running.load(std::memory_order_acquire);
}

void sync_overview_active_state(bool active) {
  g_running.store(active, std::memory_order_release);
}

void set_overview_change_callback(OverviewStateCallback callback,
                                  void *user_data) {
  std::lock_guard<std::mutex> lk(g_state_mu);
  g_state_cb = callback;
  g_state_cb_data = user_data;
}

}  // namespace winome
