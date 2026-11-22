// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Per-monitor fullscreen isolation & lock-screen auto-hide for top panels.

#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdint>

namespace winome {

// Clamp candidate panel height to sane bounds for @monitor_rect so an abnormal
// window rect or layout glitch can never reserve excessive/zero/negative space.
inline int clamp_panel_height_for_monitor(const RECT& monitor_rect,
                                          int candidate_height,
                                          int fallback_height = 32) {
  const int mon_w = monitor_rect.right - monitor_rect.left;
  const int mon_h = monitor_rect.bottom - monitor_rect.top;
  if (mon_w <= 0 || mon_h <= 4)
    return 0;

  const int max_h = std::min(96, std::max(1, mon_h / 4));
  const int min_h = std::min(20, max_h);
  if (candidate_height >= min_h && candidate_height <= max_h)
    return candidate_height;

  const int fb = (fallback_height > 0) ? fallback_height : 32;
  return std::clamp(fb, min_h, max_h);
}

// Expected top-panel HWND rect on @monitor_rect for @panel_height.
inline RECT compute_primary_panel_rect(const RECT& monitor_rect,
                                       int panel_height) {
  const int safe_h = clamp_panel_height_for_monitor(monitor_rect, panel_height, 32);
  if (safe_h <= 0)
    return RECT{};
  return RECT{
      monitor_rect.left,
      monitor_rect.top,
      monitor_rect.right,
      monitor_rect.top + safe_h,
  };
}

enum class WorkareaAssertAction {
  SkipUnchanged,
  SkipHidden,
  SkipInMoveSize,
  SkipInvalidMonitor,
  DeferCooldown,
  DeferBackoff,
  Apply,
};

// Rate-limits and loop-protects SPI_SETWORKAREA re-assertions so external OS
// workarea updates or window-edge drags never trigger an oscillation storm,
// while recovering quickly without multi-second unreserved gaps.
struct WorkareaAssertGuard {
  static constexpr int64_t kMinApplyIntervalMs = 120;
  static constexpr int64_t kBurstWindowMs = 3000;
  static constexpr int kMaxBurstApplies = 8;
  static constexpr int64_t kBackoffDurationMs = 1000;
  static constexpr int64_t kWorkareaSettleGraceMs = 600;

  int64_t last_apply_ms = -1;
  int64_t burst_window_start_ms = -1;
  int64_t backoff_until_ms = 0;
  int64_t workarea_settle_until_ms = 0;
  int burst_count = 0;
  bool pending_after_defer = false;

  bool in_settle_grace(int64_t now_ms) const {
    return now_ms < workarea_settle_until_ms;
  }

  static bool rects_equal(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top &&
           a.right == b.right && a.bottom == b.bottom;
  }

  WorkareaAssertAction evaluate(int64_t now_ms,
                                const RECT& desired,
                                const RECT& current,
                                bool primary_valid,
                                bool panel_hidden,
                                bool in_move_size) {
    if (!primary_valid || desired.right <= desired.left ||
        desired.bottom <= desired.top) {
      return WorkareaAssertAction::SkipInvalidMonitor;
    }

    if (rects_equal(desired, current)) {
      if (burst_window_start_ms >= 0 &&
          (now_ms - burst_window_start_ms) >= kBurstWindowMs) {
        burst_window_start_ms = -1;
        burst_count = 0;
      }
      pending_after_defer = false;
      return WorkareaAssertAction::SkipUnchanged;
    }

    if (panel_hidden) {
      pending_after_defer = true;
      return WorkareaAssertAction::SkipHidden;
    }
    if (in_move_size) {
      pending_after_defer = true;
      return WorkareaAssertAction::SkipInMoveSize;
    }
    if (now_ms < backoff_until_ms) {
      pending_after_defer = true;
      return WorkareaAssertAction::DeferBackoff;
    }
    if (last_apply_ms >= 0 && (now_ms - last_apply_ms) < kMinApplyIntervalMs) {
      pending_after_defer = true;
      return WorkareaAssertAction::DeferCooldown;
    }

    if (burst_window_start_ms < 0 ||
        (now_ms - burst_window_start_ms) > kBurstWindowMs) {
      burst_window_start_ms = now_ms;
      burst_count = 1;
    } else {
      ++burst_count;
      if (burst_count > kMaxBurstApplies) {
        backoff_until_ms = now_ms + kBackoffDurationMs;
        pending_after_defer = true;
        return WorkareaAssertAction::DeferBackoff;
      }
    }

    last_apply_ms = now_ms;
    if (desired.top > current.top)
      workarea_settle_until_ms = now_ms + kWorkareaSettleGraceMs;
    pending_after_defer = false;
    return WorkareaAssertAction::Apply;
  }

