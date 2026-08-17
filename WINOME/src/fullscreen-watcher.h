// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Auto-hide the top panel while a fullscreen window or the lock screen is
// active, and show it again otherwise.

#pragma once

#include <windows.h>

namespace winome {

// Watch the foreground window and hide/show @panel_hwnd accordingly. The
// panel stays visible over the overview and over maximized windows; only
// genuine fullscreen windows and the lock screen hide it.
void start_fullscreen_watcher(HWND panel_hwnd);

}  // namespace winome
