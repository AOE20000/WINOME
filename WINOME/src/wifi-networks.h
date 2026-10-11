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
  char     ssid[33];       // UTF-8 network name, "" when unknown/hidden
  int      signal_quality; // 0..100
  gboolean connected;      // associated with this network right now
  gboolean secured;        // requires a key
  gboolean connectable;    // WLAN reports it as connectable
} WinomeWifiNetwork;

// One wireless interface's current association.
typedef struct {
  char     ssid[33];
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

// Disconnect the current association (the radio stays on).
gboolean winome_wifi_disconnect (void);