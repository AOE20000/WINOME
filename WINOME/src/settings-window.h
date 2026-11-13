// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Native GTK4 WINOME Settings preferences window.

#pragma once

namespace winome {

// Open (or focus if already open) the WINOME Settings window.
void open_settings_window();

// Destroy the settings window if currently open (called during shutdown).
void close_settings_window();

}  // namespace winome
