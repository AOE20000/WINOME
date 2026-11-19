// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Pure C++17 raw COM/WinRT bridge via combase.dll (Radios, SMTC media, Toasts).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace winome {

struct WinomeMediaInfo {
  bool active = false;
  bool playing = false;
  std::string title;
  std::string artist;
  std::string album;
  std::string app_id;
};

enum class WinomeMediaAction {
  PlayPause,
  Next,
  Previous,
};

struct WinomeSystemNotification {
  uint32_t id = 0;
  int64_t timestamp_unix = 0;
  std::string app_name;
  std::string title;
  std::string body;
};

// Query/set the system Bluetooth radio via Windows.Devices.Radios.Radio.
bool winrt_get_bluetooth_radio(bool* present, bool* enabled);
bool winrt_set_bluetooth_radio(bool enabled);

// Query/control active global media playback via SMTC.
bool winrt_get_media_info(WinomeMediaInfo* out);
bool winrt_media_control(WinomeMediaAction action);

// Query/dismiss Windows toast notifications via UserNotificationListener.
// If Windows denies notification access (unpackaged desktop app policy),
// returns false cleanly and sets *access_allowed = false without blocking.
bool winrt_get_notifications(std::vector<WinomeSystemNotification>* out,
                             bool* access_allowed = nullptr);
bool winrt_remove_notification(uint32_t id);
bool winrt_clear_notifications();

// Invokes a zero-argument COM property getter vtable slot
// (`HRESULT STDMETHODCALLTYPE fn(void*, void**)`), probing via a second call
// while `owner` stays alive to detect and compensate for getters (such as
// Windows 11 wpnapps.dll!NotificationVisual::get_Bindings) that return an
// internal COM pointer without calling AddRef().
long winrt_acquire_com_getter_safe(void* owner,
                                   size_t vtable_slot,
                                   void** out,
                                   bool* out_compensated_addref = nullptr);

// Tear down the background WinRT MTA worker thread on application exit.
void winrt_shutdown();

}  // namespace winome
