// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Windows data sources for the GNOME-style system status indicators.

#include "system-status.h"
#include "perf-metrics.h"
#include "wifi-networks.h"
#include "host-utils.h"
#include "winrt-bridge.h"

#include <windows.h>
#include <winioctl.h>
#include <wininet.h>
#include <bluetoothapis.h>
#include <shellapi.h>
#include <powrprof.h>

#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <glib.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdio.h>
#include <thread>
#include <vector>

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

namespace {
IAudioEndpointVolume* acquire_bound_endpoint_volume();
}  // namespace

bool get_volume(double* out) {
  IAudioEndpointVolume* volume = acquire_bound_endpoint_volume();
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
  IAudioEndpointVolume* volume = acquire_bound_endpoint_volume();
  if (volume == nullptr)
    return false;

  HRESULT hr = volume->SetMasterVolumeLevelScalar(
      static_cast<float>(fraction), nullptr);
  volume->Release();
  return hr == S_OK;
}

bool get_mute(bool* out) {
  IAudioEndpointVolume* volume = acquire_bound_endpoint_volume();
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
  IAudioEndpointVolume* volume = acquire_bound_endpoint_volume();
  if (volume == nullptr)
    return false;

  BOOL muted = FALSE;
  volume->GetMute(&muted);
  HRESULT hr = volume->SetMute(!muted, nullptr);
  volume->Release();
  return hr == S_OK;
}

bool get_network_connected(bool* out) {
  // Prefer the WLAN association when a wireless interface exists: it answers
  // "is this machine on Wi-Fi", which is what the indicator means. Fall back
  // to any-connection for desktops with an Ethernet-only link.
  WinomeWifiStatus wifi;
  if (winome_wifi_get_status(&wifi) && wifi.enabled) {
    if (out != nullptr)
      *out = wifi.connected;
    return true;
  }

  DWORD flags = 0;
  BOOL connected = InternetGetConnectedState(&flags, 0);
  if (out != nullptr)
    *out = (connected != FALSE);
  return connected != FALSE;
}

bool get_network_name(char* out, int out_size) {
  if (out != nullptr && out_size > 0)
    out[0] = '\0';

  WinomeWifiStatus wifi;
  if (winome_wifi_get_status(&wifi) && wifi.connected) {
    if (wifi.ssid[0] == '\0')
      return false;   // associated with a hidden network: no name to show
    if (out != nullptr && out_size > 0)
      snprintf(out, out_size, "%s", wifi.ssid);
    return true;
  }

  // Not on Wi-Fi. Fall back to the generic label when some other link is up,
  // so the menu still says something useful on Ethernet.
  bool connected = false;
  if (!get_network_connected(&connected) || !connected)
    return false;
  if (out != nullptr && out_size > 0)
    snprintf(out, out_size, "%s", "Connected");
  return true;
}

bool get_wifi_enabled(bool* out) {
  WinomeWifiStatus wifi;
  if (!winome_wifi_get_status(&wifi)) {
    // No wireless interface at all (desktop, or wlanapi.dll absent). Report
    // "enabled" so the toggle keeps its GNOME default-on appearance rather
    // than claiming the radio is off on a machine that has none.
    if (out != nullptr)
      *out = true;
    return true;
  }
  if (out != nullptr)
    *out = wifi.enabled;
  return true;
}

static double s_cached_brightness = 0.5;

