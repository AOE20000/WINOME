// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Multi-monitor Overview support (linked and unlinked simultaneous modes):
//   - Unlinked (default): each monitor can open/close Overview independently
//     and simultaneously (opening Monitor 1 does not open Monitor 2; opening
//     Monitor 2 while Monitor 1 is open keeps both open simultaneously).
//   - Linked (optional): opening/closing Overview on any monitor opens/closes
//     all monitors together in frame-locked 250ms animation.

#pragma once

#include <gdk/gdk.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace winome {

enum class OverviewDispatchAction {
  None,
  OpenMain,
  CloseMainOnly,
  OpenSecondary,
  CloseSecondaryOnly,
  OpenLinkedAll,
  CloseAll,
};

// Pure multi-monitor Overview dispatch decision logic (unit-tested in
// winome-tests).
inline OverviewDispatchAction decide_multi_monitor_overview_action(
    bool linked,
    bool is_toggle,
    uintptr_t target_mon,
    bool main_open,
    uintptr_t main_mon,
    bool target_has_secondary,
    bool any_secondary_open) {
  if (linked) {
    const bool any_open = main_open || any_secondary_open;
    if (is_toggle)
      return any_open ? OverviewDispatchAction::CloseAll
                      : OverviewDispatchAction::OpenLinkedAll;
    return any_open ? OverviewDispatchAction::None
                    : OverviewDispatchAction::OpenLinkedAll;
  }

  // Unlinked simultaneous mode (default): each monitor toggles independently.
  const bool target_is_main =
      (main_open && target_mon != 0 && target_mon == main_mon);
  if (target_is_main)
    return is_toggle ? OverviewDispatchAction::CloseMainOnly
                     : OverviewDispatchAction::None;
  if (target_has_secondary)
    return is_toggle ? OverviewDispatchAction::CloseSecondaryOnly
                     : OverviewDispatchAction::None;
  if (!main_open)
    return OverviewDispatchAction::OpenMain;
  return OverviewDispatchAction::OpenSecondary;
}

struct SecondaryOverviewGeometry {
  RECT full_box{};
  RECT picker_box{};
  std::vector<RECT> pill_rects;  // workspace selector pills at top (when ws_count > 1)
  int ws_spacing = 36;
};

// Pure geometry computation for a secondary monitor's Overview display
// (unit-tested in winome-tests). All coordinates are in physical px relative
// to the secondary monitor's top-left corner (0, 0).
inline SecondaryOverviewGeometry compute_secondary_overview_geometry(
    int mon_w,
    int mon_h,
    int panel_h,
    int scale,
    int ws_count) {
  SecondaryOverviewGeometry g{};
  const int safe_w = std::max(mon_w, 320);
  const int safe_h = std::max(mon_h, 240);
  const int safe_scale = std::max(scale, 1);
  const int safe_panel = std::clamp(panel_h, 0, safe_h / 4);
  const int wa_h = std::max(safe_h - safe_panel, 120);
  const bool show_pills = (ws_count > 1);

  g.full_box = RECT{0, safe_panel, safe_w, safe_h};

  const int pill_band_h = show_pills
                              ? std::clamp(static_cast<int>(std::lround(wa_h * 0.08)),
                                           44 * safe_scale, 96 * safe_scale)
                              : 0;
  const int top_pad = show_pills
                          ? (pill_band_h + 24 * safe_scale)
                          : std::max(static_cast<int>(std::lround(wa_h * 0.06)),
                                     24 * safe_scale);
  const int bot_pad = std::max(static_cast<int>(std::lround(wa_h * 0.06)),
                               24 * safe_scale);
  const int side_pad = std::max(static_cast<int>(std::lround(safe_w * 0.05)),
                                28 * safe_scale);

  const int avail_w = std::max(safe_w - 2 * side_pad, 160);
  const int avail_h = std::max(wa_h - top_pad - bot_pad, 120);
  const double aspect = static_cast<double>(safe_w) / static_cast<double>(wa_h);

  int card_h = avail_h;
  int card_w = static_cast<int>(std::lround(card_h * aspect));
  if (card_w > avail_w) {
    card_w = avail_w;
    card_h = std::max(static_cast<int>(std::lround(card_w / aspect)), 100);
  }
  const int card_x = (safe_w - card_w) / 2;
  const int card_y = safe_panel + top_pad + (avail_h - card_h) / 2;
  g.picker_box = RECT{card_x, card_y, card_x + card_w, card_y + card_h};
  g.ws_spacing = std::clamp(static_cast<int>(std::lround(safe_w * 0.03)),
                            24 * safe_scale, 72 * safe_scale);

  if (show_pills) {
    const int pill_h = std::max(pill_band_h - 8 * safe_scale, 32 * safe_scale);
    const int pill_w = std::max(static_cast<int>(std::lround(pill_h * aspect)),
                                56 * safe_scale);
    const int gap = 10 * safe_scale;
    const int total_w = ws_count * pill_w + (ws_count - 1) * gap;
    int px = (safe_w - total_w) / 2;
    const int py = safe_panel + 12 * safe_scale;
    g.pill_rects.reserve(static_cast<size_t>(ws_count));
    for (int i = 0; i < ws_count; ++i) {
      g.pill_rects.push_back(RECT{px, py, px + pill_w, py + pill_h});
      px += pill_w + gap;
    }
  }
  return g;
}

