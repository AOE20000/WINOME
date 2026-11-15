// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// GNOME-style modal End Session confirmation dialog (port of
// gnome-shell's js/ui/endSessionDialog.js) with a 60s countdown timer.

#pragma once

#include <string>

namespace winome {

enum class EndSessionAction {
  Restart,
  PowerOff,
  LogOut,
};

using EndSessionConfirmCallback = void (*)(EndSessionAction action,
                                           void* user_data);

// Pure localization/countdown formatting helpers covered by unit tests.
std::string end_session_format_title(EndSessionAction action, bool zh);
std::string end_session_format_description(EndSessionAction action,
                                           int seconds_left,
                                           bool zh);
std::string end_session_confirm_label(EndSessionAction action, bool zh);

// Presents the topmost centered GNOME modal countdown dialog. If the dialog is
// already open, closes and replaces it cleanly.
void show_end_session_dialog(EndSessionAction action,
                             EndSessionConfirmCallback on_confirm,
                             void* user_data);

// Cancels timer and destroys any active End Session dialog immediately.
void close_end_session_dialog();

}  // namespace winome