namespace {

// Windows LCD device IOCTLs (FILE_DEVICE_VIDEO, functions 0x126..0x128).
constexpr DWORD kIoctlQuerySupportedBrightness = 0x230498;
constexpr DWORD kIoctlQueryDisplayBrightness   = 0x23049C;
constexpr DWORD kIoctlSetDisplayBrightness     = 0x2304A0;

struct DisplayBrightness {
  BYTE ucDisplayPolicy;
  BYTE ucACBrightness;
  BYTE ucDCBrightness;
};

struct WinomePhysicalMonitor {
  HANDLE hPhysicalMonitor;
  WCHAR  szPhysicalMonitorDescription[128];
};

using FnGetNumPhysMons = BOOL (WINAPI *)(HMONITOR, LPDWORD);
using FnGetPhysMons    = BOOL (WINAPI *)(HMONITOR, DWORD, WinomePhysicalMonitor *);
using FnDestroyPhysMon = BOOL (WINAPI *)(DWORD, WinomePhysicalMonitor *);
using FnGetMonBright   = BOOL (WINAPI *)(HANDLE, LPDWORD, LPDWORD, LPDWORD);
using FnSetMonBright   = BOOL (WINAPI *)(HANDLE, DWORD);

struct Dxva2Api {
  bool             loaded = false;
  FnGetNumPhysMons get_count = nullptr;
  FnGetPhysMons    get_mons = nullptr;
  FnDestroyPhysMon destroy_mons = nullptr;
  FnGetMonBright   get_bright = nullptr;
  FnSetMonBright   set_bright = nullptr;
};

template <typename Fn>
Fn load_fn(HMODULE mod, const char *name) {
  return reinterpret_cast<Fn>(reinterpret_cast<void *>(GetProcAddress(mod, name)));
}

const Dxva2Api &dxva2_api() {
  static Dxva2Api api = []() {
    Dxva2Api a;
    HMODULE mod = LoadLibraryW(L"dxva2.dll");
    if (mod == nullptr)
      return a;
    a.get_count = load_fn<FnGetNumPhysMons>(mod, "GetNumberOfPhysicalMonitorsFromHMONITOR");
    a.get_mons = load_fn<FnGetPhysMons>(mod, "GetPhysicalMonitorsFromHMONITOR");
    a.destroy_mons = load_fn<FnDestroyPhysMon>(mod, "DestroyPhysicalMonitors");
    a.get_bright = load_fn<FnGetMonBright>(mod, "GetMonitorBrightness");
    a.set_bright = load_fn<FnSetMonBright>(mod, "SetMonitorBrightness");
    a.loaded = a.get_count && a.get_mons && a.destroy_mons &&
               a.get_bright && a.set_bright;
    return a;
  }();
  return api;
}

bool get_lcd_brightness(double *out) {
  HANDLE lcd = CreateFileW(L"\\\\.\\LCD", GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
  if (lcd == INVALID_HANDLE_VALUE)
    return false;

  DisplayBrightness db {};
  DWORD returned = 0;
  BOOL ok = DeviceIoControl(lcd, kIoctlQueryDisplayBrightness, nullptr, 0,
                            &db, sizeof(db), &returned, nullptr);
  CloseHandle(lcd);
  if (!ok)
    return false;

  BYTE level = db.ucACBrightness;
  if (db.ucDisplayPolicy == 0x02) {
    level = db.ucDCBrightness;
  } else if (db.ucDisplayPolicy != 0x01) {
    SYSTEM_POWER_STATUS sps {};
    if (GetSystemPowerStatus(&sps) && sps.ACLineStatus == 0)
      level = db.ucDCBrightness;
  }

  *out = std::clamp(static_cast<double>(level) / 100.0, 0.0, 1.0);
  return true;
}

bool set_lcd_brightness(double fraction) {
  HANDLE lcd = CreateFileW(L"\\\\.\\LCD", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
  if (lcd == INVALID_HANDLE_VALUE)
    return false;

  int raw_pct = static_cast<int>(std::clamp(fraction, 0.0, 1.0) * 100.0 + 0.5);
  BYTE target = static_cast<BYTE>(std::clamp(raw_pct, 0, 100));

  BYTE supported[256] {};
  DWORD returned = 0;
  if (DeviceIoControl(lcd, kIoctlQuerySupportedBrightness, nullptr, 0,
                      supported, sizeof(supported), &returned, nullptr) &&
      returned > 0) {
    BYTE best = supported[0];
    int best_dist = std::abs(static_cast<int>(best) - static_cast<int>(target));
    for (DWORD i = 1; i < returned; ++i) {
      int dist = std::abs(static_cast<int>(supported[i]) - static_cast<int>(target));
      if (dist < best_dist) {
        best = supported[i];
        best_dist = dist;
      }
    }
    target = best;
  }

  DisplayBrightness db {};
  db.ucDisplayPolicy = 0x03; // apply to both AC and DC
  db.ucACBrightness = target;
  db.ucDCBrightness = target;
  BOOL ok = DeviceIoControl(lcd, kIoctlSetDisplayBrightness, &db, sizeof(db),
                            nullptr, 0, &returned, nullptr);
  CloseHandle(lcd);
  return ok != FALSE;
}

struct Dxva2Cache {
  std::mutex mu;
  HMONITOR mon = nullptr;
  bool probed = false;
  bool supported = false;
  DWORD min_b = 0;
  DWORD max_b = 0;
  int64_t last_fail_us = 0;
};

Dxva2Cache g_dxva2_cache;

bool get_dxva2_brightness(double *out) {
  const Dxva2Api &api = dxva2_api();
  if (!api.loaded)
    return false;

  HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  if (mon == nullptr)
    return false;

  // Avoid repeatedly stalling the UI thread on DDC/CI I2C timeouts for desktop
  // displays that do not support DDC/CI (re-probe after 60s or display change).
  {
    std::lock_guard<std::mutex> lock(g_dxva2_cache.mu);
    const int64_t now_us = g_get_monotonic_time();
    if (g_dxva2_cache.probed && g_dxva2_cache.mon == mon &&
        !g_dxva2_cache.supported &&
        (now_us - g_dxva2_cache.last_fail_us) < 60LL * 1000 * 1000) {
      return false;
    }
  }

  DWORD count = 0;
  if (!api.get_count(mon, &count) || count == 0) {
    std::lock_guard<std::mutex> lock(g_dxva2_cache.mu);
    g_dxva2_cache.mon = mon;
    g_dxva2_cache.probed = true;
    g_dxva2_cache.supported = false;
    g_dxva2_cache.last_fail_us = g_get_monotonic_time();
    return false;
  }

  std::vector<WinomePhysicalMonitor> mons(count);
  if (!api.get_mons(mon, count, mons.data())) {
    std::lock_guard<std::mutex> lock(g_dxva2_cache.mu);
    g_dxva2_cache.mon = mon;
    g_dxva2_cache.probed = true;
    g_dxva2_cache.supported = false;
    g_dxva2_cache.last_fail_us = g_get_monotonic_time();
    return false;
  }

  bool result = false;
  DWORD got_min = 0, got_max = 0;
  for (DWORD i = 0; i < count; ++i) {
    DWORD min_b = 0, cur_b = 0, max_b = 0;
    if (api.get_bright(mons[i].hPhysicalMonitor, &min_b, &cur_b, &max_b) &&
        max_b > min_b) {
      *out = std::clamp(
          static_cast<double>(cur_b - min_b) / static_cast<double>(max_b - min_b),
          0.0, 1.0);
      got_min = min_b;
      got_max = max_b;
      result = true;
      break;
    }
  }

  api.destroy_mons(count, mons.data());
  {
    std::lock_guard<std::mutex> lock(g_dxva2_cache.mu);
    g_dxva2_cache.mon = mon;
    g_dxva2_cache.probed = true;
    g_dxva2_cache.supported = result;
    if (result) {
      g_dxva2_cache.min_b = got_min;
      g_dxva2_cache.max_b = got_max;
    } else {
      g_dxva2_cache.last_fail_us = g_get_monotonic_time();
    }
  }
  return result;
}

bool set_dxva2_brightness(double fraction) {
  const Dxva2Api &api = dxva2_api();
  if (!api.loaded)
    return false;

  HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  if (mon == nullptr)
    return false;

  bool have_bounds = false;
  DWORD cached_min = 0, cached_max = 0;
  {
    std::lock_guard<std::mutex> lock(g_dxva2_cache.mu);
    if (g_dxva2_cache.probed && g_dxva2_cache.mon == mon) {
      if (!g_dxva2_cache.supported &&
          (g_get_monotonic_time() - g_dxva2_cache.last_fail_us) <
              60LL * 1000 * 1000) {
        return false;
      }
      if (g_dxva2_cache.supported && g_dxva2_cache.max_b > g_dxva2_cache.min_b) {
        have_bounds = true;
        cached_min = g_dxva2_cache.min_b;
        cached_max = g_dxva2_cache.max_b;
      }
    }
  }

  DWORD count = 0;
  if (!api.get_count(mon, &count) || count == 0)
    return false;

  std::vector<WinomePhysicalMonitor> mons(count);
  if (!api.get_mons(mon, count, mons.data()))
    return false;

  double clamped = std::clamp(fraction, 0.0, 1.0);
  bool result = false;
  for (DWORD i = 0; i < count; ++i) {
    DWORD min_b = cached_min, cur_b = 0, max_b = cached_max;
    bool bounds_ok = have_bounds;
    if (!bounds_ok) {
      bounds_ok = api.get_bright(mons[i].hPhysicalMonitor, &min_b, &cur_b,
                                 &max_b) &&
                  max_b > min_b;
    }
    if (bounds_ok) {
      DWORD val = min_b + static_cast<DWORD>(clamped * (max_b - min_b) + 0.5);
      if (api.set_bright(mons[i].hPhysicalMonitor, val))
        result = true;
    }
  }

  api.destroy_mons(count, mons.data());
  return result;
}

constexpr wchar_t kPersonalizeKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr wchar_t kWinomeKey[] = L"Software\\WINOME";
constexpr wchar_t kNightLightKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\CloudStore\\Store\\"
    L"DefaultAccount\\Current\\default$windows.data.bluelightreduction."
    L"bluelightreductionstate\\windows.data.bluelightreduction."
    L"bluelightreductionstate";
constexpr wchar_t kRadioMgmtKey[] =
    L"SYSTEM\\CurrentControlSet\\Control\\RadioManagement";
constexpr wchar_t kRadioStateSubkey[] =
    L"SYSTEM\\CurrentControlSet\\Control\\RadioManagement\\SystemRadioState";

using FnBtFindFirstRadio  = HBLUETOOTH_RADIO_FIND (WINAPI *)(const BLUETOOTH_FIND_RADIO_PARAMS *, HANDLE *);
using FnBtFindNextRadio   = WINBOOL (WINAPI *)(HBLUETOOTH_RADIO_FIND, HANDLE *);
using FnBtFindRadioClose  = WINBOOL (WINAPI *)(HBLUETOOTH_RADIO_FIND);
using FnBtFindFirstDevice = HBLUETOOTH_DEVICE_FIND (WINAPI *)(const BLUETOOTH_DEVICE_SEARCH_PARAMS *, BLUETOOTH_DEVICE_INFO *);
using FnBtFindNextDevice  = WINBOOL (WINAPI *)(HBLUETOOTH_DEVICE_FIND, BLUETOOTH_DEVICE_INFO *);
using FnBtFindDeviceClose = WINBOOL (WINAPI *)(HBLUETOOTH_DEVICE_FIND);
using FnBtEnableDiscovery = WINBOOL (WINAPI *)(HANDLE, WINBOOL);
using FnBtEnableIncoming  = WINBOOL (WINAPI *)(HANDLE, WINBOOL);

struct BthApi {
  bool                loaded = false;
  FnBtFindFirstRadio  find_first_radio = nullptr;
  FnBtFindNextRadio   find_next_radio = nullptr;
  FnBtFindRadioClose  find_radio_close = nullptr;
  FnBtFindFirstDevice find_first_dev = nullptr;
  FnBtFindNextDevice  find_next_dev = nullptr;
  FnBtFindDeviceClose find_dev_close = nullptr;
  FnBtEnableDiscovery enable_discovery = nullptr;
  FnBtEnableIncoming  enable_incoming = nullptr;
};

const BthApi &bth_api() {
  static BthApi api = []() {
    BthApi a;
    HMODULE mod = LoadLibraryW(L"BluetoothApis.dll");
    if (mod == nullptr)
      mod = LoadLibraryW(L"bthprops.cpl");
    if (mod == nullptr)
      return a;

    a.find_first_radio = load_fn<FnBtFindFirstRadio>(mod, "BluetoothFindFirstRadio");
    a.find_next_radio  = load_fn<FnBtFindNextRadio>(mod, "BluetoothFindNextRadio");
    a.find_radio_close = load_fn<FnBtFindRadioClose>(mod, "BluetoothFindRadioClose");
    a.find_first_dev   = load_fn<FnBtFindFirstDevice>(mod, "BluetoothFindFirstDevice");
    a.find_next_dev    = load_fn<FnBtFindNextDevice>(mod, "BluetoothFindNextDevice");
    a.find_dev_close   = load_fn<FnBtFindDeviceClose>(mod, "BluetoothFindDeviceClose");
    a.enable_discovery = load_fn<FnBtEnableDiscovery>(mod, "BluetoothEnableDiscovery");
    a.enable_incoming  = load_fn<FnBtEnableIncoming>(mod, "BluetoothEnableIncomingConnections");
    a.loaded = a.find_first_radio && a.find_radio_close &&
               a.find_first_dev && a.find_next_dev && a.find_dev_close &&
               a.enable_discovery && a.enable_incoming;
    return a;
  }();
  return api;
}

bool read_dword_value(HKEY root, const wchar_t *subkey, const wchar_t *value_name, DWORD *out) {
  DWORD val = 0;
  DWORD size = sizeof(val);
  if (RegGetValueW(root, subkey, value_name, RRF_RT_REG_DWORD, nullptr,
                   &val, &size) != ERROR_SUCCESS)
    return false;
  *out = val;
  return true;
}

bool write_dword_value(HKEY root, const wchar_t *subkey, const wchar_t *value_name, DWORD val) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                      &key, nullptr) != ERROR_SUCCESS)
    return false;
  LSTATUS st = RegSetValueExW(key, value_name, 0, REG_DWORD,
                              reinterpret_cast<const BYTE *>(&val), sizeof(val));
  RegCloseKey(key);
  return st == ERROR_SUCCESS;
}

