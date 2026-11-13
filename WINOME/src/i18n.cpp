// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Centralized language resolution backed by config + Windows UI locale.

#include "i18n.h"
#include "config.h"
#include "system-status.h"

#include <cstring>

namespace winome {

const char* current_language() {
  const WinomeConfig& cfg = get_config();
  if (cfg.language == "zh")
    return "zh";
  if (cfg.language == "en")
    return "en";
  return get_system_language();
}

bool is_zh() {
  return std::strcmp(current_language(), "zh") == 0;
}

const char* tr(const char* en, const char* zh) {
  return is_zh() ? zh : en;
}

}  // namespace winome
