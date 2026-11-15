// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Wi-Fi data source over the native WLAN API (wlanapi.dll).

#include "wifi-networks.h"
#include "perf-metrics.h"

#include <windows.h>
#include <wlanapi.h>
#include <objbase.h>
#include <string.h>
#include <stdlib.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// wlanapi.dll is delay-loaded: absent on machines without a wireless stack.
// Resolve once; a NULL module means "no Wi-Fi anywhere", which the caller
// renders as a hidden indicator rather than an error.
struct WlanApi {
  HMODULE dll;
  // Signatures must match wlanapi.h exactly — a mismatched prototype does not
  // fail to link here (the symbols are resolved at runtime), it shifts the
  // arguments and the call returns ERROR_INVALID_PARAMETER.
  DWORD (WINAPI *OpenHandle)(DWORD, PVOID, PDWORD, HANDLE *);
  DWORD (WINAPI *CloseHandle)(HANDLE, PVOID);
  DWORD (WINAPI *EnumInterfaces)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST *);
  DWORD (WINAPI *QueryInterface)(HANDLE, const GUID *, WLAN_INTF_OPCODE,
                                 PVOID, PDWORD, PVOID *, PWLAN_OPCODE_VALUE_TYPE);
  DWORD (WINAPI *GetAvailableNetworkList)(HANDLE, const GUID *, DWORD, PVOID,
                                          PWLAN_AVAILABLE_NETWORK_LIST *);
  DWORD (WINAPI *SetInterface)(HANDLE, const GUID *, WLAN_INTF_OPCODE,
                               DWORD, const PVOID, PVOID);
  DWORD (WINAPI *Scan)(HANDLE, const GUID *, const PDOT11_SSID,
                       const PWLAN_RAW_DATA, PVOID);
  DWORD (WINAPI *Connect)(HANDLE, const GUID *,
                          const PWLAN_CONNECTION_PARAMETERS, PVOID);
  DWORD (WINAPI *Disconnect)(HANDLE, const GUID *, PVOID);
  DWORD (WINAPI *GetProfile)(HANDLE, const GUID *, LPCWSTR, PVOID,
                             LPWSTR *, DWORD *, PDWORD);
  DWORD (WINAPI *SetProfile)(HANDLE, const GUID *, DWORD, LPCWSTR,
                             LPCWSTR, WINBOOL, PVOID, DWORD *);
  DWORD (WINAPI *RegisterNotification)(HANDLE, DWORD, WINBOOL,
                                       WLAN_NOTIFICATION_CALLBACK, PVOID,
                                       PVOID, PDWORD);
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
  WINOME_LOAD_WLAN (SetInterface, "WlanSetInterface");
  WINOME_LOAD_WLAN (Scan, "WlanScan");
  WINOME_LOAD_WLAN (Connect, "WlanConnect");
  WINOME_LOAD_WLAN (Disconnect, "WlanDisconnect");
  WINOME_LOAD_WLAN (GetProfile, "WlanGetProfile");
  WINOME_LOAD_WLAN (SetProfile, "WlanSetProfile");
  WINOME_LOAD_WLAN (RegisterNotification, "WlanRegisterNotification");
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

struct WifiListenerEntry {
  WinomeWifiChangedCallback cb;
  gpointer user_data;
};

struct WifiNotifState {
  std::mutex mu;
  std::vector<WifiListenerEntry> listeners;
  std::atomic<bool> queued{false};
  bool registered = false;
};

WifiNotifState g_wifi_notif;

gboolean
idle_dispatch_wifi_changed (gpointer G_GNUC_UNUSED data)
{
  g_wifi_notif.queued.store (false, std::memory_order_release);
  std::vector<WifiListenerEntry> copy;
  {
    std::lock_guard<std::mutex> lock (g_wifi_notif.mu);
    copy = g_wifi_notif.listeners;
  }
  for (const auto &e : copy) {
    if (e.cb != nullptr)
      e.cb (e.user_data);
  }
  return G_SOURCE_REMOVE;
}