bool reg_key_exists(HKEY root, const wchar_t *subkey) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) == ERROR_SUCCESS) {
    RegCloseKey(key);
    return true;
  }
  return false;
}

void wide_to_utf8(const wchar_t *src, char *dst, int dst_size) {
  if (dst == nullptr || dst_size <= 0)
    return;
  dst[0] = '\0';
  if (src == nullptr || src[0] == L'\0')
    return;
  WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dst_size, nullptr, nullptr);
}

bool s_brightness_ever_available = false;

void update_brightness_capability(bool ok, const char* fail_detail) {
  if (ok) {
    s_brightness_ever_available = true;
    capability_set(SubsystemId::Brightness, CapabilityState::Active);
    return;
  }
  if (!s_brightness_ever_available) {
    capability_set(SubsystemId::Brightness, CapabilityState::Active,
                   "No controllable display brightness hardware (desktop/no DDC/CI)");
  } else {
    capability_set(SubsystemId::Brightness, CapabilityState::Degraded,
                   fail_detail != nullptr ? fail_detail : "Display brightness failure");
  }
}

} // namespace

bool has_brightness_control() {
  double value = 0.0;
  const bool ok = get_lcd_brightness(&value) || get_dxva2_brightness(&value);
  update_brightness_capability(ok, "Display brightness unavailable");
  return ok;
}

