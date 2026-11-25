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

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace winome {
namespace {

constexpr int kCornerSize = 5;
constexpr size_t kMaxHotCornerMonitors = 16;
constexpr UINT kMsgSyncHookState = WM_APP + 41;

struct HotCornerSnapshot {
  std::array<HotCornerMonitor, kMaxHotCornerMonitors> monitors{};
  size_t count = 0;
};

struct HotCornerShared {
  std::atomic<HWND> panel_hwnd{nullptr};
  // SeqLock-protected monitor bounds so WH_MOUSE_LL reads consistent,
  // untorn monitor RECTs across all displays without ever blocking on a mutex.
  std::mutex bounds_write_mu;
  std::atomic<uint64_t> bounds_seq{0};
  HotCornerMonitor monitors[kMaxHotCornerMonitors]{};
  size_t monitor_count = 0;
  std::atomic<bool> suppressed{false};
  std::atomic<int> in_corner_idx{-1};
  std::atomic<ULONGLONG> in_corner_since_tick{0};
  std::atomic<HMONITOR> pending_monitor{nullptr};
  std::atomic<gint64> last_trigger_ms{0};
  std::atomic<int> cooldown_ms{500};
  std::atomic<bool> enabled{true};
  std::atomic<DWORD> hook_tid{0};
  std::atomic<ULONGLONG> last_hook_event_tick{0};

  HANDLE trigger_event = nullptr;
  std::atomic<bool> dispatch_stop{false};
  std::thread dispatch_thread;

  std::mutex thread_mu;
  std::condition_variable thread_cv;
  std::thread hook_thread;
  bool thread_ready = false;
  bool stop_requested = false;
  HHOOK mouse_hook = nullptr;
  ULONGLONG last_reinstall_tick = 0;
  bool fg_cache_init = false;
  HWND cached_fg_hwnd = nullptr;
  bool cached_fg_elevated = false;
  bool uipi_fallback_logged = false;
};

HotCornerShared g_hc;

gboolean invoke_open_overview(gpointer data) {
  HMONITOR target_mon = reinterpret_cast<HMONITOR>(data);
  const bool hidden = target_mon != nullptr
                          ? winome::panel_hidden_for_monitor(target_mon)
                          : winome::panel_hidden();
  if (!hidden) {
    g_print("[hot] corner trigger (event-driven mon=%p)\n",
            static_cast<void*>(target_mon));
    open_overview_for_monitor("hot-corner", target_mon);
  }
  return G_SOURCE_REMOVE;
}

void store_cached_monitors(const std::vector<HotCornerMonitor>& list) {
  std::lock_guard<std::mutex> lk(g_hc.bounds_write_mu);
  const uint64_t s = g_hc.bounds_seq.load(std::memory_order_relaxed);
  g_hc.bounds_seq.store(s + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  const size_t n =
      list.size() < kMaxHotCornerMonitors ? list.size() : kMaxHotCornerMonitors;
  for (size_t i = 0; i < n; ++i)
    g_hc.monitors[i] = list[i];
  g_hc.monitor_count = n;
  std::atomic_thread_fence(std::memory_order_seq_cst);
  g_hc.bounds_seq.store(s + 2, std::memory_order_release);
}

HotCornerSnapshot load_cached_monitors() {
  HotCornerSnapshot out{};
  for (;;) {
    const uint64_t s1 = g_hc.bounds_seq.load(std::memory_order_acquire);
    if ((s1 & 1u) != 0u) {
      YieldProcessor();
      continue;
    }
    const size_t n = g_hc.monitor_count < kMaxHotCornerMonitors
                         ? g_hc.monitor_count
                         : kMaxHotCornerMonitors;
    out.count = n;
    for (size_t i = 0; i < n; ++i)
      out.monitors[i] = g_hc.monitors[i];
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const uint64_t s2 = g_hc.bounds_seq.load(std::memory_order_relaxed);
    if (s1 == s2)
      return out;
  }
}

BOOL CALLBACK enum_hot_corner_monitors_proc(HMONITOR hmon,
                                            HDC G_GNUC_UNUSED hdc,
                                            LPRECT G_GNUC_UNUSED lprc,
                                            LPARAM lp) {
  auto* list = reinterpret_cast<std::vector<HotCornerMonitor>*>(lp);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfoW(hmon, &mi)) {
    HotCornerMonitor entry{};
    entry.hmonitor = hmon;
    entry.rect = mi.rcMonitor;
    list->push_back(entry);
  }
  return TRUE;
}

void refresh_cached_bounds() {
  std::vector<HotCornerMonitor> list;
  EnumDisplayMonitors(nullptr, nullptr, enum_hot_corner_monitors_proc,
                      reinterpret_cast<LPARAM>(&list));
  if (list.empty()) {
    HWND panel = g_hc.panel_hwnd.load(std::memory_order_acquire);
    if (panel != nullptr && IsWindow(panel)) {
      HMONITOR mon = MonitorFromWindow(panel, MONITOR_DEFAULTTONEAREST);
      MONITORINFO mi{};
      mi.cbSize = sizeof(mi);
      if (mon != nullptr && GetMonitorInfoW(mon, &mi)) {
        list.push_back(HotCornerMonitor{mon, mi.rcMonitor});
      }
    }
  }
  if (!list.empty())
    store_cached_monitors(list);
}

void process_hot_corner_point(int x, int y) {
  const HotCornerSnapshot snap = load_cached_monitors();
  const int hit_idx = hot_corner_hit_test_monitors(
      x, y, snap.monitors.data(), snap.count, kCornerSize);
  const int prev_idx =
      g_hc.in_corner_idx.exchange(hit_idx, std::memory_order_acq_rel);
  if (hit_idx != prev_idx) {
    g_hc.in_corner_since_tick.store(hit_idx >= 0 ? GetTickCount64() : 0,
                                    std::memory_order_release);
  }
  if (hit_idx >= 0 && hit_idx != prev_idx &&
      g_hc.enabled.load(std::memory_order_relaxed) &&
      !g_hc.suppressed.load(std::memory_order_relaxed)) {
    const gint64 now_ms = g_get_monotonic_time() / 1000;
    const int cd = g_hc.cooldown_ms.load(std::memory_order_relaxed);
    gint64 prev_ms = g_hc.last_trigger_ms.load(std::memory_order_relaxed);
    while (now_ms - prev_ms >= static_cast<gint64>(cd)) {
      if (g_hc.last_trigger_ms.compare_exchange_weak(
              prev_ms, now_ms, std::memory_order_acq_rel,
              std::memory_order_relaxed)) {
        g_hc.pending_monitor.store(
            snap.monitors[static_cast<size_t>(hit_idx)].hmonitor,
            std::memory_order_release);
        // Never call GLib g_idle_add_full inside WH_MOUSE_LL: GMainContext
        // lock contention during Overview animations stalls the hook past
        // LowLevelHooksTimeout. Wake the dedicated dispatch thread instead.
        if (g_hc.trigger_event != nullptr)
          SetEvent(g_hc.trigger_event);
        break;
      }
    }
  }
}

bool is_current_process_elevated() {
  HANDLE tok = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
    return false;
  TOKEN_ELEVATION elev{};
  DWORD ret_len = 0;
  const BOOL ok =
      GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &ret_len);
  CloseHandle(tok);
  return ok != FALSE && elev.TokenIsElevated != 0;
}

bool is_window_elevated(HWND hwnd) {
  if (hwnd == nullptr || !IsWindow(hwnd))
    return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == 0 || pid == GetCurrentProcessId())
    return false;
  HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (proc == nullptr)
    return GetLastError() == ERROR_ACCESS_DENIED;
  HANDLE tok = nullptr;
  bool elevated = false;
  if (!OpenProcessToken(proc, TOKEN_QUERY, &tok)) {
    if (GetLastError() == ERROR_ACCESS_DENIED)
      elevated = true;
  } else {
    TOKEN_ELEVATION te{};
    DWORD ret_len = 0;
    if (GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &ret_len))
      elevated = (te.TokenIsElevated != 0);
    CloseHandle(tok);
  }
  CloseHandle(proc);
  return elevated;
}

