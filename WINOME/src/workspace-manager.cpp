// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Multi-monitor workspace manager runtime (Independent / PrimaryOnly / Span).

#include "workspace-manager.h"

#include "config.h"
#include "host-utils.h"
#include "monitor-manager.h"

#include <dwmapi.h>
#include <glib.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace winome {
namespace ws {
namespace {

struct ListenerEntry {
  WorkspaceChangedCallback cb = nullptr;
  void* user_data = nullptr;
};

struct DispatchEvent {
  HMONITOR monitor = nullptr;
  int count = 1;
  int active = 0;
};

struct RuntimeState {
  std::mutex mu;
  bool initialized = false;
  MultiMonitorWorkspaceMode active_mode = MultiMonitorWorkspaceMode::Span;
  std::atomic<MultiMonitorWorkspaceMode> active_mode_atomic{
      MultiMonitorWorkspaceMode::Span};
  int monitor_count = 1;
  HMONITOR last_primary_monitor = nullptr;
  IndependentWorkspaceModel model;
  std::unordered_set<HWND> auto_pinned_secondary;
  std::vector<ListenerEntry> listeners;
  guint primary_only_sync_timer = 0;
  std::atomic<bool> internal_vd_op{false};
};

RuntimeState g_ws;

uintptr_t mon_key(HMONITOR m) {
  return reinterpret_cast<uintptr_t>(m);
}

uintptr_t hwnd_key(HWND h) {
  return reinterpret_cast<uintptr_t>(h);
}

bool is_manageable_user_window(HWND hwnd,
                               bool use_blacklist,
                               DWORD* out_cloaked = nullptr,
                               RECT* out_rect = nullptr) {
  if (hwnd == nullptr || !IsWindow(hwnd))
    return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == 0 || pid == GetCurrentProcessId())
    return false;
  if (!IsWindowVisible(hwnd))
    return false;
  LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  if ((ex & WS_EX_TOOLWINDOW) != 0)
    return false;
  if ((ex & WS_EX_APPWINDOW) == 0 && GetWindow(hwnd, GW_OWNER) != nullptr)
    return false;
  if (is_overview_default_blacklisted_hwnd(hwnd, use_blacklist))
    return false;

  DWORD cloaked = 0;
  DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
  // Ignore app-cloaked (suspended UWP) and inherited-cloaked windows.
  if ((cloaked & (0x1u | 0x4u)) != 0)
    return false;

