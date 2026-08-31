// SPDX-License-Identifier: MIT
// Origin: VirtualDesktopAccessor-rust
// <https://github.com/Ciantic/VirtualDesktopAccessor> (MIT), ported for
// WINOME 2026-08-19.
//
// Windows 11 24H2+ virtual desktop access through explorer's undocumented
// ImmersiveShell COM services. On older builds the services are unreachable
// and every call here degrades to "unavailable" — callers fall back to
// single-workspace behavior.

#pragma once

#include <windows.h>

#include <vector>

namespace winome {
namespace vd {

struct DesktopInfo {
  int index;
  GUID id;
};

// TRUE when the virtual desktop services are reachable and at least one
// desktop was enumerated.
bool available (void);

// Desktop count, 1 when unavailable.
int count (void);

// Active desktop index, -1 when unavailable.
int current_index (void);

// Desktop list (index + GUID), empty when unavailable.
std::vector<DesktopInfo> desktops (void);

// Index of the desktop owning @hwnd, -1 when unknown or when the window is
// pinned to all desktops (GetDesktopIdByWindow yields the empty GUID).
int window_desktop_index (HWND hwnd);

// TRUE when @hwnd belongs to the current desktop (pinned windows included).
// FALSE when unavailable.
bool window_on_current (HWND hwnd);

// Switch to the desktop by index; FALSE on failure.
bool switch_to (int index);

// Pin @hwnd to ALL virtual desktops ("show this window on every desktop",
// like the taskbar): the window then stays put during desktop switches
// instead of sliding away with its owning desktop. FALSE when the pin
// services are unreachable (older builds, explorer-side failure).
bool pin_window (HWND hwnd);

}  // namespace vd
}  // namespace winome