VOID WINAPI
wlan_notif_callback (PWLAN_NOTIFICATION_DATA data, PVOID G_GNUC_UNUSED ctx)
{
  if (data == nullptr)
    return;
  if (data->NotificationSource != WLAN_NOTIFICATION_SOURCE_ACM &&
      data->NotificationSource != WLAN_NOTIFICATION_SOURCE_MSM) {
    return;
  }
  bool expected = false;
  if (g_wifi_notif.queued.compare_exchange_strong (
          expected, true, std::memory_order_acq_rel)) {
    g_idle_add (idle_dispatch_wifi_changed, nullptr);
  }
}

void
ensure_wlan_notifications (HANDLE client)
{
  std::lock_guard<std::mutex> lock (g_wifi_notif.mu);
  if (g_wifi_notif.registered || client == nullptr ||
      g_api.RegisterNotification == nullptr) {
    return;
  }
  DWORD prev = 0;
  if (g_api.RegisterNotification (
          client,
          WLAN_NOTIFICATION_SOURCE_ACM | WLAN_NOTIFICATION_SOURCE_MSM,
          TRUE, wlan_notif_callback, nullptr, nullptr, &prev) == ERROR_SUCCESS) {
    g_wifi_notif.registered = true;
  }
}

bool
ensure_client (HANDLE *out)
{
  if (!load_api ()) {
    winome::capability_set (winome::SubsystemId::Wlan,
                            winome::CapabilityState::Degraded,
                            "wlanapi.dll unavailable");
    return false;
  }

  if (g_client == nullptr) {
    DWORD version = 0;
    HANDLE client = nullptr;
    // WlanOpenHandle(dwClientVersion, pReserved, pdwNegotiatedVersion,
    //                 phClientHandle) — the negotiated version is an OUTPUT.
    // Requesting 2 is what Windows 7+ expects; the DLL negotiates downwards
    // if the service is older.
    if (g_api.OpenHandle (2, nullptr, &version, &client) != ERROR_SUCCESS) {
      winome::capability_set (winome::SubsystemId::Wlan,
                              winome::CapabilityState::Degraded,
                              "WLAN AutoConfig service unavailable");
      return false;
    }
    if (client == nullptr) {
      winome::capability_set (winome::SubsystemId::Wlan,
                              winome::CapabilityState::Degraded,
                              "WLAN client handle null");
      return false;
    }
    g_client = client;
  }

  winome::capability_set (winome::SubsystemId::Wlan,
                          winome::CapabilityState::Active);
  ensure_wlan_notifications (g_client);
  *out = g_client;
  return true;
}

// Copy a DOT11_SSID into a UTF-8 buffer.
//
// ucSSID is a raw octet string, not text: the 802.11 spec leaves the encoding
// to the network, and real deployments use UTF-8 (Windows encodes a Chinese
// SSID that way) as well as Latin-1. uSSIDLength is a BYTE count, so copying
// the bytes straight into a char buffer works only for ASCII — a 4-character
// Chinese name is 12 bytes and would be truncated mid-sequence.
//
// So: validate as UTF-8 and keep it if valid, otherwise widen the bytes as
// Latin-1 (which never fails and always yields printable output).
//
// Hidden networks have a non-zero uSSIDLength but no usable name; an embedded
// NUL means those bytes are not a name at all, so leave the buffer empty.
void
copy_ssid (const DOT11_SSID &ssid, char *out, size_t out_size)
{
  winome_wifi_decode_ssid (ssid.ucSSID, ssid.uSSIDLength, out, out_size);
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

// Query current software/hardware radio power state across the interface PHYs.
bool
query_radio_enabled (HANDLE client, const GUID &guid, bool fallback)
{
  DWORD size = 0;
  WLAN_OPCODE_VALUE_TYPE type = wlan_opcode_value_type_query_only;
  WLAN_RADIO_STATE *radio = nullptr;

  if (g_api.QueryInterface (client, &guid, wlan_intf_opcode_radio_state,
                            nullptr, &size, (PVOID *) &radio, &type)
      != ERROR_SUCCESS || radio == nullptr)
    return fallback;

  bool any_on = false;
  DWORD count = radio->dwNumberOfPhys;
  if (count > WLAN_MAX_PHY_INDEX)
    count = WLAN_MAX_PHY_INDEX;
  for (DWORD i = 0; i < count; ++i) {
    const WLAN_PHY_RADIO_STATE &phy = radio->PhyRadioState[i];
    if (phy.dot11SoftwareRadioState == dot11_radio_state_on &&
        phy.dot11HardwareRadioState != dot11_radio_state_off) {
      any_on = true;
      break;
    }
  }
  g_api.FreeMemory (radio);
  return any_on;
}

}  // namespace

