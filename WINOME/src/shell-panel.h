// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// GNOME-style top panel (port of gnome-shell's js/ui/panel.js).

#pragma once

#include <gtk/gtk.h>

// Create the full-width top panel window (Activities + clock + quick settings).
GtkWidget *winome_shell_panel_new (void);

// Panel height in pixels, resolved from the GNOME theme (#panel { height:
// 2.2em }) using the St theme context font. Matches gnome-shell's 32px bar at
// scale 1.
int winome_shell_panel_height (void);
