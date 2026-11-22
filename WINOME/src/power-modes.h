// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Windows power mode switching (PowrProf), backing the Quick Settings
// "Power Mode" secondary menu.
//
// Modern Windows 10 (1809+) and Windows 11 ship only the Balanced legacy
// power plan and control Power Saver / Balanced / Best Performance through
// Power Overlay Schemes (PowerGetEffectiveOverlayScheme /
// PowerSetActiveOverlayScheme). We prefer the overlay API so changes stay
// bidirectionally synced with Windows Settings -> Power & battery -> Power
// mode, and fall back gracefully to legacy power plans on older builds.

#pragma once

#include <glib.h>

typedef enum {
  WINOME_POWER_SAVER,
  WINOME_POWER_BALANCED,
  WINOME_POWER_PERFORMANCE,
  WINOME_POWER_MODE_COUNT
} WinomePowerMode;

// The active power mode, or WINOME_POWER_MODE_COUNT when it cannot be
// determined (no PowrProf, no stock plans installed). Never fails loudly:
// the caller renders the menu without a checked ornament in that case.
WinomePowerMode winome_power_get_active (void);

// Switch to @mode. Returns FALSE if the scheme could not be applied.
gboolean winome_power_set_mode (WinomePowerMode mode);

// Whether @mode is supported on this machine (always TRUE for all 3 modes
// when Windows Power Overlay Schemes are available; otherwise checks legacy
// power plans).
gboolean winome_power_mode_available (WinomePowerMode mode);

// Pure helper mapping a base power-plan GUID string and an optional overlay
// GUID string (both canonical "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" form,
// case-insensitive; @overlay_guid may be NULL or empty) to a WinomePowerMode.
WinomePowerMode winome_power_mode_from_guid_strings (const char *base_guid,
                                                     const char *overlay_guid);

// Human-readable name for @mode in the system UI language ("Power Saver" /
// "Balanced" / "Performance").
const char *winome_power_mode_label (WinomePowerMode mode);

// Icon name for @mode, matching the stock symbolic icons.
const char *winome_power_mode_icon (WinomePowerMode mode);