// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Centralized internationalization helpers respecting WINOME config overrides.

#pragma once

namespace winome {

// Return active UI language code ("zh" or "en"), honoring config.json override
// or falling back to Windows user UI language when config.language == "auto".
const char* current_language();

// True when current_language() == "zh".
bool is_zh();

// Select between English and Simplified Chinese static UI strings.
const char* tr(const char* en, const char* zh);

}  // namespace winome