void update_fg_elevation_cache(HWND fg) {
  if (g_hc.fg_cache_init && fg == g_hc.cached_fg_hwnd)
    return;
  g_hc.fg_cache_init = true;
  g_hc.cached_fg_hwnd = fg;
  g_hc.cached_fg_elevated = is_window_elevated(fg);
}

void CALLBACK on_hot_corner_fg_changed(HWINEVENTHOOK G_GNUC_UNUSED hook,
                                       DWORD event,
                                       HWND hwnd,
                                       LONG id_object,
                                       LONG id_child,
                                       DWORD G_GNUC_UNUSED event_thread,
                                       DWORD G_GNUC_UNUSED event_time) {
  if (event == EVENT_SYSTEM_FOREGROUND && id_object == OBJID_WINDOW &&
      id_child == CHILDID_SELF) {
    update_fg_elevation_cache(hwnd != nullptr ? hwnd : GetForegroundWindow());
  }
}

LRESULT CALLBACK hot_corner_mouse_proc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode < 0)
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
  if (nCode == HC_ACTION && wParam == WM_MOUSEMOVE) {
    g_hc.last_hook_event_tick.store(GetTickCount64(), std::memory_order_relaxed);
    const MSLLHOOKSTRUCT* ms =
        reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    process_hot_corner_point(ms->pt.x, ms->pt.y);
  }
  return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void dispatch_thread_main() {
  while (!g_hc.dispatch_stop.load(std::memory_order_acquire)) {
    WaitForSingleObject(g_hc.trigger_event, INFINITE);
    if (g_hc.dispatch_stop.load(std::memory_order_acquire))
      break;
    HMONITOR mon = g_hc.pending_monitor.load(std::memory_order_acquire);
    g_idle_add_full(G_PRIORITY_HIGH_IDLE, invoke_open_overview,
                    reinterpret_cast<gpointer>(mon), nullptr);
  }
}

