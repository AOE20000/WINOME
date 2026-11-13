// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Host process privileges and DPI awareness.

#include "host-utils.h"

#include <windows.h>
#include <shellscalingapi.h>

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

}  // namespace winome
