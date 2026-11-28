// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Port of gnome-shell js/ui/workspaceSwitcherPopup.js: the OSD shown when
// the workspace is switched from OUTSIDE the overview (Win+Ctrl+Left/Right,
// touchpad gestures...). A pill-shaped OSD panel anchored at the bottom
// center of the primary monitor holds one dot per workspace — the active
// one grows (`.ws-switcher-indicator:active`); it appears with a 100ms
// EASE_OUT_QUAD fade, stays DISPLAY_TIMEOUT (600ms), and fades out the
// same way. Switches made from the overview itself never show it (upstream
// the popup is driven by mutter's workspace-switch actions only).

#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace winome {

// Compute the physical screen rect for the workspace-switcher OSD capsule,
// anchored at bottom-center of @monitor_rect and clamped inside it.
inline RECT compute_ws_osd_hwnd_rect(const RECT& monitor_rect,
                                     int logical_w,
                                     int logical_h,
                                     double margin_bottom_logical,
                                     double shadow_pad_logical,
                                     int scale) {
  const int s = std::max(scale, 1);
  const int pw = std::max(logical_w, 1) * s;
  const int ph = std::max(logical_h, 1) * s;
  const int mw = monitor_rect.right - monitor_rect.left;
  const int mh = monitor_rect.bottom - monitor_rect.top;
  if (mw <= 0 || mh <= 0)
    return RECT{0, 0, pw, ph};
  const int margin = static_cast<int>(
      std::max(0.0, margin_bottom_logical - shadow_pad_logical) * s);
  int x = monitor_rect.left + (mw - pw) / 2;
  int y = monitor_rect.bottom - margin - ph;
  x = std::clamp(x, static_cast<int>(monitor_rect.left),
                 std::max(static_cast<int>(monitor_rect.left),
                          static_cast<int>(monitor_rect.right) - pw));
  y = std::clamp(y, static_cast<int>(monitor_rect.top),
                 std::max(static_cast<int>(monitor_rect.top),
                          static_cast<int>(monitor_rect.bottom) - ph));
  return RECT{x, y, x + pw, y + ph};
}

// Create the (hidden) OSD window and start the desktop-switch poll.
void ws_osd_init();
void ws_osd_shutdown();

}  // namespace winome
