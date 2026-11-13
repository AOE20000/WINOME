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

struct ThumbLayoutSummary {
  int desktop_pills = 0;
  bool has_add_pill = false;
  bool visible = false;
};

// Pure helper (unit-tested in winome-tests): decides pill & '+' count.
inline ThumbLayoutSummary
compute_thumbs_summary (int count, bool can_manage, bool force_show)
{
  ThumbLayoutSummary s;
  if (count <= 0)
    return s;
  if (count > 1 || (force_show && count >= 1)) {
    s.visible = true;
    s.desktop_pills = count;
    s.has_add_pill = can_manage;
  }
  return s;
}

// Create the empty row (invisible until repopulated with >1 desktop or
// force_show during drag).
GtkWidget *overview_thumbs_new (void);

// Rebuild for @count desktops (@active = current index). @can_manage adds the
// '+' add-desktop pill and hover close badges when count > 1 or @force_show.
void overview_thumbs_repopulate (GtkWidget *thumbs, int count, int active,
                                 double box_h, double avail_w,
                                 double porthole_w, double porthole_h,
                                 GdkTexture *wallpaper,
                                 bool can_manage = false,
                                 bool force_show = false);

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

// Continuous indicator position while a workspace switch animates. Negative
// snaps back to `active`.
void overview_thumbs_set_indicator_value (GtkWidget *thumbs, double value);

// Highlight target index during window drag (-1 = none, 0..count-1 = pill,
// count = '+' new-desktop pill).
void overview_thumbs_set_drop_target (GtkWidget *thumbs, int target_index);

// Hit test in widget-local LOGICAL coords for drag-and-drop: returns
// 0..count-1 for an existing desktop, @count for the '+' new-desktop pill,
// or -1 on miss.
int overview_thumbs_drop_target_at (GtkWidget *thumbs, double x, double y);

// Click callback: fired with the pill index.
void overview_thumbs_set_click_cb (GtkWidget *thumbs,
                                   void (*cb) (int index, void *data),
                                   void *data);

// Desktop management callbacks: add ('+') and remove ('x' / middle-click).
void overview_thumbs_set_manage_cbs (GtkWidget *thumbs,
                                     void (*on_add) (void *data),
                                     void (*on_remove) (int index, void *data),
                                     void *data);

}  // namespace winome
