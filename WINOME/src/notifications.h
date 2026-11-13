// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Live Windows & shell notification manager + GNOME-style top toast banner +
// cached SMTC media state.

#pragma once

#include "winrt-bridge.h"

#include <cstdint>
#include <string>
#include <vector>

namespace winome {

struct ShellNotification {
  uint64_t key = 0;
  uint32_t winrt_id = 0;
  bool from_winrt = false;
  int64_t timestamp_unix = 0;
  std::string app_name;
  std::string title;
  std::string body;
};

using NotificationsChangedCallback = void (*)(void* user_data);

// Start/stop background polling of SMTC media and Windows UserNotificationListener.
void notifications_init();
void notifications_shutdown();

// Push an internal shell notification (added to history and displayed as a
// floating banner unless Do Not Disturb is active).
void notify_shell(const std::string& title,
                  const std::string& body,
                  const std::string& app_name = "WINOME");

// Retrieve snapshots without blocking the GTK main thread.
std::vector<ShellNotification> notifications_get_all();
WinomeMediaInfo notifications_get_media();
uint64_t notifications_generation();

// Trigger dismissal / clearing / SMTC playback commands asynchronously.
void notifications_dismiss(uint64_t key);
void notifications_clear_all();
void notifications_media_action(WinomeMediaAction action);

// Subscribe to asynchronous updates when notifications or media state change.
void notifications_add_listener(NotificationsChangedCallback cb, void* user_data);
void notifications_remove_listener(NotificationsChangedCallback cb, void* user_data);

// Pure helper formatting notification age ("Now", "5m", "2h") for UI & tests.
std::string format_notification_age(int64_t timestamp_unix,
                                    int64_t now_unix,
                                    bool zh);

}  // namespace winome
