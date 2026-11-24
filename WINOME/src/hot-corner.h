// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// GNOME-style top-left hot corner that opens the Activities overview.

#pragma once

#include <windows.h>
#include <cstddef>

namespace winome {

struct HotCornerMonitor {
  HMONITOR hmonitor = nullptr;
  RECT rect{};
};

// Pure boundary hit-test helper (unit-tested in winome-tests).
inline bool hot_corner_hit_test(int x,
                                int y,
                                const RECT& monitor_rect,
                                int corner_size_px = 5) {
  if (corner_size_px < 0 || monitor_rect.right <= monitor_rect.left ||
      monitor_rect.bottom <= monitor_rect.top) {
    return false;
  }
  return x >= monitor_rect.left &&
         x <= monitor_rect.left + corner_size_px &&
         y >= monitor_rect.top &&
         y <= monitor_rect.top + corner_size_px;
}

// Pure multi-monitor hot-corner hit-test helper (unit-tested in winome-tests).
// Returns the index of the monitor whose top-left hot corner contains (x, y),
// or -1 if none matched.
inline int hot_corner_hit_test_monitors(int x,
                                        int y,
                                        const HotCornerMonitor* monitors,
                                        size_t count,
                                        int corner_size_px = 5) {
  if (monitors == nullptr || count == 0)
    return -1;
  for (size_t i = 0; i < count; ++i) {
    if (hot_corner_hit_test(x, y, monitors[i].rect, corner_size_px))
      return static_cast<int>(i);
  }
  return -1;
}

// Start event-driven top-left hot-corner monitoring across all active displays.
void start_hot_corner(HWND panel_hwnd);

// Stop the hot-corner hook thread cleanly on shutdown.
void stop_hot_corner(void);

// Suppress/un-suppress hot-corner hook globally while all monitors are hidden
// (locked session or fullscreen on all panels) so fullscreen games incur zero
// hook work. Per-monitor suppression is also checked at trigger time.
void hot_corner_set_suppressed(bool suppressed);

// Refresh cached monitor coordinates across all displays after display/DPI changes.
void hot_corner_refresh_bounds(void);

}  // namespace winome
