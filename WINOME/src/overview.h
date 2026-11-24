// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// In-host Activities overview window: a borderless GTK4 toplevel covering
// the panel's monitor, z-ordered directly below the panel (layering
// contract: popover > panel > overview > apps > wallpaper). Live window
// previews are DWM thumbnails on child host windows laid out by
// overview-layout.cpp.

#pragma once

#include <windows.h>

namespace winome {

// Create the overview window (once, main thread). @panel_hwnd is the primary
// panel window used as fallback when no per-monitor panel is registered.
void overview_init(HWND panel_hwnd);

// Set the requested target monitor for the next/current overview display.
// Passing nullptr selects the active (pointer/foreground) monitor.
void overview_set_target_monitor(HMONITOR monitor);

// Return the monitor currently hosting (or requested for) the overview.
HMONITOR overview_target_monitor();

// Show/hide on the main thread (idempotent).
void overview_show();
void overview_hide();

// Re-assert the layering contract popover > panel > overview > apps. Cheap
// (two SetWindowPos); call after any window maps, and periodically — GDK
// and Windows both reshuffle the topmost band behind our back.
void overview_restack();

// Thread-safe variants, marshaled to the GLib main context.
void overview_show_async();
void overview_hide_async();

// TRUE while any overview window (main or secondary) is mapped.
int overview_is_open();

// Per-monitor and aggregate overview open queries (main thread).
bool overview_is_main_open();
HMONITOR overview_main_monitor();
bool overview_is_open_for_monitor(HMONITOR monitor);
bool overview_any_open();

// Independent (unlinked) per-monitor Secondary Overview helpers (main thread).
void overview_open_secondary_for_monitor(HMONITOR monitor);
void overview_close_secondary_for_monitor(HMONITOR monitor);
void overview_close_all_secondary();

// Cross-monitor drag-and-drop helpers for dropping into the main Overview from
// a Secondary Overview or from a live desktop window on another monitor.
bool overview_main_hit_test_screen_point(POINT screen_pt,
                                         HMONITOR* out_mon,
                                         int* out_ws,
                                         bool* out_create_new);
void overview_main_set_external_drop_highlight(POINT screen_pt);
void overview_main_clear_external_drop_highlight();
void overview_refresh_all_open();

// Live desktop window drag hooks (called on GTK main thread from WinEvent).
void overview_on_external_window_drag_start(HWND hwnd);
void overview_on_external_window_drag_move(HWND hwnd);
void overview_on_external_window_drag_end(HWND hwnd);

// Number of virtual desktops of the last layout (0 when the virtual
// desktop module is unavailable). The OSD uses it to size its indicator
// row without re-enumerating the COM interface on every poll.
int overview_workspace_count();

// Reload desktop wallpaper texture on WM_SETTINGCHANGE (SPI_SETDESKWALLPAPER).
void overview_invalidate_wallpaper();

// Reflow overview geometry and window bounds after display/DPI changes.
void overview_reposition();

// Toggle in-host paginated App Grid view inside the open overview.
void overview_toggle_app_grid();

// Thread-safe double-Win progression: interrupt/advance an opening or open
// WINDOW_PICKER overview into the APP_GRID state.
void overview_shift_to_app_grid_async();
bool overview_can_shift_to_app_grid();

// Discard any saved pre-overview foreground HWND and release foreground lock
// before launching or activating a target outside the overview.
void overview_discard_restore_focus();

// Re-acquire Win32 foreground and restore search-ready keyboard/IME focus
// when an overlaid panel popover or menu closes while overview remains active.
void overview_restore_focus_if_active();

}  // namespace winome
