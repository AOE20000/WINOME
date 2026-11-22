// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Bottom dash of the Activities overview: a faithful visual port of
// gnome-shell js/ui/dash.js (WINDOW_PICKER state). Shows one tile per
// running application (grouped by AppUserModelID, falling back to the
// owner process image), a white running dot per tile, and the "Show Apps"
// grid button at the end. Geometry mirrors the compiled theme:
//   #dash              padding-left/right 6px
//   .dash-background   radius 28px, padding 12px v / 10px h, margin-bottom 12
//   .overview-icon     radius 16px, padding 6px around the icon
//   .dash-item-container margin 0 2px  (4px gap between tiles)
//   .app-grid-running-dot 5px dot, centered in the tile's bottom padding
// Hover shows a .dash-label pill above the dash after 300ms (dash.js
// DASH_ITEM_HOVER_TIMEOUT).

#pragma once

#include "app-index.h"

#include <windows.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <string>
#include <vector>

namespace winome {

// Pure helper (unit-tested): merges persisted pinned IDs first, followed by any
// unpinned running IDs in order, setting @out_pinned_count cleanly.
inline std::vector<std::string>
build_dash_item_order (const std::vector<std::string> &pinned_ids,
                       const std::vector<std::string> &running_ids,
                       int *out_pinned_count)
{
  std::vector<std::string> ordered;
  for (const std::string &raw : pinned_ids) {
    std::string norm = normalize_app_id (raw);
    if (norm.empty ())
      continue;
    if (std::find (ordered.begin (), ordered.end (), norm) == ordered.end ())
      ordered.push_back (norm);
  }
  int pinned_cnt = (int)ordered.size ();
  for (const std::string &raw : running_ids) {
    std::string norm = normalize_app_id (raw);
    if (norm.empty ())
      continue;
    if (std::find (ordered.begin (), ordered.end (), norm) == ordered.end ())
      ordered.push_back (norm);
  }
  if (out_pinned_count != nullptr)
    *out_pinned_count = pinned_cnt;
  return ordered;
}

// Pure helper (unit-tested): determines whether a candidate window belongs in
// the Dash under either "All Workspaces" (@isolate_workspaces == false) or
// "Current Workspace Only" (@isolate_workspaces == true).
// @cloaked_flags is the DWORD returned by DwmGetWindowAttribute(DWMWA_CLOAKED).
inline bool
should_include_dash_window (bool isolate_workspaces, bool vd_available,
                            int active_desktop, int window_desktop,
                            bool on_current, DWORD cloaked_flags)
{
  // App-cloaked (suspended/pre-launched UWP, 0x1) and inherited-cloaked (0x4)
  // windows are never real user windows in either mode.
  if ((cloaked_flags & (0x1u | 0x4u)) != 0)
    return false;
  if (!vd_available)
    return cloaked_flags == 0;
  if (isolate_workspaces) {
    if (active_desktop >= 0) {
      if (window_desktop >= 0)
        return window_desktop == active_desktop;
      return on_current && cloaked_flags == 0;
    }
    return on_current && cloaked_flags == 0;
  }
  // All Workspaces mode: include uncloaked windows on the active desktop as
  // well as DWM_CLOAKED_SHELL (0x2) windows belonging to any valid virtual
  // desktop, while still excluding suspended UWP (DWM_CLOAKED_APP = 0x1) apps.
  if (cloaked_flags == 0)
    return true;
  return window_desktop >= 0 && ((cloaked_flags & 0x2u) != 0);
}

// Create the dash. @root_fixed is the overview root GtkFixed; the floating
// hover label is added there so it can extend above the dash allocation.
GtkWidget *overview_dash_new (GtkWidget *root_fixed);

// Re-enumerate the running applications on @monitor + persisted favorites and
// rebuild the tiles. @max_height is the available dash height in LOGICAL px.
// @active_desktop optionally supplies the overview's current desktop index
// (>= 0) so workspace-isolated mode updates immediately on workspace switches.
void overview_dash_repopulate (GtkWidget *dash, HMONITOR monitor,
                               int max_height, int active_desktop = -1);

void overview_dash_set_show_apps_cb (GtkWidget *dash, void (*cb) (void *),
                                     void *user_data);
void overview_dash_set_apps_active (GtkWidget *dash, bool active);
void overview_dash_set_changed_cb (GtkWidget *dash, void (*cb) (void *),
                                   void *user_data);
void overview_dash_close_menu (GtkWidget *dash);
bool overview_dash_has_open_menu (GtkWidget *dash);

// Natural size in LOGICAL px, valid after repopulate(). Width includes the
// #dash side padding; height includes the 12px bottom margin.
int overview_dash_get_width (GtkWidget *dash);
int overview_dash_get_height (GtkWidget *dash);

// Application icon for a window at (at least) @size PHYSICAL px (WM_GETICON /
// class icon / IShellItemImageFactory on the exe, backed by LRU cache).
GdkTexture *winome_window_app_icon (HWND hwnd, int size);

}  // namespace winome
