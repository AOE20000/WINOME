// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// GNOME-style top-left hot corner: moving the cursor into the corner of the
// primary monitor opens the Activities overview.

#include "hot-corner.h"
#include "fullscreen-watcher.h"
#include "overview-trigger.h"

#include <windows.h>
#include <glib.h>

namespace winome {

namespace {

// Size of the hot-corner target in pixels inside the monitor's top-left
// corner, poll interval, and re-trigger cooldown.
constexpr int kCornerSize = 5;
constexpr int kPollMs = 100;
constexpr int kCooldownMs = 500;

gboolean hot_corner_tick(gpointer user_data) {
  HWND panel = static_cast<HWND>(user_data);
  static bool in_corner = false;
  static guint64 last_trigger_ms = 0;
  static RECT corner_monitor = {};
  static guint64 next_refresh_ms = 0;

  POINT pt;
  if (!GetCursorPos(&pt))
    return G_SOURCE_CONTINUE;

  // The hot corner lives on the panel: while the panel is hidden (a
  // fullscreen window or the lock screen is active) the corner must not
  // trigger. Reset the entry state so an overview does not pop open when the
  // fullscreen ends while the cursor is still sitting in the corner (GNOME
  // requires re-entering the corner).
  if (winome::panel_hidden()) {
    in_corner = false;
    return G_SOURCE_CONTINUE;
  }

  guint64 now_ms = g_get_monotonic_time() / 1000;

  // The primary monitor rect is cached and refreshed once a second, so each
  // tick is just GetCursorPos + a bounds check (negligible CPU).
  if (now_ms >= next_refresh_ms) {
    HMONITOR mon = MonitorFromWindow(panel, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    if (mon != nullptr && GetMonitorInfo(mon, &mi))
      corner_monitor = mi.rcMonitor;
    next_refresh_ms = now_ms + 1000;
  }

  bool in = pt.x >= corner_monitor.left &&
            pt.x <= corner_monitor.left + kCornerSize &&
            pt.y >= corner_monitor.top &&
            pt.y <= corner_monitor.top + kCornerSize;

  if (in && !in_corner && now_ms - last_trigger_ms >= kCooldownMs) {
    last_trigger_ms = now_ms;
    g_print("[hot] corner trigger\n");
    open_overview("hot-corner");
  }
  in_corner = in;
  return G_SOURCE_CONTINUE;
}

}  // namespace

void start_hot_corner(HWND panel_hwnd) {
  g_timeout_add(kPollMs, hot_corner_tick, static_cast<gpointer>(panel_hwnd));
}

}  // namespace winome