bool get_brightness(double* out) {
  double value = s_cached_brightness;
  if (get_lcd_brightness(&value) || get_dxva2_brightness(&value)) {
    s_cached_brightness = value;
    update_brightness_capability(true, nullptr);
    if (out != nullptr)
      *out = value;
    return true;
  }
  update_brightness_capability(false, "Display brightness read failed");
  return false;
}

struct BrightnessWorkerState {
  std::mutex mu;
  std::condition_variable cv;
  std::thread worker;
  bool running = false;
  bool stop = false;
  bool has_pending = false;
  double pending_value = 0.5;
};

BrightnessWorkerState g_bright_worker;

void brightness_worker_loop() {
  for (;;) {
    double target = 0.5;
    {
      std::unique_lock<std::mutex> lock(g_bright_worker.mu);
      g_bright_worker.cv.wait(lock, [] {
        return g_bright_worker.stop || g_bright_worker.has_pending;
      });
      if (g_bright_worker.stop)
        break;
      target = g_bright_worker.pending_value;
      g_bright_worker.has_pending = false;
    }
    set_brightness(target);
  }
}

void stop_brightness_worker() {
  std::thread to_join;
  {
    std::lock_guard<std::mutex> lock(g_bright_worker.mu);
    if (!g_bright_worker.running)
      return;
    g_bright_worker.stop = true;
    g_bright_worker.has_pending = false;
    g_bright_worker.running = false;
    to_join = std::move(g_bright_worker.worker);
  }
  g_bright_worker.cv.notify_all();
  if (to_join.joinable())
    to_join.join();
}

void set_brightness_async(double fraction) {
  const double clamped = std::clamp(fraction, 0.0, 1.0);
  s_cached_brightness = clamped;
  {
    std::lock_guard<std::mutex> lock(g_bright_worker.mu);
    g_bright_worker.pending_value = clamped;
    g_bright_worker.has_pending = true;
    if (!g_bright_worker.running) {
      g_bright_worker.stop = false;
      g_bright_worker.running = true;
      g_bright_worker.worker = std::thread(brightness_worker_loop);
    }
  }
  g_bright_worker.cv.notify_one();
}

void invalidate_brightness_cache() {
  std::lock_guard<std::mutex> lock(g_dxva2_cache.mu);
  g_dxva2_cache.probed = false;
  g_dxva2_cache.supported = false;
  g_dxva2_cache.mon = nullptr;
  g_dxva2_cache.last_fail_us = 0;
}

bool set_brightness(double fraction) {
  double clamped = std::clamp(fraction, 0.0, 1.0);
  s_cached_brightness = clamped;
  bool ok = set_lcd_brightness(clamped) || set_dxva2_brightness(clamped);
  update_brightness_capability(ok, "Display brightness write failed");
  return ok;
}

bool get_theme_modes(bool* apps_dark, bool* system_dark) {
  DWORD apps_val = 1;
  DWORD apps_size = sizeof(apps_val);
  const bool have_apps =
      RegGetValueW(HKEY_CURRENT_USER, kPersonalizeKey, L"AppsUseLightTheme",
                   RRF_RT_REG_DWORD, nullptr, &apps_val,
                   &apps_size) == ERROR_SUCCESS;

  DWORD sys_val = apps_val;
  DWORD sys_size = sizeof(sys_val);
  const bool have_sys =
      RegGetValueW(HKEY_CURRENT_USER, kPersonalizeKey, L"SystemUsesLightTheme",
                   RRF_RT_REG_DWORD, nullptr, &sys_val,
                   &sys_size) == ERROR_SUCCESS;

  if (!have_apps && !have_sys)
    return false;
  if (apps_dark != nullptr)
    *apps_dark = have_apps ? (apps_val == 0) : (sys_val == 0);
  if (system_dark != nullptr)
    *system_dark = have_sys ? (sys_val == 0) : (apps_val == 0);
  return true;
}

