// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Wi-Fi data source over the native WLAN API (wlanapi.dll).

#include "wifi-networks.h"

#include <windows.h>
#include <wlanapi.h>
#include <objbase.h>
#include <string.h>
#include <stdlib.h>

namespace {

// wlanapi.dll is delay-loaded: absent on machines without a wireless stack.
// Resolve once; a NULL module means "no Wi-Fi anywhere", which the caller
// renders as a hidden indicator rather than an error.
struct WlanApi {
  HMODULE dll;
  DWORD (WINAPI *OpenHandle)(HANDLE, DWORD, PVOID, PVOID, HANDLE *, DWORD *);
  DWORD (WINAPI *CloseHandle)(HANDLE, PVOID);
  DWORD (WINAPI *EnumInterfaces)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST *);
  DWORD (WINAPI *QueryInterface)(HANDLE, const GUID *, WLAN_INTF_OPCODE,
                                 PVOID, PDWORD, PVOID *, PWLAN_OPCODE_VALUE_TYPE);
  DWORD (WINAPI *GetAvailableNetworkList)(HANDLE, const GUID *, DWORD, PVOID,
                                          PWLAN_AVAILABLE_NETWORK_LIST *);
  DWORD (WINAPI *Connect)(HANDLE, PVOID, const GUID *, const WCHAR *,
                          DWORD, PVOID, PVOID);
  DWORD (WINAPI *Disconnect)(HANDLE, PVOID, const GUID *);
  void  (WINAPI *FreeMemory)(PVOID);

  bool loaded;
  bool probed;
};

WlanApi g_api = {};

bool
load_api ()
{
  if (g_api.probed)
    return g_api.loaded;
  g_api.probed = true;

  g_api.dll = LoadLibraryW (L"wlanapi.dll");
  if (g_api.dll == nullptr)
    return false;

  // GetProcAddress returns FARPROC, and casting it straight to a typed
  // function pointer trips -Wcast-function-type. Route through void* first:
  // the object/function pointer conversion is allowed there, so the dynamic
  // load stays warning-free.
  #define WINOME_LOAD_WLAN(field, name)                                   \
    g_api.field = reinterpret_cast<decltype (g_api.field)> (               \
        reinterpret_cast<void *> (GetProcAddress (g_api.dll, name)))

  WINOME_LOAD_WLAN (OpenHandle, "WlanOpenHandle");
  WINOME_LOAD_WLAN (CloseHandle, "WlanCloseHandle");
  WINOME_LOAD_WLAN (EnumInterfaces, "WlanEnumInterfaces");
  WINOME_LOAD_WLAN (QueryInterface, "WlanQueryInterface");
  WINOME_LOAD_WLAN (GetAvailableNetworkList, "WlanGetAvailableNetworkList");
  WINOME_LOAD_WLAN (Connect, "WlanConnect");
  WINOME_LOAD_WLAN (Disconnect, "WlanDisconnect");
  WINOME_LOAD_WLAN (FreeMemory, "WlanFreeMemory");

  #undef WINOME_LOAD_WLAN

  g_api.loaded = g_api.OpenHandle != nullptr &&
                 g_api.CloseHandle != nullptr &&
                 g_api.EnumInterfaces != nullptr &&
                 g_api.QueryInterface != nullptr &&
                 g_api.GetAvailableNetworkList != nullptr &&
                 g_api.Connect != nullptr &&
                 g_api.Disconnect != nullptr &&
                 g_api.FreeMemory != nullptr;
  return g_api.loaded;
}

// Session handle shared by every call. The WLAN API is client-session scoped and
// opening a handle costs a round trip to the driver, so open once and keep it
// for the process lifetime. We never close it: the UI thread is the only
// caller and the handle dies with the process.
HANDLE g_client = nullptr;

bool
ensure_client (HANDLE *out)
{
  if (!load_api ())
    return false;

  if (g_client == nullptr) {
    DWORD version = 0;
    HANDLE client = nullptr;
    // The WLAN API is COM-flavoured: negotiate the version on open.
    if (g_api.OpenHandle (nullptr, 2, nullptr, nullptr, &client, &version)
        != ERROR_SUCCESS)
      return false;
    if (client == nullptr)
      return false;
    g_client = client;
  }

  *out = g_client;
  return true;
}

