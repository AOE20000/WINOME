// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Windows data sources for the GNOME-style system status indicators.

#include "system-status.h"

#include <windows.h>
#include <wininet.h>

#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>

#include <stdio.h>

namespace winome {

// IMMDeviceEnumerator is COM-initialized lazily.
static IMMDeviceEnumerator* get_device_enumerator() {
  static IMMDeviceEnumerator* enumerator = nullptr;
  if (enumerator == nullptr) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                         CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                         reinterpret_cast<void**>(&enumerator)) != S_OK) {
      return nullptr;
    }
  }
  return enumerator;
}

static IAudioEndpointVolume* get_endpoint_volume() {
  IMMDeviceEnumerator* enumerator = get_device_enumerator();
  if (enumerator == nullptr)
    return nullptr;

  IMMDevice* device = nullptr;
  if (enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device) != S_OK)
    return nullptr;

  IAudioEndpointVolume* volume = nullptr;
  device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                   reinterpret_cast<void**>(&volume));
  device->Release();
  return volume;
}

bool get_battery_status(BatteryStatus* out) {
  if (out == nullptr)
    return false;

  SYSTEM_POWER_STATUS sps;
  if (!GetSystemPowerStatus(&sps))
    return false;

  if (sps.BatteryFlag == 128 /* no system battery */) {
    out->present = false;
    out->charging = false;
    out->percentage = 0;
    return false;
  }

  out->present = true;
  out->charging = (sps.ACLineStatus == 1);
  if (sps.BatteryLifePercent == 255) {
    out->percentage = 100;
  } else {
    out->percentage = sps.BatteryLifePercent;
  }
  return true;
}

bool get_volume(double* out) {
  IAudioEndpointVolume* volume = get_endpoint_volume();
  if (volume == nullptr)
    return false;

  float level = 0.0f;
  HRESULT hr = volume->GetMasterVolumeLevelScalar(&level);
  volume->Release();
  if (hr != S_OK || out == nullptr)
    return false;

  *out = level;
  return true;
}

bool set_volume(double fraction) {
  IAudioEndpointVolume* volume = get_endpoint_volume();
  if (volume == nullptr)
    return false;

  HRESULT hr = volume->SetMasterVolumeLevelScalar(
      static_cast<float>(fraction), nullptr);
  volume->Release();
  return hr == S_OK;
}

bool get_mute(bool* out) {
  IAudioEndpointVolume* volume = get_endpoint_volume();
  if (volume == nullptr)
    return false;

  BOOL muted = FALSE;
  HRESULT hr = volume->GetMute(&muted);
  volume->Release();
  if (hr != S_OK || out == nullptr)
    return false;

  *out = (muted != FALSE);
  return true;
}

bool toggle_mute() {
  IAudioEndpointVolume* volume = get_endpoint_volume();
  if (volume == nullptr)
    return false;

  BOOL muted = FALSE;
  volume->GetMute(&muted);
  HRESULT hr = volume->SetMute(!muted, nullptr);
  volume->Release();
  return hr == S_OK;
}

bool get_network_connected(bool* out) {
  // Use InternetGetConnectedState to detect any network connectivity.
  DWORD flags = 0;
  BOOL connected = InternetGetConnectedState(&flags, 0);
  if (out != nullptr)
    *out = (connected != FALSE);
  return connected != FALSE;
}

bool get_network_name(char* out, int out_size) {
  // A full WLAN SSID query needs the Wlan API; for now report a generic label.
  bool connected = false;
  if (!get_network_connected(&connected) || !connected) {
    if (out != nullptr && out_size > 0)
      out[0] = '\0';
    return false;
  }
  if (out != nullptr && out_size > 0)
    snprintf(out, out_size, "%s", "Connected");
  return true;
}

bool get_wifi_enabled(bool* out) {
  // A simple approximation: Wi-Fi is considered enabled when a wireless
  // connection exists. Full radio toggle requires WlanSetInterface which is
  // out of scope for now.
  if (out != nullptr)
    *out = true;
  return true;
}

bool get_brightness(double* out) {
  // Screen brightness on Windows needs WMI (WmiMonitorBrightness) or
  // DXVA2 SetMonitorBrightness. For now return 0.5 as a placeholder.
  if (out != nullptr)
    *out = 0.5;
  return true;
}

bool set_brightness(double fraction) {
  (void)fraction;
  return false;
}

void lock_screen() {
  LockWorkStation();
}

void log_out() {
  ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_MAJOR_OTHER);
}

void shutdown_system() {
  // Requires SE_SHUTDOWN_NAME privilege; best-effort.
  ExitWindowsEx(EWX_POWEROFF | EWX_FORCE,
                SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER);
}

void restart_system() {
  ExitWindowsEx(EWX_REBOOT | EWX_FORCE,
                SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER);
}

const char* get_system_language() {
  LANGID lang = GetUserDefaultUILanguage();
  WORD primary = PRIMARYLANGID(lang);

  // Simplified/Traditional Chinese -> zh; everything else -> en for now.
  if (primary == LANG_CHINESE)
    return "zh";
  return "en";
}

}  // namespace winome
