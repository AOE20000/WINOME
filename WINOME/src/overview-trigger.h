// SPDX-License-Identifier: GPL-2.0-only
// Origin: WinOverview <https://github.com/valinet/WinOverview> (GPL-2.0-or-later)
// Modified for WINOME, 2026-08-16. Ported daemon trigger into the host.
// Keyboard/mouse hooks that trigger the Activities overview.

#pragma once

namespace winome {

// Callback invoked (on the main thread) whenever the Activities overview opens
// or closes. Used e.g. by the panel to close its popovers when the overview
// takes over.
typedef void (*OverviewStateCallback)(bool active, void *user_data);

// Start the low-level keyboard/mouse hooks (a bare Win press toggles the
// overview, Win+Tab opens the Start menu, Esc closes it; other Win combos are
// forwarded to Windows). Runs on a background thread with its own message
// loop.
void start_overview_trigger();

// Open the Activities overview if it is not already running (used by the panel
// Activities button and the top-left hot corner).
void open_overview();

// Toggle the Activities overview (used by the panel Activities button).
void toggle_overview();

// Whether the Activities overview is currently open.
bool overview_active();

// Register a callback for overview open/close state changes. Only one
// callback is kept; pass NULL to unregister.
void set_overview_change_callback(OverviewStateCallback callback,
                                  void *user_data);

}  // namespace winome
