// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// WinEventHook that keeps the native Windows UI hidden when Explorer
// recreates it.

#pragma once

namespace winome {

// Start the WinEventHook on a background thread with its own message loop.
// Keeps the native taskbar / notification center / XAML flyouts hidden when
// Explorer recreates them.
void start_win_event_hook();

}  // namespace winome
