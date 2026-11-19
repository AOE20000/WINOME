// SPDX-License-Identifier: GPL-2.0-only
// Origin: WinOverview <https://github.com/valinet/WinOverview> (GPL-2.0-or-later)
// Modified for WINOME, 2026-08-16. Ported daemon trigger into the host.
// 2026-08-18: the overview is rendered in-host (overview.cpp); this module
// only owns the global hotkeys and the overview state flag.
// Keyboard hook that triggers the Activities overview.

#pragma once

namespace winome {

enum class WinKeyComboAction {
  ModifierPassthrough,
  StartMenu,
  WorkspacePrev,
  WorkspaceNext,
  PassthroughWithVdHint,
  PassthroughCombo,
};

// Returns true if @vk is a Shift/Ctrl/Alt modifier key that should pass
// through while Win is held without prematurely injecting Win.
inline bool is_win_combo_modifier_vk(unsigned vk) {
  return vk == 0x10 /* VK_SHIFT */ || vk == 0x11 /* VK_CONTROL */ ||
         vk == 0x12 /* VK_MENU */ || vk == 0xA0 /* VK_LSHIFT */ ||
         vk == 0xA1 /* VK_RSHIFT */ || vk == 0xA2 /* VK_LCONTROL */ ||
         vk == 0xA3 /* VK_RCONTROL */ || vk == 0xA4 /* VK_LMENU */ ||
         vk == 0xA5 /* VK_RMENU */;
}

// Classifies a keydown arriving while Win is held down. WINOME only consumes
// plain Win+Tab (Start menu) and Win(+Shift)+PageUp/PageDown (workspaces);
// every non-conflicting Win shortcut (e.g. Win+V clipboard, Win+E, Win+R,
// Win+Shift+S, Win+Ctrl+Arrows, Win+Alt+Tab) is passed through.
inline WinKeyComboAction classify_win_key_combo(unsigned vk,
                                                bool is_syskeydown,
                                                bool ctrl_held,
                                                bool shift_held,
                                                bool alt_held,
                                                bool win_already_injected) {
  if (is_win_combo_modifier_vk(vk))
    return WinKeyComboAction::ModifierPassthrough;

  if (!win_already_injected && !is_syskeydown && !ctrl_held && !alt_held &&
      !shift_held && vk == 0x09 /* VK_TAB */) {
    return WinKeyComboAction::StartMenu;
  }

  if (!win_already_injected && !ctrl_held && !alt_held &&
      (vk == 0x21 /* VK_PRIOR */ || vk == 0x22 /* VK_NEXT */)) {
    return (vk == 0x21) ? WinKeyComboAction::WorkspacePrev
                        : WinKeyComboAction::WorkspaceNext;
  }

  if (ctrl_held && !alt_held &&
      (vk == 0x25 /* VK_LEFT */ || vk == 0x27 /* VK_RIGHT */ ||
       vk == 'D' || vk == 0x73 /* VK_F4 */)) {
    return WinKeyComboAction::PassthroughWithVdHint;
  }

  return WinKeyComboAction::PassthroughCombo;
}

// Callback invoked (on the main thread) whenever the Activities overview opens
// or closes. Used e.g. by the panel to close its popovers when the overview
// takes over.
typedef void (*OverviewStateCallback)(bool active, void *user_data);

// Start the low-level keyboard hook (a bare Win press toggles the overview,
// Win+Tab opens the Start menu, Esc closes it; other Win combos are forwarded
// to Windows). Runs on a background thread with its own message loop.
void start_overview_trigger();
void stop_overview_trigger();

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
