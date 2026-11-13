// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Host process privileges and DPI awareness.

#pragma once

#include <string>

namespace winome {

// Enable a named privilege (e.g. L"SeTcbPrivilege") on the current process
// token. Returns true when the privilege was enabled.
bool enable_privilege(const wchar_t* name);

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

}  // namespace winome
