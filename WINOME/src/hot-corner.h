// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// GNOME-style top-left hot corner that opens the Activities overview.

#pragma once

#include <windows.h>

namespace winome {

// Watch the cursor and open the overview when it enters the top-left corner of
// the primary monitor. @panel_hwnd is used to find the primary monitor (the
// panel sits on it). A cooldown prevents rapid re-triggering.
void start_hot_corner(HWND panel_hwnd);

}  // namespace winome
