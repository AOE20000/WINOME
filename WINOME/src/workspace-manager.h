// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Multi-monitor workspace manager supporting 3 modes:
//   1. "independent"  (WINOME built-in per-monitor workspace engine: keeps
//                     Windows native Desktop 0 as the always-active Live Stage
//                     and parks inactive per-monitor workspaces on native
//                     Desktop 1 via single-pass batch MoveViewToDesktop so
//                     Monitor 1 switching 1A -> 1B never touches Monitor 2).
//   2. "primary_only" (GNOME Shell default: workspaces live on the primary
//                     monitor; secondary-monitor user windows are auto-pinned
//                     across virtual desktops and unpinned on return/exit).
//   3. "span"         (Windows / GNOME span-displays: all monitors switch
//                     native virtual desktops together).

#pragma once

#include "virtual-desktop.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace winome {
namespace ws {

enum class MultiMonitorWorkspaceMode {
  Independent,
  PrimaryOnly,
  Span,
};

inline MultiMonitorWorkspaceMode parse_workspace_mode(const std::string& s) {
  if (s == "primary_only")
    return MultiMonitorWorkspaceMode::PrimaryOnly;
  if (s == "span")
    return MultiMonitorWorkspaceMode::Span;
  return MultiMonitorWorkspaceMode::Independent;
}

inline const char* workspace_mode_to_string(MultiMonitorWorkspaceMode m) {
  switch (m) {
    case MultiMonitorWorkspaceMode::PrimaryOnly: return "primary_only";
    case MultiMonitorWorkspaceMode::Span:        return "span";
    case MultiMonitorWorkspaceMode::Independent:
    default:                                     return "independent";
  }
}

// --- Pure unit-testable core model for Independent per-monitor workspaces ---

struct LiveWindowSnapshot {
  uintptr_t hwnd = 0;
  uintptr_t monitor = 0;
  bool on_parking_desktop = false;  // true if currently cloaked/on Desktop 1
};

struct BatchMoveIntent {
  uintptr_t hwnd = 0;
  int native_desktop = 0;  // 0 = Live Stage, 1 = Parking Desktop
};

struct MonitorWorkspaceState {
  int count = 2;
  int active = 0;
};

struct WindowPlacement {
  uintptr_t monitor = 0;
  int workspace = 0;
};

class IndependentWorkspaceModel {
 public:
  static constexpr int kLiveDesktop = 0;
  static constexpr int kParkingDesktop = 1;

  void ensure_monitor(uintptr_t mon) {
    if (mon == 0)
      return;
    if (monitors_.find(mon) == monitors_.end())
      monitors_[mon] = MonitorWorkspaceState{2, 0};
  }

  MonitorWorkspaceState state_for(uintptr_t mon) const {
    auto it = monitors_.find(mon);
    if (it != monitors_.end())
      return it->second;
    return MonitorWorkspaceState{2, 0};
  }