bool set_single_personalize_theme_flag(const wchar_t* value_name, bool dark) {
  const DWORD desired = dark ? 0u : 1u;
  DWORD current = desired;
  DWORD size = sizeof(current);
  if (RegGetValueW(HKEY_CURRENT_USER, kPersonalizeKey, value_name,
                   RRF_RT_REG_DWORD, nullptr, &current,
                   &size) == ERROR_SUCCESS &&
      current == desired) {
    return true;
  }

  HKEY key = nullptr;
  LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, kPersonalizeKey, 0,
                                   nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                                   nullptr);
  if (status != ERROR_SUCCESS)
    return false;

  const bool ok = RegSetValueExW(
      key, value_name, 0, REG_DWORD,
      reinterpret_cast<const BYTE*>(&desired), sizeof(desired)) == ERROR_SUCCESS;
  RegCloseKey(key);

  if (ok) {
    SendNotifyMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                       reinterpret_cast<LPARAM>(L"ImmersiveColorSet"));
  }
  return ok;
}

bool set_app_dark_mode(bool dark) {
  return set_single_personalize_theme_flag(L"AppsUseLightTheme", dark);
}

bool set_system_dark_mode(bool dark) {
  return set_single_personalize_theme_flag(L"SystemUsesLightTheme", dark);
}

bool get_dark_mode(bool* out) {
  if (out == nullptr)
    return false;
  bool apps_dark = false;
  bool sys_dark = false;
  if (!get_theme_modes(&apps_dark, &sys_dark))
    return false;
  *out = apps_dark;
  return true;
}

bool set_dark_mode(bool dark) {
  bool cur_apps = !dark;
  bool cur_sys = !dark;
  if (get_theme_modes(&cur_apps, &cur_sys) &&
      cur_apps == dark && cur_sys == dark) {
    return true;
  }

  HKEY key = nullptr;
  LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, kPersonalizeKey, 0,
                                   nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                                   nullptr);
  if (status != ERROR_SUCCESS)
    return false;

  DWORD val = dark ? 0 : 1;
  bool ok_apps = RegSetValueExW(
      key, L"AppsUseLightTheme", 0, REG_DWORD,
      reinterpret_cast<const BYTE *>(&val), sizeof(val)) == ERROR_SUCCESS;
  bool ok_sys = RegSetValueExW(
      key, L"SystemUsesLightTheme", 0, REG_DWORD,
      reinterpret_cast<const BYTE *>(&val), sizeof(val)) == ERROR_SUCCESS;
  RegCloseKey(key);

  if (ok_apps || ok_sys) {
    SendNotifyMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                       reinterpret_cast<LPARAM>(L"ImmersiveColorSet"));
    return true;
  }
  return false;
}

void lock_screen() {
  LockWorkStation();
}

void log_out() {
  ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_MAJOR_OTHER);
}

void suspend_system() {
  // SetSuspendState requires SeShutdownPrivilege.
  enable_privilege(L"SeShutdownPrivilege");
  SetSuspendState(FALSE, FALSE, FALSE);
}

void shutdown_system() {
  enable_privilege(L"SeShutdownPrivilege");
  ExitWindowsEx(EWX_POWEROFF | EWX_FORCE,
                SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER);
}

void restart_system() {
  enable_privilege(L"SeShutdownPrivilege");
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

bool get_bluetooth_status(bool* present, bool* enabled,
                          char* connected_name, int name_size) {
  if (present != nullptr)
    *present = false;
  if (enabled != nullptr)
    *enabled = false;
  if (connected_name != nullptr && name_size > 0)
    connected_name[0] = '\0';

  const BthApi &api = bth_api();
  if (!api.loaded)
    return false;

  BLUETOOTH_FIND_RADIO_PARAMS rfp{};
  rfp.dwSize = sizeof(rfp);
  HANDLE radio = nullptr;
  HBLUETOOTH_RADIO_FIND rfind = api.find_first_radio(&rfp, &radio);
  if (rfind != nullptr) {
    if (present != nullptr)
      *present = true;
    if (enabled != nullptr)
      *enabled = true;

    if (connected_name != nullptr && name_size > 0 && radio != nullptr) {
      BLUETOOTH_DEVICE_SEARCH_PARAMS sp{};
      sp.dwSize = sizeof(sp);
      sp.fReturnAuthenticated = TRUE;
      sp.fReturnRemembered = TRUE;
      sp.fReturnConnected = TRUE;
      sp.fReturnUnknown = FALSE;
      sp.fIssueInquiry = FALSE;
      sp.cTimeoutMultiplier = 0;
      sp.hRadio = radio;

      BLUETOOTH_DEVICE_INFO dev{};
      dev.dwSize = sizeof(dev);
      HBLUETOOTH_DEVICE_FIND dfind = api.find_first_dev(&sp, &dev);
      if (dfind != nullptr) {
        do {
          if (dev.fConnected != 0 && dev.szName[0] != L'\0') {
            wide_to_utf8(dev.szName, connected_name, name_size);
            break;
          }
          dev.dwSize = sizeof(dev);
        } while (api.find_next_dev(dfind, &dev));
        api.find_dev_close(dfind);
      }
    }

    if (radio != nullptr)
      CloseHandle(radio);
    api.find_radio_close(rfind);
    return true;
  }

  // No powered radio handle: never block the UI thread for up to 3s on
  // WinRT Radio::GetRadiosAsync. Serve the cached WinRT probe result and
  // refresh it asynchronously when stale.
  struct WinRtBtCache {
    std::mutex mu;
    bool has_result = false;
    bool present = false;
    bool enabled = false;
    int64_t updated_us = 0;
    std::atomic<bool> in_flight{false};
  };
  static auto* s_bt_cache_ptr = new std::shared_ptr<WinRtBtCache>(
      std::make_shared<WinRtBtCache>());
  auto cache = *s_bt_cache_ptr;

  bool cached_valid = false;
  bool cached_present = false;
  bool cached_enabled = false;
  bool need_refresh = false;
  const int64_t now_us = g_get_monotonic_time();
  {
    std::lock_guard<std::mutex> lock(cache->mu);
    cached_valid = cache->has_result;
    cached_present = cache->present;
    cached_enabled = cache->enabled;
    if (!cache->has_result ||
        (now_us - cache->updated_us) >= 10LL * 1000 * 1000) {
      need_refresh = true;
    }
  }
  if (need_refresh) {
    bool expected = false;
    if (cache->in_flight.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
      std::thread([cache] {
        bool wp = false, we = false;
        const bool ok = winrt_get_bluetooth_radio(&wp, &we);
        {
          std::lock_guard<std::mutex> lock(cache->mu);
          if (ok) {
            cache->has_result = true;
            cache->present = wp;
            cache->enabled = we;
          }
          cache->updated_us = g_get_monotonic_time();
        }
        cache->in_flight.store(false, std::memory_order_release);
      }).detach();
    }
  }
  if (cached_valid && cached_present) {
    if (present != nullptr)
      *present = true;
    if (enabled != nullptr)
      *enabled = cached_enabled;
    return true;
  }

  // Fallback: check if Bluetooth hardware/devices exist in registry.
  if (reg_key_exists(HKEY_LOCAL_MACHINE,
                     L"SYSTEM\\CurrentControlSet\\Services\\BTHPORT\\Parameters\\Devices") ||
      reg_key_exists(HKEY_LOCAL_MACHINE,
                     L"SYSTEM\\CurrentControlSet\\Services\\BTHPORT\\Parameters\\Radio Support")) {
    if (present != nullptr)
      *present = true;
    if (enabled != nullptr)
      *enabled = false;
    return true;
  }

  return false;
}

bool set_bluetooth_enabled(bool enabled) {
  if (winrt_set_bluetooth_radio(enabled))
    return true;

  const BthApi &api = bth_api();
  if (!api.loaded)
    return false;

  BLUETOOTH_FIND_RADIO_PARAMS rfp{};
  rfp.dwSize = sizeof(rfp);
  HANDLE radio = nullptr;
  HBLUETOOTH_RADIO_FIND rfind = api.find_first_radio(&rfp, &radio);
  if (rfind != nullptr) {
    if (radio != nullptr) {
      api.enable_incoming(radio, enabled ? TRUE : FALSE);
      api.enable_discovery(radio, enabled ? TRUE : FALSE);
      CloseHandle(radio);
    }
    api.find_radio_close(rfind);
    if (!enabled) {
      ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr,
                    SW_SHOWNORMAL);
    }
    return true;
  }

  if (enabled) {
    ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr,
                  SW_SHOWNORMAL);
    return true;
  }
  return false;
}

