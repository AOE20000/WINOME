// SPDX-License-Identifier: GPL-2.0-only
// Origin: WinOverview <https://github.com/valinet/WinOverview> (GPL-2.0-or-later)
// Modified for WINOME, 2026-08-16. Ported daemon trigger into the host.
// 2026-08-18: the overview is rendered in-host (overview.cpp); this module
// only owns the global hotkeys and the overview state flag.
// Keyboard hook that triggers the Activities overview.

#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace winome {

// Wait-free single-producer / single-consumer ring buffer used so low-level
// hook callbacks (WH_KEYBOARD_LL) never touch mutexes or GLib locks.
template <typename T, size_t Capacity>
class AtomicTriggerRing {
  static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                "Capacity must be a power of two >= 2");

 public:
  bool try_push(const T& item) {
    const size_t h = head_.load(std::memory_order_relaxed);
    const size_t next = (h + 1) & (Capacity - 1);
    if (next == tail_.load(std::memory_order_acquire))
      return false;
    buf_[h] = item;
    head_.store(next, std::memory_order_release);
    return true;
  }

  bool try_pop(T* out) {
    const size_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire))
      return false;
    *out = buf_[t];
    tail_.store((t + 1) & (Capacity - 1), std::memory_order_release);
    return true;
  }

  void clear() {
    tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
  }

 private:
  std::array<T, Capacity> buf_{};
  std::atomic<size_t> head_{0};
  std::atomic<size_t> tail_{0};
};

struct HookWatchdogInput {
  uint64_t now_tick = 0;
  bool win_down = false;
  bool win_injected = false;
  bool win_physically_down = false;
  uint64_t win_last_keydown_tick = 0;
  uint64_t win_inject_tick = 0;
  unsigned swallowed_combo_vk = 0;
  uint64_t combo_swallow_tick = 0;
  bool alt_f2_down = false;
  uint64_t alt_f2_tick = 0;
  bool esc_swallowed = false;
  uint64_t esc_swallow_tick = 0;
};

struct HookWatchdogOutcome {
  bool reset_win = false;
  bool clear_combo = false;
  bool clear_alt_f2 = false;
  bool clear_esc = false;
  bool safe_to_reinstall = false;
};

// Determines whether any swallowed hook state has gone stale (e.g. a keyup was
// lost across UIPI, focus transitions, or silent hook eviction) so the hook
// watchdog never locks itself out permanently.
inline HookWatchdogOutcome evaluate_hook_watchdog(const HookWatchdogInput& in) {
  HookWatchdogOutcome out{};
  if (in.win_down) {
    if (in.win_injected) {
      if ((in.now_tick - in.win_inject_tick) > 200 && !in.win_physically_down)
        out.reset_win = true;
    } else if ((in.now_tick - in.win_last_keydown_tick) > 2500) {
      out.reset_win = true;
    }
  }
  if (in.swallowed_combo_vk != 0 &&
      (in.now_tick - in.combo_swallow_tick) > 1500) {
    out.clear_combo = true;
  }
  if (in.alt_f2_down && (in.now_tick - in.alt_f2_tick) > 1500)
    out.clear_alt_f2 = true;
  if (in.esc_swallowed && (in.now_tick - in.esc_swallow_tick) > 1500)
    out.clear_esc = true;

  const bool active_win = in.win_down && !out.reset_win;
  const bool active_combo =
      (in.swallowed_combo_vk != 0) && !out.clear_combo && !out.reset_win;
  const bool active_f2 = in.alt_f2_down && !out.clear_alt_f2 && !out.reset_win;
  const bool active_esc =
      in.esc_swallowed && !out.clear_esc && !out.reset_win;
  out.safe_to_reinstall =
      !active_win && !active_combo && !active_f2 && !active_esc;
  return out;
}

// Determines whether a low-level Windows hook needs to be (re-)installed.
// Avoids churning a healthy live hook that is actively receiving events.
inline bool should_reinstall_ll_hook(bool has_hook,
                                     uint64_t now_tick,
                                     uint64_t last_event_tick,
                                     uint64_t last_reinstall_tick,
                                     uint64_t silent_threshold_ms,
                                     uint64_t min_spacing_ms) {
  if (!has_hook)
    return true;
  if (last_event_tick != 0 &&
      (now_tick - last_event_tick) < silent_threshold_ms) {
    return false;
  }
  if (last_reinstall_tick != 0 &&
      (now_tick - last_reinstall_tick) < min_spacing_ms) {
    return false;
  }
  return true;
}

// Determines whether a foreground Shell CoreWindow/Xaml host actually represents
// a visible, uncloaked Start Menu / Search surface (and not a cloaked Win11
// StartMenuExperienceHost window left foreground after dismissal).
inline bool is_start_menu_window_active(bool visible,
                                        unsigned cloaked,
                                        bool is_core_class,
                                        bool is_start_exe) {
  return visible && cloaked == 0 && is_core_class && is_start_exe;
}

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