  // Reconcile tracked window table with live enumerated top-level windows:
  //   - Removes closed HWNDs.
  //   - Newly seen windows on Live Stage join their monitor's active workspace.
  //   - Visible (unparked) windows dragged to a different monitor automatically
  //     adopt the destination monitor's active workspace.
  //   - Parked windows stay assigned to their recorded (monitor, workspace).
  void reconcile(const std::vector<LiveWindowSnapshot>& live_windows) {
    std::unordered_set<uintptr_t> alive;
    alive.reserve(live_windows.size());
    for (const auto& w : live_windows) {
      if (w.hwnd == 0 || w.monitor == 0)
        continue;
      alive.insert(w.hwnd);
      ensure_monitor(w.monitor);
      const MonitorWorkspaceState& ms = monitors_[w.monitor];
      auto it = windows_.find(w.hwnd);
      if (it == windows_.end()) {
        // Brand-new window: visible windows belong to this monitor's active ws;
        // if an untracked window was already on parking desktop, keep it on
        // the nearest inactive workspace (1 if active==0 else 0).
        int ws = w.on_parking_desktop ? (ms.active == 0 ? 1 : 0) : ms.active;
        if (ws >= ms.count)
          monitors_[w.monitor].count = ws + 1;
        windows_[w.hwnd] = WindowPlacement{w.monitor, ws};
      } else {
        if (!w.on_parking_desktop && it->second.monitor != w.monitor) {
          // User dragged this live window onto another monitor.
          it->second.monitor = w.monitor;
          it->second.workspace = ms.active;
        } else if (!w.on_parking_desktop && it->second.workspace != ms.active &&
                   sticky_.find(w.hwnd) == sticky_.end()) {
          // Window appeared on Live Stage while assigned elsewhere (e.g.
          // restored by user/app onto active workspace).
          it->second.workspace = ms.active;
        }
      }
    }

    for (auto it = windows_.begin(); it != windows_.end();) {
      if (alive.find(it->first) == alive.end()) {
        sticky_.erase(it->first);
        it = windows_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void mark_sticky(uintptr_t hwnd) {
    if (hwnd != 0)
      sticky_.insert(hwnd);
  }

  bool is_sticky(uintptr_t hwnd) const {
    return sticky_.find(hwnd) != sticky_.end();
  }

  int window_workspace(uintptr_t hwnd, uintptr_t fallback_mon = 0) const {
    auto it = windows_.find(hwnd);
    if (it != windows_.end())
      return it->second.workspace;
    if (fallback_mon != 0)
      return state_for(fallback_mon).active;
    return 0;
  }

  bool find_window(uintptr_t hwnd, WindowPlacement* out) const {
    auto it = windows_.find(hwnd);
    if (it == windows_.end())
      return false;
    if (out != nullptr)
      *out = it->second;
    return true;
  }

  // Switch ONLY @mon from its current workspace to @target_ws.
  // Returns the exact batch of native desktop moves required (only touching
  // windows owned by @mon). Other monitors are untouched.
  std::vector<BatchMoveIntent> switch_monitor(uintptr_t mon, int target_ws) {
    std::vector<BatchMoveIntent> moves;
    if (mon == 0)
      return moves;
    ensure_monitor(mon);
    MonitorWorkspaceState& ms = monitors_[mon];
    target_ws = std::clamp(target_ws, 0, std::max(ms.count - 1, 0));
    if (target_ws == ms.active)
      return moves;

    const int old_ws = ms.active;
    ms.active = target_ws;

    for (const auto& kv : windows_) {
      if (kv.second.monitor != mon)
        continue;
      if (sticky_.find(kv.first) != sticky_.end())
        continue;
      if (kv.second.workspace == old_ws) {
        moves.push_back(BatchMoveIntent{kv.first, kParkingDesktop});
      } else if (kv.second.workspace == target_ws) {
        moves.push_back(BatchMoveIntent{kv.first, kLiveDesktop});
      }
    }
    return moves;
  }

  // Move a single window to @target_ws on @mon (auto-growing workspace count
  // if @target_ws == ms.count). Returns the native move (0 or 1) if needed.
  std::vector<BatchMoveIntent> move_window(uintptr_t hwnd,
                                           uintptr_t mon,
                                           int target_ws) {
    std::vector<BatchMoveIntent> moves;
    if (hwnd == 0 || mon == 0 || target_ws < 0)
      return moves;
    uintptr_t prev_mon = 0;
    auto prev_it = windows_.find(hwnd);
    if (prev_it != windows_.end())
      prev_mon = prev_it->second.monitor;
    ensure_monitor(mon);
    MonitorWorkspaceState& ms = monitors_[mon];
    if (target_ws >= ms.count)
      ms.count = target_ws + 1;
    windows_[hwnd] = WindowPlacement{mon, target_ws};
    if (sticky_.find(hwnd) == sticky_.end()) {
      moves.push_back(BatchMoveIntent{
          hwnd, target_ws == ms.active ? kLiveDesktop : kParkingDesktop});
    }
    sync_dynamic(mon);
    if (prev_mon != 0 && prev_mon != mon)
      sync_dynamic(prev_mon);
    return moves;
  }

  // Create a new workspace at the end of @mon and return its index.
  int create_workspace(uintptr_t mon) {
    ensure_monitor(mon);
    MonitorWorkspaceState& ms = monitors_[mon];
    const int idx = ms.count;
    ++ms.count;
    return idx;
  }

  // Remove workspace @index on @mon, shifting its windows to an adjacent
  // fallback workspace and decrementing higher indices.
  std::vector<BatchMoveIntent> remove_workspace(uintptr_t mon, int index) {
    std::vector<BatchMoveIntent> moves;
    ensure_monitor(mon);
    MonitorWorkspaceState& ms = monitors_[mon];
    if (ms.count <= 1 || index < 0 || index >= ms.count)
      return moves;

    const int fallback = (index > 0) ? (index - 1) : 0;
    const bool was_active = (ms.active == index);
    if (ms.active > index)
      --ms.active;
    else if (was_active)
      ms.active = fallback;
    --ms.count;

    for (auto& kv : windows_) {
      if (kv.second.monitor != mon)
        continue;
      if (kv.second.workspace == index) {
        kv.second.workspace = fallback;
        if (sticky_.find(kv.first) == sticky_.end() &&
            fallback == ms.active) {
          moves.push_back(BatchMoveIntent{kv.first, kLiveDesktop});
        }
      } else if (kv.second.workspace > index) {
        --kv.second.workspace;
      }
    }
    return moves;
  }

  // GNOME dynamic workspaces per monitor:
  //   - Always keep at least 2 workspaces (and always 1 trailing empty ws).
  //   - Never prune the currently active workspace while the user is on it.
  //   - Prune intermediate empty workspaces (except active) and extra trailing
  //     empty workspaces (except active).
  bool sync_dynamic(uintptr_t mon) {
    if (mon == 0)
      return false;
    ensure_monitor(mon);
    MonitorWorkspaceState& ms = monitors_[mon];
    if (ms.count < 1)
      ms.count = 1;

    std::vector<int> counts(static_cast<size_t>(ms.count), 0);
    for (const auto& kv : windows_) {
      if (kv.second.monitor == mon && kv.second.workspace >= 0 &&
          kv.second.workspace < ms.count) {
        ++counts[static_cast<size_t>(kv.second.workspace)];
      }
    }

    bool changed = false;
    // Remove intermediate empty workspaces (highest index first) except active.
    for (int i = ms.count - 2; i >= 0; --i) {
      if (ms.count <= 2)
        break;
      if (i != ms.active && counts[static_cast<size_t>(i)] == 0) {
        // Shift windows above i down by 1.
        for (auto& kv : windows_) {
          if (kv.second.monitor == mon && kv.second.workspace > i)
            --kv.second.workspace;
        }
        if (ms.active > i)
          --ms.active;
        counts.erase(counts.begin() + i);
        --ms.count;
        changed = true;
      }
    }

    // Remove extra trailing empty workspaces beyond a single trailing empty ws,
    // never removing the active workspace.
    while (ms.count > 2 &&
           counts[static_cast<size_t>(ms.count - 1)] == 0 &&
           counts[static_cast<size_t>(ms.count - 2)] == 0 &&
           ms.active != ms.count - 1) {
      counts.pop_back();
      --ms.count;
      changed = true;
    }

    // Ensure at least one trailing empty workspace (or 2 total).
    if (ms.count < 2 || counts.back() > 0) {
      ++ms.count;
      changed = true;
    }

    if (ms.active >= ms.count)
      ms.active = ms.count - 1;
    return changed;
  }

  // Produce batch moves returning ALL parked windows across ALL monitors to
  // Live Stage (Desktop 0), used when shutting down or leaving Independent mode.
  std::vector<BatchMoveIntent> build_restore_all_to_live_moves() const {
    std::vector<BatchMoveIntent> moves;
    for (const auto& kv : windows_) {
      if (sticky_.find(kv.first) != sticky_.end())
        continue;
      auto mit = monitors_.find(kv.second.monitor);
      const int active = (mit != monitors_.end()) ? mit->second.active : 0;
      if (kv.second.workspace != active)
        moves.push_back(BatchMoveIntent{kv.first, kLiveDesktop});
    }
    return moves;
  }

  void clear() {
    monitors_.clear();
    windows_.clear();
    sticky_.clear();
  }

 private:
  std::unordered_map<uintptr_t, MonitorWorkspaceState> monitors_;
  std::unordered_map<uintptr_t, WindowPlacement> windows_;
  std::unordered_set<uintptr_t> sticky_;
};

// --- Runtime WorkspaceManager API -------------------------------------------

// Callback invoked on the GTK main thread whenever workspace count or active
// index changes. @monitor == nullptr indicates a global (Span-mode) change;
// non-null @monitor indicates only that display's workspaces changed.
using WorkspaceChangedCallback = void (*)(HMONITOR monitor,
                                          int count,
                                          int active_index,
                                          void* user_data);

void init();
void shutdown();

// Refresh monitor topology & reconcile auto-pin / independent state.
void on_monitors_changed();

// Effective active mode (automatically degrades to Span on single-monitor or
// when Windows virtual desktop COM services are unavailable).
MultiMonitorWorkspaceMode effective_mode();
bool is_independent_active();

// Query workspaces for a specific monitor (nullptr -> active monitor).
int count_for_monitor(HMONITOR mon);
int active_for_monitor(HMONITOR mon);
std::vector<vd::DesktopInfo> desktops_for_monitor(HMONITOR mon);

// Switch workspace for @mon (nullptr -> active monitor).
bool switch_to(HMONITOR mon, int target_index);

// Create or remove a workspace on @mon.
bool create_workspace(HMONITOR mon);
bool remove_workspace(HMONITOR mon, int index);

// Pure helper: compute destination window RECT when transferring a window
// across monitors (unit-tested in winome-tests). Preserves size (clamped to
// @dst_work); uses @drop_pt when dropped directly on a non-overview desktop,
// or maps proportionally from @src_mon onto @dst_work when dropped into an
// open Overview.
inline RECT compute_cross_monitor_window_rect(const RECT& src_win,
                                              const RECT& src_mon,
                                              const RECT& dst_work,
                                              POINT drop_pt,
                                              bool use_drop_point) {
  const int dst_w = std::max<int>(dst_work.right - dst_work.left, 100);
  const int dst_h = std::max<int>(dst_work.bottom - dst_work.top, 100);
  const int orig_w = std::max<int>(src_win.right - src_win.left, 160);
  const int orig_h = std::max<int>(src_win.bottom - src_win.top, 120);
  const int w = std::min(orig_w, dst_w);
  const int h = std::min(orig_h, dst_h);

  int x = dst_work.left + (dst_w - w) / 2;
  int y = dst_work.top + (dst_h - h) / 2;
  if (use_drop_point) {
    x = drop_pt.x - w / 2;
    y = drop_pt.y - 24;
  } else {
    const int src_w = std::max<int>(src_mon.right - src_mon.left, 1);
    const int src_h = std::max<int>(src_mon.bottom - src_mon.top, 1);
    const double rx =
        static_cast<double>(src_win.left - src_mon.left) / static_cast<double>(src_w);
    const double ry =
        static_cast<double>(src_win.top - src_mon.top) / static_cast<double>(src_h);
    x = dst_work.left + static_cast<int>(std::lround(rx * dst_w));
    y = dst_work.top + static_cast<int>(std::lround(ry * dst_h));
  }

  x = std::clamp(x, static_cast<int>(dst_work.left),
                 std::max<int>(dst_work.left, dst_work.right - w));
  y = std::clamp(y, static_cast<int>(dst_work.top),
                 std::max<int>(dst_work.top, dst_work.bottom - h));
  return RECT{x, y, x + w, y + h};
}

// Query or move a specific window's workspace.
int window_workspace(HWND hwnd, HMONITOR mon_hint = nullptr);
bool window_on_active_workspace(HWND hwnd, HMONITOR mon_hint = nullptr);
bool move_window_to_workspace(HWND hwnd, HMONITOR mon, int target_index);

// Transfer @hwnd onto (@dst_mon, @dst_ws), repositioning it across displays
// cleanly (handling normal, maximized, and minimized windows) and updating
// workspace assignment across all 3 multi-monitor modes.
bool transfer_window_to_monitor_workspace(HWND hwnd,
                                          HMONITOR dst_mon,
                                          int dst_ws,
                                          POINT drop_pt,
                                          bool use_drop_point);

// Synchronize dynamic workspaces for @mon (returns true if count changed).
bool sync_dynamic_workspaces(HMONITOR mon);

// True if create/remove/move workspace operations are supported right now.
bool can_manage_workspaces();

// Subscribe/unsubscribe to workspace change notifications (main thread).
void add_change_listener(WorkspaceChangedCallback cb, void* user_data);
void remove_change_listener(WorkspaceChangedCallback cb, void* user_data);

}  // namespace ws
}  // namespace winome
