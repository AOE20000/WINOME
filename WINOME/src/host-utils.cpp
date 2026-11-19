// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Host process privileges and DPI awareness.

#include "host-utils.h"

#include <windows.h>
#include <dwmapi.h>
#include <shellscalingapi.h>

#include <cctype>

namespace winome {

namespace {

// SetProcessDpiAwarenessContext lives in user32.dll on Windows 10 1703+; the
// older SetProcessDpiAwareness lives in shcore.dll. Both are loaded
// dynamically so the binary keeps running on older systems.
using SetDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
using SetProcessDpiAwarenessFn = HRESULT(WINAPI*)(PROCESS_DPI_AWARENESS);

}  // namespace

bool enable_privilege(const wchar_t* name) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(),
                        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
    return false;
  }

  LUID luid{};
  bool ok = LookupPrivilegeValueW(nullptr, name, &luid);
  if (!ok) {
    CloseHandle(token);
    return false;
  }

  TOKEN_PRIVILEGES tp{};
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Luid = luid;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

  // AdjustTokenPrivileges reports success even when the change was a no-op
  // because the caller lacks the privilege; GetLastError() disambiguates.
  ok = AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr) != 0;
  bool granted = ok && GetLastError() == ERROR_SUCCESS;

  CloseHandle(token);
  return granted;
}

bool set_process_dpi_aware() {
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  auto set_context = reinterpret_cast<SetDpiAwarenessContextFn>(
      reinterpret_cast<void *>(
          user32 != nullptr
              ? GetProcAddress(user32, "SetProcessDpiAwarenessContext")
              : nullptr));
  if (set_context != nullptr)
    return set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;

  HMODULE shcore = LoadLibraryW(L"shcore.dll");
  auto set_awareness = reinterpret_cast<SetProcessDpiAwarenessFn>(
      reinterpret_cast<void *>(
          shcore != nullptr ? GetProcAddress(shcore, "SetProcessDpiAwareness")
                            : nullptr));
  if (set_awareness == nullptr) {
    if (shcore != nullptr)
      FreeLibrary(shcore);
    return false;
  }

  HRESULT hr = set_awareness(PROCESS_PER_MONITOR_DPI_AWARE);
  FreeLibrary(shcore);
  return SUCCEEDED(hr);
}

std::wstring app_data_dir() {
  wchar_t base[MAX_PATH] = {0};
  DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
  std::wstring dir;
  if (len > 0 && len < MAX_PATH) {
    dir.assign(base, len);
  } else {
    len = GetTempPathW(MAX_PATH, base);
    if (len > 0 && len < MAX_PATH)
      dir.assign(base, len);
  }
  if (!dir.empty() && dir.back() == L'\\')
    dir.pop_back();
  if (!dir.empty())
    dir += L"\\WINOME";
  else
    dir = L"WINOME";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

std::string app_data_dir_ascii() {
  std::wstring dir = app_data_dir();
  if (dir.empty())
    return {};

  wchar_t short_buf[MAX_PATH] = {0};
  DWORD slen = GetShortPathNameW(dir.c_str(), short_buf, MAX_PATH);
  const std::wstring& chosen =
      (slen > 0 && slen < MAX_PATH) ? std::wstring(short_buf, slen) : dir;

  int bytes = WideCharToMultiByte(CP_UTF8, 0, chosen.c_str(),
                                  static_cast<int>(chosen.size()),
                                  nullptr, 0, nullptr, nullptr);
  if (bytes <= 0)
    return {};
  std::string out(static_cast<size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, chosen.c_str(),
                      static_cast<int>(chosen.size()),
                      out.data(), bytes, nullptr, nullptr);
  return out;
}

bool is_overview_default_blacklisted_window_info(
    const std::string& class_name_utf8,
    const std::string& proc_name_utf8,
    const std::string& title_utf8,
    bool cloaked) {
  auto to_ascii_lower_char = [](unsigned char c) -> char {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a')
                                  : static_cast<char>(c);
  };
  auto ascii_lower = [&](const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
      out.push_back(to_ascii_lower_char(c));
    return out;
  };
  auto trim_ws = [](const std::string& s) {
    size_t b = 0;
    while (b < s.size() &&
           (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) {
      ++b;
    }
    size_t e = s.size();
    while (e > b &&
           (s[e - 1] == ' ' || s[e - 1] == '\t' ||
            s[e - 1] == '\r' || s[e - 1] == '\n')) {
      --e;
    }
    return s.substr(b, e - b);
  };
  auto compact_ascii_lower = [&](const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        continue;
      out.push_back(to_ascii_lower_char(c));
    }
    return out;
  };

  const std::string cls_low = ascii_lower(trim_ws(class_name_utf8));
  std::string proc_low = ascii_lower(trim_ws(proc_name_utf8));
  const size_t sep = proc_low.find_last_of("\\/");
  if (sep != std::string::npos)
    proc_low = proc_low.substr(sep + 1);

  const std::string title_trimmed = trim_ws(title_utf8);
  const std::string title_compact = compact_ascii_lower(title_trimmed);

  // 1. "Windows 输入体验" / "Windows Input Experience" (TextInputHost).
  if (proc_low == "textinputhost.exe" ||
      proc_low == "windowsinternal.composableshell.experiences.textinput.inputapp.exe" ||
      title_compact == "windows\xE8\xBE\x93\xE5\x85\xA5\xE4\xBD\x93\xE9\xAA\x8C" ||
      title_compact == "windowsinputexperience") {
    return true;
  }

  // 2. Nameless, blank/ghost windows hosted by explorer.exe (such as hidden
  // ApplicationFrameWindow shells or un-titled WorkerW containers), while
  // keeping normal CabinetWClass / ExploreWClass folder windows and dialogs.
  if (proc_low == "explorer.exe" && title_trimmed.empty()) {
    if (cloaked)
      return true;
    const bool is_real_explorer_ui =
        cls_low == "cabinetwclass" ||
        cls_low == "explorewclass" ||
        cls_low == "#32770";
    if (!is_real_explorer_ui)
      return true;
  }

  return false;
}

bool is_overview_default_blacklisted_hwnd(HWND hwnd, bool enabled) {
  if (!enabled || hwnd == nullptr)
    return false;

  auto wide_to_utf8_buf = [](const wchar_t* w) -> std::string {
    if (w == nullptr || w[0] == L'\0')
      return {};
    int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1)
      return {};
    std::string out(static_cast<size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), need, nullptr, nullptr);
    return out;
  };

  wchar_t cls[128] = {};
  GetClassNameW(hwnd, cls, 128);

  wchar_t title[256] = {};
  GetWindowTextW(hwnd, title, 256);

  std::wstring proc_name;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != 0) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc != nullptr) {
      wchar_t path[MAX_PATH] = {};
      DWORD len = MAX_PATH;
      if (QueryFullProcessImageNameW(proc, 0, path, &len) && len > 0)
        proc_name.assign(path, len);
      CloseHandle(proc);
    }
  }

  BOOL cloaked = FALSE;
  DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));

  return is_overview_default_blacklisted_window_info(
      wide_to_utf8_buf(cls),
      wide_to_utf8_buf(proc_name.c_str()),
      wide_to_utf8_buf(title),
      cloaked != FALSE);
}

}  // namespace winome