// Copy a DOT11_SSID into a UTF-8 buffer. Hidden networks report an empty SSID
// but a non-zero uSSIDLength (the bytes are not a name), so we only decode
// when the length looks like plausible UTF-8 text.
void
copy_ssid (const DOT11_SSID &ssid, char *out, size_t out_size)
{
  out[0] = '\0';
  if (ssid.uSSIDLength == 0 || ssid.uSSIDLength >= out_size)
    return;

  bool printable = true;
  for (ULONG i = 0; i < ssid.uSSIDLength; i++) {
    if (ssid.ucSSID[i] == 0) {
      printable = false;
      break;
    }
  }
  if (!printable)
    return;

  memcpy (out, ssid.ucSSID, ssid.uSSIDLength);
  out[ssid.uSSIDLength] = '\0';
}

// Fetch the interface list; caller frees with WlanFreeMemory.
bool
enum_interfaces (HANDLE client, PWLAN_INTERFACE_INFO_LIST *out)
{
  PWLAN_INTERFACE_INFO_LIST list = nullptr;
  if (g_api.EnumInterfaces (client, nullptr, &list) != ERROR_SUCCESS)
    return false;
  if (list == nullptr)
    return false;
  *out = list;
  return true;
}

// First interface that is not in the "radio off" state, or just the first one.
const WLAN_INTERFACE_INFO *
pick_interface (const WLAN_INTERFACE_INFO_LIST *list)
{
  for (DWORD i = 0; i < list->dwNumberOfItems; i++) {
    const WLAN_INTERFACE_INFO &info = list->InterfaceInfo[i];
    if (info.isState != wlan_interface_state_not_ready)
      return &info;
  }
  if (list->dwNumberOfItems > 0)
    return &list->InterfaceInfo[0];
  return nullptr;
}

// Query the current connection attributes (SSID + signal quality).
bool
query_connection (HANDLE client, const GUID &guid, char *ssid, size_t ssid_size,
                  int *quality)
{
  DWORD size = 0;
  WLAN_OPCODE_VALUE_TYPE type = wlan_opcode_value_type_query_only;
  WLAN_CONNECTION_ATTRIBUTES *attrs = nullptr;

  if (g_api.QueryInterface (client, &guid, wlan_intf_opcode_current_connection,
                            nullptr, &size, (PVOID *) &attrs, &type)
      != ERROR_SUCCESS || attrs == nullptr)
    return false;

  copy_ssid (attrs->wlanAssociationAttributes.dot11Ssid, ssid, ssid_size);
  *quality = (int) attrs->wlanAssociationAttributes.wlanSignalQuality;
  g_api.FreeMemory (attrs);
  return true;
}

}  // namespace

gboolean
winome_wifi_get_status (WinomeWifiStatus *out)
{
  if (out == nullptr)
    return FALSE;
  memset (out, 0, sizeof (*out));

  HANDLE client = nullptr;
  if (!ensure_client (&client))
    return FALSE;

  PWLAN_INTERFACE_INFO_LIST list = nullptr;
  bool ok = FALSE;
  if (!enum_interfaces (client, &list))
    return FALSE;

  const WLAN_INTERFACE_INFO *info = pick_interface (list);
  if (info != nullptr) {
    out->enabled = (info->isState != wlan_interface_state_not_ready);
    out->connected =
        (info->isState == wlan_interface_state_connected ||
         info->isState == wlan_interface_state_ad_hoc_network_formed);
    if (query_connection (client, info->InterfaceGuid, out->ssid,
                          sizeof (out->ssid), &out->signal_quality))
      out->connected = TRUE;
    ok = TRUE;
  }

  g_api.FreeMemory (list);
  return ok;
}

