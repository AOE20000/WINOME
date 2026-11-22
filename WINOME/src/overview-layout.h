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

// Result of clipping a miniature workspace-thumbnail DWM preview against the
// active/adjacent workspace background rects so DWM thumbnails attached to the
// overview HWND never composite on top of an expanding/shrinking workspace card.
struct MiniThumbClipResult {
  bool visible = false;
  RECT dest_rect{};
  RECT src_rect{};
};

MiniThumbClipResult compute_mini_thumb_clip(
    const RECT &full_dest,
    SIZE source_size,
    const std::vector<RECT> &occluder_boxes);

// True when @target is completely contained inside at least one box in @boxes.
bool is_rect_fully_covered_by_boxes(const RECT &target,
                                    const std::vector<RECT> &boxes);

// True when @target has non-empty intersection with any box in @boxes.
bool is_rect_overlapping_any_box(const RECT &target,
                                 const std::vector<RECT> &boxes);

// Transform @rect around (@pivot_x, @pivot_y) by uniform @scale and vertical
// translation @translate_y (matching GNOME Shell ThumbnailsBox pivot (0.5, 0.5)
// scale 1.0->0.5 & translation_y 0->height/2 during WINDOW_PICKER<->APP_GRID).
RECT transform_rect_around_pivot(const RECT &rect,
                                 double pivot_x,
                                 double pivot_y,
                                 double scale,
                                 double translate_y);

struct WallpaperDrawRect {
  double x = 0.0;
  double y = 0.0;
  double w = 0.0;
  double h = 0.0;
};

// Compute the cover-fitted wallpaper draw rect inside a workarea-aspect
// workspace card (or thumbnail pill), matching upstream GNOME Shell's
// shell-workspace-background.c:96-113: the wallpaper is fitted to the FULL
// monitor (including the top panel strip) and shifted up by
// card_h * (panel_h / workarea_h) so t=0 aligns pixel-for-pixel with the
// live desktop underneath the top bar with zero twitch/jump.
WallpaperDrawRect compute_workarea_wallpaper_draw_rect(double card_x,
                                                       double card_y,
                                                       double card_w,
                                                       double card_h,
                                                       double tex_w,
                                                       double tex_h,
                                                       double panel_top_ratio);

}  // namespace winome
