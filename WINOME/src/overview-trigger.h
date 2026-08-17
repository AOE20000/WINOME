// SPDX-License-Identifier: GPL-2.0-only
// Origin: WinOverview <https://github.com/valinet/WinOverview> (GPL-2.0-or-later)
// Modified for WINOME, 2026-08-16. Ported daemon trigger into the host;
// hotkey changed to Win+W.
// Keyboard/mouse hooks that trigger the Activities overview.

#pragma once

namespace winome {

// Start the low-level keyboard/mouse hooks (Win+W triggers the overview,
// Esc closes it). Runs on a background thread with its own message loop.
void start_overview_trigger();

// Toggle the Activities overview (used by the panel Activities button).
void toggle_overview();

}  // namespace winome
