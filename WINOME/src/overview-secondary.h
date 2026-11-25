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

#include "workspace-manager.h"

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

// Whether CloseAll must explicitly close secondary Overview hosts (when unlinked
// or when main Overview is not open to drive linked close).
inline bool should_close_secondary_on_close_all(bool linked,
                                                bool main_open,
                                                bool any_secondary_open) {
  return any_secondary_open && (!linked || !main_open);
}

// Whether a Secondary Overview display should render its own bottom Dash:
// both Independent and Span workspace modes show a Dash on every open
// overview display (in both linked and unlinked simultaneous modes), while
// PrimaryOnly (GNOME default) keeps a single Dash on the active overview display.
inline bool should_secondary_overview_show_dash(
    ws::MultiMonitorWorkspaceMode mode) {
  return mode != ws::MultiMonitorWorkspaceMode::PrimaryOnly;
}

// Unified per-monitor Overview role resolved from workspace mode and whether
// the monitor is currently WINOME's Shell Primary (unit-tested in winome-tests).
// All monitors share the exact same Overview engine, with independent search
// and App Grid available in all states; Dash & multi-workspace switching follow
// the active workspace mode and dynamic Shell Primary.
struct OverviewMonitorRole {
  bool is_shell_primary = true;
  bool show_search = true;
  bool show_dash = true;
  bool allow_app_grid = true;
  bool multi_workspace = true;
};

inline OverviewMonitorRole resolve_monitor_overview_role(
    ws::MultiMonitorWorkspaceMode mode,
    bool is_shell_primary) {
  OverviewMonitorRole role{};
  role.is_shell_primary = is_shell_primary;
  role.show_search = true;
  role.allow_app_grid = true;
  if (mode == ws::MultiMonitorWorkspaceMode::PrimaryOnly) {
    role.show_dash = is_shell_primary;
    role.multi_workspace = is_shell_primary;
  } else {
    role.show_dash = true;
    role.multi_workspace = true;
  }
  return role;
}

enum class UnifiedOverviewAction {
  None,
  OpenMonitor,
  CloseMonitor,
  OpenAllLinked,
  CloseAll,
};

// Pure unified multi-monitor Overview dispatch logic (no primary/secondary
// engine split: every monitor runs the same OverviewView engine).
inline UnifiedOverviewAction decide_unified_overview_action(
    bool linked,
    bool is_toggle,
    bool target_is_open,
    bool any_open) {
  if (linked) {
    if (is_toggle)
      return any_open ? UnifiedOverviewAction::CloseAll
                      : UnifiedOverviewAction::OpenAllLinked;
    return any_open ? UnifiedOverviewAction::None
                    : UnifiedOverviewAction::OpenAllLinked;
  }
  if (target_is_open)
    return is_toggle ? UnifiedOverviewAction::CloseMonitor
                     : UnifiedOverviewAction::None;
  return UnifiedOverviewAction::OpenMonitor;
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
    int ws_count,
    int dash_reserved_h = 0) {
  SecondaryOverviewGeometry g{};
  const int safe_w = std::max(mon_w, 320);
  const int safe_h = std::max(mon_h, 240);
  const int safe_scale = std::max(scale, 1);
  const int safe_panel = std::clamp(panel_h, 0, safe_h / 4);
  const int wa_h = std::max(safe_h - safe_panel, 120);
  const bool show_pills = (ws_count > 1);
  const int safe_dash = std::clamp(dash_reserved_h, 0, wa_h / 3);

  g.full_box = RECT{0, safe_panel, safe_w, safe_h};

  const int pill_band_h = show_pills
                              ? std::clamp(static_cast<int>(std::lround(wa_h * 0.08)),
                                           44 * safe_scale, 96 * safe_scale)
                              : 0;
  const int top_pad = show_pills
                          ? (pill_band_h + 24 * safe_scale)
                          : std::max(static_cast<int>(std::lround(wa_h * 0.06)),
                                     24 * safe_scale);
  const int bot_pad =
      (safe_dash > 0)
          ? (safe_dash + 14 * safe_scale)
          : std::max(static_cast<int>(std::lround(wa_h * 0.06)),
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

// Guard against a delayed pre-switch Virtual Desktop poll echoing the previous
// desktop index right after an Overview-initiated switch (e.g. rapid 0 -> 1 -> 2
// where an async poll for hop 1 reports active=1 right after moving to 2).
// Never suppresses genuine switches to another desktop or after the 350ms grace.
inline bool should_ignore_stale_desktop_echo(int incoming_active,
                                             int last_internal_from,
                                             int last_internal_to,
                                             int64_t elapsed_us) {
  constexpr int64_t kStaleEchoGraceUs = 350000;
  return last_internal_from >= 0 && last_internal_from != last_internal_to &&
         incoming_active == last_internal_from && elapsed_us >= 0 &&
         elapsed_us < kStaleEchoGraceUs;
}

}  // namespace winome