void sort_bluetooth_devices(BluetoothDevice* devs, int count) {
  if (devs == nullptr || count <= 1)
    return;
  std::sort(devs, devs + count,
            [](const BluetoothDevice& a, const BluetoothDevice& b) {
              if (a.connected != b.connected)
                return a.connected;
              if (a.paired != b.paired)
                return a.paired;
              return _stricmp(a.name, b.name) < 0;
            });
}

int get_bluetooth_devices_ex(BluetoothDevice* out, int max_count,
                             bool issue_inquiry) {
  if (out == nullptr || max_count <= 0)
    return 0;

  const BthApi &api = bth_api();
  if (!api.loaded)
    return 0;

  BLUETOOTH_FIND_RADIO_PARAMS rfp{};
  rfp.dwSize = sizeof(rfp);
  HANDLE radio = nullptr;
  HBLUETOOTH_RADIO_FIND rfind = api.find_first_radio(&rfp, &radio);
  if (rfind == nullptr)
    return 0;

  int count = 0;
  std::vector<ULONGLONG> seen_addrs;
  if (radio != nullptr) {
    BLUETOOTH_DEVICE_SEARCH_PARAMS sp{};
    sp.dwSize = sizeof(sp);
    sp.fReturnAuthenticated = TRUE;
    sp.fReturnRemembered = TRUE;
    sp.fReturnConnected = TRUE;
    sp.fReturnUnknown = issue_inquiry ? TRUE : FALSE;
    sp.fIssueInquiry = issue_inquiry ? TRUE : FALSE;
    sp.cTimeoutMultiplier = issue_inquiry ? 2 : 0;
    sp.hRadio = radio;

    BLUETOOTH_DEVICE_INFO dev{};
    dev.dwSize = sizeof(dev);
    HBLUETOOTH_DEVICE_FIND dfind = api.find_first_dev(&sp, &dev);
    if (dfind != nullptr) {
      do {
        if (dev.szName[0] != L'\0') {
          const ULONGLONG addr = dev.Address.ullLong;
          const bool dup =
              addr != 0 &&
              std::find(seen_addrs.begin(), seen_addrs.end(), addr) !=
                  seen_addrs.end();
          if (!dup) {
            if (addr != 0)
              seen_addrs.push_back(addr);
            BluetoothDevice &dst = out[count];
            wide_to_utf8(dev.szName, dst.name, static_cast<int>(sizeof(dst.name)));
            dst.connected = (dev.fConnected != 0);
            dst.paired = (dev.fRemembered != 0 || dev.fAuthenticated != 0);
            ++count;
          }
        }
        dev.dwSize = sizeof(dev);
      } while (count < max_count && api.find_next_dev(dfind, &dev));
      api.find_dev_close(dfind);
    }
    CloseHandle(radio);
  }
  api.find_radio_close(rfind);
  sort_bluetooth_devices(out, count);
  return count;
}

int get_bluetooth_devices(BluetoothDevice* out, int max_count) {
  return get_bluetooth_devices_ex(out, max_count, false);
}

