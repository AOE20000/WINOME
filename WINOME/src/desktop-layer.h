// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Embeds a window into the WorkerW desktop layer (below desktop icons).

#pragma once

#include <windows.h>

namespace winome {

// DesktopLayer reparents a window into the Explorer "WorkerW" desktop
// surface, i.e. the layer that sits above the wallpaper but below the desktop
// icons. This is what lets WINOME draw its own top bar without overlapping
// shell chrome.
class DesktopLayer {
 public:
  // Reparent @hwnd into the desktop layer. Returns true on success.
  static bool set_under_desktop_items(HWND hwnd);

 private:
  // Ask Explorer to materialize the WorkerW desktop surface.
  static void request_worker_surface();

  // Locate the WorkerW window that hosts the desktop surface, creating it via
  // request_worker_surface() when missing.
  static HWND find_worker_surface();
};

}  // namespace winome
