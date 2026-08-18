// SPDX-License-Identifier: GPL-2.0-only
// Origin: WinOverview <https://github.com/valinet/WinOverview> (GPL-2.0-or-later)
// Modified for WINOME, 2026-08-16. Ported daemon trigger into the host.
// 2026-08-18: the overview is rendered in-host (overview.cpp); this module
// only owns the global hotkeys and the overview state flag.
// Keyboard hook that triggers the Activities overview.

#pragma once

namespace winome {

// Callback invoked (on the main thread) whenever the Activities overview opens
// or closes. Used e.g. by the panel to close its popovers when the overview
// takes over.
typedef void (*OverviewStateCallback)(bool active, void *user_data);

// Start the low-level keyboard hook (a bare Win press toggles the overview,
// Win+Tab opens the Start menu, Esc closes it; other Win combos are forwarded
// to Windows). Runs on a background thread with its own message loop.
void start_overview_trigger();

// Open the Activities overview if it is not already running (used by the panel
// Activities button and the top-left hot corner). @source is a short tag for
// diagnostics (which trigger fired); may be null.
void open_overview(const char *source);

// Close the Activities overview if it is running. Thread-safe; safe to call
// from the overview window's own click handlers. @source is a diagnostic tag
// identifying the close path; may be null.
void close_overview(const char *source);

// Toggle the Activities overview (used by the panel Activities button).
// @source is a diagnostic tag; may be null.
void toggle_overview(const char *source);

// Open the Windows Start menu via a synthetic (tagged) Win key press; the
// tag lets our own keyboard hook pass it through to the system. Used by the
// overview dash "Show Apps" button.
void open_start_menu();

// Whether the Activities overview is currently open.
bool overview_active();

// Register a callback for overview open/close state changes. Only one
// callback is kept; pass NULL to unregister.
void set_overview_change_callback(OverviewStateCallback callback,
                                  void *user_data);

}  // namespace winome
