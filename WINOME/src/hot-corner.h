// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// GNOME-style top-left hot corner that opens the Activities overview.

#pragma once

#include <windows.h>

namespace winome {

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

// Start event-driven top-left hot-corner monitoring for @panel_hwnd's monitor.
void start_hot_corner(HWND panel_hwnd);

// Stop the hot-corner hook thread cleanly on shutdown.
void stop_hot_corner(void);

// Suppress/un-suppress hot-corner hook while primary panel is hidden
// (fullscreen window / locked session) so fullscreen games incur zero hook work.
void hot_corner_set_suppressed(bool suppressed);

// Refresh cached primary monitor coordinates after display/DPI changes.
void hot_corner_refresh_bounds(void);

}  // namespace winome
