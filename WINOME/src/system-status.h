// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Windows data sources for the GNOME-style system status indicators:
// battery (GetSystemPowerStatus) and volume (WASAPI IAudioEndpointVolume).

#pragma once

namespace winome {

// Battery state, mirroring UPower semantics used by gnome-shell status/system.js.
struct BatteryStatus {
  bool present;           // a battery is present
  bool charging;          // AC connected
  int percentage;         // 0..100
};

// Read the current battery status. Returns false if no battery is present.
bool get_battery_status(BatteryStatus* out);

// Get the current system volume as a 0..1 fraction (0 = mute, 1 = 100%).
// Returns false on failure.
bool get_volume(double* out);

// Set the system volume as a 0..1 fraction.
bool set_volume(double fraction);

// Get mute state.
bool get_mute(bool* out);

// Toggle mute.
bool toggle_mute();

// Get Wi-Fi (wireless network) enabled state.
bool get_wifi_enabled(bool* out);

// Get whether any network is connected.
bool get_network_connected(bool* out);

// Get connected network name (SSID), if any.
bool get_network_name(char* out, int out_size);

// Get current screen brightness as 0..1 fraction.
bool get_brightness(double* out);

// Set current screen brightness as 0..1 fraction.
bool set_brightness(double fraction);

// Lock the workstation (matches gnome-shell SystemActions lock screen).
void lock_screen();

// Sign out / shutdown / restart (Windows equivalents of system actions).
void log_out();
void shutdown_system();
void restart_system();

// System UI language: 'zh' or 'en' (from GetUserDefaultUILanguage).
const char* get_system_language();

}  // namespace winome
