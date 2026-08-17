// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Embeds a window into the WorkerW desktop layer (below desktop icons).

#include "desktop-layer.h"

#include <chrono>
#include <thread>

namespace winome {

namespace {

// Explorer's private message that asks Program Manager to create the WorkerW
// desktop surface. It is passed (0xD, 0x1) and is stable across Windows 10/11.
constexpr UINT kMsgCreateDesktopWorker = 0x052C;
constexpr UINT kMsgWParam = 0xD;
constexpr UINT kMsgLParam = 0x1;
constexpr int kWorkerWaitAttempts = 10;
constexpr auto kWorkerWaitDelay = std::chrono::milliseconds(100);

constexpr wchar_t kProgmanClass[] = L"Progman";
constexpr wchar_t kWorkerClass[] = L"WorkerW";
constexpr wchar_t kDesktopIconsClass[] = L"SHELLDLL_DefView";

// EnumWindows callback: a WorkerW hosting SHELLDLL_DefView is the "icons"
// layer; the desktop surface is its sibling WorkerW (the one that directly
// covers the wallpaper).
BOOL CALLBACK find_desktop_surface(HWND hwnd, LPARAM lparam) {
  auto* result = reinterpret_cast<HWND*>(lparam);

  if (FindWindowExW(hwnd, nullptr, kDesktopIconsClass, nullptr) != nullptr) {
    HWND sibling = FindWindowExW(nullptr, hwnd, kWorkerClass, nullptr);
    if (sibling != nullptr) {
      *result = sibling;
      return FALSE;  // stop enumerating
    }
  }
  return TRUE;
}

}  // namespace

void DesktopLayer::request_worker_surface() {
  HWND progman = FindWindowW(kProgmanClass, nullptr);
  if (progman == nullptr)
    return;
  SendMessageTimeoutW(progman, kMsgCreateDesktopWorker, kMsgWParam, kMsgLParam,
                      SMTO_ABORTIFHUNG, 1000, nullptr);
}

HWND DesktopLayer::find_worker_surface() {
  // Case 1: standard layout 鈥?a top-level WorkerW's child holds the icons and
  // a sibling WorkerW is the desktop surface.
  HWND surface = nullptr;
  EnumWindows(find_desktop_surface, reinterpret_cast<LPARAM>(&surface));
  if (surface != nullptr)
    return surface;

  // Case 2: Windows 11 layered shell view 鈥?Progman itself contains the
  // icons view and a direct WorkerW child is the desktop surface.
  HWND progman = FindWindowW(kProgmanClass, nullptr);
  if (progman == nullptr)
    return nullptr;

  if (FindWindowExW(progman, nullptr, kDesktopIconsClass, nullptr) == nullptr)
    return nullptr;

  for (int attempt = 0; attempt < kWorkerWaitAttempts; ++attempt) {
    HWND child = FindWindowExW(progman, nullptr, kWorkerClass, nullptr);
    if (child != nullptr)
      return child;
    std::this_thread::sleep_for(kWorkerWaitDelay);
  }
  return nullptr;
}

bool DesktopLayer::set_under_desktop_items(HWND hwnd) {
  HWND surface = find_worker_surface();
  if (surface == nullptr) {
    // The desktop surface does not exist yet (e.g. right after logon); ask
    // Explorer to create it, then retry.
    request_worker_surface();
    surface = find_worker_surface();
  }
  if (surface == nullptr)
    return false;

  // Convert the window into a child of the WorkerW surface. Dropping the
  // app-window edge bits keeps it from being picked up by task switching and
  // window management.
  LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  style |= WS_CHILDWINDOW;
  style &= ~WS_CLIPSIBLINGS;
  SetWindowLongPtrW(hwnd, GWL_STYLE, style);

  LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  ex_style &= ~WS_EX_ACCEPTFILES;
  ex_style &= ~WS_EX_APPWINDOW;
  ex_style &= ~WS_EX_WINDOWEDGE;
  SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex_style);

  SetParent(hwnd, surface);
  return true;
}

}  // namespace winome
