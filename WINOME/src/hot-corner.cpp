// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Event-driven GNOME-style top-left hot corner (zero polling at idle).
// Uses a lightweight WH_MOUSE_LL hook thread that is automatically unhooked
// whenever hot-corner is disabled in config or suppressed by a fullscreen app.

#include "hot-corner.h"
#include "config.h"
#include "fullscreen-watcher.h"
#include "overview-trigger.h"

#include <windows.h>
#include <glib.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace winome {
namespace {

constexpr int kCornerSize = 5;
constexpr UINT kMsgSyncHookState = WM_APP + 41;

struct HotCornerShared {
  std::atomic<HWND> panel_hwnd{nullptr};
  // SeqLock-protected monitor bounds so WH_MOUSE_LL reads a consistent,
  // untorn RECT without ever blocking on a mutex.
  std::mutex bounds_write_mu;
  std::atomic<uint64_t> bounds_seq{0};
  std::atomic<LONG> left{0};
  std::atomic<LONG> top{0};
  std::atomic<LONG> right{0};
  std::atomic<LONG> bottom{0};
  std::atomic<bool> suppressed{false};
  std::atomic<bool> in_corner{false};
  std::atomic<ULONGLONG> in_corner_since_tick{0};
  std::atomic<gint64> last_trigger_ms{0};
  std::atomic<int> cooldown_ms{500};
  std::atomic<bool> enabled{true};
  std::atomic<DWORD> hook_tid{0};

  HANDLE trigger_event = nullptr;
  std::atomic<bool> dispatch_stop{false};
  std::thread dispatch_thread;