// Compute the bounding rect of neighboring workspace @ws_index relative to
// @active_ws inside a secondary monitor display.
inline RECT compute_secondary_workspace_box(const SecondaryOverviewGeometry& g,
                                            int ws_index,
                                            int active_ws) {
  const int card_w = g.picker_box.right - g.picker_box.left;
  const int delta = ws_index - active_ws;
  if (delta == 0)
    return g.picker_box;
  const int dx = delta * (card_w + g.ws_spacing);
  return RECT{g.picker_box.left + dx, g.picker_box.top,
              g.picker_box.right + dx, g.picker_box.bottom};
}

// --- Linked mode API (driven in lockstep with overview.cpp) -----------------

// Synchronize and show Secondary Overview windows on all monitors other than
// @primary_overview_monitor. Shares @wallpaper_texture (may be nullptr).
void overview_secondary_sync(HMONITOR primary_overview_monitor,
                             GdkTexture* wallpaper_texture,
                             double progress);

// Update open/close animation progress [0..1] in lockstep for linked hosts.
void overview_secondary_set_progress(double progress);

// --- Independent (unlinked simultaneous) mode API ---------------------------

// Open a self-animated Secondary Overview on @monitor only.
void overview_secondary_open_monitor(HMONITOR monitor,
                                     GdkTexture* wallpaper_texture);

// Close @monitor's Secondary Overview with smooth 250ms animation.
void overview_secondary_close_monitor(HMONITOR monitor);

// Close all currently open Secondary Overviews with smooth 250ms animation.
void overview_secondary_close_all_animated();

// Query per-monitor or aggregate Secondary Overview open state.
bool overview_secondary_is_open_for_monitor(HMONITOR monitor);
bool overview_secondary_any_open();

struct CrossMonitorDropTarget {
  HMONITOR monitor = nullptr;
  int workspace = -1;
  bool create_new = false;
  bool is_overview = false;
};

// Hit-test a global screen point against any currently open Secondary Overview
// (top workspace pills, peeking neighbor workspace cards, or active workspace
// card). Returns true if @screen_pt lands inside an open Secondary Overview.
bool overview_secondary_hit_test_screen_point(POINT screen_pt,
                                              CrossMonitorDropTarget* out);

// Update visual drop-target highlight on Secondary Overviews during an active
// drag (from Main Overview, another Secondary Overview, or a live desktop
// window). Pass clear to remove all highlights.
void overview_secondary_set_drop_highlight(POINT screen_pt);
void overview_secondary_clear_drop_highlight();

// Rebuild an open Secondary Overview on @monitor (or all open hosts if null).
void overview_secondary_refresh_monitor(HMONITOR monitor);

// Prune hosts belonging to disconnected monitors and reposition/restack any
// remaining open Secondary Overview windows on WM_DISPLAYCHANGE / DPI change.
void overview_secondary_reposition_all();

// --- Shared maintenance API -------------------------------------------------

// Update shared wallpaper texture when wallpaper reloads.
void overview_secondary_set_wallpaper(GdkTexture* wallpaper_texture);

// Immediately hide all Secondary Overview windows and release thumbnails.
void overview_secondary_hide_all();

// Re-assert z-order (panel > secondary overview) for each visible host.
void overview_secondary_restack();

// Destroy all Secondary Overview windows cleanly on shutdown.
void overview_secondary_shutdown();

// True if @hwnd belongs to any managed Secondary Overview window.
bool overview_secondary_is_hwnd(HWND hwnd);

}  // namespace winome
