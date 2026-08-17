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

 private:
  static std::vector<HWND> find_taskbars();
  static void set_visibility(HWND hwnd, bool visible);
  static void keeper_loop();

  // Set to true while we are suppressing the taskbar. Guarded by the state
  // below so that a late restore() never touches a taskbar we did not hide.
  static inline std::atomic_bool hidden_{false};

  // Keeper thread lifecycle: the thread is parked on stop_event_ and wakes up
  // every kRehideIntervalMs to re-apply the hidden state. restore() signals
  // the event to wind the thread down before showing the taskbar again.
  static std::thread keeper_thread_;
  static HANDLE stop_event_;
};

}  // namespace winome
