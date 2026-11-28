// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Host process privileges and DPI awareness.

#pragma once

#include <windows.h>

#include <cctype>
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

// Merge "full-redraw" into GSK_DEBUG when running GskCairoRenderer on
// Win32 DirectComposition (GDK_WIN32_FORCE_DCOMP=1) to work around GTK 4.22.4
// gdkcairocontext-win32.c calling Present1 with DirtyRectsCount > 0 after
// ResizeBuffers (which causes permanent DXGI_ERROR_INVALID_CALL freezes).
inline std::string merge_gsk_debug_full_redraw(const char* current_gsk_debug,
                                               const char* renderer,
                                               const char* force_dcomp) {
  auto eq_ascii_ci = [](const std::string& a, const char* b) {
    size_t n = 0;
    while (b[n] != '\0')
      ++n;
    if (a.size() != n)
      return false;
    for (size_t i = 0; i < n; ++i) {
      if (std::tolower(static_cast<unsigned char>(a[i])) !=
          std::tolower(static_cast<unsigned char>(b[i])))
        return false;
    }
    return true;
  };
  const std::string cur = current_gsk_debug ? current_gsk_debug : "";
  if (renderer == nullptr || !eq_ascii_ci(renderer, "cairo"))
    return cur;
  if (force_dcomp == nullptr || force_dcomp[0] == '\0')
    return cur;
  const std::string fd = force_dcomp;
  if (fd == "0" || eq_ascii_ci(fd, "false"))
    return cur;

  bool has_any_token = false;
  std::string tok;
  auto flush_tok = [&](bool* found_full) {
    if (tok.empty())
      return;
    has_any_token = true;
    if (eq_ascii_ci(tok, "full-redraw") || eq_ascii_ci(tok, "all"))
      *found_full = true;
    tok.clear();
  };
  bool found_full = false;
  for (unsigned char ch : cur) {
    if (ch == ',' || ch == ':' || std::isspace(ch))
      flush_tok(&found_full);
    else
      tok.push_back(static_cast<char>(ch));
  }
  flush_tok(&found_full);
  if (found_full)
    return cur;
  if (!has_any_token)
    return "full-redraw";
  return cur + ",full-redraw";
}

// Upstream gnome-shell calendar.js 6-week padding policy:
// @first_day_dow_sun0 and @week_start_sun0 are 0=Sunday..6=Saturday.
// When a month starts on @week_start_sun0 (days_to_week_start == 0), pad one
// full week before the 1st so balanced 6-week grids don't dump two trailing
// rows of next-month days at the bottom.
inline int compute_calendar_grid_lead_days(int first_day_dow_sun0,
                                           int week_start_sun0) {
  const int ws = ((week_start_sun0 % 7) + 7) % 7;
  const int fd = ((first_day_dow_sun0 % 7) + 7) % 7;
  const int days_to_week_start = (7 + fd - ws) % 7;
  const int week_padding = (days_to_week_start == 0) ? 7 : 0;
  return week_padding + days_to_week_start;
}

inline int days_in_calendar_month(int year, int month_1_based) {
  static constexpr int kDays[12] = {
      31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const int m =
      (month_1_based < 1) ? 1 : (month_1_based > 12 ? 12 : month_1_based);
  if (m == 2) {
    const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    return leap ? 29 : 28;
  }
  return kDays[m - 1];
}

inline int clamp_day_for_month(int year, int month_1_based, int day) {
  const int max_d = days_in_calendar_month(year, month_1_based);
  if (day < 1)
    return 1;
  return day > max_d ? max_d : day;
}

}  // namespace winome