void
winome_wifi_decode_ssid (const guint8 *bytes, gsize len,
                         char *out, gsize out_size)
{
  if (out == nullptr || out_size == 0)
    return;
  out[0] = '\0';

  if (bytes == nullptr || len == 0 || len > 32)
    return;

  for (gsize i = 0; i < len; i++) {
    if (bytes[i] == 0)
      return;   // hidden network: opaque bytes, not a name
  }

  const int slen = static_cast<int> (len);
  wchar_t wide[33] = {};
  int wide_len = MultiByteToWideChar (CP_UTF8, MB_ERR_INVALID_CHARS,
                                      reinterpret_cast<const char *> (bytes),
                                      slen, wide, 32);
  if (wide_len <= 0) {
    // Not valid UTF-8 — widen as Windows-1252 (Latin-1 superset) so accented
    // router names remain readable.
    wide_len = MultiByteToWideChar (1252, 0,
                                    reinterpret_cast<const char *> (bytes),
                                    slen, wide, 32);
    if (wide_len <= 0) {
      for (gsize i = 0; i < len; i++)
        wide[i] = static_cast<wchar_t> (bytes[i]);
      wide_len = slen;
    }
  }
  wide[wide_len] = L'\0';

  char utf8[132] = {};
  WideCharToMultiByte (CP_UTF8, 0, wide, -1, utf8,
                       static_cast<int> (sizeof (utf8)), nullptr, nullptr);
  g_strlcpy (out, utf8, out_size);
}

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
    bool base_enabled = (info->isState != wlan_interface_state_not_ready);
    out->enabled = query_radio_enabled (client, info->InterfaceGuid,
                                        base_enabled);
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
    if (g_api.Scan != nullptr)
      g_api.Scan (client, &info->InterfaceGuid, nullptr, nullptr, nullptr);

    PWLAN_AVAILABLE_NETWORK_LIST avail = nullptr;
    // The third parameter is documented as reserved (must be 0); the
    // WLAN_AVAILABLE_NETWORK_* flags are dwFlags on the struct, not here.
    if (g_api.GetAvailableNetworkList (client, &info->InterfaceGuid, 0,
                                       nullptr, &avail) == ERROR_SUCCESS &&
        avail != nullptr) {
      result = (WinomeWifiNetwork *) calloc (avail->dwNumberOfItems + 1,
                                             sizeof (WinomeWifiNetwork));

      if (result != nullptr) {
        char current_ssid[97] = "";
        int current_quality = 0;
        query_connection (client, info->InterfaceGuid, current_ssid,
                          sizeof (current_ssid), &current_quality);

        // One entry per BSSID, so an SSID served by several access points
        // (roaming candidates, mesh) arrives repeatedly. Merge them, keeping
        // the strongest signal — that is the row GNOME shows.
        for (DWORD i = 0; i < avail->dwNumberOfItems; i++) {
          const WLAN_AVAILABLE_NETWORK &net = avail->Network[i];
          char name[97] = "";
          copy_ssid (net.dot11Ssid, name, sizeof (name));

          if (name[0] == '\0')
            continue;   // hidden network: nothing to show in a list

          bool merged = false;
          for (DWORD k = 0; k < count; k++) {
            if (strcmp (result[k].ssid, name) == 0) {
              if ((int) net.wlanSignalQuality > result[k].signal_quality) {
                result[k].signal_quality = (int) net.wlanSignalQuality;
                result[k].secured = net.bSecurityEnabled != 0;
                result[k].connectable = net.bNetworkConnectable != 0;
              }
              merged = true;
              break;
            }
          }
          if (merged)
            continue;

          WinomeWifiNetwork *slot = &result[count];
          memcpy (slot->ssid, name, sizeof (name));
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

static std::wstring
utf8_to_wide (const char *s)
{
  if (s == nullptr || s[0] == '\0')
    return {};
  int len = MultiByteToWideChar (CP_UTF8, 0, s, -1, nullptr, 0);
  if (len <= 1)
    return {};
  std::wstring out (static_cast<size_t> (len - 1), L'\0');
  MultiByteToWideChar (CP_UTF8, 0, s, -1, out.data (), len);
  return out;
}

static std::string
xml_escape (const char *s)
{
  if (s == nullptr)
    return {};
  std::string out;
  out.reserve (std::strlen (s) + 8);
  for (const unsigned char *p = reinterpret_cast<const unsigned char *> (s);
       *p != '\0'; ++p) {
    switch (*p) {
    case '&':  out += "&amp;"; break;
    case '<':  out += "&lt;"; break;
    case '>':  out += "&gt;"; break;
    case '"':  out += "&quot;"; break;
    case '\'': out += "&apos;"; break;
    default:   out.push_back (static_cast<char> (*p)); break;
    }
  }
  return out;
}

gboolean
winome_wifi_connect (const char *ssid)
{
  if (ssid == nullptr || ssid[0] == '\0')
    return FALSE;

  if (!load_api ())
    return FALSE;

  DWORD negotiated = 0;
  HANDLE client = nullptr;
  if (g_api.OpenHandle (2, nullptr, &negotiated, &client) != ERROR_SUCCESS ||
      client == nullptr)
    return FALSE;

  PWLAN_INTERFACE_INFO_LIST list = nullptr;
  gboolean ok = FALSE;
  if (enum_interfaces (client, &list)) {
    const WLAN_INTERFACE_INFO *info = pick_interface (list);
    std::wstring wide = utf8_to_wide (ssid);
    if (info != nullptr && !wide.empty ()) {
      DOT11_SSID dot11 = {};
      dot11.uSSIDLength =
          static_cast<ULONG> (std::min<size_t> (std::strlen (ssid), 32));
      memcpy (dot11.ucSSID, ssid, dot11.uSSIDLength);

      WLAN_CONNECTION_PARAMETERS params = {};
      params.wlanConnectionMode = wlan_connection_mode_profile;
      params.strProfile = wide.c_str ();
      params.pDot11Ssid = &dot11;
      params.dot11BssType = dot11_BSS_type_infrastructure;
      params.dwFlags = 0;

      ok = g_api.Connect (client, &info->InterfaceGuid, &params, nullptr)
           == ERROR_SUCCESS;
    }
    g_api.FreeMemory (list);
  }
  g_api.CloseHandle (client, nullptr);
  return ok;
}

gboolean
winome_wifi_has_profile (const char *ssid)
{
  if (ssid == nullptr || ssid[0] == '\0')
    return FALSE;
  HANDLE client = nullptr;
  if (!ensure_client (&client) || g_api.GetProfile == nullptr)
    return FALSE;

  PWLAN_INTERFACE_INFO_LIST list = nullptr;
  gboolean found = FALSE;
  if (enum_interfaces (client, &list)) {
    const WLAN_INTERFACE_INFO *info = pick_interface (list);
    std::wstring wide = utf8_to_wide (ssid);
    if (info != nullptr && !wide.empty ()) {
      LPWSTR xml = nullptr;
      DWORD flags = 0;
      DWORD access = 0;
      if (g_api.GetProfile (client, &info->InterfaceGuid, wide.c_str (),
                            nullptr, &xml, &flags, &access) == ERROR_SUCCESS) {
        found = TRUE;
        if (xml != nullptr)
          g_api.FreeMemory (xml);
      }
    }
    g_api.FreeMemory (list);
  }
  return found;
}

gchar *
winome_wifi_build_profile_xml (const char *ssid,
                               const char *password,
                               gboolean open_network)
{
  if (ssid == nullptr || ssid[0] == '\0')
    return nullptr;

  std::string hex;
  static const char kHexDigits[] = "0123456789ABCDEF";
  const size_t raw_len = std::min<size_t> (std::strlen (ssid), 32);
  hex.reserve (raw_len * 2);
  for (size_t i = 0; i < raw_len; ++i) {
    unsigned char b = static_cast<unsigned char> (ssid[i]);
    hex.push_back (kHexDigits[(b >> 4) & 0xF]);
    hex.push_back (kHexDigits[b & 0xF]);
  }

  std::string esc_name = xml_escape (ssid);
  std::string xml;
  xml.reserve (640);
  xml += "<?xml version=\"1.0\"?>\n";
  xml += "<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">\n";
  xml += "  <name>" + esc_name + "</name>\n";
  xml += "  <SSIDConfig>\n";
  xml += "    <SSID>\n";
  xml += "      <hex>" + hex + "</hex>\n";
  xml += "      <name>" + esc_name + "</name>\n";
  xml += "    </SSID>\n";
  xml += "  </SSIDConfig>\n";
  xml += "  <connectionType>ESS</connectionType>\n";
  xml += "  <connectionMode>auto</connectionMode>\n";
  xml += "  <MSM>\n";
  xml += "    <security>\n";
  xml += "      <authEncryption>\n";
  if (open_network) {
    xml += "        <authentication>open</authentication>\n";
    xml += "        <encryption>none</encryption>\n";
    xml += "        <useOneX>false</useOneX>\n";
    xml += "      </authEncryption>\n";
  } else {
    xml += "        <authentication>WPA2PSK</authentication>\n";
    xml += "        <encryption>AES</encryption>\n";
    xml += "        <useOneX>false</useOneX>\n";
    xml += "      </authEncryption>\n";
    xml += "      <sharedKey>\n";
    xml += "        <keyType>passPhrase</keyType>\n";
    xml += "        <protected>false</protected>\n";
    xml += "        <keyMaterial>" + xml_escape (password ? password : "") + "</keyMaterial>\n";
    xml += "      </sharedKey>\n";
  }
  xml += "    </security>\n";
  xml += "  </MSM>\n";
  xml += "</WLANProfile>\n";
  return g_strdup (xml.c_str ());
}

gboolean
winome_wifi_connect_with_password (const char *ssid,
                                   const char *password,
                                   gboolean open_network)
{
  if (ssid == nullptr || ssid[0] == '\0')
    return FALSE;
  if (!load_api () || g_api.SetProfile == nullptr)
    return FALSE;

  gchar *xml_utf8 = winome_wifi_build_profile_xml (ssid, password, open_network);
  if (xml_utf8 == nullptr)
    return FALSE;
  std::wstring xml_wide = utf8_to_wide (xml_utf8);
  g_free (xml_utf8);
  if (xml_wide.empty ())
    return FALSE;

  DWORD negotiated = 0;
  HANDLE client = nullptr;
  if (g_api.OpenHandle (2, nullptr, &negotiated, &client) != ERROR_SUCCESS ||
      client == nullptr)
    return FALSE;

  gboolean ok = FALSE;
  PWLAN_INTERFACE_INFO_LIST list = nullptr;
  if (enum_interfaces (client, &list)) {
    const WLAN_INTERFACE_INFO *info = pick_interface (list);
    std::wstring ssid_wide = utf8_to_wide (ssid);
    if (info != nullptr && !ssid_wide.empty ()) {
      DWORD reason = 0;
      DWORD set_rc = g_api.SetProfile (client, &info->InterfaceGuid, 0,
                                       xml_wide.c_str (), nullptr, TRUE,
                                       nullptr, &reason);
      if (set_rc == ERROR_SUCCESS) {
        DOT11_SSID dot11 = {};
        dot11.uSSIDLength =
            static_cast<ULONG> (std::min<size_t> (std::strlen (ssid), 32));
        memcpy (dot11.ucSSID, ssid, dot11.uSSIDLength);

        WLAN_CONNECTION_PARAMETERS params = {};
        params.wlanConnectionMode = wlan_connection_mode_profile;
        params.strProfile = ssid_wide.c_str ();
        params.pDot11Ssid = &dot11;
        params.dot11BssType = dot11_BSS_type_infrastructure;
        params.dwFlags = 0;
        ok = g_api.Connect (client, &info->InterfaceGuid, &params, nullptr)
             == ERROR_SUCCESS;
      }
    }
    g_api.FreeMemory (list);
  }
  g_api.CloseHandle (client, nullptr);
  return ok;
}

void
winome_wifi_trigger_scan_async (void)
{
  if (!load_api () || g_api.Scan == nullptr)
    return;
  std::thread ([] {
    DWORD negotiated = 0;
    HANDLE client = nullptr;
    if (g_api.OpenHandle (2, nullptr, &negotiated, &client) != ERROR_SUCCESS ||
        client == nullptr)
      return;
    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    if (enum_interfaces (client, &list)) {
      const WLAN_INTERFACE_INFO *info = pick_interface (list);
      if (info != nullptr)
        g_api.Scan (client, &info->InterfaceGuid, nullptr, nullptr, nullptr);
      g_api.FreeMemory (list);
    }
    g_api.CloseHandle (client, nullptr);
  }).detach ();
}

gboolean
winome_wifi_disconnect (void)
{
  HANDLE client = nullptr;
  if (!ensure_client (&client))
    return FALSE;

  PWLAN_INTERFACE_INFO_LIST list = nullptr;
  gboolean ok = FALSE;
  if (enum_interfaces (client, &list)) {
    const WLAN_INTERFACE_INFO *info = pick_interface (list);
    if (info != nullptr)
      ok = g_api.Disconnect (client, &info->InterfaceGuid, nullptr)
           == ERROR_SUCCESS;
    g_api.FreeMemory (list);
  }
  return ok;
}

gboolean
winome_wifi_set_enabled (gboolean enabled)
{
  HANDLE client = nullptr;
  if (!ensure_client (&client) || g_api.SetInterface == nullptr)
    return FALSE;

  PWLAN_INTERFACE_INFO_LIST list = nullptr;
  if (!enum_interfaces (client, &list))
    return FALSE;

  gboolean ok = FALSE;
  const WLAN_INTERFACE_INFO *info = pick_interface (list);
  if (info != nullptr) {
    DWORD num_phys = 1;
    DWORD size = 0;
    WLAN_OPCODE_VALUE_TYPE type = wlan_opcode_value_type_query_only;
    WLAN_RADIO_STATE *radio = nullptr;
    if (g_api.QueryInterface (client, &info->InterfaceGuid,
                              wlan_intf_opcode_radio_state, nullptr, &size,
                              (PVOID *) &radio, &type) == ERROR_SUCCESS &&
        radio != nullptr) {
      if (radio->dwNumberOfPhys > 0 &&
          radio->dwNumberOfPhys <= WLAN_MAX_PHY_INDEX)
        num_phys = radio->dwNumberOfPhys;
      g_api.FreeMemory (radio);
    }

    for (DWORD i = 0; i < num_phys; ++i) {
      WLAN_PHY_RADIO_STATE phy = {};
      phy.dwPhyIndex = i;
      phy.dot11SoftwareRadioState =
          enabled ? dot11_radio_state_on : dot11_radio_state_off;
      phy.dot11HardwareRadioState = dot11_radio_state_unknown;
      if (g_api.SetInterface (client, &info->InterfaceGuid,
                              wlan_intf_opcode_radio_state, sizeof (phy),
                              &phy, nullptr) == ERROR_SUCCESS)
        ok = TRUE;
    }

    if (enabled && g_api.Scan != nullptr)
      g_api.Scan (client, &info->InterfaceGuid, nullptr, nullptr, nullptr);
  }

  g_api.FreeMemory (list);
  return ok;
}

void
winome_wifi_add_listener (WinomeWifiChangedCallback cb, gpointer user_data)
{
  if (cb == nullptr)
    return;
  HANDLE client = nullptr;
  ensure_client (&client);
  std::lock_guard<std::mutex> lock (g_wifi_notif.mu);
  g_wifi_notif.listeners.push_back ({cb, user_data});
}

void
winome_wifi_remove_listener (WinomeWifiChangedCallback cb, gpointer user_data)
{
  std::lock_guard<std::mutex> lock (g_wifi_notif.mu);
  g_wifi_notif.listeners.erase (
      std::remove_if (g_wifi_notif.listeners.begin (),
                      g_wifi_notif.listeners.end (),
                      [&] (const WifiListenerEntry &e) {
                        return e.cb == cb && e.user_data == user_data;
                      }),
      g_wifi_notif.listeners.end ());
}

void
winome_wifi_shutdown_notifications (void)
{
  std::lock_guard<std::mutex> lock (g_wifi_notif.mu);
  g_wifi_notif.listeners.clear ();
  if (g_wifi_notif.registered && g_client != nullptr &&
      g_api.RegisterNotification != nullptr) {
    DWORD prev = 0;
    g_api.RegisterNotification (g_client, WLAN_NOTIFICATION_SOURCE_NONE,
                                TRUE, nullptr, nullptr, nullptr, &prev);
    g_wifi_notif.registered = false;
  }
}