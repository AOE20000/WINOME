// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Hides and restores the native Windows taskbar (Shell_TrayWnd).

#pragma once

#include <windows.h>

#include <atomic>
#include <thread>
#include <vector>

namespace winome {

// Owns the visibility of the native Windows taskbar. hide() launches a
// keeper thread that re-hides the taskbar whenever Explorer brings it back,
// so the shell bar cannot reappear while WINOME is running. restore() stops
// the keeper and shows the taskbar again, but only if this process was the
// one that hid it.
class NativeTaskbar {
 public:
  static void hide();
  static void restore();
  // Lock/join-free best-effort signal for crash handlers; preserves recovery.dat
  // so the watchdog completes guaranteed restoration + opens crash recovery UI.
  static void restore_emergency_no_join();
  // Restores taskbars and workarea aggressively and verifies visibility across
  // retries; returns true when at least one taskbar confirms visible.
  static bool force_restore_all(int desired_state = -1);
  static bool is_any_taskbar_visible();
  static int original_state();
  // Temporarily allow Shell_TrayWnd to stay active/accessible while native
  // system tray overflow or Win11 UIA tray inspection/activation runs.
  static void suspend_keeper_ms(DWORD ms);
  static void set_primary_cloaked(bool cloaked);
  static bool is_tray_overflow_open();

 private:
  static std::vector<HWND> find_taskbars(HWND* out_primary = nullptr);
  static void set_visibility(HWND hwnd, bool visible, int restore_state);
  static void keeper_loop();

  // Set to true while we are suppressing the taskbar. Guarded by the state
  // below so that a late restore() never touches a taskbar we did not hide.
  static inline std::atomic_bool hidden_{false};
  static inline std::atomic_int original_state_{2 /*ABS_ALWAYSONTOP*/};
  static inline std::atomic<uint64_t> suspend_until_tick_{0};
  static inline std::atomic_bool primary_cloaked_{false};

  // Keeper thread lifecycle: the thread is parked on stop_event_ and wakes up
  // every kRehideIntervalMs to re-apply the hidden state. restore() signals
  // the event to wind the thread down before showing the taskbar again.
  static std::thread keeper_thread_;
  static HANDLE stop_event_;
};

}  // namespace winome
