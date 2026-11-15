// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Zero-injection opt-in window frame unification via documented Win32
// DwmSetWindowAttribute calls (dark titlebars & rounded corners on Windows 11).

#pragma once

#include <windows.h>

namespace winome {

// Pure filter tested independently of live HWND handles: returns TRUE only for
// external standard top-level application windows eligible for DWM theming.
bool should_apply_dwm_theming_to_hwnd_info(const wchar_t* class_name,
                                           LONG_PTR style,
                                           LONG_PTR ex_style,
                                           bool visible,
                                           bool is_own_process);

// Starts listening to config changes and (when enabled) foreground/show events.
void window_theming_init();

// Unhooks WinEvent hooks and detaches listeners cleanly.
void window_theming_shutdown();

// Forces an immediate re-evaluation against top-level windows.
void window_theming_sync_all();

}  // namespace winome
