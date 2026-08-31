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

// Create the overview window (once, main thread). @panel_hwnd is the panel
// window the overview is z-ordered below.
void overview_init(HWND panel_hwnd);

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

// TRUE while the overview window is mapped. The workspace-switch OSD only
// shows for switches made OUTSIDE the overview (GNOME: the popup is driven
// by mutter's switch actions, never from the overview's own controls).
int overview_is_open();

// Number of virtual desktops of the last layout (0 when the virtual
// desktop module is unavailable). The OSD uses it to size its indicator
// row without re-enumerating the COM interface on every poll.
int overview_workspace_count();

}  // namespace winome
