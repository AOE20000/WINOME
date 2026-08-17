// SPDX-License-Identifier: GPL-2.0-only
// Origin: WinOverview <https://github.com/valinet/WinOverview> (GPL-2.0-or-later)
// Modified for WINOME, 2026-08-16. Ported daemon trigger into the host.
// Keyboard/mouse hooks that trigger the Activities overview.

#pragma once

namespace winome {

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

}  // namespace winome