  int remaining_defer_ms(int64_t now_ms) const {
    if (now_ms < backoff_until_ms) {
      const int64_t rem = backoff_until_ms - now_ms;
      return static_cast<int>(std::clamp<int64_t>(rem + 25, 50, kBackoffDurationMs + 50));
    }
    if (last_apply_ms >= 0 && (now_ms - last_apply_ms) < kMinApplyIntervalMs) {
      const int64_t rem = kMinApplyIntervalMs - (now_ms - last_apply_ms);
      return static_cast<int>(std::clamp<int64_t>(rem + 25, 50, kMinApplyIntervalMs + 50));
    }
    return 200;
  }
};

// Returns true when @window_rect genuinely covers @monitor_rect in fullscreen
// without tripping on normal maximized windows when workarea is unreserved.
inline bool is_fullscreen_bounds_for_monitor(const RECT& window_rect,
                                             const RECT& monitor_rect,
                                             const RECT& work_rect,
                                             bool is_zoomed,
                                             int tolerance_px = 2,
                                             bool has_caption = false,
                                             bool suppress_zoomed = false) {
  if (monitor_rect.right <= monitor_rect.left ||
      monitor_rect.bottom <= monitor_rect.top) {
    return false;
  }
  if (window_rect.right <= window_rect.left ||
      window_rect.bottom <= window_rect.top) {
    return false;
  }
  const bool covers =
      window_rect.left <= monitor_rect.left + tolerance_px &&
      window_rect.top <= monitor_rect.top + tolerance_px &&
      window_rect.right >= monitor_rect.right - tolerance_px &&
      window_rect.bottom >= monitor_rect.bottom - tolerance_px;
  if (!covers)
    return false;

  if (is_zoomed) {
    // Decorated maximized windows (WS_CAPTION + IsZoomed) are normal maximized
    // desktop windows, never true fullscreen. Also suppress captionless zoomed
    // checks right after SPI_SETWORKAREA raises work_rect.top so async window
    // re-layout never produces a 1-tick false fullscreen hide.
    if (has_caption || suppress_zoomed)
      return false;
    // Normal maximized windows fill mi.rcMonitor if top workarea wasn't
    // reserved yet; only treat zoomed windows as fullscreen when top workarea
    // is reserved and the window still intentionally covers monitor_rect.top.
    const bool top_reserved = work_rect.top > monitor_rect.top + tolerance_px;
    if (!top_reserved)
      return false;
  }
  return true;
}

struct FullscreenEvalInput {
  int64_t now_ms = 0;
  bool session_locked = false;
  bool overview_open = false;
  bool popover_open = false;
  bool currently_hidden = false;
  HMONITOR raw_fullscreen_mon = nullptr;
  bool fg_is_transient_or_null = false;
  bool fg_owned_by_tracked_fs = false;
  bool tracked_fs_still_covering = false;
  HMONITOR tracked_fs_mon = nullptr;
};

struct FullscreenEvalResult {
  HMONITOR effective_fullscreen_mon = nullptr;
  bool hide_all_locked = false;
  bool schedule_unhide_timer = false;
  int unhide_delay_ms = 0;
  bool cancel_unhide_timer = false;
  bool schedule_hide_timer = false;
  int hide_delay_ms = 0;
  bool cancel_hide_timer = false;
};

// Stabilizes fullscreen detection against transient focus drops, IME/tooltip
// popups, owned overlays, and rapid enter/exit mode toggles.
struct FullscreenHysteresisTracker {
  static constexpr int64_t kUnhideDebounceMs = 160;

  int64_t hide_debounce_ms = 0;
  int64_t pending_unhide_deadline_ms = -1;
  int64_t pending_hide_deadline_ms = -1;
  HMONITOR pending_hide_mon = nullptr;

  FullscreenHysteresisTracker() = default;
  explicit FullscreenHysteresisTracker(int64_t enter_debounce_ms)
      : hide_debounce_ms(enter_debounce_ms > 0 ? enter_debounce_ms : 0) {}

