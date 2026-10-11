// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// GNOME-style Quick Settings panel (matches quickSettings.js structure).

#pragma once

#include <gtk/gtk.h>

// Callback for a secondary-menu item. @user_data is the pointer handed to
// winome_menu_toggle_new_menu().
typedef void (*WinomeMenuItemCallback) (gpointer user_data);

// A single row of a secondary menu (QuickToggleMenu in quickSettings.js).
typedef struct {
  const char              *label;     // item text
  const char              *icon_name; // optional 16px leading icon, may be NULL
  gboolean                 checked;   // draw the active-item ornament
  WinomeMenuItemCallback  callback;  // run on activation, may be NULL
  gpointer                 user_data; // passed to @callback
} WinomeMenuEntry;

// Create the Quick Settings popover widget (the panel that opens when the
// status area button is clicked). Contains Wi-Fi/Bluetooth toggles, volume and
// brightness sliders, and the system item (battery + screenshot/settings/
// lock/power buttons).
GtkWidget *winome_quick_settings_new (void);

// Create the calendar/date popover widget (opens when the clock is clicked).
GtkWidget *winome_calendar_new (void);

// Refresh battery percentage and volume state in the quick settings.
void winome_quick_settings_refresh (GtkWidget *quick_settings);

// Build a secondary menu popup for a `.quick-toggle-menu-button` arrow, from
// @n_entries rows of @entries. The returned widget is the themed popup content
// (.popup-menu > .popup-menu-content > .popup-menu-box > .popup-menu-item);
// it is not a window — the caller anchors it in a toplevel and positions it
// under the arrow. Free with g_object_unref when done.
GtkWidget *winome_quick_menu_new (const WinomeMenuEntry *entries, guint n_entries);
