// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Hides and restores the native Windows taskbar (Shell_TrayWnd).

#include "native-taskbar.h"

#include <dwmapi.h>
#include <shellapi.h>

#include <chrono>
#include <cstring>
#include <string>

namespace winome {

namespace {

constexpr wchar_t kPrimaryTaskbarClass[] = L"Shell_TrayWnd";
constexpr wchar_t kSecondaryTaskbarClass[] = L"Shell_SecondaryTrayWnd";
constexpr DWORD kRehideIntervalMs = 500;
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

std::vector<HWND> NativeTaskbar::find_taskbars(HWND* out_primary) {
  if (out_primary != nullptr)
    *out_primary = nullptr;
  std::vector<HWND> found;
  EnumWindows(collect_taskbar, reinterpret_cast<LPARAM>(&found));
  if (out_primary != nullptr) {
    for (HWND hwnd : found) {
      wchar_t cls[kClassBuffer] = {0};
      if (GetClassNameW(hwnd, cls, kClassBuffer) > 0 &&
          wcscmp(cls, kPrimaryTaskbarClass) == 0) {
        *out_primary = hwnd;
        break;
      }
    }
  }
  return found;
}

void NativeTaskbar::set_visibility(HWND hwnd, bool visible, int restore_state) {
  APPBARDATA abd{};
  abd.cbSize = sizeof(APPBARDATA);
  abd.hWnd = hwnd;
  if (visible) {
    // Always restore as visible non-autohide unless caller preserves an
    // explicit state; crash recovery defaults to ABS_ALWAYSONTOP so user
    // immediately sees the taskbar.
    abd.lParam = (restore_state >= 0) ? restore_state : ABS_ALWAYSONTOP;
  } else {
    abd.lParam = ABS_AUTOHIDE;
  }
  SHAppBarMessage(ABM_SETSTATE, &abd);
  ShowWindowAsync(hwnd, visible ? SW_SHOWNORMAL : SW_HIDE);
  if (visible) {
    // Ensure taskbar is never left DWM-cloaked on restore or emergency exit.
    BOOL uncloak = FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &uncloak, sizeof(uncloak));
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS);
  }
}

void NativeTaskbar::suspend_keeper_ms(DWORD ms) {
  if (ms == 0) {
    suspend_until_tick_.store(0, std::memory_order_release);
    return;
  }
  const uint64_t target = GetTickCount64() + static_cast<uint64_t>(ms);
  uint64_t cur = suspend_until_tick_.load(std::memory_order_acquire);
  while (cur < target &&
         !suspend_until_tick_.compare_exchange_weak(cur, target,
                                                    std::memory_order_acq_rel)) {
  }
}

void NativeTaskbar::set_primary_cloaked(bool cloaked) {
  HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr);
  if (primary == nullptr)
    return;
  BOOL val = cloaked ? TRUE : FALSE;
  DwmSetWindowAttribute(primary, DWMWA_CLOAK, &val, sizeof(val));
  primary_cloaked_.store(cloaked, std::memory_order_release);
}

bool NativeTaskbar::is_tray_overflow_open() {
  if (HWND w = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr)) {
    if (IsWindowVisible(w) != FALSE)
      return true;
  }
  if (HWND w = FindWindowW(L"NotifyIconOverflowWindow", nullptr)) {
    if (IsWindowVisible(w) != FALSE)
      return true;
  }
  if (HWND menu = FindWindowW(L"#32768", nullptr)) {
    if (IsWindowVisible(menu) != FALSE)
      return true;
  }
  return false;
}

void NativeTaskbar::keeper_loop() {
  // Park on the stop event, waking periodically to re-assert the hidden
  // state only if Explorer made a taskbar visible again.
  bool first_pass = true;
  for (;;) {
    const uint64_t now = GetTickCount64();
    const bool suspended =
        now < suspend_until_tick_.load(std::memory_order_acquire) ||
        is_tray_overflow_open();
    if (!suspended) {
      for (HWND hwnd : find_taskbars()) {
        if (first_pass || IsWindowVisible(hwnd))
          set_visibility(hwnd, false, ABS_ALWAYSONTOP);
      }
      if (primary_cloaked_.exchange(false, std::memory_order_acq_rel))
        set_primary_cloaked(false);
    }
    first_pass = false;
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

  HWND primary = nullptr;
  find_taskbars(&primary);
  if (primary != nullptr) {
    APPBARDATA abd{};
    abd.cbSize = sizeof(APPBARDATA);
    abd.hWnd = primary;
    const UINT_PTR st = SHAppBarMessage(ABM_GETSTATE, &abd);
    original_state_.store((st & ABS_AUTOHIDE) ? ABS_AUTOHIDE : ABS_ALWAYSONTOP,
                          std::memory_order_release);
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

  const int state = original_state_.load(std::memory_order_acquire);
  for (HWND hwnd : find_taskbars())
    set_visibility(hwnd, true, state);
}

void NativeTaskbar::restore_emergency_no_join() {
  hidden_.store(false, std::memory_order_release);
  if (stop_event_ != nullptr)
    SetEvent(stop_event_);
  for (HWND hwnd : find_taskbars())
    set_visibility(hwnd, true, ABS_ALWAYSONTOP);
}

bool NativeTaskbar::is_any_taskbar_visible() {
  HWND primary = nullptr;
  auto bars = find_taskbars(&primary);
  if (primary != nullptr)
    return IsWindowVisible(primary) != FALSE;
  for (HWND hwnd : bars) {
    if (IsWindowVisible(hwnd) != FALSE)
      return true;
  }
  return bars.empty();
}

int NativeTaskbar::original_state() {
  return original_state_.load(std::memory_order_acquire);
}

bool NativeTaskbar::force_restore_all(int desired_state) {
  hidden_.store(false, std::memory_order_release);

  if (stop_event_ != nullptr) {
    SetEvent(stop_event_);
    if (keeper_thread_.joinable())
      keeper_thread_.join();
    CloseHandle(stop_event_);
    stop_event_ = nullptr;
  }

  const int target_state =
      (desired_state >= 0) ? desired_state : ABS_ALWAYSONTOP;

  for (int attempt = 0; attempt < 8; ++attempt) {
    auto bars = find_taskbars();
    for (HWND hwnd : bars)
      set_visibility(hwnd, true, target_state);

    if (attempt == 0) {
      DWORD_PTR res = 0;
      SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, SPI_SETWORKAREA,
                          reinterpret_cast<LPARAM>(L"Environment"),
                          SMTO_ABORTIFHUNG, 300, &res);
    }

    if (is_any_taskbar_visible())
      return true;
    Sleep(90);
  }
  return is_any_taskbar_visible();
}

}  // namespace winome