enum class VdComboInProcessAction {
  Passthrough,
  SwitchPrev,
  SwitchNext,
  CreateAndSwitch,
  RemoveCurrent,
};

// Decides whether a Win+Ctrl+{Left,Right,D,F4} hotkey should be handled
// directly in-process via WINOME's workspace/VD COM backend instead of
// re-injecting synthetic Win keystrokes toward Windows.
inline VdComboInProcessAction resolve_vd_combo_in_process_action(
    unsigned vk,
    bool shift_held,
    bool alt_held,
    bool win_already_injected,
    bool independent_mode,
    bool native_vd_ready,
    bool native_can_manage_desktops) {
  if (win_already_injected || shift_held || alt_held)
    return VdComboInProcessAction::Passthrough;
  if (vk == 0x25 /* VK_LEFT */ || vk == 0x27 /* VK_RIGHT */) {
    if (independent_mode || native_vd_ready) {
      return (vk == 0x25) ? VdComboInProcessAction::SwitchPrev
                          : VdComboInProcessAction::SwitchNext;
    }
    return VdComboInProcessAction::Passthrough;
  }
  if (vk == 'D') {
    return (independent_mode || (native_vd_ready && native_can_manage_desktops))
               ? VdComboInProcessAction::CreateAndSwitch
               : VdComboInProcessAction::Passthrough;
  }
  if (vk == 0x73 /* VK_F4 */) {
    return (independent_mode || (native_vd_ready && native_can_manage_desktops))
               ? VdComboInProcessAction::RemoveCurrent
               : VdComboInProcessAction::Passthrough;
  }
  return VdComboInProcessAction::Passthrough;
}

// Callback invoked (on the main thread) whenever the Activities overview opens
// or closes. @monitor identifies the affected monitor (or nullptr when all
// monitors transition together, e.g. linked open / close-all). Used e.g. by
// the panel to close only popovers on the affected monitor.
typedef void (*OverviewStateCallback)(bool active, HMONITOR monitor, void *user_data);

// Start the low-level keyboard hook (a bare Win press toggles the overview,
// Win+Tab opens the Start menu, Esc closes it; other Win combos are forwarded
// to Windows). Runs on a background thread with its own message loop.
void start_overview_trigger();
void stop_overview_trigger();

// Open the Activities overview if it is not already running (used by the panel
// Activities button and the top-left hot corner). @source is a short tag for
// diagnostics (which trigger fired); may be null.
void open_overview(const char *source);

// Open the Activities overview on a specific target monitor (or relocate an
// already-open overview to @monitor if it is currently on another display).
// Passing nullptr targets the active (pointer/foreground) monitor.
void open_overview_for_monitor(const char *source, HMONITOR monitor);

// Close the Activities overview if it is running. Thread-safe; safe to call
// from the overview window's own click handlers. @source is a diagnostic tag
// identifying the close path; may be null.
void close_overview(const char *source);

// Close the Activities overview on a specific monitor (in unlinked mode) or
// across all monitors (in linked mode).
void close_overview_for_monitor(const char *source, HMONITOR monitor);

// Toggle the Activities overview (used by the panel Activities button).
// @source is a diagnostic tag; may be null.
void toggle_overview(const char *source);

// Toggle the Activities overview for a specific monitor: if open on a
// different monitor, relocates it to @monitor instead of closing.
void toggle_overview_for_monitor(const char *source, HMONITOR monitor);

// Open the Windows Start menu via a synthetic (tagged) Win key press; the
// tag lets our own keyboard hook pass it through to the system. Used by the
// overview dash "Show Apps" button.
void open_start_menu();

// Whether the Activities overview is currently open on any monitor.
bool overview_active();

// Synchronize the trigger's active flag with actual open state (main thread).
void sync_overview_active_state(bool active);

// Register a callback for overview open/close state changes. Only one
// callback is kept; pass NULL to unregister.
void set_overview_change_callback(OverviewStateCallback callback,
                                  void *user_data);

// Developer-mode automated testing helpers:
// - simulate_bare_win_tap(): directly enqueues a BareWinUp action into the
//   real trigger state machine (deterministic, bypasses OS input queue).
// - simulate_os_win_tap(): injects a real untagged VK_LWIN down/up via
//   SendInput so Windows routes it end-to-end through WH_KEYBOARD_LL.
// - simulate_os_key_tap(): injects an arbitrary untagged VK down/up via
//   SendInput (e.g. VK_ESCAPE).
void simulate_bare_win_tap();
void simulate_os_win_tap(int hold_ms = 25);
void simulate_os_key_tap(unsigned vk, int hold_ms = 20);

}  // namespace winome
