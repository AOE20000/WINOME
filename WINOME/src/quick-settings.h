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
void winome_calendar_refresh (GtkWidget *calendar);
void winome_calendar_reset_to_today (GtkWidget *calendar);

// Refresh battery percentage and volume state in the quick settings.
void winome_quick_settings_refresh (GtkWidget *quick_settings);

// If Quick Settings currently has an inline accordion menu drawer open,
// close it (animated when @animate is TRUE) and return TRUE; otherwise return
// FALSE. Safe to call with any widget.
gboolean winome_quick_settings_close_menu (GtkWidget *quick_settings,
                                           gboolean animate);

// Build a secondary menu popup for a `.quick-toggle-menu-button` arrow, from
// @n_entries rows of @entries. The returned widget is the themed popup content
// (.quick-toggle-menu > .popup-menu-box > .popup-menu-item); it is not a
// window — the caller anchors it in a toplevel and positions it under the
// arrow. Free with g_object_unref when done.
//
// When @header_icon is non-NULL the menu gets QuickToggleMenu's header (see
// QuickToggleMenu._init in quickSettings.js:389): a circular icon badge, a
// title and an optional subtitle, above the rows. Upstream calls setHeader()
// when a submenu has a summary to show; pass NULL for menus that do not.
GtkWidget *winome_quick_menu_new_full (const WinomeMenuEntry *entries,
                                       guint n_entries,
                                       const char *header_icon,
                                       const char *header_title,
                                       const char *header_subtitle,
                                       gboolean header_active);

GtkWidget *winome_quick_menu_new (const WinomeMenuEntry *entries,
                                  guint n_entries);

// Declarative QuickSettings plugin toggles.
typedef void (*WinomePluginQuickToggleCb) (const char *plugin_id,
                                             const char *slot,
                                             gboolean active);
void winome_quick_settings_set_plugin_toggle_cb (WinomePluginQuickToggleCb cb);
void winome_quick_settings_plugin_upsert (const char *key,
                                          const char *plugin_id,
                                          const char *slot,
                                          const char *icon_name,
                                          const char *title,
                                          const char *subtitle,
                                          gboolean active);
void winome_quick_settings_plugin_remove (const char *key);