void sync_hook_install_on_hook_thread(bool force_reinstall = false) {
  const bool want_hook =
      g_hc.enabled.load(std::memory_order_acquire) &&
      !g_hc.suppressed.load(std::memory_order_acquire);

  HMODULE self_mod = GetModuleHandleW(nullptr);
  if (want_hook) {
    if (g_hc.mouse_hook == nullptr) {
      g_hc.in_corner_idx.store(-1, std::memory_order_relaxed);
      g_hc.in_corner_since_tick.store(0, std::memory_order_relaxed);
      g_hc.mouse_hook =
          SetWindowsHookExW(WH_MOUSE_LL, hot_corner_mouse_proc, self_mod, 0);
      if (g_hc.mouse_hook != nullptr) {
        g_hc.last_reinstall_tick = GetTickCount64();
      } else {
        g_print("[hot] SetWindowsHookExW(WH_MOUSE_LL) failed gle=%lu\n",
                GetLastError());
      }
    } else if (force_reinstall) {
      if (g_hc.in_corner_idx.load(std::memory_order_relaxed) >= 0) {
        const ULONGLONG since =
            g_hc.in_corner_since_tick.load(std::memory_order_acquire);
        POINT pt{};
        const HotCornerSnapshot snap = load_cached_monitors();
        if (since != 0 && (GetTickCount64() - since) >= 1500 &&
            GetCursorPos(&pt) &&
            hot_corner_hit_test_monitors(pt.x, pt.y, snap.monitors.data(),
                                         snap.count, kCornerSize) < 0) {
          g_hc.in_corner_idx.store(-1, std::memory_order_relaxed);
          g_hc.in_corner_since_tick.store(0, std::memory_order_relaxed);
        }
      }
      const ULONGLONG now_tick = GetTickCount64();
      const ULONGLONG last_ev =
          g_hc.last_hook_event_tick.load(std::memory_order_relaxed);
      if (g_hc.in_corner_idx.load(std::memory_order_relaxed) < 0 &&
          should_reinstall_ll_hook(true, now_tick, last_ev,
                                   g_hc.last_reinstall_tick, 5000, 2500)) {
        HHOOK fresh =
            SetWindowsHookExW(WH_MOUSE_LL, hot_corner_mouse_proc, self_mod, 0);
        if (fresh != nullptr) {
          UnhookWindowsHookEx(g_hc.mouse_hook);
          g_hc.mouse_hook = fresh;
          g_hc.last_reinstall_tick = now_tick;
        } else {
          g_print("[hot] SetWindowsHookExW(WH_MOUSE_LL) reinstall failed gle=%lu\n",
                  GetLastError());
        }
      }
    }
  } else if (g_hc.mouse_hook != nullptr) {
    UnhookWindowsHookEx(g_hc.mouse_hook);
    g_hc.mouse_hook = nullptr;
    g_hc.last_reinstall_tick = 0;
    g_hc.in_corner_idx.store(-1, std::memory_order_relaxed);
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

  const bool self_elevated = is_current_process_elevated();
  HWINEVENTHOOK fg_hook = SetWinEventHook(
      EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
      on_hot_corner_fg_changed, 0, 0, WINEVENT_OUTOFCONTEXT);
  g_hc.fg_cache_init = false;
  g_hc.cached_fg_hwnd = nullptr;
  g_hc.cached_fg_elevated = false;
  update_fg_elevation_cache(GetForegroundWindow());

  sync_hook_install_on_hook_thread();
  constexpr DWORD kWatchdogIntervalMs = 2500;
  constexpr DWORD kUipiFallbackPollMs = 50;
  ULONGLONG last_watchdog_tick = GetTickCount64();

  bool running = true;
  while (running) {
    const bool want_hook =
        g_hc.enabled.load(std::memory_order_acquire) &&
        !g_hc.suppressed.load(std::memory_order_acquire);
    const bool uipi_active =
        want_hook && !self_elevated && g_hc.cached_fg_elevated;
    if (uipi_active && !g_hc.uipi_fallback_logged) {
      g_hc.uipi_fallback_logged = true;
      g_print("[hot] UIPI: foreground window is elevated while WINOME is non-elevated; enabling panel+cursor hot-corner fallback\n");
    }

    const DWORD wait_ms =
        uipi_active ? kUipiFallbackPollMs : kWatchdogIntervalMs;
    const DWORD wait_rc = MsgWaitForMultipleObjectsEx(
        0, nullptr, wait_ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
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

    if (wait_rc == WAIT_TIMEOUT)
      update_fg_elevation_cache(GetForegroundWindow());

    const bool want_now =
        g_hc.enabled.load(std::memory_order_acquire) &&
        !g_hc.suppressed.load(std::memory_order_acquire);
    if (want_now && !self_elevated && g_hc.cached_fg_elevated) {
      POINT pt{};
      if (GetCursorPos(&pt))
        process_hot_corner_point(pt.x, pt.y);
    }

    const ULONGLONG now_tick = GetTickCount64();
    if ((now_tick - last_watchdog_tick) >= kWatchdogIntervalMs) {
      last_watchdog_tick = now_tick;
      sync_hook_install_on_hook_thread(true);
    }
  }

  if (fg_hook != nullptr) {
    UnhookWinEvent(fg_hook);
    fg_hook = nullptr;
  }
  if (g_hc.mouse_hook != nullptr) {
    UnhookWindowsHookEx(g_hc.mouse_hook);
    g_hc.mouse_hook = nullptr;
    g_hc.last_reinstall_tick = 0;
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
    g_hc.in_corner_idx.store(-1, std::memory_order_relaxed);
  if (prev != suppressed)
    post_sync_to_hook_thread();
}

void hot_corner_refresh_bounds(void) {
  refresh_cached_bounds();
}

void hot_corner_notify_pointer(int screen_x, int screen_y) {
  if (!g_hc.enabled.load(std::memory_order_acquire) ||
      g_hc.suppressed.load(std::memory_order_acquire)) {
    return;
  }
  process_hot_corner_point(screen_x, screen_y);
}

bool hot_corner_is_in_corner(void) {
  return g_hc.in_corner_idx.load(std::memory_order_relaxed) >= 0;
}

}  // namespace winome
