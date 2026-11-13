// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Automated unit tests for WINOME core modules.

#include "config.h"
#include "i18n.h"
#include "overview-layout.h"
#include "wifi-networks.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check_true(bool cond, const char* expr, const char* file, int line) {
  if (!cond) {
    std::fprintf(stderr, "FAIL %s:%d: expected %s\n", file, line, expr);
    ++g_failures;
  }
}

void check_streq(const char* actual, const char* expected, const char* file, int line) {
  if ((actual == nullptr) != (expected == nullptr) ||
      (actual != nullptr && std::strcmp(actual, expected) != 0)) {
    std::fprintf(stderr, "FAIL %s:%d: got \"%s\", expected \"%s\"\n",
                 file, line,
                 actual ? actual : "<null>",
                 expected ? expected : "<null>");
    ++g_failures;
  }
}

#define CHECK(cond) check_true((cond), #cond, __FILE__, __LINE__)
#define CHECK_STREQ(a, b) check_streq((a), (b), __FILE__, __LINE__)

struct ScopedTempConfigDir {
  std::wstring dir;
  ScopedTempConfigDir() {
    wchar_t tmp[MAX_PATH] = {};
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n > 0 && n < MAX_PATH) {
      dir = std::wstring(tmp) + L"winome_test_" +
            std::to_wstring(GetCurrentProcessId());
      CreateDirectoryW(dir.c_str(), nullptr);
      SetEnvironmentVariableW(L"WINOME_CONFIG_DIR", dir.c_str());
    }
  }
  ~ScopedTempConfigDir() {
    SetEnvironmentVariableW(L"WINOME_CONFIG_DIR", nullptr);
    if (!dir.empty()) {
      DeleteFileW((dir + L"\\config.json").c_str());
      DeleteFileW((dir + L"\\config.json.tmp").c_str());
      RemoveDirectoryW(dir.c_str());
    }
  }
};

void test_config_roundtrip() {
  winome::WinomeConfig src;
  src.language = "zh";
  src.clock_format = "24h";
  src.clock_show_seconds = true;
  src.show_battery_percentage = true;
  src.hot_corner_enabled = false;
  src.hot_corner_cooldown_ms = 1250;
  src.win_key_overview = false;
  src.multi_monitor_panels = false;
  src.pinned_apps = {
    "C:\\Program Files\\\"Demo\"\\demo.exe",
    "notepad.exe",
    "\xE8\x85\xBE\xE8\xAE\xAF\xE4\xBC\x9A\xE8\xAE\xAE",  // UTF-8 Chinese
  };

  std::string json = winome::serialize_config(src);
  winome::WinomeConfig dst;
  CHECK(winome::parse_config(json, &dst));
  CHECK(dst.language == src.language);
  CHECK(dst.clock_format == src.clock_format);
  CHECK(dst.clock_show_seconds == src.clock_show_seconds);
  CHECK(dst.show_battery_percentage == src.show_battery_percentage);
  CHECK(dst.hot_corner_enabled == src.hot_corner_enabled);
  CHECK(dst.hot_corner_cooldown_ms == src.hot_corner_cooldown_ms);
  CHECK(dst.win_key_overview == src.win_key_overview);
  CHECK(dst.multi_monitor_panels == src.multi_monitor_panels);
  CHECK(dst.pinned_apps == src.pinned_apps);
}

void test_config_sanitization_and_unknown_keys() {
  winome::WinomeConfig out;
  CHECK(!winome::parse_config("", &out));
  CHECK(!winome::parse_config("not json", &out));

  const std::string json = R"({
    "future_obj": {"nested": [1, true, null, "x"]},
    "language": "fr",
    "clock_format": "99h",
    "hot_corner_cooldown_ms": -50,
    "show_battery_percentage": true,
    "unknown_num": 42,
    "pinned_apps": ["a.exe", "b\\c.exe"]
  })";
  CHECK(winome::parse_config(json, &out));
  CHECK(out.language == "auto");
  CHECK(out.clock_format == "auto");
  CHECK(out.hot_corner_cooldown_ms == 100);
  CHECK(out.show_battery_percentage == true);
  CHECK(out.pinned_apps.size() == 2);
  CHECK(out.pinned_apps[0] == "a.exe");
  CHECK(out.pinned_apps[1] == "b\\c.exe");

  winome::WinomeConfig hi;
  CHECK(winome::parse_config(R"({"hot_corner_cooldown_ms": 99999})", &hi));
  CHECK(hi.hot_corner_cooldown_ms == 5000);
}

void test_config_persistence_and_i18n(ScopedTempConfigDir* /*temp_dir*/) {
  int notify_count = 0;
  winome::ConfigListener cb = [](void* ud) {
    ++(*static_cast<int*>(ud));
  };
  winome::add_config_listener(cb, &notify_count);

  winome::WinomeConfig zh_cfg = winome::get_config();
  zh_cfg.language = "zh";
  zh_cfg.clock_show_seconds = true;
  CHECK(winome::update_config(zh_cfg));
  CHECK(notify_count == 1);
  CHECK_STREQ(winome::current_language(), "zh");
  CHECK(winome::is_zh());
  CHECK_STREQ(winome::tr("Settings", "设置"), "设置");

  winome::WinomeConfig en_cfg = zh_cfg;
  en_cfg.language = "en";
  CHECK(winome::update_config(en_cfg));
  CHECK(notify_count == 2);
  CHECK_STREQ(winome::current_language(), "en");
  CHECK(!winome::is_zh());
  CHECK_STREQ(winome::tr("Settings", "设置"), "Settings");

  CHECK(winome::reload_config());
  CHECK(notify_count == 3);
  CHECK(winome::get_config().language == "en");
  CHECK(winome::get_config().clock_show_seconds == true);

  winome::remove_config_listener(cb, &notify_count);
}

