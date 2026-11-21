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

#include <vector>

namespace winome {

struct ThumbLayoutSummary {
  int desktop_pills = 0;
  bool has_add_pill = false;
  bool visible = false;
};

// Pure helper (unit-tested in winome-tests): decides pill visibility.
// Following GNOME Shell dynamic workspaces (windowManager.js WorkspaceTracker
// + workspaceThumbnail.js ThumbnailsBox), an automatic trailing empty
// workspace replaces any manual '+' button pill.
inline ThumbLayoutSummary
compute_thumbs_summary (int count, bool can_manage, bool force_show)
{
  (void)can_manage;
  ThumbLayoutSummary s;
  if (count <= 0)
    return s;
  if (count > 1 || (force_show && count >= 1)) {
    s.visible = true;
    s.desktop_pills = count;
    s.has_add_pill = false;
  }
  return s;
}

// Pure helper implementing GNOME Shell's WorkspaceTracker._checkWorkspaces()
// dynamic workspace lifecycle (upstream/gnome-shell/js/ui/windowManager.js):
// - Always keeps at least MIN_NUM_WORKSPACES (2) workspaces (so 1 workspace on
//   startup becomes 2 workspaces).
// - If the last workspace has windows, appends a new trailing empty workspace.
// - Removes any empty workspace except the single trailing empty workspace
//   (iterating from the end to avoid index shift), while never dropping below
//   2 workspaces.
struct DynamicWorkspacePlan {
  int append_count = 0;
  std::vector<int> remove_indices;  // descending order
  int final_count = 0;
};

inline DynamicWorkspacePlan
compute_dynamic_workspace_plan (const std::vector<int> &window_counts)
{
  DynamicWorkspacePlan plan;
  if (window_counts.empty ())
    return plan;

  std::vector<bool> empty;
  empty.reserve (window_counts.size () + 2);
  int last_non_empty = -1;
  for (size_t i = 0; i < window_counts.size (); ++i) {
    const bool is_empty = (window_counts[i] <= 0);
    empty.push_back (is_empty);
    if (!is_empty)
      last_non_empty = static_cast<int> (i);
  }

  // 1. If we don't have an empty workspace at the end, add one.
  if (!empty.back ()) {
    empty.push_back (true);
    ++plan.append_count;
  }

  // 2. Enforce minimum number of workspaces (MIN_NUM_WORKSPACES = 2).
  while (empty.size () < 2) {
    empty.push_back (true);
    ++plan.append_count;
  }

  // 3. Delete empty workspaces except for the single trailing empty workspace;
  //    iterate from the end to avoid index changes.
  const int keep_empty_idx = (last_non_empty >= 0) ? (last_non_empty + 1) : 1;
  int current_count = static_cast<int> (empty.size ());
  for (int i = static_cast<int> (empty.size ()) - 1; i >= 0; --i) {
    if (current_count <= 2)
      break;
    if (empty[static_cast<size_t> (i)] && i != keep_empty_idx) {
      plan.remove_indices.push_back (i);
      --current_count;
    }
  }
  plan.final_count = current_count;
  return plan;
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
