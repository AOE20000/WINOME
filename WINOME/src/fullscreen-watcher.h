// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Per-monitor fullscreen isolation & lock-screen auto-hide for top panels.

#pragma once

#include <windows.h>

namespace winome {

// Start watching foreground fullscreen windows and work area for @primary_panel.
void start_fullscreen_watcher(HWND primary_panel_hwnd);

// Unhook event listeners and release tracked panel windows.
void stop_fullscreen_watcher(void);

// Register/unregister secondary monitor panel windows for per-monitor isolation.
void register_panel_window(HWND panel_hwnd);
void unregister_panel_window(HWND panel_hwnd);

// Track workstation lock state (WM_WTSSESSION_CHANGE) to immediately hide/show.
void set_session_locked(bool locked);

// Re-evaluate all panels immediately (called on display or lock changes).
void evaluate_fullscreen_state(void);

// True while the primary panel is hidden (fullscreen app on primary or locked).
bool panel_hidden(void);

// True while the panel covering @mon is hidden.
bool panel_hidden_for_monitor(HMONITOR mon);

}  // namespace winome
