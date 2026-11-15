// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Zero-injection opt-in DWM frame attribute helper for external app windows.

#include "window-theming.h"

#include "config.h"

#include <dwmapi.h>
#include <glib.h>

#include <cwchar>
#include <mutex>
#include <set>

namespace winome {
namespace {

constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmWindowCornerPreference = 33;
constexpr DWORD kDwmCornerDefault = 0;
constexpr DWORD kDwmCornerRound = 2;

struct ThemingState {
  std::mutex mu;
  HWINEVENTHOOK show_hook = nullptr;
  HWINEVENTHOOK fg_hook = nullptr;
  bool dark_enabled = false;
  bool corners_enabled = false;
  bool prev_dark_enabled = false;
  bool prev_corners_enabled = false;
  std::set<HWND> touched_hwnds;
};

ThemingState g_theming;

bool starts_with_insensitive(const wchar_t* s, const wchar_t* prefix) {
  if (s == nullptr || prefix == nullptr)
    return false;
  const size_t n = std::wcslen(prefix);
  return _wcsnicmp(s, prefix, n) == 0;
}

bool equals_insensitive(const wchar_t* a, const wchar_t* b) {
  if (a == nullptr || b == nullptr)
    return false;
  return _wcsicmp(a, b) == 0;
}

void apply_dwm_to_hwnd(HWND hwnd, bool set_dark, bool reset_dark,
                       bool set_corners, bool reset_corners) {
  if (hwnd == nullptr || !IsWindow(hwnd))
    return;

  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  const bool own = (pid == GetCurrentProcessId());
  const bool visible = IsWindowVisible(hwnd) != FALSE;
  const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  wchar_t cls[128] = {};
  GetClassNameW(hwnd, cls, 128);

  if (!should_apply_dwm_theming_to_hwnd_info(cls, style, ex_style, visible, own))
    return;

  bool touched = false;
  if (set_dark || reset_dark) {
    BOOL val = set_dark ? TRUE : FALSE;
    if (SUCCEEDED(DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkMode,
                                        &val, sizeof(val)))) {
      touched = true;
    }
  }
  if (set_corners || reset_corners) {
    DWORD pref = set_corners ? kDwmCornerRound : kDwmCornerDefault;
    if (SUCCEEDED(DwmSetWindowAttribute(hwnd, kDwmWindowCornerPreference,
                                        &pref, sizeof(pref)))) {
      touched = true;
    }
  }

  if (touched) {
    std::lock_guard<std::mutex> lock(g_theming.mu);
    if (set_dark || set_corners) {
      g_theming.touched_hwnds.insert(hwnd);
    }
  }
}

BOOL CALLBACK enum_apply_proc(HWND hwnd, LPARAM lparam) {
  (void)lparam;
  bool dark = false, reset_dark = false;
  bool corners = false, reset_corners = false;
  {
    std::lock_guard<std::mutex> lock(g_theming.mu);
    dark = g_theming.dark_enabled;
    reset_dark = !dark && g_theming.prev_dark_enabled;
    corners = g_theming.corners_enabled;
    reset_corners = !corners && g_theming.prev_corners_enabled;
  }
  if (!dark && !reset_dark && !corners && !reset_corners)
    return TRUE;
  apply_dwm_to_hwnd(hwnd, dark, reset_dark, corners, reset_corners);
  return TRUE;
}

void CALLBACK on_theming_win_event(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                   LONG id_object, LONG id_child,
                                   DWORD thread, DWORD ms) {
  (void)hook;
  (void)event;
  (void)thread;
  (void)ms;
  if (id_object != OBJID_WINDOW || id_child != CHILDID_SELF || hwnd == nullptr)
    return;
  bool dark = false;
  bool corners = false;
  {
    std::lock_guard<std::mutex> lock(g_theming.mu);
    dark = g_theming.dark_enabled;
    corners = g_theming.corners_enabled;
  }
  if (!dark && !corners)
    return;
  apply_dwm_to_hwnd(hwnd, dark, false, corners, false);
}

void ensure_hooks_for_state() {
  bool need_hooks = false;
  HWINEVENTHOOK to_unhook_show = nullptr;
  HWINEVENTHOOK to_unhook_fg = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_theming.mu);
    need_hooks = g_theming.dark_enabled || g_theming.corners_enabled;
    if (!need_hooks) {
      to_unhook_show = g_theming.show_hook;
      to_unhook_fg = g_theming.fg_hook;
      g_theming.show_hook = nullptr;
      g_theming.fg_hook = nullptr;
    } else {
      if (g_theming.show_hook == nullptr) {
        g_theming.show_hook = SetWinEventHook(
            EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr,
            on_theming_win_event, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
      }
      if (g_theming.fg_hook == nullptr) {
        g_theming.fg_hook = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
            on_theming_win_event, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
      }
    }
  }
  if (to_unhook_show != nullptr)
    UnhookWinEvent(to_unhook_show);
  if (to_unhook_fg != nullptr)
    UnhookWinEvent(to_unhook_fg);
}