  RECT r{};
  if (IsIconic(hwnd)) {
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(hwnd, &wp))
      return false;
    r = wp.rcNormalPosition;
  } else if (!SUCCEEDED(DwmGetWindowAttribute(
                 hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) {
    if (!GetWindowRect(hwnd, &r))
      return false;
  }
  if (r.right <= r.left || r.bottom <= r.top)
    return false;
  if (out_cloaked != nullptr)
    *out_cloaked = cloaked;
  if (out_rect != nullptr)
    *out_rect = r;
  return true;
}

HMONITOR monitor_of_window(HWND hwnd) {
  RECT r{};
  if (IsIconic(hwnd)) {
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (GetWindowPlacement(hwnd, &wp))
      r = wp.rcNormalPosition;
  } else if (!SUCCEEDED(DwmGetWindowAttribute(
                 hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) {
    GetWindowRect(hwnd, &r);
  }
  if (r.right > r.left && r.bottom > r.top) {
    HMONITOR m = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
    if (m != nullptr)
      return m;
  }
  return MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
}

struct EnumLiveCtx {
  bool use_blacklist = true;
  std::vector<LiveWindowSnapshot>* out = nullptr;
};

BOOL CALLBACK enum_live_windows_proc(HWND hwnd, LPARAM lp) {
  auto* ctx = reinterpret_cast<EnumLiveCtx*>(lp);
  DWORD cloaked = 0;
  RECT rect{};
  if (!is_manageable_user_window(hwnd, ctx->use_blacklist, &cloaked, &rect))
    return TRUE;
  HMONITOR mon = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
  if (mon == nullptr)
    mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  if (mon == nullptr)
    return TRUE;
  LiveWindowSnapshot snap{};
  snap.hwnd = hwnd_key(hwnd);
  snap.monitor = mon_key(mon);
  snap.on_parking_desktop = (cloaked & 0x2u /* DWM_CLOAKED_SHELL */) != 0;
  ctx->out->push_back(snap);
  return TRUE;
}

std::vector<LiveWindowSnapshot> enumerate_live_windows() {
  std::vector<LiveWindowSnapshot> out;
  EnumLiveCtx ctx{get_config().overview_window_blacklist, &out};
  EnumWindows(enum_live_windows_proc, reinterpret_cast<LPARAM>(&ctx));
  return out;
}

gboolean dispatch_change_idle(gpointer data) {
  auto* ev = static_cast<DispatchEvent*>(data);
  if (ev == nullptr)
    return G_SOURCE_REMOVE;

  std::vector<ListenerEntry> snapshot;
  {
    std::lock_guard<std::mutex> lk(g_ws.mu);
    snapshot = g_ws.listeners;
  }
  for (const auto& l : snapshot) {
    if (l.cb != nullptr)
      l.cb(ev->monitor, ev->count, ev->active, l.user_data);
  }
  delete ev;
  return G_SOURCE_REMOVE;
}

void emit_change(HMONITOR mon, int count, int active) {
  auto* ev = new DispatchEvent{mon, std::max(count, 1), std::max(active, 0)};
  g_main_context_invoke(nullptr, dispatch_change_idle, ev);
}

void execute_batch_moves(const std::vector<BatchMoveIntent>& intents) {
  if (intents.empty())
    return;
  std::vector<vd::WindowMoveOp> ops;
  ops.reserve(intents.size());
  for (const auto& m : intents) {
    ops.push_back(vd::WindowMoveOp{
        reinterpret_cast<HWND>(m.hwnd), m.native_desktop});
  }
  g_ws.internal_vd_op.store(true, std::memory_order_release);
  std::vector<bool> ok = vd::move_windows_batch(ops);
  g_ws.internal_vd_op.store(false, std::memory_order_release);

  std::lock_guard<std::mutex> lk(g_ws.mu);
  for (size_t i = 0; i < intents.size() && i < ok.size(); ++i) {
    if (!ok[i] && intents[i].native_desktop == IndependentWorkspaceModel::kParkingDesktop) {
      // Window refused MoveViewToDesktop (e.g. elevated/protected); treat as
      // sticky on this monitor so it never glitches or gets lost.
      g_ws.model.mark_sticky(intents[i].hwnd);
    }
  }
}

bool ensure_parking_desktop_ready() {
  if (!vd::available())
    return false;
  int n = vd::count();
  if (n < 2) {
    g_ws.internal_vd_op.store(true, std::memory_order_release);
    const bool created = vd::create_desktop();
    g_ws.internal_vd_op.store(false, std::memory_order_release);
    if (!created)
      return false;
    n = vd::count();
  }
  if (n < 2)
    return false;
  if (vd::current_index() != 0) {
    g_ws.internal_vd_op.store(true, std::memory_order_release);
    vd::switch_to(0);
    g_ws.internal_vd_op.store(false, std::memory_order_release);
  }
  return true;
}

void unpin_all_auto_pinned_locked(std::vector<HWND>* out_to_unpin) {
  for (HWND h : g_ws.auto_pinned_secondary) {
    if (h != nullptr && IsWindow(h))
      out_to_unpin->push_back(h);
  }
  g_ws.auto_pinned_secondary.clear();
}

void sync_primary_only_secondary_pins() {
  const auto monitors = enumerate_monitors();
  if (monitors.size() <= 1 || !vd::cached_snapshot().valid)
    return;
  HMONITOR primary = monitor_manager_primary_monitor();
  if (primary == nullptr)
    primary = monitors.front().hmonitor;
  const auto live = enumerate_live_windows();

  std::vector<HWND> to_pin;
  std::vector<HWND> to_unpin;
  std::unordered_set<HWND> live_set;
  live_set.reserve(live.size());

  {
    std::lock_guard<std::mutex> lk(g_ws.mu);
    if (g_ws.active_mode != MultiMonitorWorkspaceMode::PrimaryOnly)
      return;

    for (const auto& w : live) {
      HWND h = reinterpret_cast<HWND>(w.hwnd);
      HMONITOR m = reinterpret_cast<HMONITOR>(w.monitor);
      live_set.insert(h);
      if (m != primary) {
        if (g_ws.auto_pinned_secondary.insert(h).second)
          to_pin.push_back(h);
      } else {
        auto it = g_ws.auto_pinned_secondary.find(h);
        if (it != g_ws.auto_pinned_secondary.end()) {
          g_ws.auto_pinned_secondary.erase(it);
          to_unpin.push_back(h);
        }
      }
    }
    for (auto it = g_ws.auto_pinned_secondary.begin();
         it != g_ws.auto_pinned_secondary.end();) {
      if (live_set.find(*it) == live_set.end())
        it = g_ws.auto_pinned_secondary.erase(it);
      else
        ++it;
    }
  }

  for (HWND h : to_pin)
    vd::pin_window_async(h);
  for (HWND h : to_unpin)
    vd::unpin_window(h);
}

gboolean primary_only_sync_tick(gpointer G_GNUC_UNUSED data) {
  sync_primary_only_secondary_pins();
  return G_SOURCE_CONTINUE;
}

void stop_primary_only_timer_locked() {
  if (g_ws.primary_only_sync_timer != 0) {
    g_source_remove(g_ws.primary_only_sync_timer);
    g_ws.primary_only_sync_timer = 0;
  }
}

void apply_mode_transition() {
  const auto monitors = enumerate_monitors();
  const int mon_count = static_cast<int>(monitors.size());
  const MultiMonitorWorkspaceMode desired_cfg =
      parse_workspace_mode(get_config().multi_monitor_workspace_mode);
  MultiMonitorWorkspaceMode target =
      (mon_count >= 2 && vd::available())
          ? desired_cfg
          : MultiMonitorWorkspaceMode::Span;

  if (target == MultiMonitorWorkspaceMode::Independent &&
      !ensure_parking_desktop_ready()) {
    g_print("[ws] independent mode fallback -> span (parking desktop unavailable)\n");
    target = MultiMonitorWorkspaceMode::Span;
  }

  std::vector<BatchMoveIntent> restore_moves;
  std::vector<HWND> to_unpin;
  bool changed = false;
  const HMONITOR cur_primary = monitor_manager_primary_monitor();
  {
    std::lock_guard<std::mutex> lk(g_ws.mu);
    g_ws.monitor_count = std::max(mon_count, 1);
    const MultiMonitorWorkspaceMode prev = g_ws.active_mode;
    const bool primary_changed =
        (target == MultiMonitorWorkspaceMode::PrimaryOnly &&
         g_ws.last_primary_monitor != cur_primary);
    g_ws.last_primary_monitor = cur_primary;
    if (prev != target) {
      changed = true;
      if (prev == MultiMonitorWorkspaceMode::Independent) {
        restore_moves = g_ws.model.build_restore_all_to_live_moves();
        g_ws.model.clear();
      }
      if (prev == MultiMonitorWorkspaceMode::PrimaryOnly) {
        stop_primary_only_timer_locked();
        unpin_all_auto_pinned_locked(&to_unpin);
      }
      g_ws.active_mode = target;
      g_ws.active_mode_atomic.store(target, std::memory_order_release);
      if (target == MultiMonitorWorkspaceMode::Independent) {
        for (const auto& m : monitors)
          g_ws.model.ensure_monitor(mon_key(m.hmonitor));
      } else if (target == MultiMonitorWorkspaceMode::PrimaryOnly &&
                 g_ws.primary_only_sync_timer == 0) {
        g_ws.primary_only_sync_timer =
            g_timeout_add(3000, primary_only_sync_tick, nullptr);
      }
    } else if (target == MultiMonitorWorkspaceMode::Independent) {
      for (const auto& m : monitors)
        g_ws.model.ensure_monitor(mon_key(m.hmonitor));
    } else if (primary_changed) {
      changed = true;
    }
  }

  if (!restore_moves.empty())
    execute_batch_moves(restore_moves);
  for (HWND h : to_unpin)
    vd::unpin_window(h);

  if (target == MultiMonitorWorkspaceMode::Independent) {
    const auto live = enumerate_live_windows();
    std::lock_guard<std::mutex> lk(g_ws.mu);
    g_ws.model.reconcile(live);
  } else if (target == MultiMonitorWorkspaceMode::PrimaryOnly) {
    sync_primary_only_secondary_pins();
  }

  if (changed) {
    g_print("[ws] multi-monitor workspace mode -> %s (monitors=%d)\n",
            workspace_mode_to_string(target), mon_count);
    for (const auto& m : monitors) {
      emit_change(m.hmonitor, count_for_monitor(m.hmonitor),
                  active_for_monitor(m.hmonitor));
    }
  }
}

void on_ws_config_changed(void* data) {
  (void)data;
  const WinomeConfig& cfg = get_config();
  static bool initialized = false;
  static std::string last_mode;
  static bool last_sync_primary = false;
  if (initialized &&
      cfg.multi_monitor_workspace_mode == last_mode &&
      cfg.sync_windows_primary_display == last_sync_primary) {
    return;
  }
  initialized = true;
  last_mode = cfg.multi_monitor_workspace_mode;
  last_sync_primary = cfg.sync_windows_primary_display;
  apply_mode_transition();
}

// Handles native Windows virtual desktop switches:
//  - In Independent mode: if Windows jumped onto Parking Desktop 1 (e.g. user
//    clicked a tray icon or notification for a parked app), automatically
//    switch that app's monitor to its workspace and return Windows to Desktop 0.
//  - In PrimaryOnly / Span modes: forward updates to WINOME workspace listeners.
void on_native_vd_changed(int count, int active_index, void* data) {
  (void)data;
  if (g_ws.internal_vd_op.load(std::memory_order_acquire))
    return;

  MultiMonitorWorkspaceMode mode;
  {
    std::lock_guard<std::mutex> lk(g_ws.mu);
    mode = g_ws.active_mode;
  }

  if (mode == MultiMonitorWorkspaceMode::Independent) {
    if (active_index != IndependentWorkspaceModel::kLiveDesktop) {
      HWND fg = GetForegroundWindow();
      WindowPlacement wp{};
      bool found = false;
      std::vector<BatchMoveIntent> moves;
      int new_count = 2;
      int new_active = 0;
      {
        std::lock_guard<std::mutex> lk(g_ws.mu);
        found = (fg != nullptr) && g_ws.model.find_window(hwnd_key(fg), &wp);
        if (found) {
          moves = g_ws.model.switch_monitor(wp.monitor, wp.workspace);
          const auto st = g_ws.model.state_for(wp.monitor);
          new_count = st.count;
          new_active = st.active;
        }
      }
      if (!moves.empty())
        execute_batch_moves(moves);

      g_ws.internal_vd_op.store(true, std::memory_order_release);
      vd::switch_to(IndependentWorkspaceModel::kLiveDesktop);
      g_ws.internal_vd_op.store(false, std::memory_order_release);

      if (found) {
        g_print("[ws] auto-followed parked window %p -> mon=%p ws=%d\n",
                static_cast<void*>(fg),
                reinterpret_cast<void*>(wp.monitor), new_active);
        emit_change(reinterpret_cast<HMONITOR>(wp.monitor), new_count, new_active);
        if (IsWindow(fg)) {
          AllowSetForegroundWindow(ASFW_ANY);
          SetForegroundWindow(fg);
        }
      }
    }
    return;
  }

  if (mode == MultiMonitorWorkspaceMode::PrimaryOnly) {
    emit_change(monitor_manager_primary_monitor(), count, active_index);
    sync_primary_only_secondary_pins();
    return;
  }

  // Span mode: notify all monitors (monitor == nullptr).
  emit_change(nullptr, count, active_index);
}

HMONITOR resolve_monitor(HMONITOR mon) {
  if (mon != nullptr)
    return mon;
  return monitor_manager_active_monitor();
}

}  // namespace

void init() {
  {
    std::lock_guard<std::mutex> lk(g_ws.mu);
    if (g_ws.initialized)
      return;
    g_ws.initialized = true;
  }
  add_config_listener(on_ws_config_changed, nullptr);
  vd::add_change_listener(on_native_vd_changed, nullptr);
  apply_mode_transition();
}

void shutdown() {
  std::vector<BatchMoveIntent> restore_moves;
  std::vector<HWND> to_unpin;
  bool was_indep = false;
  {
    std::lock_guard<std::mutex> lk(g_ws.mu);
    if (!g_ws.initialized)
      return;
    g_ws.initialized = false;
    stop_primary_only_timer_locked();
    was_indep = (g_ws.active_mode == MultiMonitorWorkspaceMode::Independent);
    if (was_indep) {
      restore_moves = g_ws.model.build_restore_all_to_live_moves();
      g_ws.model.clear();
    }
    unpin_all_auto_pinned_locked(&to_unpin);
    g_ws.active_mode = MultiMonitorWorkspaceMode::Span;
    g_ws.active_mode_atomic.store(MultiMonitorWorkspaceMode::Span,
                                  std::memory_order_release);
    g_ws.listeners.clear();
  }

  remove_config_listener(on_ws_config_changed, nullptr);
  vd::remove_change_listener(on_native_vd_changed, nullptr);

  if (!restore_moves.empty()) {
    g_print("[ws] shutdown: restoring %zu parked windows to desktop 0\n",
            restore_moves.size());
    execute_batch_moves(restore_moves);
  }
  if (was_indep && vd::available() && vd::current_index() != 0)
    vd::switch_to(0);
  for (HWND h : to_unpin)
    vd::unpin_window(h);
}

void on_monitors_changed() {
  bool ready = false;
  {
    std::lock_guard<std::mutex> lk(g_ws.mu);
    ready = g_ws.initialized;
  }
  if (ready)
    apply_mode_transition();
}

MultiMonitorWorkspaceMode effective_mode() {
  return g_ws.active_mode_atomic.load(std::memory_order_acquire);
}

bool is_independent_active() {
  return effective_mode() == MultiMonitorWorkspaceMode::Independent;
}

int count_for_monitor(HMONITOR mon) {
  const MultiMonitorWorkspaceMode mode = effective_mode();
  mon = resolve_monitor(mon);
  if (mode == MultiMonitorWorkspaceMode::Independent) {
    std::lock_guard<std::mutex> lk(g_ws.mu);
    g_ws.model.ensure_monitor(mon_key(mon));
    return g_ws.model.state_for(mon_key(mon)).count;
  }
  if (mode == MultiMonitorWorkspaceMode::PrimaryOnly &&
      mon != monitor_manager_primary_monitor()) {
    return 1;
  }
  const auto snap = vd::cached_snapshot();
  return snap.valid ? std::max(snap.count, 1) : std::max(vd::count(), 1);
}

int active_for_monitor(HMONITOR mon) {
  const MultiMonitorWorkspaceMode mode = effective_mode();
  mon = resolve_monitor(mon);
  if (mode == MultiMonitorWorkspaceMode::Independent) {
    std::lock_guard<std::mutex> lk(g_ws.mu);
    g_ws.model.ensure_monitor(mon_key(mon));
    return g_ws.model.state_for(mon_key(mon)).active;
  }
  if (mode == MultiMonitorWorkspaceMode::PrimaryOnly &&
      mon != monitor_manager_primary_monitor()) {
    return 0;
  }
  const auto snap = vd::cached_snapshot();
  const int idx = snap.valid ? snap.active_index : vd::current_index();
  return idx >= 0 ? idx : 0;
}

std::vector<vd::DesktopInfo> desktops_for_monitor(HMONITOR mon) {
  const MultiMonitorWorkspaceMode mode = effective_mode();
  mon = resolve_monitor(mon);
  if (mode == MultiMonitorWorkspaceMode::Independent) {
    const int n = count_for_monitor(mon);
    std::vector<vd::DesktopInfo> out;
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      vd::DesktopInfo di{};
      di.index = i;
      di.id.Data1 = static_cast<unsigned long>(i + 1);
      di.id.Data2 = static_cast<unsigned short>(mon_key(mon) & 0xFFFFu);
      out.push_back(di);
    }
    return out;
  }
  if (mode == MultiMonitorWorkspaceMode::PrimaryOnly &&
      mon != monitor_manager_primary_monitor()) {
    vd::DesktopInfo di{};
    di.index = 0;
    return {di};
  }
  return vd::desktops();
}

bool switch_to(HMONITOR mon, int target_index) {
  const MultiMonitorWorkspaceMode mode = effective_mode();
  mon = resolve_monitor(mon);

  if (mode == MultiMonitorWorkspaceMode::Independent) {
    if (!ensure_parking_desktop_ready())
      return false;
    const auto live = enumerate_live_windows();
    std::vector<BatchMoveIntent> moves;
    MonitorWorkspaceState st{};
    int old_active = 0;
    {
      std::lock_guard<std::mutex> lk(g_ws.mu);
      g_ws.model.reconcile(live);
      g_ws.model.ensure_monitor(mon_key(mon));
      old_active = g_ws.model.state_for(mon_key(mon)).active;
      moves = g_ws.model.switch_monitor(mon_key(mon), target_index);
      g_ws.model.sync_dynamic(mon_key(mon));
      st = g_ws.model.state_for(mon_key(mon));
    }
    if (st.active == old_active && moves.empty())
      return false;
    execute_batch_moves(moves);
    g_print("[ws] independent switch mon=%p %d -> %d (count=%d moves=%zu)\n",
            static_cast<void*>(mon), old_active, st.active, st.count,
            moves.size());
    emit_change(mon, st.count, st.active);
    return true;
  }

  if (mode == MultiMonitorWorkspaceMode::PrimaryOnly &&
      mon != monitor_manager_primary_monitor()) {
    return false;
  }

  if (vd::switch_to(target_index)) {
    vd::notify_change_hint_async();
    return true;
  }
  return false;
}

bool create_workspace(HMONITOR mon) {
  const MultiMonitorWorkspaceMode mode = effective_mode();
  mon = resolve_monitor(mon);
  if (mode == MultiMonitorWorkspaceMode::Independent) {
    MonitorWorkspaceState st{};
    {
      std::lock_guard<std::mutex> lk(g_ws.mu);
      g_ws.model.create_workspace(mon_key(mon));
      st = g_ws.model.state_for(mon_key(mon));
    }
    emit_change(mon, st.count, st.active);
    return true;
  }
  if (mode == MultiMonitorWorkspaceMode::PrimaryOnly &&
      mon != monitor_manager_primary_monitor()) {
    return false;
  }
  return vd::create_desktop();
}

bool remove_workspace(HMONITOR mon, int index) {
  const MultiMonitorWorkspaceMode mode = effective_mode();
  mon = resolve_monitor(mon);
  if (mode == MultiMonitorWorkspaceMode::Independent) {
    std::vector<BatchMoveIntent> moves;
    MonitorWorkspaceState st{};
    {
      std::lock_guard<std::mutex> lk(g_ws.mu);
      moves = g_ws.model.remove_workspace(mon_key(mon), index);
      g_ws.model.sync_dynamic(mon_key(mon));
      st = g_ws.model.state_for(mon_key(mon));
    }
    execute_batch_moves(moves);
    emit_change(mon, st.count, st.active);
    return true;
  }
  if (mode == MultiMonitorWorkspaceMode::PrimaryOnly &&
      mon != monitor_manager_primary_monitor()) {
    return false;
  }
  return vd::remove_desktop(index);
}

int window_workspace(HWND hwnd, HMONITOR mon_hint) {
  if (hwnd == nullptr)
    return -1;
  const MultiMonitorWorkspaceMode mode = effective_mode();
  if (mode == MultiMonitorWorkspaceMode::Independent) {
    HMONITOR m = mon_hint != nullptr ? mon_hint : monitor_of_window(hwnd);
    std::lock_guard<std::mutex> lk(g_ws.mu);
    return g_ws.model.window_workspace(hwnd_key(hwnd), mon_key(m));
  }
  if (mode == MultiMonitorWorkspaceMode::PrimaryOnly) {
    HMONITOR m = mon_hint != nullptr ? mon_hint : monitor_of_window(hwnd);
    if (m != monitor_manager_primary_monitor())
      return 0;
  }
  return vd::window_desktop_index(hwnd);
}

bool window_on_active_workspace(HWND hwnd, HMONITOR mon_hint) {
  if (hwnd == nullptr)
    return false;
  const MultiMonitorWorkspaceMode mode = effective_mode();
  if (mode == MultiMonitorWorkspaceMode::Independent) {
    std::lock_guard<std::mutex> lk(g_ws.mu);
    if (g_ws.model.is_sticky(hwnd_key(hwnd)))
      return true;
    WindowPlacement wp{};
    if (!g_ws.model.find_window(hwnd_key(hwnd), &wp))
      return true;
    return wp.workspace == g_ws.model.state_for(wp.monitor).active;
  }
  if (mode == MultiMonitorWorkspaceMode::PrimaryOnly) {
    HMONITOR m = mon_hint != nullptr ? mon_hint : monitor_of_window(hwnd);
    if (m != monitor_manager_primary_monitor())
      return true;
  }
  return vd::window_on_current(hwnd);
}

bool move_window_to_workspace(HWND hwnd, HMONITOR mon, int target_index) {
  if (hwnd == nullptr || target_index < 0)
    return false;
  const MultiMonitorWorkspaceMode mode = effective_mode();
  mon = resolve_monitor(mon != nullptr ? mon : monitor_of_window(hwnd));

  if (mode == MultiMonitorWorkspaceMode::Independent) {
    if (!ensure_parking_desktop_ready())
      return false;
    const auto live = enumerate_live_windows();
    std::vector<BatchMoveIntent> moves;
    MonitorWorkspaceState st{};
    HMONITOR prev_mon = nullptr;
    MonitorWorkspaceState prev_st{};
    {
      std::lock_guard<std::mutex> lk(g_ws.mu);
      WindowPlacement wp{};
      if (g_ws.model.find_window(hwnd_key(hwnd), &wp))
        prev_mon = reinterpret_cast<HMONITOR>(wp.monitor);
      g_ws.model.reconcile(live);
      moves = g_ws.model.move_window(hwnd_key(hwnd), mon_key(mon), target_index);
      st = g_ws.model.state_for(mon_key(mon));
      if (prev_mon != nullptr && prev_mon != mon) {
        g_ws.model.sync_dynamic(mon_key(prev_mon));
        prev_st = g_ws.model.state_for(mon_key(prev_mon));
      }
    }
    execute_batch_moves(moves);
    emit_change(mon, st.count, st.active);
    if (prev_mon != nullptr && prev_mon != mon)
      emit_change(prev_mon, prev_st.count, prev_st.active);
    return true;
  }

  if (mode == MultiMonitorWorkspaceMode::PrimaryOnly) {
    if (mon != monitor_manager_primary_monitor()) {
      std::lock_guard<std::mutex> lk(g_ws.mu);
      g_ws.auto_pinned_secondary.insert(hwnd);
      vd::pin_window_async(hwnd);
      return true;
    }
    bool need_unpin = false;
    {
      std::lock_guard<std::mutex> lk(g_ws.mu);
      need_unpin = (g_ws.auto_pinned_secondary.erase(hwnd) > 0);
    }
    if (need_unpin)
      vd::unpin_window(hwnd);
    return vd::move_window_to_desktop(hwnd, target_index);
  }
  return vd::move_window_to_desktop(hwnd, target_index);
}

bool transfer_window_to_monitor_workspace(HWND hwnd,
                                          HMONITOR dst_mon,
                                          int dst_ws,
                                          POINT drop_pt,
                                          bool use_drop_point) {
  if (hwnd == nullptr || !IsWindow(hwnd) || dst_mon == nullptr)
    return false;

  MONITORINFO dst_mi{};
  dst_mi.cbSize = sizeof(dst_mi);
  if (!GetMonitorInfoW(dst_mon, &dst_mi))
    return false;

  RECT dst_work = dst_mi.rcWork;
  HWND dst_panel = monitor_manager_panel_for_monitor(dst_mon);
  if (dst_panel != nullptr && IsWindowVisible(dst_panel)) {
    RECT pr{};
    if (GetWindowRect(dst_panel, &pr) && pr.bottom > dst_work.top)
      dst_work.top = pr.bottom;
  }

  HMONITOR src_mon = monitor_of_window(hwnd);
  const bool cross_mon = (src_mon != dst_mon);
  if (cross_mon) {
    MONITORINFO src_mi{};
    src_mi.cbSize = sizeof(src_mi);
    RECT src_mon_rect = dst_mi.rcMonitor;
    if (src_mon != nullptr && GetMonitorInfoW(src_mon, &src_mi))
      src_mon_rect = src_mi.rcMonitor;

    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    const bool have_wp = GetWindowPlacement(hwnd, &wp) != FALSE;
    const bool was_iconic = IsIconic(hwnd) != FALSE;
    const bool was_zoomed = IsZoomed(hwnd) != FALSE;

    RECT src_win{};
    if (was_iconic || was_zoomed) {
      if (have_wp)
        src_win = wp.rcNormalPosition;
      else
        GetWindowRect(hwnd, &src_win);
    } else if (!GetWindowRect(hwnd, &src_win)) {
      src_win = RECT{0, 0, 960, 640};
    }

    const RECT dst_rect = compute_cross_monitor_window_rect(
        src_win, src_mon_rect, dst_work, drop_pt, use_drop_point);

    if (was_iconic && have_wp) {
      wp.rcNormalPosition = dst_rect;
      SetWindowPlacement(hwnd, &wp);
    } else if (was_zoomed) {
      ShowWindow(hwnd, SW_RESTORE);
      SetWindowPos(hwnd, nullptr, dst_rect.left, dst_rect.top,
                   dst_rect.right - dst_rect.left,
                   dst_rect.bottom - dst_rect.top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      ShowWindow(hwnd, SW_MAXIMIZE);
    } else {
      SetWindowPos(hwnd, nullptr, dst_rect.left, dst_rect.top,
                   dst_rect.right - dst_rect.left,
                   dst_rect.bottom - dst_rect.top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    }
  } else if (!IsIconic(hwnd) && !IsZoomed(hwnd)) {
    // If Windows already dragged >50% of a live window onto dst_mon, ensure
    // it does not remain overhanging outside dst_work (e.g. onto the source
    // monitor or underneath the top panel).
    RECT cur{};
    if (GetWindowRect(hwnd, &cur)) {
      const bool overhangs =
          cur.left < dst_work.left || cur.top < dst_work.top ||
          cur.right > dst_work.right || cur.bottom > dst_work.bottom;
      if (overhangs) {
        const RECT clamped = compute_cross_monitor_window_rect(
            cur, dst_mi.rcMonitor, dst_work, drop_pt, use_drop_point);
        SetWindowPos(hwnd, nullptr, clamped.left, clamped.top,
                     clamped.right - clamped.left,
                     clamped.bottom - clamped.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
      }
    }
  }

  const int target_ws =
      (dst_ws >= 0) ? dst_ws : active_for_monitor(dst_mon);
  bool ok = move_window_to_workspace(hwnd, dst_mon, target_ws);
  if (!ok && cross_mon) {
    // Even if native VD move wasn't needed (e.g. same native desktop in Span),
    // the cross-monitor reposition itself succeeded.
    ok = true;
  }
  g_print("[ws] transfer hwnd=%p mon %p -> %p ws=%d (drop=%ld,%ld ok=%d)\n",
          static_cast<void*>(hwnd), static_cast<void*>(src_mon),
          static_cast<void*>(dst_mon), target_ws, drop_pt.x, drop_pt.y,
          ok ? 1 : 0);
  return ok;
}

bool sync_dynamic_workspaces(HMONITOR mon) {
  const MultiMonitorWorkspaceMode mode = effective_mode();
  mon = resolve_monitor(mon);
  if (mode == MultiMonitorWorkspaceMode::Independent) {
    const auto live = enumerate_live_windows();
    bool changed = false;
    MonitorWorkspaceState st{};
    {
      std::lock_guard<std::mutex> lk(g_ws.mu);
      g_ws.model.reconcile(live);
      changed = g_ws.model.sync_dynamic(mon_key(mon));
      st = g_ws.model.state_for(mon_key(mon));
    }
    if (changed)
      emit_change(mon, st.count, st.active);
    return changed;
  }
  return false;
}

bool can_manage_workspaces() {
  if (is_independent_active())
    return true;
  return vd::vtable_slots_for_build(vd::windows_build_number())
      .can_manage_desktops;
}

void add_change_listener(WorkspaceChangedCallback cb, void* user_data) {
  if (cb == nullptr)
    return;
  {
    std::lock_guard<std::mutex> lk(g_ws.mu);
    for (const auto& e : g_ws.listeners) {
      if (e.cb == cb && e.user_data == user_data)
        return;
    }
    g_ws.listeners.push_back(ListenerEntry{cb, user_data});
  }
}

void remove_change_listener(WorkspaceChangedCallback cb, void* user_data) {
  std::lock_guard<std::mutex> lk(g_ws.mu);
  g_ws.listeners.erase(
      std::remove_if(g_ws.listeners.begin(), g_ws.listeners.end(),
                     [cb, user_data](const ListenerEntry& e) {
                       return e.cb == cb && e.user_data == user_data;
                     }),
      g_ws.listeners.end());
}

}  // namespace ws
}  // namespace winome