  std::mutex thread_mu;
  std::condition_variable thread_cv;
  std::thread hook_thread;
  bool thread_ready = false;
  bool stop_requested = false;
  HHOOK mouse_hook = nullptr;
};

HotCornerShared g_hc;

gboolean invoke_open_overview(gpointer G_GNUC_UNUSED data) {
  if (!winome::panel_hidden()) {
    g_print("[hot] corner trigger (event-driven)\n");
    open_overview("hot-corner");
  }
  return G_SOURCE_REMOVE;
}

void store_cached_bounds(const RECT& rc) {
  std::lock_guard<std::mutex> lk(g_hc.bounds_write_mu);
  const uint64_t s = g_hc.bounds_seq.load(std::memory_order_relaxed);
  g_hc.bounds_seq.store(s + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  g_hc.left.store(rc.left, std::memory_order_relaxed);
  g_hc.top.store(rc.top, std::memory_order_relaxed);
  g_hc.right.store(rc.right, std::memory_order_relaxed);
  g_hc.bottom.store(rc.bottom, std::memory_order_relaxed);
  g_hc.bounds_seq.store(s + 2, std::memory_order_release);
}

RECT load_cached_bounds() {
  for (;;) {
    const uint64_t s1 = g_hc.bounds_seq.load(std::memory_order_acquire);
    if ((s1 & 1u) != 0u) {
      YieldProcessor();
      continue;
    }
    const RECT out{
        g_hc.left.load(std::memory_order_relaxed),
        g_hc.top.load(std::memory_order_relaxed),
        g_hc.right.load(std::memory_order_relaxed),
        g_hc.bottom.load(std::memory_order_relaxed),
    };
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const uint64_t s2 = g_hc.bounds_seq.load(std::memory_order_relaxed);
    if (s1 == s2)
      return out;
  }
}

void refresh_cached_bounds() {
  HWND panel = g_hc.panel_hwnd.load(std::memory_order_acquire);
  if (panel == nullptr || !IsWindow(panel))
    return;
  HMONITOR mon = MonitorFromWindow(panel, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (mon != nullptr && GetMonitorInfoW(mon, &mi)) {
    store_cached_bounds(mi.rcMonitor);
  }
}

LRESULT CALLBACK hot_corner_mouse_proc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode < 0)
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
  if (nCode == HC_ACTION && wParam == WM_MOUSEMOVE) {
    const MSLLHOOKSTRUCT* ms =
        reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    const RECT mon = load_cached_bounds();
    const bool inside =
        hot_corner_hit_test(ms->pt.x, ms->pt.y, mon, kCornerSize);
    const bool prev =
        g_hc.in_corner.exchange(inside, std::memory_order_acq_rel);
    if (inside != prev) {
      g_hc.in_corner_since_tick.store(inside ? GetTickCount64() : 0,
                                      std::memory_order_release);
    }
    if (inside && !prev &&
        g_hc.enabled.load(std::memory_order_relaxed) &&
        !g_hc.suppressed.load(std::memory_order_relaxed)) {
      const gint64 now_ms = g_get_monotonic_time() / 1000;
      const gint64 prev_ms =
          g_hc.last_trigger_ms.load(std::memory_order_relaxed);
      const int cd = g_hc.cooldown_ms.load(std::memory_order_relaxed);
      if (now_ms - prev_ms >= static_cast<gint64>(cd)) {
        g_hc.last_trigger_ms.store(now_ms, std::memory_order_relaxed);
        // Never call GLib g_idle_add_full inside WH_MOUSE_LL: GMainContext
        // lock contention during Overview animations stalls the hook past
        // LowLevelHooksTimeout. Wake the dedicated dispatch thread instead.
        if (g_hc.trigger_event != nullptr)
          SetEvent(g_hc.trigger_event);
      }
    }
  }
  return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void dispatch_thread_main() {
  while (!g_hc.dispatch_stop.load(std::memory_order_acquire)) {
    WaitForSingleObject(g_hc.trigger_event, INFINITE);
    if (g_hc.dispatch_stop.load(std::memory_order_acquire))
      break;
    g_idle_add_full(G_PRIORITY_HIGH_IDLE, invoke_open_overview,
                    nullptr, nullptr);
  }
}

void sync_hook_install_on_hook_thread(bool force_reinstall = false) {
  const bool want_hook =
      g_hc.enabled.load(std::memory_order_acquire) &&
      !g_hc.suppressed.load(std::memory_order_acquire);

  HMODULE self_mod = GetModuleHandleW(nullptr);
  if (want_hook) {
    if (g_hc.mouse_hook == nullptr) {
      g_hc.in_corner.store(false, std::memory_order_relaxed);
      g_hc.in_corner_since_tick.store(0, std::memory_order_relaxed);
      g_hc.mouse_hook =
          SetWindowsHookExW(WH_MOUSE_LL, hot_corner_mouse_proc, self_mod, 0);
    } else if (force_reinstall) {
      if (g_hc.in_corner.load(std::memory_order_relaxed)) {
        const ULONGLONG since =
            g_hc.in_corner_since_tick.load(std::memory_order_acquire);
        POINT pt{};
        if (since != 0 && (GetTickCount64() - since) >= 1500 &&
            GetCursorPos(&pt) &&
            !hot_corner_hit_test(pt.x, pt.y, load_cached_bounds(), kCornerSize)) {
          g_hc.in_corner.store(false, std::memory_order_relaxed);
          g_hc.in_corner_since_tick.store(0, std::memory_order_relaxed);
        }
      }
      if (!g_hc.in_corner.load(std::memory_order_relaxed)) {
        HHOOK fresh =
            SetWindowsHookExW(WH_MOUSE_LL, hot_corner_mouse_proc, self_mod, 0);
        if (fresh != nullptr) {
          UnhookWindowsHookEx(g_hc.mouse_hook);
          g_hc.mouse_hook = fresh;
        }
      }
    }
  } else if (g_hc.mouse_hook != nullptr) {
    UnhookWindowsHookEx(g_hc.mouse_hook);
    g_hc.mouse_hook = nullptr;
    g_hc.in_corner.store(false, std::memory_order_relaxed);
    g_hc.in_corner_since_tick.store(0, std::memory_order_relaxed);
  }
}

void hook_thread_main() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  MSG dummy{};
  PeekMessageW(&dummy, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

  {
    std::lock_guard<std::mutex> lock(g_hc.thread_mu);
    if (g_hc.stop_requested) {
      g_hc.hook_tid.store(0, std::memory_order_release);
      g_hc.thread_ready = true;
      g_hc.thread_cv.notify_all();
      return;
    }
    g_hc.hook_tid.store(GetCurrentThreadId(), std::memory_order_release);
    g_hc.thread_ready = true;
    g_hc.thread_cv.notify_all();
  }

  sync_hook_install_on_hook_thread();
  constexpr DWORD kWatchdogIntervalMs = 2500;
  ULONGLONG last_watchdog_tick = GetTickCount64();

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
      if (msg.message == kMsgSyncHookState) {
        sync_hook_install_on_hook_thread();
        continue;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (!running)
      break;

    const ULONGLONG now_tick = GetTickCount64();
    if (wait_rc == WAIT_TIMEOUT || (now_tick - last_watchdog_tick) >= kWatchdogIntervalMs) {
      last_watchdog_tick = now_tick;
      sync_hook_install_on_hook_thread(true);
    }
  }

  if (g_hc.mouse_hook != nullptr) {
    UnhookWindowsHookEx(g_hc.mouse_hook);
    g_hc.mouse_hook = nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(g_hc.thread_mu);
    g_hc.hook_tid.store(0, std::memory_order_release);
    g_hc.thread_ready = true;
    g_hc.thread_cv.notify_all();
  }
}

void post_sync_to_hook_thread() {
  DWORD tid = g_hc.hook_tid.load(std::memory_order_acquire);
  if (tid != 0)
    PostThreadMessageW(tid, kMsgSyncHookState, 0, 0);
}

void on_hot_corner_config_changed(void* data) {
  (void)data;
  const auto& cfg = get_config();
  g_hc.enabled.store(cfg.hot_corner_enabled, std::memory_order_release);
  g_hc.cooldown_ms.store(cfg.hot_corner_cooldown_ms, std::memory_order_release);
  post_sync_to_hook_thread();
}

}  // namespace

void start_hot_corner(HWND panel_hwnd) {
  std::unique_lock<std::mutex> lock(g_hc.thread_mu);
  g_hc.panel_hwnd.store(panel_hwnd, std::memory_order_release);
  const auto& cfg = get_config();
  g_hc.enabled.store(cfg.hot_corner_enabled, std::memory_order_release);
  g_hc.cooldown_ms.store(cfg.hot_corner_cooldown_ms, std::memory_order_release);
  g_hc.suppressed.store(winome::panel_hidden(), std::memory_order_release);
  refresh_cached_bounds();

  if (!g_hc.hook_thread.joinable()) {
    add_config_listener(on_hot_corner_config_changed, nullptr);
    g_hc.dispatch_stop.store(false, std::memory_order_release);
    if (g_hc.trigger_event == nullptr)
      g_hc.trigger_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_hc.dispatch_thread.joinable() && g_hc.trigger_event != nullptr)
      g_hc.dispatch_thread = std::thread(dispatch_thread_main);
    g_hc.stop_requested = false;
    g_hc.thread_ready = false;
    g_hc.hook_thread = std::thread(hook_thread_main);
    g_hc.thread_cv.wait(lock, [] { return g_hc.thread_ready; });
  } else {
    post_sync_to_hook_thread();
  }
}