void on_theming_config_changed(void* user_data) {
  (void)user_data;
  window_theming_sync_all();
}

}  // namespace

bool should_apply_dwm_theming_to_hwnd_info(const wchar_t* class_name,
                                           LONG_PTR style,
                                           LONG_PTR ex_style,
                                           bool visible,
                                           bool is_own_process) {
  if (!visible || is_own_process || class_name == nullptr ||
      class_name[0] == L'\0') {
    return false;
  }
  if ((style & WS_CHILD) != 0)
    return false;
  if ((ex_style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0)
    return false;
  if ((style & WS_CAPTION) != WS_CAPTION && (style & WS_THICKFRAME) == 0)
    return false;

  // Never alter shell desktop containers, menus, tooltips, or WINOME chrome.
  if (equals_insensitive(class_name, L"#32768") ||
      equals_insensitive(class_name, L"#32769") ||
      equals_insensitive(class_name, L"Progman") ||
      equals_insensitive(class_name, L"WorkerW") ||
      equals_insensitive(class_name, L"Shell_TrayWnd") ||
      equals_insensitive(class_name, L"Shell_SecondaryTrayWnd") ||
      equals_insensitive(class_name, L"tooltips_class32") ||
      equals_insensitive(class_name, L"NotifyIconOverflowWindow") ||
      equals_insensitive(class_name, L"TopLevelWindowForOverflowXamlIsland") ||
      equals_insensitive(class_name, L"Windows.UI.Core.CoreWindow") ||
      equals_insensitive(class_name, L"EdgeUiInputTopWndClass") ||
      starts_with_insensitive(class_name, L"WINOME_") ||
      starts_with_insensitive(class_name, L"gdkSurface")) {
    return false;
  }
  return true;
}

void window_theming_sync_all() {
  const WinomeConfig& cfg = get_config();
  bool reset_dark = false;
  bool reset_corners = false;
  {
    std::lock_guard<std::mutex> lock(g_theming.mu);
    g_theming.prev_dark_enabled = g_theming.dark_enabled;
    g_theming.prev_corners_enabled = g_theming.corners_enabled;
    g_theming.dark_enabled = cfg.unify_dark_titlebars;
    g_theming.corners_enabled = cfg.unify_rounded_corners;
    reset_dark = !g_theming.dark_enabled && g_theming.prev_dark_enabled;
    reset_corners = !g_theming.corners_enabled && g_theming.prev_corners_enabled;
  }

  ensure_hooks_for_state();

  if (cfg.unify_dark_titlebars || cfg.unify_rounded_corners) {
    EnumWindows(enum_apply_proc, 0);
  } else if (reset_dark || reset_corners) {
    std::set<HWND> snapshot;
    {
      std::lock_guard<std::mutex> lock(g_theming.mu);
      snapshot.swap(g_theming.touched_hwnds);
    }
    for (HWND h : snapshot)
      apply_dwm_to_hwnd(h, false, reset_dark, false, reset_corners);
  }
}

void window_theming_init() {
  add_config_listener(on_theming_config_changed, nullptr);
  window_theming_sync_all();
}

void window_theming_shutdown() {
  remove_config_listener(on_theming_config_changed, nullptr);
  HWINEVENTHOOK to_unhook_show = nullptr;
  HWINEVENTHOOK to_unhook_fg = nullptr;
  bool reset_dark = false;
  bool reset_corners = false;
  std::set<HWND> snapshot;
  {
    std::lock_guard<std::mutex> lock(g_theming.mu);
    to_unhook_show = g_theming.show_hook;
    to_unhook_fg = g_theming.fg_hook;
    g_theming.show_hook = nullptr;
    g_theming.fg_hook = nullptr;
    reset_dark = g_theming.dark_enabled;
    reset_corners = g_theming.corners_enabled;
    g_theming.dark_enabled = false;
    g_theming.corners_enabled = false;
    snapshot.swap(g_theming.touched_hwnds);
  }
  if (to_unhook_show != nullptr)
    UnhookWinEvent(to_unhook_show);
  if (to_unhook_fg != nullptr)
    UnhookWinEvent(to_unhook_fg);
  if (reset_dark || reset_corners) {
    for (HWND h : snapshot)
      apply_dwm_to_hwnd(h, false, reset_dark, false, reset_corners);
  }
}

}  // namespace winome
