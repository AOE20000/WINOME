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

namespace winome {

// Create the (hidden) OSD window and start the desktop-switch poll.
void ws_osd_init();

}  // namespace winome
