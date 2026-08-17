// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// GNOME-style Quick Settings panel (matches quickSettings.js structure).

#pragma once

#include <gtk/gtk.h>

// Create the Quick Settings popover widget (the panel that opens when the
// status area button is clicked). Contains Wi-Fi/Bluetooth toggles, volume and
// brightness sliders, and the system item (battery + screenshot/settings/
// lock/power buttons).
GtkWidget *winome_quick_settings_new (void);

// Create the calendar/date popover widget (opens when the clock is clicked).
GtkWidget *winome_calendar_new (void);

// Refresh battery percentage and volume state in the quick settings.
void winome_quick_settings_refresh (GtkWidget *quick_settings);
