// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Host process privileges and DPI awareness.

#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace winome {

// Enable a named privilege (e.g. L"SeTcbPrivilege") on the current process
// token. Returns true when the privilege was enabled.
bool enable_privilege(const wchar_t* name);

// Returns true when the current process runs with an elevated administrator
// token (TokenElevation).
bool is_process_elevated();

// Pure helpers controlling startup self-elevation and safe relaunch arguments.
inline bool should_attempt_auto_elevate(bool config_enabled,
                                        bool already_elevated,
                                        bool no_elevate_flag) {
  return config_enabled && !already_elevated && !no_elevate_flag;
}

inline std::wstring quote_win32_arg(const std::wstring& arg) {
  if (!arg.empty() &&
      arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    return arg;
  }
  std::wstring out;
  out.push_back(L'"');
  size_t backslashes = 0;
  for (wchar_t ch : arg) {
    if (ch == L'\\') {
      ++backslashes;
    } else if (ch == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
      backslashes = 0;
    } else {
      if (backslashes > 0) {
        out.append(backslashes, L'\\');
        backslashes = 0;
      }
      out.push_back(ch);
    }
  }
  if (backslashes > 0)
    out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

inline std::wstring build_elevated_relaunch_args(
    const std::vector<std::wstring>& original_args) {
  std::wstring out;
  for (const auto& arg : original_args) {
    if (arg == L"--no-elevate")
      continue;
    if (!out.empty())
      out.push_back(L' ');
    out += quote_win32_arg(arg);
  }
  if (!out.empty())
    out.push_back(L' ');
  out += L"--no-elevate";
  return out;
}

// Set per-monitor DPI awareness. Prefers the Windows 10 1703+ API and falls
// back to the shcore variant on older systems. Returns true on success.
bool set_process_dpi_aware();

// Returns the writable per-user data directory (%LOCALAPPDATA%\WINOME),
// creating it if necessary.
std::wstring app_data_dir();

// Returns an ASCII-safe (8.3 short path when available, UTF-8 otherwise)
// path to app_data_dir() for libraries like fontconfig/FreeType that fail
// on non-ASCII paths on Windows.
std::string app_data_dir_ascii();

// Pure & HWND helpers for the Overview default blacklist (filters system ghost
// surfaces like "Windows Input Experience" / TextInputHost, nameless blank
// explorer.exe windows, and suspended/cloaked UWP "Settings" / "设置" shells
// when enabled in settings).
// @cloaked_flags is the optional DWMWA_CLOAKED bitmask (DWM_CLOAKED_APP = 0x1,
// DWM_CLOAKED_SHELL = 0x2, DWM_CLOAKED_INHERITED = 0x4); when 0 and @cloaked is
// true, it is treated as DWM_CLOAKED_SHELL.
// @has_core_window indicates whether an ApplicationFrameWindow hosts a live
// Windows.UI.Core.CoreWindow child (ignored for non-UWP classes).
bool is_overview_default_blacklisted_window_info(
    const std::string& class_name_utf8,
    const std::string& proc_name_utf8,
    const std::string& title_utf8,
    bool cloaked,
    DWORD cloaked_flags = 0,
    bool has_core_window = true);
bool is_overview_default_blacklisted_hwnd(HWND hwnd, bool enabled);

// Strip minimize/maximize/thick-frame styles from internal shell overlays so
// Windows cannot collapse WS_EX_TOOLWINDOW surfaces into 160x28 classic bars.
inline LONG_PTR strip_overlay_minimize_styles(LONG_PTR style) {
  return style & ~static_cast<LONG_PTR>(WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
                                        WS_THICKFRAME);
}

// Harden a shell overlay HWND (panel, popover, toast banner, OSD, overview
// chrome) against accidental minimization on Win+D / display changes.
void harden_shell_overlay_hwnd(HWND hwnd);

// Detach, cloak, and make click-through GTK4's internal GdkWin32GL backing
// swapchain HWNDs (created when GDK_WIN32_FORCE_DCOMP=1) so owner DWM uncloak
// or SetWindowPos/BringWindowToTop can never expose them fullscreen above UI.
void sanitize_gdk_gl_backing_windows();

}  // namespace winome