bool get_night_light(bool* out) {
  if (out == nullptr)
    return false;

  BYTE buf[128]{};
  DWORD size = sizeof(buf);
  if (RegGetValueW(HKEY_CURRENT_USER, kNightLightKey, L"Data",
                   RRF_RT_REG_BINARY, nullptr, buf, &size) == ERROR_SUCCESS &&
      size >= 19 && (buf[18] == 0x15 || buf[18] == 0x13)) {
    *out = (buf[18] == 0x15);
    return true;
  }

  DWORD val = 0;
  if (read_dword_value(HKEY_CURRENT_USER, kWinomeKey, L"NightLight", &val)) {
    *out = (val != 0);
    return true;
  }

  *out = false;
  return true;
}

bool set_night_light(bool enabled) {
  bool saved = write_dword_value(HKEY_CURRENT_USER, kWinomeKey, L"NightLight",
                                 enabled ? 1 : 0);

  BYTE buf[128]{};
  DWORD size = sizeof(buf);
  if (RegGetValueW(HKEY_CURRENT_USER, kNightLightKey, L"Data",
                   RRF_RT_REG_BINARY, nullptr, buf, &size) == ERROR_SUCCESS &&
      size >= 25 && size <= 100 && (buf[18] == 0x15 || buf[18] == 0x13)) {
    bool currently_on = (buf[18] == 0x15);
    if (currently_on != enabled) {
      for (int i = 10; i <= 14; ++i) {
        if (++buf[i] != 0)
          break;
      }

      if (enabled) {
        buf[18] = 0x15;
        std::memmove(&buf[25], &buf[23], size - 23);
        buf[23] = 0x10;
        buf[24] = 0x00;
        size += 2;
      } else if (size >= 27) {
        buf[18] = 0x13;
        std::memmove(&buf[23], &buf[25], size - 25);
        size -= 2;
      }

      HKEY key = nullptr;
      if (RegCreateKeyExW(HKEY_CURRENT_USER, kNightLightKey, 0, nullptr, 0,
                          KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(key, L"Data", 0, REG_BINARY, buf, size);
        RegCloseKey(key);
        saved = true;
      }
    }
  }

  return saved;
}

bool get_do_not_disturb(bool* out) {
  if (out == nullptr)
    return false;

  DWORD val = 0;
  if (read_dword_value(HKEY_CURRENT_USER, kWinomeKey, L"DoNotDisturb", &val)) {
    *out = (val != 0);
    return true;
  }

  QUERY_USER_NOTIFICATION_STATE quns = QUNS_ACCEPTS_NOTIFICATIONS;
  if (SUCCEEDED(SHQueryUserNotificationState(&quns))) {
    *out = (quns == QUNS_BUSY || quns == QUNS_PRESENTATION_MODE ||
            quns == QUNS_QUIET_TIME);
    return true;
  }

  *out = false;
  return true;
}

bool set_do_not_disturb(bool enabled) {
  return write_dword_value(HKEY_CURRENT_USER, kWinomeKey, L"DoNotDisturb",
                           enabled ? 1 : 0);
}

bool get_airplane_mode(bool* out) {
  if (out == nullptr)
    return false;

  DWORD val = 0;
  if (read_dword_value(HKEY_LOCAL_MACHINE, kRadioStateSubkey, nullptr, &val) ||
      read_dword_value(HKEY_LOCAL_MACHINE, kRadioMgmtKey, L"SystemRadioState", &val)) {
    *out = (val != 0);
    return true;
  }

  if (read_dword_value(HKEY_CURRENT_USER, kWinomeKey, L"AirplaneMode", &val)) {
    *out = (val != 0);
    return true;
  }

  *out = false;
  return true;
}

bool set_airplane_mode(bool enabled) {
  DWORD val = enabled ? 1 : 0;
  bool ok = write_dword_value(HKEY_CURRENT_USER, kWinomeKey, L"AirplaneMode", val);
  winome_wifi_set_enabled(enabled ? FALSE : TRUE);
  if (write_dword_value(HKEY_LOCAL_MACHINE, kRadioStateSubkey, nullptr, val) ||
      write_dword_value(HKEY_LOCAL_MACHINE, kRadioMgmtKey, L"SystemRadioState", val)) {
    ok = true;
  }
  return ok;
}

bool has_keyboard_backlight() {
  return false;
}

namespace {

struct VolumeListenerEntry {
  VolumeChangedCallback cb;
  void* user_data;
};

struct VolumeMonitorState {
  std::mutex mu;
  std::vector<VolumeListenerEntry> listeners;
  std::atomic<bool> notify_queued{false};
  std::atomic<bool> rebind_queued{false};
  bool started = false;
  IMMDeviceEnumerator* enumerator = nullptr;
  IAudioEndpointVolume* bound_volume = nullptr;
  IAudioEndpointVolumeCallback* volume_cb = nullptr;
  IMMNotificationClient* device_cb = nullptr;
};

VolumeMonitorState g_vol_mon;

IAudioEndpointVolume* acquire_bound_endpoint_volume() {
  {
    std::lock_guard<std::mutex> lock(g_vol_mon.mu);
    if (g_vol_mon.started && g_vol_mon.bound_volume != nullptr) {
      g_vol_mon.bound_volume->AddRef();
      return g_vol_mon.bound_volume;
    }
  }
  return get_endpoint_volume();
}

gboolean idle_dispatch_volume_changed(gpointer G_GNUC_UNUSED data) {
  g_vol_mon.notify_queued.store(false, std::memory_order_release);
  std::vector<VolumeListenerEntry> copy;
  {
    std::lock_guard<std::mutex> lock(g_vol_mon.mu);
    copy = g_vol_mon.listeners;
  }
  for (const auto& entry : copy) {
    if (entry.cb != nullptr)
      entry.cb(entry.user_data);
  }
  return G_SOURCE_REMOVE;
}

void queue_volume_changed_notification() {
  bool expected = false;
  if (g_vol_mon.notify_queued.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) {
    g_idle_add(idle_dispatch_volume_changed, nullptr);
  }
}

void rebind_default_endpoint_locked();

gboolean idle_rebind_default_endpoint(gpointer G_GNUC_UNUSED data) {
  g_vol_mon.rebind_queued.store(false, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(g_vol_mon.mu);
    if (g_vol_mon.started)
      rebind_default_endpoint_locked();
  }
  queue_volume_changed_notification();
  return G_SOURCE_REMOVE;
}

void queue_rebind_default_endpoint() {
  bool expected = false;
  if (g_vol_mon.rebind_queued.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) {
    g_idle_add(idle_rebind_default_endpoint, nullptr);
  }
}

class EndpointVolumeCallbackImpl final : public IAudioEndpointVolumeCallback {
 public:
  ULONG STDMETHODCALLTYPE AddRef() override {
    return ++ref_count_;
  }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG n = --ref_count_;
    if (n == 0)
      delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr)
      return E_POINTER;
    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(IAudioEndpointVolumeCallback)) {
      *ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE OnNotify(
      AUDIO_VOLUME_NOTIFICATION_DATA* pNotify G_GNUC_UNUSED) override {
    queue_volume_changed_notification();
    return S_OK;
  }

 private:
  std::atomic<ULONG> ref_count_{1};
};

class DeviceNotificationClientImpl final : public IMMNotificationClient {
 public:
  ULONG STDMETHODCALLTYPE AddRef() override {
    return ++ref_count_;
  }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG n = --ref_count_;
    if (n == 0)
      delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr)
      return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
      *ppv = static_cast<IMMNotificationClient*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(
      LPCWSTR pwstrDeviceId G_GNUC_UNUSED,
      DWORD dwNewState G_GNUC_UNUSED) override {
    queue_rebind_default_endpoint();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(
      LPCWSTR pwstrDeviceId G_GNUC_UNUSED) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(
      LPCWSTR pwstrDeviceId G_GNUC_UNUSED) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(
      EDataFlow flow,
      ERole role,
      LPCWSTR pwstrDeviceId G_GNUC_UNUSED) override {
    if (flow == eRender && role == eConsole)
      queue_rebind_default_endpoint();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(
      LPCWSTR pwstrDeviceId G_GNUC_UNUSED,
      const PROPERTYKEY key G_GNUC_UNUSED) override {
    return S_OK;
  }

 private:
  std::atomic<ULONG> ref_count_{1};
};

void unbind_volume_locked() {
  if (g_vol_mon.bound_volume != nullptr) {
    if (g_vol_mon.volume_cb != nullptr)
      g_vol_mon.bound_volume->UnregisterControlChangeNotify(g_vol_mon.volume_cb);
    g_vol_mon.bound_volume->Release();
    g_vol_mon.bound_volume = nullptr;
  }
}

void rebind_default_endpoint_locked() {
  unbind_volume_locked();
  if (g_vol_mon.enumerator == nullptr || g_vol_mon.volume_cb == nullptr)
    return;

  IMMDevice* dev = nullptr;
  if (g_vol_mon.enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &dev) != S_OK ||
      dev == nullptr) {
    return;
  }
  IAudioEndpointVolume* vol = nullptr;
  if (dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                    reinterpret_cast<void**>(&vol)) == S_OK &&
      vol != nullptr) {
    if (vol->RegisterControlChangeNotify(g_vol_mon.volume_cb) == S_OK) {
      g_vol_mon.bound_volume = vol;
    } else {
      vol->Release();
    }
  }
  dev->Release();
}

}  // namespace

