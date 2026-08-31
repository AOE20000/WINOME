// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// .workspace-thumbnails row: visual port of gnome-shell
// js/ui/workspaceThumbnail.js ThumbnailsBox (WINDOW_PICKER state). One
// 4px-rounded wallpaper pill per virtual desktop (the porthole scaled down
// to the pill), the active desktop framed by the 3px accent
// .workspace-thumbnail-indicator. The per-desktop window mini-previews are
// DWM live thumbnails registered by overview.cpp at the pill rects exposed
// here (ThumbnailsBox shows clones of each workspace's windows). Clicking a
// pill activates that desktop (ThumbnailsBox click action ->
// Workspace.activate): the overview stays open and relayouts.

#pragma once

#include <windows.h>

#include <gtk/gtk.h>

namespace winome {

// Create the empty row (invisible until repopulated with >1 desktop).
GtkWidget *overview_thumbs_new (void);

// Rebuild for @count desktops (@active = current index). @box_h is the row
// height in LOGICAL px (ControlsManagerLayout caps it at 5% of the workarea
// height); @avail_w the available width in LOGICAL px; @porthole_w/@porthole_h
// the workarea the pills shrink (ThumbnailsBox scale =
// min(vScale, hScale, MAX_THUMBNAIL_SCALE)). @wallpaper is ref'd by the
// widget. A count <= 1 clears the row (GNOME hides the box for a single
// workspace).
void overview_thumbs_repopulate (GtkWidget *thumbs, int count, int active,
                                 double box_h, double avail_w,
                                 double porthole_w, double porthole_h,
                                 GdkTexture *wallpaper);

// Natural size in LOGICAL px; height 0 when there is nothing to show.
int overview_thumbs_get_width (GtkWidget *thumbs);
int overview_thumbs_get_height (GtkWidget *thumbs);

// Pill @index bounds in widget coordinates (LOGICAL px), for hit tests and
// the DWM mini-thumbnail layout.
gboolean overview_thumbs_get_rect (GtkWidget *thumbs, int index,
                                   GdkRectangle *out);

// Pill-local scale (pill px per porthole px): a window at porthole
// coordinate (x, y) lands at pill-local (x * scale, y * scale).
double overview_thumbs_get_scale (GtkWidget *thumbs);

// Continuous indicator position while a workspace switch animates
// (ThumbnailsBox tracks the eased scroll adjustment, sliding the accent
// frame between the adjacent pills). Negative snaps back to `active`.
void overview_thumbs_set_indicator_value (GtkWidget *thumbs, double value);

// Click callback: fired with the pill index.
void overview_thumbs_set_click_cb (GtkWidget *thumbs,
                                   void (*cb) (int index, void *data),
                                   void *data);

}  // namespace winome
