// SPDX-License-Identifier: GPL-2.0-only
// Origin: GNOME Shell js/ui/workspace.js (GPL-2.0-or-later)
// Ported for WINOME, 2026-08-19: exact upstream UnalignedLayoutStrategy math
// (window scale lerp(1.5,1) vs monitor height, WINDOW_PREVIEW_MAXIMUM_SCALE
// 0.95, ceil column count, single-row vertical centering, .window-picker
// spacing 6px). The overall layout scale is computed against the work area
// while the final slots are fitted into the (smaller) picker box, exactly as
// WorkspaceLayout._createBestLayout(workarea) + _getWindowSlots(containerBox).

#pragma once

#include <windows.h>

#include <vector>

namespace winome {

// A switcher window: its rect is the visible (DWM extended) frame rect,
// normalized to the overview client area before layout.
struct OvWindowInfo {
  HWND hwnd;
  RECT rect;
};

// One computed preview slot: top-left position and scale, in the coordinate
// space of the picker @box passed to overview_compute_slots.
struct OvSlot {
  double x;
  double y;
  double scale;
  OvWindowInfo window;
};

// Compute preview slots for @windows.
//   @monitor_rect: full monitor bounds (window scale uses its height).
//   @workarea:     scale-fit target (upstream _workarea).
//   @box:          the actual picker area slots must fit (upstream container
//                  box: the workspace actor's allocation, i.e. the centered
//                  aspect-preserving box, NOT the full-width container).
//   @row_spacing/@column_spacing: effective spacing in the same px space,
//                  i.e. (.window-picker spacing 6 + chrome oversize) scaled
//                  (WorkspaceLayout._adjustSpacingAndPadding adds the window
//                  chrome oversize — close button half-height / icon
//                  overhang / active-size increment — to the theme spacing).
// All rects are in the same coordinate space (overview client px).
void overview_compute_slots(const std::vector<OvWindowInfo> &windows,
                            const RECT &monitor_rect,
                            const RECT &workarea,
                            const RECT &box,
                            double row_spacing,
                            double column_spacing,
                            std::vector<OvSlot> *slots);

}  // namespace winome
