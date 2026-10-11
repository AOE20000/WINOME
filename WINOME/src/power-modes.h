// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Windows power scheme switching (PowrProf), backing the Quick Settings
// "Power Mode" secondary menu.
//
// GNOME's power-profile rows map onto Windows' three stock power plans
// (PowerSetActiveScheme). We enumerate the GUIDs once and keep the mapping
// symbolic, so a machine that only exposes a subset (e.g. a desktop with no
// Balanced plan) simply omits the missing rows.

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

// Whether @mode exists on this machine, so the menu can hide unsupported
// rows instead of offering one that silently fails.
gboolean winome_power_mode_available (WinomePowerMode mode);

// Human-readable name for @mode in the system UI language ("Power Saver" /
// "Balanced" / "Performance").
const char *winome_power_mode_label (WinomePowerMode mode);

// Icon name for @mode, matching the stock symbolic icons.
const char *winome_power_mode_icon (WinomePowerMode mode);