// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Host process privileges and DPI awareness.

#pragma once

namespace winome {

// Enable a named privilege (e.g. L"SeTcbPrivilege") on the current process
// token. Returns true when the privilege was enabled.
bool enable_privilege(const wchar_t* name);

// Set per-monitor DPI awareness. Prefers the Windows 10 1703+ API and falls
// back to the shcore variant on older systems. Returns true on success.
bool set_process_dpi_aware();

}  // namespace winome
