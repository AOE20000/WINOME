// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Hides and restores the native Windows taskbar (Shell_TrayWnd).

#include "native-taskbar.h"

#include <shellapi.h>

#include <chrono>
#include <cstring>
#include <string>

namespace winome {

namespace {

constexpr wchar_t kPrimaryTaskbarClass[] = L"Shell_TrayWnd";
constexpr wchar_t kSecondaryTaskbarClass[] = L"Shell_SecondaryTrayWnd";
constexpr DWORD kRehideIntervalMs = 50;
constexpr int kClassBuffer = 256;

// EnumWindows callback: collects every top-level window whose class is one of
// the taskbar classes and whose title is empty (the tray window has no title
// text; a titled window with the same class is a different shell surface).
BOOL CALLBACK collect_taskbar(HWND hwnd, LPARAM lparam) {
  auto* out = reinterpret_cast<std::vector<HWND>*>(lparam);
  wchar_t cls[kClassBuffer] = {0};
  if (GetClassNameW(hwnd, cls, kClassBuffer) == 0)
    return TRUE;

  const bool is_primary = wcscmp(cls, kPrimaryTaskbarClass) == 0;
  const bool is_secondary = wcscmp(cls, kSecondaryTaskbarClass) == 0;
  if (!is_primary && !is_secondary)
    return TRUE;

  std::wstring title(kClassBuffer, L'\0');
  int len = GetWindowTextW(hwnd, title.data(), kClassBuffer);
  if (len > 0)
    return TRUE;  // not a bare taskbar surface

  out->push_back(hwnd);
  return TRUE;
}

}  // namespace

std::thread NativeTaskbar::keeper_thread_;
HANDLE NativeTaskbar::stop_event_ = nullptr;

std::vector<HWND> NativeTaskbar::find_taskbars() {
  std::vector<HWND> found;
  EnumWindows(collect_taskbar, reinterpret_cast<LPARAM>(&found));
  return found;
}

void NativeTaskbar::set_visibility(HWND hwnd, bool visible) {
  APPBARDATA abd{};
  abd.cbSize = sizeof(APPBARDATA);
  abd.hWnd = hwnd;
  abd.lParam = visible ? ABS_ALWAYSONTOP : ABS_AUTOHIDE;
  SHAppBarMessage(ABM_SETSTATE, &abd);
  ShowWindowAsync(hwnd, visible ? SW_SHOWNORMAL : SW_HIDE);
}

void NativeTaskbar::keeper_loop() {
  // Park on the stop event, waking periodically to re-assert the hidden
  // state. Explorer recreates the taskbar window on various shell events, so
  // we keep re-applying until restore() signals us to stop.
  for (;;) {
    for (HWND hwnd : find_taskbars())
      set_visibility(hwnd, false);
    if (WaitForSingleObject(stop_event_, kRehideIntervalMs) == WAIT_OBJECT_0)
      break;
  }
}

void NativeTaskbar::hide() {
  bool expected = false;
  if (!hidden_.compare_exchange_strong(expected, true,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
    // Already suppressing; nothing further to start.
    return;
  }

  if (keeper_thread_.joinable()) {
    // A previous keeper is still running from an earlier session; leave it
    // to keep re-hiding.
    return;
  }

  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (stop_event_ == nullptr) {
    hidden_.store(false, std::memory_order_release);
    return;
  }

  keeper_thread_ = std::thread(keeper_loop);
}

void NativeTaskbar::restore() {
  bool expected = true;
  if (!hidden_.compare_exchange_strong(expected, false,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
    // We never hid the taskbar; leave it alone.
    return;
  }

  if (stop_event_ != nullptr) {
    SetEvent(stop_event_);
    if (keeper_thread_.joinable())
      keeper_thread_.join();
    CloseHandle(stop_event_);
    stop_event_ = nullptr;
  }

  for (HWND hwnd : find_taskbars())
    set_visibility(hwnd, true);
}

}  // namespace winome