  FullscreenEvalResult step(const FullscreenEvalInput& in) {
    FullscreenEvalResult out{};
    if (in.session_locked) {
      pending_unhide_deadline_ms = -1;
      pending_hide_deadline_ms = -1;
      pending_hide_mon = nullptr;
      out.hide_all_locked = true;
      out.cancel_unhide_timer = true;
      out.cancel_hide_timer = true;
      return out;
    }

    if (in.overview_open || in.popover_open) {
      pending_unhide_deadline_ms = -1;
      pending_hide_deadline_ms = -1;
      pending_hide_mon = nullptr;
      out.effective_fullscreen_mon = nullptr;
      out.cancel_unhide_timer = true;
      out.cancel_hide_timer = true;
      return out;
    }

    if (in.raw_fullscreen_mon != nullptr) {
      pending_unhide_deadline_ms = -1;
      out.cancel_unhide_timer = true;
      if (!in.currently_hidden && hide_debounce_ms > 0) {
        if (pending_hide_deadline_ms < 0 ||
            pending_hide_mon != in.raw_fullscreen_mon) {
          pending_hide_mon = in.raw_fullscreen_mon;
          pending_hide_deadline_ms = in.now_ms + hide_debounce_ms;
          out.effective_fullscreen_mon = nullptr;
          out.schedule_hide_timer = true;
          out.hide_delay_ms = static_cast<int>(hide_debounce_ms);
          return out;
        }
        if (in.now_ms < pending_hide_deadline_ms) {
          out.effective_fullscreen_mon = nullptr;
          return out;
        }
      }
      pending_hide_deadline_ms = -1;
      pending_hide_mon = nullptr;
      out.effective_fullscreen_mon = in.raw_fullscreen_mon;
      out.cancel_hide_timer = true;
      return out;
    }

    pending_hide_deadline_ms = -1;
    pending_hide_mon = nullptr;
    out.cancel_hide_timer = true;

    if (in.currently_hidden && in.tracked_fs_mon != nullptr &&
        in.tracked_fs_still_covering &&
        (in.fg_owned_by_tracked_fs || in.fg_is_transient_or_null)) {
      pending_unhide_deadline_ms = -1;
      out.effective_fullscreen_mon = in.tracked_fs_mon;
      out.cancel_unhide_timer = true;
      return out;
    }

    if (!in.currently_hidden || in.tracked_fs_mon == nullptr) {
      pending_unhide_deadline_ms = -1;
      out.effective_fullscreen_mon = nullptr;
      out.cancel_unhide_timer = true;
      return out;
    }

    // Currently hidden and no active fullscreen foreground: wait out transient
    // bounces (mode switches, rapid F11 redraws) before re-showing the panel.
    if (pending_unhide_deadline_ms < 0) {
      pending_unhide_deadline_ms = in.now_ms + kUnhideDebounceMs;
      out.effective_fullscreen_mon = in.tracked_fs_mon;
      out.schedule_unhide_timer = true;
      out.unhide_delay_ms = static_cast<int>(kUnhideDebounceMs);
      return out;
    }

    if (in.now_ms < pending_unhide_deadline_ms) {
      out.effective_fullscreen_mon = in.tracked_fs_mon;
      return out;
    }

    pending_unhide_deadline_ms = -1;
    out.effective_fullscreen_mon = nullptr;
    return out;
  }
};

// Start watching foreground fullscreen windows and work area for @primary_panel.
void start_fullscreen_watcher(HWND primary_panel_hwnd);

// Update the expected canonical physical panel height (px) used for workarea
// assertions and panel geometry repairs.
void set_expected_panel_height(int height);

// Unhook event listeners and release tracked panel windows.
void stop_fullscreen_watcher(void);

// Register/unregister secondary monitor panel windows for per-monitor isolation.
void register_panel_window(HWND panel_hwnd);
void unregister_panel_window(HWND panel_hwnd);

// Track workstation lock state (WM_WTSSESSION_CHANGE) to immediately hide/show.
void set_session_locked(bool locked);

// Re-evaluate all panels immediately (called on display or lock changes).
void evaluate_fullscreen_state(void);

// Immediately re-assert panel workarea & toolwindow style (e.g. SPI_SETWORKAREA).
void reassert_panel_workarea_now(void);

// True while the primary panel is hidden (fullscreen app on primary or locked).
bool panel_hidden(void);

// True while the panel covering @mon is hidden.
bool panel_hidden_for_monitor(HMONITOR mon);

}  // namespace winome