void test_wifi_ssid_decoding() {
  char out[129] = {};

  // Empty and hidden networks.
  winome_wifi_decode_ssid(nullptr, 0, out, sizeof(out));
  CHECK_STREQ(out, "");

  const guint8 zeros[6] = {0, 0, 0, 0, 0, 0};
  winome_wifi_decode_ssid(zeros, sizeof(zeros), out, sizeof(out));
  CHECK_STREQ(out, "");

  // Overlong (> 32 bytes) rejected.
  const guint8 overlong[33] = {
    '1','2','3','4','5','6','7','8','9','0',
    '1','2','3','4','5','6','7','8','9','0',
    '1','2','3','4','5','6','7','8','9','0',
    '1','2','3'
  };
  winome_wifi_decode_ssid(overlong, 33, out, sizeof(out));
  CHECK_STREQ(out, "");

  // Exact 32-byte IEEE 802.11 maximum SSID preserved.
  winome_wifi_decode_ssid(overlong, 32, out, sizeof(out));
  CHECK_STREQ(out, "12345678901234567890123456789012");

  // Valid UTF-8 Chinese SSID.
  const char* utf8_cn = "\xE5\xAE\xB6\xE9\x87\x8CWiFi";
  winome_wifi_decode_ssid(reinterpret_cast<const guint8*>(utf8_cn),
                          std::strlen(utf8_cn), out, sizeof(out));
  CHECK_STREQ(out, utf8_cn);

  // Non-UTF-8 Windows-1252 / Latin-1 ("Caf\xe9" -> "Café").
  const guint8 latin1[4] = {'C', 'a', 'f', 0xE9};
  winome_wifi_decode_ssid(latin1, 4, out, sizeof(out));
  CHECK_STREQ(out, "Caf\xC3\xA9");

  // Small buffer truncation never overflows and always NUL-terminates.
  char tiny[4] = {'x', 'x', 'x', 'x'};
  winome_wifi_decode_ssid(reinterpret_cast<const guint8*>("OfficeNet"),
                          9, tiny, sizeof(tiny));
  CHECK_STREQ(tiny, "Off");
}

double slot_width(const winome::OvSlot& s) {
  return static_cast<double>(s.window.rect.right - s.window.rect.left) * s.scale;
}

double slot_height(const winome::OvSlot& s) {
  return static_cast<double>(s.window.rect.bottom - s.window.rect.top) * s.scale;
}

bool rects_overlap(const winome::OvSlot& a, const winome::OvSlot& b) {
  constexpr double kEps = 0.5;
  const double aw = slot_width(a), ah = slot_height(a);
  const double bw = slot_width(b), bh = slot_height(b);
  const double ox = std::min(a.x + aw, b.x + bw) - std::max(a.x, b.x);
  const double oy = std::min(a.y + ah, b.y + bh) - std::max(a.y, b.y);
  return ox > kEps && oy > kEps;
}

void test_overview_layout_geometry() {
  const RECT monitor{0, 0, 1920, 1080};
  const RECT workarea{0, 32, 1920, 1080};
  const RECT picker{160, 80, 1760, 960};

  // Empty case.
  {
    std::vector<winome::OvSlot> slots;
    winome::overview_compute_slots({}, monitor, workarea, picker, 30.0, 30.0, &slots);
    CHECK(slots.empty());
  }

  // Zero-size window entry mixed with normal window never produces NaN/Inf.
  {
    std::vector<winome::OvWindowInfo> bad = {
      {nullptr, RECT{0, 0, 0, 0}},
      {nullptr, RECT{0, 0, 1280, 720}},
    };
    std::vector<winome::OvSlot> slots;
    winome::overview_compute_slots(bad, monitor, workarea, picker, 30.0, 30.0, &slots);
    CHECK(slots.size() == bad.size());
    for (const auto& s : slots) {
      CHECK(std::isfinite(s.x) && std::isfinite(s.y));
      CHECK(std::isfinite(s.scale) && s.scale >= 0.0);
    }
  }

  // Sweep counts 1..13 across a typical picker box.
  for (int n : {1, 2, 3, 4, 5, 7, 10, 13}) {
    std::vector<winome::OvWindowInfo> wins;
    for (int i = 0; i < n; ++i) {
      int w = 800 + (i % 4) * 280;
      int h = 600 + ((i + 1) % 3) * 220;
      wins.push_back({nullptr, RECT{0, 0, w, h}});
    }
    std::vector<winome::OvSlot> slots;
    winome::overview_compute_slots(wins, monitor, workarea, picker, 36.0, 36.0, &slots);
    CHECK(static_cast<int>(slots.size()) == n);
    for (size_t i = 0; i < slots.size(); ++i) {
      const auto& s = slots[i];
      const double sw = slot_width(s);
      const double sh = slot_height(s);
      CHECK(sw > 0.0 && sh > 0.0);
      CHECK(s.scale > 0.0 && s.scale <= 0.951);
      CHECK(s.x >= picker.left - 1.5 && s.y >= picker.top - 1.5);
      CHECK(s.x + sw <= picker.right + 1.5 && s.y + sh <= picker.bottom + 1.5);
      for (size_t j = i + 1; j < slots.size(); ++j)
        CHECK(!rects_overlap(s, slots[j]));
    }
  }
}

}  // namespace

int main() {
  ScopedTempConfigDir temp_dir;
  test_config_roundtrip();
  test_config_sanitization_and_unknown_keys();
  test_config_persistence_and_i18n(&temp_dir);
  test_wifi_ssid_decoding();
  test_overview_layout_geometry();

  if (g_failures == 0)
    std::printf("ALL WINOME UNIT TESTS PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