void start_volume_monitor() {
  std::lock_guard<std::mutex> lock(g_vol_mon.mu);
  if (g_vol_mon.started)
    return;
  IMMDeviceEnumerator* enumerator = get_device_enumerator();
  if (enumerator == nullptr)
    return;

  g_vol_mon.started = true;
  g_vol_mon.enumerator = enumerator;
  g_vol_mon.enumerator->AddRef();
  g_vol_mon.volume_cb = new EndpointVolumeCallbackImpl();
  g_vol_mon.device_cb = new DeviceNotificationClientImpl();
  g_vol_mon.enumerator->RegisterEndpointNotificationCallback(g_vol_mon.device_cb);
  rebind_default_endpoint_locked();
}

void stop_volume_monitor() {
  stop_brightness_worker();
  std::lock_guard<std::mutex> lock(g_vol_mon.mu);
  g_vol_mon.listeners.clear();
  if (!g_vol_mon.started)
    return;
  g_vol_mon.started = false;
  unbind_volume_locked();
  if (g_vol_mon.enumerator != nullptr) {
    if (g_vol_mon.device_cb != nullptr) {
      g_vol_mon.enumerator->UnregisterEndpointNotificationCallback(
          g_vol_mon.device_cb);
    }
    g_vol_mon.enumerator->Release();
    g_vol_mon.enumerator = nullptr;
  }
  if (g_vol_mon.volume_cb != nullptr) {
    g_vol_mon.volume_cb->Release();
    g_vol_mon.volume_cb = nullptr;
  }
  if (g_vol_mon.device_cb != nullptr) {
    g_vol_mon.device_cb->Release();
    g_vol_mon.device_cb = nullptr;
  }
}

void add_volume_listener(VolumeChangedCallback cb, void* user_data) {
  if (cb == nullptr)
    return;
  std::lock_guard<std::mutex> lock(g_vol_mon.mu);
  g_vol_mon.listeners.push_back({cb, user_data});
}

void remove_volume_listener(VolumeChangedCallback cb, void* user_data) {
  std::lock_guard<std::mutex> lock(g_vol_mon.mu);
  g_vol_mon.listeners.erase(
      std::remove_if(g_vol_mon.listeners.begin(), g_vol_mon.listeners.end(),
                     [&](const VolumeListenerEntry& e) {
                       return e.cb == cb && e.user_data == user_data;
                     }),
      g_vol_mon.listeners.end());
}

}  // namespace winome
