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
#include <mutex>
#include <thread>

namespace winome {
namespace {

constexpr int kCornerSize = 5;
constexpr UINT kMsgSyncHookState = WM_APP + 41;

struct HotCornerShared {
  std::atomic<HWND> panel_hwnd{nullptr};
  std::atomic<LONG> left{0};
  std::atomic<LONG> top{0};
  std::atomic<LONG> right{0};
  std::atomic<LONG> bottom{0};
  std::atomic<bool> suppressed{false};
  std::atomic<bool> in_corner{false};
  std::atomic<gint64> last_trigger_ms{0};
  std::atomic<int> cooldown_ms{500};
  std::atomic<bool> enabled{true};
  std::atomic<DWORD> hook_tid{0};

  std::mutex thread_mu;
  std::thread hook_thread;
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

void refresh_cached_bounds() {
  HWND panel = g_hc.panel_hwnd.load(std::memory_order_acquire);
  if (panel == nullptr || !IsWindow(panel))
    return;
  HMONITOR mon = MonitorFromWindow(panel, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (mon != nullptr && GetMonitorInfoW(mon, &mi)) {
    g_hc.left.store(mi.rcMonitor.left, std::memory_order_relaxed);
    g_hc.top.store(mi.rcMonitor.top, std::memory_order_relaxed);
    g_hc.right.store(mi.rcMonitor.right, std::memory_order_relaxed);
    g_hc.bottom.store(mi.rcMonitor.bottom, std::memory_order_release);
  }
}

void sync_hook_install_on_hook_thread() {
  const bool want_hook =
      g_hc.enabled.load(std::memory_order_acquire) &&
      !g_hc.suppressed.load(std::memory_order_acquire);

  if (want_hook && g_hc.mouse_hook == nullptr) {
    g_hc.in_corner.store(false, std::memory_order_relaxed);
    g_hc.mouse_hook = SetWindowsHookExW(
        WH_MOUSE_LL,
        +[](int nCode, WPARAM wParam, LPARAM lParam) -> LRESULT {
          if (nCode == HC_ACTION && wParam == WM_MOUSEMOVE) {
            const MSLLHOOKSTRUCT* ms =
                reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
            RECT mon{
                g_hc.left.load(std::memory_order_relaxed),
                g_hc.top.load(std::memory_order_relaxed),
                g_hc.right.load(std::memory_order_relaxed),
                g_hc.bottom.load(std::memory_order_acquire),
            };
            const bool inside =
                hot_corner_hit_test(ms->pt.x, ms->pt.y, mon, kCornerSize);
            const bool prev =
                g_hc.in_corner.exchange(inside, std::memory_order_acq_rel);
            if (inside && !prev &&
                g_hc.enabled.load(std::memory_order_relaxed) &&
                !g_hc.suppressed.load(std::memory_order_relaxed)) {
              const gint64 now_ms = g_get_monotonic_time() / 1000;
              const gint64 prev_ms =
                  g_hc.last_trigger_ms.load(std::memory_order_relaxed);
              const int cd = g_hc.cooldown_ms.load(std::memory_order_relaxed);
              if (now_ms - prev_ms >= static_cast<gint64>(cd)) {
                g_hc.last_trigger_ms.store(now_ms, std::memory_order_relaxed);
                g_main_context_invoke(nullptr, invoke_open_overview, nullptr);
              }
            }
          }
          return CallNextHookEx(nullptr, nCode, wParam, lParam);
        },
        nullptr, 0);
  } else if (!want_hook && g_hc.mouse_hook != nullptr) {
    UnhookWindowsHookEx(g_hc.mouse_hook);
    g_hc.mouse_hook = nullptr;
    g_hc.in_corner.store(false, std::memory_order_relaxed);
  }
}

void hook_thread_main() {
  MSG dummy{};
  PeekMessageW(&dummy, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
  g_hc.hook_tid.store(GetCurrentThreadId(), std::memory_order_release);

  sync_hook_install_on_hook_thread();

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.message == kMsgSyncHookState) {
      sync_hook_install_on_hook_thread();
      continue;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  if (g_hc.mouse_hook != nullptr) {
    UnhookWindowsHookEx(g_hc.mouse_hook);
    g_hc.mouse_hook = nullptr;
  }
  g_hc.hook_tid.store(0, std::memory_order_release);
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
  std::lock_guard<std::mutex> lock(g_hc.thread_mu);
  g_hc.panel_hwnd.store(panel_hwnd, std::memory_order_release);
  const auto& cfg = get_config();
  g_hc.enabled.store(cfg.hot_corner_enabled, std::memory_order_release);
  g_hc.cooldown_ms.store(cfg.hot_corner_cooldown_ms, std::memory_order_release);
  g_hc.suppressed.store(winome::panel_hidden(), std::memory_order_release);
  refresh_cached_bounds();

  if (!g_hc.hook_thread.joinable()) {
    add_config_listener(on_hot_corner_config_changed, nullptr);
    g_hc.hook_thread = std::thread(hook_thread_main);
  } else {
    post_sync_to_hook_thread();
  }
}

void stop_hot_corner(void) {
  remove_config_listener(on_hot_corner_config_changed, nullptr);
  std::thread to_join;
  {
    std::lock_guard<std::mutex> lock(g_hc.thread_mu);
    DWORD tid = g_hc.hook_tid.exchange(0, std::memory_order_acq_rel);
    if (tid != 0)
      PostThreadMessageW(tid, WM_QUIT, 0, 0);
    to_join = std::move(g_hc.hook_thread);
  }
  if (to_join.joinable())
    to_join.join();
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