gboolean
winome_wifi_scan (WinomeWifiNetwork **out, guint *out_count)
{
  if (out == nullptr)
    return FALSE;
  *out = nullptr;
  if (out_count != nullptr)
    *out_count = 0;

  HANDLE client = nullptr;
  if (!ensure_client (&client))
    return FALSE;

  PWLAN_INTERFACE_INFO_LIST list = nullptr;
  WinomeWifiNetwork *result = nullptr;
  DWORD count = 0;
  bool ok = FALSE;

  if (!enum_interfaces (client, &list))
    return FALSE;

  const WLAN_INTERFACE_INFO *info = pick_interface (list);
  if (info != nullptr) {
    PWLAN_AVAILABLE_NETWORK_LIST avail = nullptr;
    // The third parameter is documented as reserved (must be 0); the
    // WLAN_AVAILABLE_NETWORK_* flags are dwFlags on the struct, not here.
    if (g_api.GetAvailableNetworkList (client, &info->InterfaceGuid, 0,
                                       nullptr, &avail) == ERROR_SUCCESS &&
        avail != nullptr) {
      result = (WinomeWifiNetwork *) calloc (avail->dwNumberOfItems + 1,
                                             sizeof (WinomeWifiNetwork));

      if (result != nullptr) {
        char current_ssid[33] = "";
        int current_quality = 0;
        query_connection (client, info->InterfaceGuid, current_ssid,
                          sizeof (current_ssid), &current_quality);

        for (DWORD i = 0; i < avail->dwNumberOfItems; i++) {
          const WLAN_AVAILABLE_NETWORK &net = avail->Network[i];
          WinomeWifiNetwork *slot = &result[count];

          copy_ssid (net.dot11Ssid, slot->ssid, sizeof (slot->ssid));
          slot->signal_quality = (int) net.wlanSignalQuality;
          slot->secured = net.bSecurityEnabled != 0;
          slot->connectable = net.bNetworkConnectable != 0;
          slot->connected =
              (current_ssid[0] != '\0' &&
               strcmp (current_ssid, slot->ssid) == 0);
          count++;
        }
        *out = result;
        if (out_count != nullptr)
          *out_count = count;
        result = nullptr;
        ok = TRUE;
      }
    }
    if (avail != nullptr)
      g_api.FreeMemory (avail);
  }

  free (result);
  g_api.FreeMemory (list);
  return ok;
}

void
winome_wifi_free_networks (WinomeWifiNetwork *networks)
{
  free (networks);
}

gboolean
winome_wifi_connect (const char *ssid)
{
  if (ssid == nullptr || ssid[0] == '\0')
    return FALSE;

  HANDLE client = nullptr;
  if (!ensure_client (&client))
    return FALSE;

  PWLAN_INTERFACE_INFO_LIST list = nullptr;
  bool ok = FALSE;
  if (enum_interfaces (client, &list)) {
    const WLAN_INTERFACE_INFO *info = pick_interface (list);
    if (info != nullptr) {
      int len = MultiByteToWideChar (CP_UTF8, 0, ssid, -1, nullptr, 0);
      if (len > 0) {
        wchar_t *wide = (wchar_t *) calloc ((size_t) len, sizeof (wchar_t));
        if (wide != nullptr) {
          MultiByteToWideChar (CP_UTF8, 0, ssid, -1, wide, len);
          DWORD flags = 0;
          ok = g_api.Connect (client, nullptr, &info->InterfaceGuid, wide,
                              flags, nullptr, nullptr) == ERROR_SUCCESS;
          free (wide);
        }
      }
    }
    g_api.FreeMemory (list);
  }
  return ok;
}

gboolean
winome_wifi_disconnect (void)
{
  HANDLE client = nullptr;
  if (!ensure_client (&client))
    return FALSE;

  PWLAN_INTERFACE_INFO_LIST list = nullptr;
  bool ok = FALSE;
  if (enum_interfaces (client, &list)) {
    const WLAN_INTERFACE_INFO *info = pick_interface (list);
    if (info != nullptr)
      ok = g_api.Disconnect (client, nullptr, &info->InterfaceGuid)
           == ERROR_SUCCESS;
    g_api.FreeMemory (list);
  }
  return ok;
}