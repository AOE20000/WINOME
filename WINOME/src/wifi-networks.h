// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Wi-Fi data source over the native WLAN API (wlanapi.dll).
//
// wlanapi.dll is delay-loaded by Windows: it is absent on systems without a
// wireless stack, so we resolve the entry points dynamically instead of
// linking them. A missing DLL simply leaves the Wi-Fi indicator off, matching
// how the indicator already behaves on a desktop with no radio.

#pragma once

#include <glib.h>

typedef struct {
  // UTF-8, NUL-terminated. A 32-byte SSID can expand to 96 bytes when every
  // byte is multi-byte UTF-8, so this is sized for the worst case; "" when
  // the network is hidden or its name cannot be decoded.
  char     ssid[97];
  int      signal_quality; // 0..100
  gboolean connected;      // associated with this network right now
  gboolean secured;        // requires a key
  gboolean connectable;    // WLAN reports it as connectable
} WinomeWifiNetwork;

// One wireless interface's current association.
typedef struct {
  char     ssid[97];
  int      signal_quality;
  gboolean connected;      // the radio is associated
  gboolean enabled;        // the interface itself is on (radio not off)
} WinomeWifiStatus;

// Fill @out with the first connected interface's state. Returns false when
// there is no wireless interface at all (desktop, driver missing, DLL absent).
gboolean winome_wifi_get_status (WinomeWifiStatus *out);

// Enumerate visible networks for the first wireless interface. On success
// @out receives a contiguous array of @out_count WinomeWifiNetwork, owned by
// the caller (free with winome_wifi_free_networks). @out_count may be 0 when
// the radio is on but sees nothing. Returns false when enumeration itself
// failed (no interface, driver error, wlanapi.dll absent).
gboolean winome_wifi_scan (WinomeWifiNetwork **out, guint *out_count);

void winome_wifi_free_networks (WinomeWifiNetwork *networks);

// Ask Windows to connect to @ssid using its stored profile. Returns false if
// no matching profile exists or the request was rejected.
gboolean winome_wifi_connect (const char *ssid);

// Check whether a saved WLAN profile already exists for @ssid.
gboolean winome_wifi_has_profile (const char *ssid);

// Save a user WLAN profile (open or WPA2-PSK) and connect to @ssid.
gboolean winome_wifi_connect_with_password (const char *ssid,
                                            const char *password,
                                            gboolean open_network);

// Build a Windows WLAN profile XML payload for @ssid (heap allocated with
// g_malloc; caller frees with g_free). Exposed for automated unit testing.
gchar *winome_wifi_build_profile_xml (const char *ssid,
                                      const char *password,
                                      gboolean open_network);

// Issue a non-blocking hardware WLAN scan on a background thread.
void winome_wifi_trigger_scan_async (void);

// Disconnect the current association (the radio stays on).
gboolean winome_wifi_disconnect (void);

// Enable or disable the first wireless interface's software radio state.
gboolean winome_wifi_set_enabled (gboolean enabled);

// Event-driven WLAN notification subscription (WlanRegisterNotification).
typedef void (*WinomeWifiChangedCallback)(gpointer user_data);
void winome_wifi_add_listener (WinomeWifiChangedCallback cb, gpointer user_data);
void winome_wifi_remove_listener (WinomeWifiChangedCallback cb, gpointer user_data);
void winome_wifi_shutdown_notifications (void);

// Decode raw IEEE 802.11 SSID bytes (up to 32 bytes) into a UTF-8 C string.
// Invalid UTF-8 falls back to Windows-1252 / Latin-1; hidden NUL-containing
// SSIDs produce "".
void winome_wifi_decode_ssid (const guint8 *bytes, gsize len,
                              char *out, gsize out_size);