void stop_hot_corner(void) {
  remove_config_listener(on_hot_corner_config_changed, nullptr);
  std::thread to_join;
  std::thread dispatch_to_join;
  {
    std::unique_lock<std::mutex> lock(g_hc.thread_mu);
    g_hc.stop_requested = true;
    if (g_hc.hook_thread.joinable()) {
      g_hc.thread_cv.wait(lock, [] { return g_hc.thread_ready; });
      DWORD tid = g_hc.hook_tid.exchange(0, std::memory_order_acq_rel);
      if (tid != 0)
        PostThreadMessageW(tid, WM_QUIT, 0, 0);
      to_join = std::move(g_hc.hook_thread);
    }
    if (g_hc.dispatch_thread.joinable()) {
      g_hc.dispatch_stop.store(true, std::memory_order_release);
      if (g_hc.trigger_event != nullptr)
        SetEvent(g_hc.trigger_event);
      dispatch_to_join = std::move(g_hc.dispatch_thread);
    }
  }
  if (to_join.joinable())
    to_join.join();
  if (dispatch_to_join.joinable())
    dispatch_to_join.join();
  if (g_hc.trigger_event != nullptr) {
    CloseHandle(g_hc.trigger_event);
    g_hc.trigger_event = nullptr;
  }
}

void hot_corner_set_suppressed(bool suppressed) {
  const bool prev =
      g_hc.suppressed.exchange(suppressed, std::memory_order_acq_rel);
  if (suppressed)
    g_hc.in_corner.store(false, std::memory_order_relaxed);
  if (prev != suppressed)
    post_sync_to_hook_thread();
}

void hot_corner_refresh_bounds(void) {
  refresh_cached_bounds();
}

}  // namespace winome
