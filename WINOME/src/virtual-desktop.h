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

#include <cstdint>
#include <vector>

namespace winome {
namespace vd {

struct DesktopInfo {
  int index;
  GUID id;
};

struct VtableProfile {
  int move_view_to_desktop = 4;
  int create_desktop = -1;
  int move_desktop = -1;
  int remove_desktop = -1;
  bool can_manage_desktops = false;
};

// Pure helper mapping an OS build number to safe IVirtualDesktopManagerInternal
// vtable slot indices (22621/23H2 vs 26100+ 24H2). Unrecognized builds return
// can_manage_desktops = false so create/remove are safely disabled.
VtableProfile vtable_slots_for_build (uint32_t build_number);

// Queries the true Windows build number via ntdll!RtlGetVersion (0 on failure).
uint32_t windows_build_number (void);

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
void pin_window_async (HWND hwnd);

// Reset the transient stuck-COM breaker flag when Explorer recovers.
void reset_circuit_breaker (void);

// Stop background observer thread cleanly during shell shutdown.
void shutdown (void);

// Unpin @hwnd from all virtual desktops; FALSE on failure.
bool unpin_window (HWND hwnd);

// Move @hwnd to virtual desktop @target_index (0-based); FALSE on failure.
bool move_window_to_desktop (HWND hwnd, int target_index);

// Create a new virtual desktop; FALSE if unsupported on this OS build or failed.
bool create_desktop (void);

// Remove virtual desktop @index (moves its windows to an adjacent fallback
// desktop). Refuses when count <= 1 or build vtable layout is unrecognized.
bool remove_desktop (int index);

// --- Centralized event-driven desktop observer ----------------------------

struct DesktopSnapshot {
  int count = 1;
  int active_index = 0;
  bool valid = false;
};

using DesktopChangedCallback = void (*) (int count, int active_index,
                                         void *user_data);

// Last observed desktop count / active index without blocking on COM.
DesktopSnapshot cached_snapshot (void);

// Subscribe to virtual desktop count / active-index transitions. Replaces
// per-module polling timers with a single WinEvent + hotkey + safety observer.
void add_change_listener (DesktopChangedCallback cb, void *user_data);
void remove_change_listener (DesktopChangedCallback cb, void *user_data);

// Safe to call from any thread (e.g. WH_KEYBOARD_LL): schedules an immediate
// desktop-state check and notifies listeners if anything changed.
void notify_change_hint_async (void);

}  // namespace vd
}  // namespace winome
