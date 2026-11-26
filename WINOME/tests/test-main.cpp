// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Automated unit tests for WINOME core modules.

#include "app-grid.h"
#include "app-index.h"
#include "config.h"
#include "crash-dialog.h"
#include "debug-runtime.h"
#include "diagnostics-export.h"
#include "end-session-dialog.h"
#include "../ctl/ctl-protocol.h"
#include "fonts.h"
#include "host-utils.h"
#include "hot-corner.h"
#include "i18n.h"
#include "live-wallpaper.h"
#include "monitor-manager.h"
#include "native-taskbar.h"
#include "notifications.h"
#include "overview-dash.h"
#include "overview-layout.h"
#include "overview-search.h"
#include "overview-secondary.h"
#include "overview-thumbs.h"
#include "overview-trigger.h"
#include "fullscreen-watcher.h"
#include "winome-version.h"
#include "perf-metrics.h"
#include "plugin-host.h"
#include "power-modes.h"
#include "run-dialog.h"
#include "shell-ipc.h"
#include "shell-panel.h"
#include "st-engine.h"
#include "system-status.h"
#include "systray.h"
#include "virtual-desktop.h"
#include "workspace-manager.h"
#include "wifi-networks.h"
#include "window-theming.h"
#include "winrt-bridge.h"

#include <cmath>
#include <zlib.h>

#include <windows.h>
#include <dwmapi.h>
#include <glib.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
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
  winome::WinomeConfig def;
  CHECK(def.end_session_confirm == true);
  CHECK(def.overview_window_blacklist == true);
  CHECK(def.live_wallpaper_mode == "live");
  CHECK(def.dash_isolate_workspaces == false);
  CHECK(def.auto_elevate == true);
  CHECK(def.unify_dark_titlebars == false);
  CHECK(def.unify_rounded_corners == false);
  CHECK(def.sync_windows_primary_display == false);

  winome::WinomeConfig src;
  src.language = "zh";
  src.clock_format = "24h";
  src.clock_show_seconds = true;
  src.show_battery_percentage = true;
  src.hot_corner_enabled = false;
  src.hot_corner_cooldown_ms = 1250;
  src.win_key_overview = false;
  src.auto_elevate = false;
  src.overview_window_blacklist = false;
  src.live_wallpaper_mode = "snapshot";
  src.dash_isolate_workspaces = true;
  src.multi_monitor_panels = false;
  src.sync_windows_primary_display = true;
  src.end_session_confirm = false;
  src.unify_dark_titlebars = true;
  src.unify_rounded_corners = true;
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
  CHECK(dst.auto_elevate == src.auto_elevate);
  CHECK(dst.overview_window_blacklist == src.overview_window_blacklist);
  CHECK(dst.live_wallpaper_mode == src.live_wallpaper_mode);
  CHECK(dst.dash_isolate_workspaces == src.dash_isolate_workspaces);
  CHECK(dst.multi_monitor_panels == src.multi_monitor_panels);
  CHECK(dst.sync_windows_primary_display == src.sync_windows_primary_display);
  CHECK(dst.end_session_confirm == src.end_session_confirm);
  CHECK(dst.unify_dark_titlebars == src.unify_dark_titlebars);
  CHECK(dst.unify_rounded_corners == src.unify_rounded_corners);
  CHECK(dst.pinned_apps == src.pinned_apps);

  // Legacy configs without new keys preserve safe defaults.
  winome::WinomeConfig legacy;
  CHECK(winome::parse_config("{\"language\":\"en\"}", &legacy));
  CHECK(legacy.end_session_confirm == true);
  CHECK(legacy.auto_elevate == true);
  CHECK(legacy.overview_window_blacklist == true);
  CHECK(legacy.live_wallpaper_mode == "live");
  CHECK(legacy.dash_isolate_workspaces == false);
  CHECK(legacy.sync_windows_primary_display == false);
  CHECK(legacy.unify_dark_titlebars == false);
  CHECK(legacy.unify_rounded_corners == false);

  // Startup auto-elevation & relaunch command-line quoting helpers.
  CHECK(winome::should_attempt_auto_elevate(true, false, false));
  CHECK(!winome::should_attempt_auto_elevate(false, false, false));
  CHECK(!winome::should_attempt_auto_elevate(true, true, false));
  CHECK(!winome::should_attempt_auto_elevate(true, false, true));
  CHECK(winome::build_elevated_relaunch_args({}) == L"--no-elevate");
  CHECK(winome::build_elevated_relaunch_args({L"--no-elevate"}) ==
        L"--no-elevate");
  CHECK(winome::build_elevated_relaunch_args(
            {L"--safe-mode", L"C:\\My Folder\\arg\"x", L"--no-elevate"}) ==
        L"--safe-mode \"C:\\My Folder\\arg\\\"x\" --no-elevate");
}

void test_config_sanitization_and_unknown_keys() {
  winome::WinomeConfig out;
  CHECK(!winome::parse_config("", &out));
  CHECK(!winome::parse_config("not json", &out));

  const std::string json = R"({
    "future_obj": {"nested": [1, true, null, "x"]},
    "language": "fr",
    "clock_format": "99h",
    "live_wallpaper_mode": "invalid_mode",
    "hot_corner_cooldown_ms": -50,
    "show_battery_percentage": true,
    "unknown_num": 42,
    "pinned_apps": ["a.exe", "b\\c.exe"]
  })";
  CHECK(winome::parse_config(json, &out));
  CHECK(out.language == "auto");
  CHECK(out.clock_format == "auto");
  CHECK(out.live_wallpaper_mode == "live");
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

  // Mini workspace-thumbnail occlusion clipping (prevents DWM mini-previews
  // from bleeding through expanding/shrinking workspace backgrounds).
  {
    const RECT mini{800, 140, 920, 220};  // 120x80 mini preview
    const SIZE src{1200, 800};

    // 1. Settled WINDOW_PICKER: workspace box sits cleanly below thumbnails.
    {
      std::vector<RECT> boxes = {RECT{335, 240, 1585, 930}};
      auto res = winome::compute_mini_thumb_clip(mini, src, boxes);
      CHECK(res.visible);
      CHECK(res.dest_rect.left == 800 && res.dest_rect.top == 140 &&
            res.dest_rect.right == 920 && res.dest_rect.bottom == 220);
      CHECK(res.src_rect.left == 0 && res.src_rect.top == 0 &&
            res.src_rect.right == 1200 && res.src_rect.bottom == 800);
    }

    // 2. Fullscreen / early-open / late-close: completely covered -> hidden.
    {
      std::vector<RECT> boxes = {RECT{0, 32, 1920, 1080}};
      auto res = winome::compute_mini_thumb_clip(mini, src, boxes);
      CHECK(!res.visible);
    }

    // 3. Mid-open / mid-close: active workspace box cuts across y = 180 ->
    // only top strip [140..180] and top half of source [0..400] stay visible.
    {
      std::vector<RECT> boxes = {RECT{200, 180, 1720, 1000}};
      auto res = winome::compute_mini_thumb_clip(mini, src, boxes);
      CHECK(res.visible);
      CHECK(res.dest_rect.left == 800 && res.dest_rect.top == 140 &&
            res.dest_rect.right == 920 && res.dest_rect.bottom == 180);
      CHECK(res.src_rect.left == 0 && res.src_rect.top == 0 &&
            res.src_rect.right == 1200 && res.src_rect.bottom == 400);
    }

    // 4. Multi-box occlusion (left/right workspace cards in APP_GRID): disjoint side strip retained.
    {
      std::vector<RECT> boxes = {
          RECT{700, 120, 840, 270},   // covers [800..840]
          RECT{900, 120, 1060, 270},  // covers [900..920]
      };
      auto res = winome::compute_mini_thumb_clip(mini, src, boxes);
      CHECK(res.visible);
      CHECK(res.dest_rect.left == 840 && res.dest_rect.right == 900 &&
            res.dest_rect.top == 140 && res.dest_rect.bottom == 220);
      CHECK(res.src_rect.left == 400 && res.src_rect.right == 1000);
    }

    // 5. Unknown source size: unoccluded ok, partially occluded must stay hidden.
    {
      std::vector<RECT> below = {RECT{335, 240, 1585, 930}};
      CHECK(winome::compute_mini_thumb_clip(mini, SIZE{0, 0}, below).visible);
      std::vector<RECT> overlap = {RECT{200, 180, 1720, 1000}};
      CHECK(!winome::compute_mini_thumb_clip(mini, SIZE{0, 0}, overlap).visible);
    }

    // 6. Sub-2px sliver near moving edge is suppressed.
    {
      std::vector<RECT> sliver = {RECT{200, 141, 1720, 1000}};
      CHECK(!winome::compute_mini_thumb_clip(mini, src, sliver).visible);
    }

    // 7. Coverage & overlap helpers for thumbnails occlusion gating.
    {
      const RECT thumbs{740, 130, 1180, 226};
      std::vector<RECT> full = {RECT{0, 32, 1920, 1080}};
      std::vector<RECT> mid = {RECT{200, 180, 1720, 1000}};
      std::vector<RECT> settled = {RECT{335, 240, 1585, 930}};
      CHECK(winome::is_rect_fully_covered_by_boxes(thumbs, full));
      CHECK(winome::is_rect_overlapping_any_box(thumbs, full));
      CHECK(!winome::is_rect_fully_covered_by_boxes(thumbs, mid));
      CHECK(winome::is_rect_overlapping_any_box(thumbs, mid));
      CHECK(!winome::is_rect_fully_covered_by_boxes(thumbs, settled));
      CHECK(!winome::is_rect_overlapping_any_box(thumbs, settled));
    }

    // 8. Pivot transform matching GNOME Shell ThumbnailsBox APP_GRID animation
    //    (pivot=(0.5, 0.5), scale=0.5, translate_y=+height/2).
    {
      const RECT thumbs{740, 120, 1180, 220};  // center = (960, 170), h = 100
      const RECT id = winome::transform_rect_around_pivot(thumbs, 960.0, 170.0, 1.0, 0.0);
      CHECK(EqualRect(&id, &thumbs));

      const RECT shrunk = winome::transform_rect_around_pivot(thumbs, 960.0, 170.0, 0.5, 50.0);
      CHECK(shrunk.left == 850 && shrunk.right == 1070);
      CHECK(shrunk.top == 195 && shrunk.bottom == 245);
    }

    // 9. Workarea wallpaper projection (GNOME shell-workspace-background.c):
    //    at t=0 (1920x1048 below 32px panel), 1920x1080 wallpaper must land at
    //    (0, -32, 1920, 1080) with zero vertical shift against live desktop.
    {
      const double ratio = 32.0 / 1048.0;
      const auto full = winome::compute_workarea_wallpaper_draw_rect(
          0.0, 0.0, 1920.0, 1048.0, 1920.0, 1080.0, ratio);
      CHECK(std::abs(full.x - 0.0) < 0.01);
      CHECK(std::abs(full.y - (-32.0)) < 0.01);
      CHECK(std::abs(full.w - 1920.0) < 0.01);
      CHECK(std::abs(full.h - 1080.0) < 0.01);

      // Scaled workspace card preserves exact top-panel crop ratio.
      const auto picker = winome::compute_workarea_wallpaper_draw_rect(
          274.0, 181.0, 1372.0, 749.0, 1920.0, 1080.0, ratio);
      const double expected_top = 181.0 - 749.0 * ratio;
      CHECK(std::abs(picker.y - expected_top) < 0.5);
      CHECK(picker.w >= 1372.0 - 0.1 && picker.h >= 749.0 * (1.0 + ratio) - 0.1);
    }
  }
}

void test_wifi_profile_xml() {
  char* wpa = winome_wifi_build_profile_xml("Café & \"Home\" <5G>", "p&w<d>1'", FALSE);
  CHECK(wpa != nullptr);
  if (wpa != nullptr) {
    CHECK(std::strstr(wpa, "<name>Café &amp; &quot;Home&quot; &lt;5G&gt;</name>") != nullptr);
    CHECK(std::strstr(wpa, "<authentication>WPA2PSK</authentication>") != nullptr);
    CHECK(std::strstr(wpa, "<encryption>AES</encryption>") != nullptr);
    CHECK(std::strstr(wpa, "<keyMaterial>p&amp;w&lt;d&gt;1&apos;</keyMaterial>") != nullptr);
    CHECK(std::strstr(wpa, "<hex>436166C3A920262022486F6D6522203C35473E</hex>") != nullptr);
    g_free(wpa);
  }

  char* open = winome_wifi_build_profile_xml("GuestWifi", "", TRUE);
  CHECK(open != nullptr);
  if (open != nullptr) {
    CHECK(std::strstr(open, "<authentication>open</authentication>") != nullptr);
    CHECK(std::strstr(open, "<encryption>none</encryption>") != nullptr);
    CHECK(std::strstr(open, "<sharedKey>") == nullptr);
    g_free(open);
  }

  CHECK(winome_wifi_build_profile_xml("", "secret", FALSE) == nullptr);
}

void test_notification_age_formatting() {
  const int64_t now = 1700000000;
  CHECK(winome::format_notification_age(now - 20, now, false) == "Just now");
  CHECK(winome::format_notification_age(now - 20, now, true) == "刚刚");
  CHECK(winome::format_notification_age(now - 300, now, false) == "5m");
  CHECK(winome::format_notification_age(now - 300, now, true) == "5 分钟前");
  CHECK(winome::format_notification_age(now - 7200, now, false) == "2h");
  CHECK(winome::format_notification_age(now - 7200, now, true) == "2 小时前");
  CHECK(winome::format_notification_age(now - 172800, now, false) == "2d");
  CHECK(winome::format_notification_age(now - 172800, now, true) == "2 天前");
}

void test_systray_filtering_and_fallback() {
  CHECK(winome::is_system_builtin_tray_item("Show hidden icons", ""));
  CHECK(winome::is_system_builtin_tray_item("显示隐藏的图标", ""));
  CHECK(winome::is_system_builtin_tray_item("Volume", "SystemTrayIcon"));
  CHECK(winome::is_system_builtin_tray_item("Battery status: 98% remaining", ""));
  CHECK(!winome::is_system_builtin_tray_item("BatteryMon Pro", ""));
  CHECK(!winome::is_system_builtin_tray_item("Steam - Updates", "NotifyItemIcon_1"));
  CHECK(!winome::is_system_builtin_tray_item("WeChat", ""));

  CHECK_STREQ(winome::tray_fallback_icon_name("NVIDIA Settings", ""),
              "video-display-symbolic");
  CHECK_STREQ(winome::tray_fallback_icon_name("Steam", ""),
              "applications-games-symbolic");
  CHECK_STREQ(winome::tray_fallback_icon_name("OneDrive - Personal", ""),
              "weather-overcast-symbolic");
  CHECK_STREQ(winome::tray_fallback_icon_name("Clash Verge", ""),
              "network-wired-symbolic");
  CHECK_STREQ(winome::tray_fallback_icon_name("Microsoft Pinyin IME", ""),
              "input-keyboard-symbolic");
  CHECK_STREQ(winome::tray_fallback_icon_name("Unknown Utility", ""),
              "application-x-executable-symbolic");
}

void test_config_listener_updates() {
  int notifications = 0;
  auto listener = [](void* ud) {
    int* n = static_cast<int*>(ud);
    ++(*n);
  };
  winome::add_config_listener(listener, &notifications);
  winome::WinomeConfig updated = winome::get_config();
  updated.show_battery_percentage = !updated.show_battery_percentage;
  updated.clock_show_seconds = !updated.clock_show_seconds;
  winome::update_config(updated);
  CHECK(notifications >= 1);
  CHECK(winome::get_config().show_battery_percentage == updated.show_battery_percentage);
  winome::remove_config_listener(listener, &notifications);
}

void test_monitor_manager_geometry() {
  RECT mon{1920, 0, 3840, 1080};
  RECT work = winome::compute_panel_work_area(mon, 32);
  CHECK(work.left == 1920);
  CHECK(work.top == 32);
  CHECK(work.right == 3840);
  CHECK(work.bottom == 1080);

  winome::MonitorDescriptor d1;
  d1.monitor_rect = mon;
  d1.dpi_x = 96;
  d1.dpi_y = 96;
  d1.is_primary = true;

  winome::MonitorDescriptor d2 = d1;
  std::vector<winome::MonitorDescriptor> a{d1};
  std::vector<winome::MonitorDescriptor> b{d2};
  CHECK(winome::same_monitor_topology(a, b));
  b[0].dpi_x = 144;
  CHECK(!winome::same_monitor_topology(a, b));

  auto live = winome::enumerate_monitors();
  CHECK(!live.empty());
  if (!live.empty())
    CHECK(live[0].is_primary);
}

void test_virtual_desktop_profiles() {
  auto win10 = winome::vd::vtable_slots_for_build(19045);
  CHECK(!win10.can_manage_desktops);
  CHECK(win10.create_desktop == -1);
  CHECK(win10.remove_desktop == -1);

  auto win11_23h2 = winome::vd::vtable_slots_for_build(22621);
  CHECK(win11_23h2.can_manage_desktops);
  CHECK(win11_23h2.move_view_to_desktop == 4);
  CHECK(win11_23h2.create_desktop == 10);
  CHECK(win11_23h2.remove_desktop == 12);

  auto win11_24h2 = winome::vd::vtable_slots_for_build(26100);
  CHECK(win11_24h2.can_manage_desktops);
  CHECK(win11_24h2.move_view_to_desktop == 4);
  CHECK(win11_24h2.create_desktop == 11);
  CHECK(win11_24h2.remove_desktop == 13);

  CHECK(winome::vd::windows_build_number() > 0);
}

void test_app_index_and_lru_cache() {
  CHECK(winome::is_ignored_shortcut_name("uninstall my app"));
  CHECK(winome::is_ignored_shortcut_name("卸载某某软件"));
  CHECK(!winome::is_ignored_shortcut_name("notepad++"));

  CHECK(winome::canonical_app_id(L"Microsoft.Terminal!App", L"C:\\a.exe",
                                 L"C:\\a.lnk") == "microsoft.terminal!app");
  CHECK(winome::canonical_app_id(L"", L"C:/Apps/Foo.exe", L"C:\\a.lnk") ==
        "c:\\apps\\foo.exe");

  winome::AppEntry a;
  a.id = "1";
  a.name = "My Terminal";
  a.name_lower = "my terminal";
  a.target_lower = "wt.exe";

  winome::AppEntry b;
  b.id = "2";
  b.name = "Terminal Emulator";
  b.name_lower = "terminal emulator";
  b.target_lower = "term.exe";

  winome::AppEntry c;
  c.id = "3";
  c.name = "Notepad";
  c.name_lower = "notepad";
  c.target_lower = "notepad.exe";

  auto hits = winome::filter_and_sort_apps({a, b, c}, "term");
  CHECK(hits.size() == 2);
  CHECK(hits[0].id == "2"); // prefix beats word match
  CHECK(hits[1].id == "1");

  // Pinning persistence roundtrip.
  winome::set_app_id_pinned("c:/Program Files/App.exe", true);
  CHECK(winome::is_app_id_pinned(winome::get_config().pinned_apps,
                                 "C:\\Program Files\\App.exe"));
  CHECK(!winome::toggle_app_id_pinned("C:\\PROGRAM FILES\\APP.EXE"));
  CHECK(!winome::is_app_id_pinned(winome::get_config().pinned_apps,
                                  "c:\\program files\\app.exe"));

  // LRU texture eviction test.
  winome::app_icon_cache_clear();
  uint32_t px = 0xFFFFFFFFu;
  GBytes *bytes = g_bytes_new(&px, sizeof(px));
  GdkTexture *tex = gdk_memory_texture_new(1, 1, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED,
                                           bytes, 4);
  g_bytes_unref(bytes);
  size_t cap = winome::app_icon_cache_capacity();
  CHECK(cap >= 512);
  for (size_t i = 0; i < cap + 5; ++i)
    winome::app_icon_cache_store("app-" + std::to_string(i), 64, tex);
  g_object_unref(tex);

  CHECK(winome::app_icon_cache_count() == cap);
  GdkTexture *evicted = winome::app_icon_cache_lookup("app-0", 64);
  CHECK(evicted == nullptr);
  GdkTexture *kept =
      winome::app_icon_cache_lookup("app-" + std::to_string(cap + 4), 64);
  CHECK(kept != nullptr);
  if (kept != nullptr)
    g_object_unref(kept);
  // Canonical icon cache bucket sharing (<=128 -> 128, >128 -> 256).
  {
    GBytes *b2 = g_bytes_new(&px, sizeof(px));
    GdkTexture *t2 = gdk_memory_texture_new(1, 1, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED,
                                            b2, 4);
    g_bytes_unref(b2);
    winome::app_icon_cache_store("bucket.app", 96, t2);
    GdkTexture *hit = winome::app_icon_cache_lookup("bucket.app", 128);
    CHECK(hit != nullptr);
    if (hit != nullptr)
      g_object_unref(hit);
    GdkTexture *miss = winome::app_icon_cache_lookup("bucket.app", 256);
    CHECK(miss == nullptr);
    g_object_unref(t2);
  }
  winome::app_icon_cache_clear();
  CHECK(winome::app_icon_cache_count() == 0);

  // Straight vs premultiplied RawDecodedIcon materialization + revision tracking.
  uint32_t raw_px[4] = {0x80FFFFFFu, 0x800000FFu, 0xFF00FF00u, 0x00000000u};
  for (bool unpremul : {true, false}) {
    winome::RawDecodedIcon raw;
    raw.id = "raw.test";
    raw.size = 96;
    raw.width = 2;
    raw.height = 2;
    raw.unpremultiplied = unpremul;
    raw.bytes = g_bytes_new(raw_px, sizeof(raw_px));
    GdkTexture *t = winome::app_icon_texture_from_raw(raw);
    CHECK(t != nullptr);
    if (t != nullptr) {
      CHECK(gdk_texture_get_width(t) == 2);
      CHECK(gdk_texture_get_height(t) == 2);
      g_object_unref(t);
    }
    winome::app_icon_raw_clear(raw);
    CHECK(raw.bytes == nullptr);
    CHECK(winome::app_icon_texture_from_raw(raw) == nullptr);
  }
  (void)winome::app_index_get_all();
  CHECK(winome::app_index_revision() >= 1);
}

void test_app_grid_and_running_window_matching() {
  // 1. match_entry_running_windows: AUMID match, multi-PWA isolation, exe fallback, and FrameHost guard.
  winome::AppEntry pwa_mail;
  pwa_mail.id = "msedge.mail.aumid";
  pwa_mail.aumid = L"MSEdge.Mail.AUMID";
  pwa_mail.target_exe = L"C:\\Program Files\\Edge\\msedge_proxy.exe";

  winome::AppEntry desktop_app;
  desktop_app.id = "c:\\tools\\editor.exe";
  desktop_app.target_exe = L"C:\\Tools\\Editor.exe";

  winome::AppEntry uwp_calc;
  uwp_calc.id = "microsoft.windowscalculator_8wekyb3d8bbwe!app";
  uwp_calc.aumid = L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App";
  uwp_calc.target_exe = L"C:\\Windows\\System32\\ApplicationFrameHost.exe";
  uwp_calc.is_uwp = true;

  winome::RunningWindow w_mail;
  w_mail.hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1001));
  w_mail.title = "Mail PWA";
  w_mail.canonical_id = "msedge.mail.aumid";
  w_mail.aumid_norm = "msedge.mail.aumid";
  w_mail.exe_norm = "c:\\program files\\edge\\msedge_proxy.exe";
  w_mail.exe_base_lower = "msedge_proxy.exe";

  winome::RunningWindow w_calendar;
  w_calendar.hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1002));
  w_calendar.title = "Calendar PWA";
  w_calendar.canonical_id = "msedge.calendar.aumid";
  w_calendar.aumid_norm = "msedge.calendar.aumid";
  w_calendar.exe_norm = "c:\\program files\\edge\\msedge_proxy.exe";
  w_calendar.exe_base_lower = "msedge_proxy.exe";

  winome::RunningWindow w_editor;
  w_editor.hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1003));
  w_editor.title = "Editor";
  w_editor.canonical_id = "c:\\tools\\editor.exe";
  w_editor.exe_norm = "c:\\tools\\editor.exe";
  w_editor.exe_base_lower = "editor.exe";

  winome::RunningWindow w_other_uwp;
  w_other_uwp.hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1004));
  w_other_uwp.title = "Photos";
  w_other_uwp.canonical_id = "c:\\windows\\system32\\applicationframehost.exe";
  w_other_uwp.aumid_norm = "";
  w_other_uwp.exe_norm = "c:\\windows\\system32\\applicationframehost.exe";
  w_other_uwp.exe_base_lower = "applicationframehost.exe";

  winome::RunningWindow w_calc;
  w_calc.hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1005));
  w_calc.title = "Calculator";
  w_calc.canonical_id = "microsoft.windowscalculator_8wekyb3d8bbwe!app";
  w_calc.aumid_norm = "microsoft.windowscalculator_8wekyb3d8bbwe!app";
  w_calc.exe_norm = "c:\\windows\\system32\\applicationframehost.exe";
  w_calc.exe_base_lower = "applicationframehost.exe";

  std::vector<winome::RunningWindow> windows = {
      w_mail, w_calendar, w_editor, w_other_uwp, w_calc};

  auto mail_hits = winome::match_entry_running_windows(pwa_mail, windows);
  CHECK(mail_hits.size() == 1 && mail_hits[0].title == "Mail PWA");

  auto editor_hits = winome::match_entry_running_windows(desktop_app, windows);
  CHECK(editor_hits.size() == 1 && editor_hits[0].title == "Editor");

  auto calc_hits = winome::match_entry_running_windows(uwp_calc, windows);
  CHECK(calc_hits.size() == 1 && calc_hits[0].title == "Calculator");

  // 2. compute_app_grid_dimensions across desktop and portrait viewports.
  auto d_xl = winome::compute_app_grid_dimensions(1320, 920);
  CHECK(d_xl.cols == 6 && d_xl.rows == 4 && d_xl.icon_size == 96 &&
        d_xl.page_size == 24);

  auto d_md = winome::compute_app_grid_dimensions(1320, 670);
  CHECK(d_md.cols == 6 && d_md.rows == 4 && d_md.icon_size == 80 &&
        d_md.page_size == 24);

  auto d_mid = winome::compute_app_grid_dimensions(1000, 560);
  CHECK(d_mid.cols == 5 && d_mid.rows == 3 && d_mid.icon_size == 96 &&
        d_mid.page_size == 15);

  auto d_port = winome::compute_app_grid_dimensions(500, 760);
  CHECK(d_port.cols == 4 && d_port.rows == 5 && d_port.icon_size == 56 &&
        d_port.page_size == 20);

  // 3. GTK4 AppGrid pagination, running refresh, and disposal lifecycle.
  if (gtk_init_check()) {
    GtkWidget* grid = winome::app_grid_new(nullptr, nullptr, nullptr);
    CHECK(grid != nullptr);
    if (grid != nullptr) {
      g_object_ref_sink(grid);
      CHECK(gtk_widget_get_halign(grid) == GTK_ALIGN_FILL);
      CHECK(gtk_widget_get_valign(grid) == GTK_ALIGN_FILL);
      CHECK(gtk_widget_get_overflow(grid) == GTK_OVERFLOW_HIDDEN);
      winome::app_grid_set_query(grid, "");
      winome::app_grid_rebuild(grid, 1320, 670);
      winome::app_grid_set_transition_opacity(grid, true, 0.35);
      CHECK(std::abs(gtk_widget_get_opacity(grid) - 0.35) < 0.02);
      winome::app_grid_set_transition_opacity(grid, false, 1.0);
      CHECK(std::abs(gtk_widget_get_opacity(grid) - 1.0) < 0.02);
      winome::app_grid_last_page(grid);
      winome::app_grid_first_page(grid);
      winome::app_grid_next_page(grid);
      winome::app_grid_prev_page(grid);
      winome::app_grid_refresh_running(grid);
      winome::app_grid_close_menu(grid);
      g_object_unref(grid);
    }
  }
}

void test_app_grid_pagination() {
  CHECK(winome::compute_app_grid_page_count(0, 24) == 1);
  CHECK(winome::compute_app_grid_page_count(24, 24) == 1);
  CHECK(winome::compute_app_grid_page_count(25, 24) == 2);

  auto s0 = winome::compute_app_grid_slice(50, -3, 24);
  CHECK(s0.page == 0 && s0.begin == 0 && s0.end == 24 && s0.total_pages == 3);

  auto s_last = winome::compute_app_grid_slice(50, 99, 24);
  CHECK(s_last.page == 2 && s_last.begin == 48 && s_last.end == 50);

  auto d_lg = winome::compute_app_grid_dimensions(1600, 900);
  CHECK(d_lg.cols == 6 && d_lg.rows == 4 && d_lg.page_size == 24);
  auto d_sm = winome::compute_app_grid_dimensions(700, 380);
  CHECK(d_sm.cols == 4 && d_sm.rows == 3 && d_sm.page_size == 12);

  if (gtk_init_check()) {
    GtkWidget* grid = winome::app_grid_new(nullptr, nullptr, nullptr);
    CHECK(grid != nullptr);
    g_object_ref_sink(grid);
    winome::app_grid_set_query(grid, "");
    winome::app_grid_set_query(grid, "win");
    winome::app_grid_set_query(grid, "");
    winome::app_grid_rebuild(grid, 1000, 560);
    // Repeated unchanged rebuild must hit structure cache cleanly.
    winome::app_grid_rebuild(grid, 1000, 560);
    // Guard against GTK4 "min height > natural height" and horizontal/vertical
    // box overflow beyond the 1320x620 App Grid budget.
    winome::app_grid_rebuild(grid, 1320, 620);
    {
      int min_w = 0, nat_w = 0, mw_b = -1, nw_b = -1;
      gtk_widget_measure(grid, GTK_ORIENTATION_HORIZONTAL, -1,
                         &min_w, &nat_w, &mw_b, &nw_b);
      CHECK(nat_w >= min_w);
      CHECK(min_w <= 1320);
      CHECK(nat_w <= 1320);
    }
    for (const char* q : {"", "a", "nonexistent_zzzz"}) {
      winome::app_grid_set_query(grid, q);
      int min_h = 0, nat_h = 0, min_b = -1, nat_b = -1;
      gtk_widget_measure(grid, GTK_ORIENTATION_VERTICAL, 1320,
                         &min_h, &nat_h, &min_b, &nat_b);
      CHECK(nat_h >= min_h);
    }
    winome::app_grid_set_query(grid, "");
    winome::app_grid_next_page(grid);
    winome::app_grid_prev_page(grid);
    winome::app_grid_reset_page(grid);
    winome::app_grid_close_menu(grid);
    g_object_unref(grid);
  }
}

void test_dash_favorites_and_thumbs_summary() {
  int pinned_cnt = 0;
  auto order = winome::build_dash_item_order(
      {"C:/Apps/Foo.exe", "Bar.App!Main"},
      {"bar.app!main", "c:\\apps\\foo.exe", "c:\\apps\\baz.exe"},
      &pinned_cnt);
  CHECK(pinned_cnt == 2);
  CHECK(order.size() == 3);
  CHECK(order[0] == "c:\\apps\\foo.exe");
  CHECK(order[1] == "bar.app!main");
  CHECK(order[2] == "c:\\apps\\baz.exe");

  auto t_idle = winome::compute_thumbs_summary(1, true, false);
  CHECK(!t_idle.visible);

  auto t_drag = winome::compute_thumbs_summary(1, true, true);
  CHECK(t_drag.visible && t_drag.desktop_pills == 1 && !t_drag.has_add_pill);

  auto t_multi = winome::compute_thumbs_summary(2, true, false);
  CHECK(t_multi.visible && t_multi.desktop_pills == 2 && !t_multi.has_add_pill);

  // GNOME dynamic workspaces (windowManager.js WorkspaceTracker._checkWorkspaces):
  // 1) Startup with 1 workspace -> 2 workspaces ([W1, Empty W2])
  auto dw_one_empty = winome::compute_dynamic_workspace_plan({0});
  CHECK(dw_one_empty.append_count == 1 && dw_one_empty.remove_indices.empty() &&
        dw_one_empty.final_count == 2);

  auto dw_one_full = winome::compute_dynamic_workspace_plan({3});
  CHECK(dw_one_full.append_count == 1 && dw_one_full.remove_indices.empty() &&
        dw_one_full.final_count == 2);

  // 2) Workspace 2 has window -> create Workspace 3 ([W1, W2, Empty W3])
  auto dw_two_full = winome::compute_dynamic_workspace_plan({3, 1});
  CHECK(dw_two_full.append_count == 1 && dw_two_full.remove_indices.empty() &&
        dw_two_full.final_count == 3);

  // 3) Workspace 2 has no windows & Workspace 3 has no windows -> Workspace 3 disappears ([W1, Empty W2])
  auto dw_w2_empty_w3_empty = winome::compute_dynamic_workspace_plan({3, 0, 0});
  CHECK(dw_w2_empty_w3_empty.append_count == 0 &&
        dw_w2_empty_w3_empty.remove_indices.size() == 1 &&
        dw_w2_empty_w3_empty.remove_indices[0] == 2 &&
        dw_w2_empty_w3_empty.final_count == 2);

  // 4) Workspace 2 has no windows, but Workspace 3 has windows -> Workspace 2 disappears ([W1, W2(old W3), Empty W3])
  auto dw_w2_empty_w3_full = winome::compute_dynamic_workspace_plan({3, 0, 2});
  CHECK(dw_w2_empty_w3_full.append_count == 1 &&
        dw_w2_empty_w3_full.remove_indices.size() == 1 &&
        dw_w2_empty_w3_full.remove_indices[0] == 1 &&
        dw_w2_empty_w3_full.final_count == 3);

  auto dw_w2_empty_w3_full_w4_empty = winome::compute_dynamic_workspace_plan({3, 0, 2, 0});
  CHECK(dw_w2_empty_w3_full_w4_empty.append_count == 0 &&
        dw_w2_empty_w3_full_w4_empty.remove_indices.size() == 1 &&
        dw_w2_empty_w3_full_w4_empty.remove_indices[0] == 1 &&
        dw_w2_empty_w3_full_w4_empty.final_count == 3);
}

void test_st_engine_theme_metrics() {
  const char* candidates[] = {
    "../src/theme/gnome-shell-dark.css",
    "../WINOME/src/theme/gnome-shell-dark.css",
    "WINOME/src/theme/gnome-shell-dark.css",
    "src/theme/gnome-shell-dark.css",
  };
  const char* css_path = nullptr;
  for (const char* p : candidates) {
    if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) {
      css_path = p;
      break;
    }
  }
  CHECK(css_path != nullptr);
  if (!css_path)
    return;

  winome::StEngine engine;
  CHECK(engine.load_stylesheet(css_path));

  auto check_len = [&](const char* id, const char* cls, const char* prop,
                       double expected) {
    winome::StEngine::Node node(engine, nullptr, id, cls, "");
    double v = -1.0;
    CHECK(node.lookup_length(prop, &v));
    CHECK(std::abs(v - expected) < 0.5);
  };

  check_len("panel", "", "height", 32.0);
  check_len("", "quick-settings-grid", "spacing-rows", 12.0);
  check_len("", "quick-settings-grid", "spacing-columns", 12.0);
  check_len("", "slider", "-barlevel-height", 4.0);
  check_len("", "window-close", "width", 32.0);
  check_len("", "window-close", "height", 32.0);
  check_len("", "workspace-switcher", "spacing", 12.0);

  {
    winome::StEngine::Node node(engine, nullptr, "", "popup-menu-content", "");
    std::string color;
    CHECK(node.lookup_color("background-color", &color));
    CHECK(color == "#36363a");
  }

  // Upstream GNOME 42+ panel popovers float without a BoxPointer arrow triangle:
  // .popup-menu-boxpointer only provides -arrow-rise spacing (no fill/border/base).
  {
    winome::StEngine::Node bp(engine, nullptr, "", "popup-menu-boxpointer", "");
    double rise = -1.0;
    CHECK(bp.lookup_length("-arrow-rise", &rise));
    CHECK(std::abs(rise - 6.0) < 0.5);
    std::string arrow_bg;
    double arrow_bw = -1.0;
    CHECK(!bp.lookup_color("-arrow-background-color", &arrow_bg));
    CHECK(!bp.lookup_length("-arrow-border-width", &arrow_bw));
  }
}

void test_perf_metrics_and_isolated_fontmap() {
  winome::FrameTimeTracker tracker(20000);
  tracker.record_frame_time_us(1000000);
  tracker.record_frame_time_us(1016000);  // 16ms
  tracker.record_frame_time_us(1032000);  // 16ms
  tracker.record_frame_time_us(1062000);  // 30ms -> dropped
  tracker.record_frame_time_us(1500000);  // gap >250ms -> ignored
  tracker.record_frame_time_us(1510000);  // 10ms
  auto fstats = tracker.snapshot();
  CHECK(fstats.frames == 4);
  CHECK(fstats.dropped_frames == 1);
  CHECK(fstats.max_frame_ms >= 29.5 && fstats.max_frame_ms <= 30.5);

  winome::capability_set(winome::SubsystemId::Wlan, winome::CapabilityState::Active, "test-wlan");
  winome::capability_record_failure(winome::SubsystemId::SysTray, "fail-1", 2);
  CHECK(winome::capability_get(winome::SubsystemId::SysTray).state ==
        winome::CapabilityState::Degraded);
  winome::capability_record_failure(winome::SubsystemId::SysTray, "fail-2", 2);
  CHECK(winome::capability_get(winome::SubsystemId::SysTray).state ==
        winome::CapabilityState::Tripped);

  // Verify overview_init phase spelling and expanded startup breakdowns.
  winome::perf_record_phase_ms("overview_init", 42.5);
  winome::perf_record_phase_ms("theming", 3.25);
  winome::perf_record_phase_ms("plugins", 18.0);
  winome::perf_record_phase_ms("recovery", 1.5);
  winome::perf_record_phase_ms("activate", 95.0);
  const auto st = winome::perf_get_startup_timings();
  CHECK(st.overview_init_ms >= 42.4 && st.overview_init_ms <= 42.6);
  CHECK(st.theming_ms >= 3.2 && st.theming_ms <= 3.3);
  CHECK(st.plugins_ms >= 17.9 && st.plugins_ms <= 18.1);
  CHECK(st.recovery_ms >= 1.4 && st.recovery_ms <= 1.6);
  CHECK(st.gtk_activate_ms >= 94.9 && st.gtk_activate_ms <= 95.1);

  auto mem = winome::perf_sample_process_telemetry();
  CHECK(mem.private_bytes > 0);
  CHECK(mem.working_set_bytes > 0);

  // Verify isolated Fontconfig map initializes cleanly, stays lightweight,
  // and resolves ASCII + Simplified/Traditional Chinese + UI symbols without
  // missing glyphs (resources in winome-tests don't bundle gresource TTFs, so
  // check fallback/system CJK + fontset integrity directly).
  PangoFontMap* fmap = winome::bundled_font_map();
  CHECK(fmap != nullptr);
  if (fmap != nullptr) {
    PangoContext* pctx = pango_font_map_create_context(fmap);
    CHECK(pctx != nullptr);
    if (pctx != nullptr) {
      PangoLayout* layout = pango_layout_new(pctx);
      PangoFontDescription* desc =
          pango_font_description_from_string("Cantarell, Sans 11");
      pango_layout_set_font_description(layout, desc);
      pango_layout_set_text(
          layout, "Activities 概览 繁體中文 設定 Quick Settings 12:34", -1);
      CHECK(pango_layout_get_unknown_glyphs_count(layout) == 0);
      pango_font_description_free(desc);
      g_object_unref(layout);
      g_object_unref(pctx);
    }
  }
}

void test_hot_corner_hit_test() {
  const RECT primary{0, 0, 1920, 1080};
  CHECK(winome::hot_corner_hit_test(0, 0, primary, 5));
  CHECK(winome::hot_corner_hit_test(5, 5, primary, 5));
  CHECK(!winome::hot_corner_hit_test(6, 0, primary, 5));
  CHECK(!winome::hot_corner_hit_test(0, 6, primary, 5));
  CHECK(!winome::hot_corner_hit_test(-1, 0, primary, 5));

  // Secondary monitor positioned left of primary still hits its own top-left.
  const RECT left_mon{-1920, 0, 0, 1080};
  CHECK(winome::hot_corner_hit_test(-1920, 0, left_mon, 5));
  CHECK(winome::hot_corner_hit_test(-1915, 5, left_mon, 5));
  CHECK(!winome::hot_corner_hit_test(-1914, 0, left_mon, 5));
  CHECK(!winome::hot_corner_hit_test(0, 0, left_mon, 5));

  const RECT invalid{10, 10, 10, 20};
  CHECK(!winome::hot_corner_hit_test(10, 10, invalid, 5));
  CHECK(!winome::hot_corner_hit_test(0, 0, primary, -1));
}

void test_end_session_formatting() {
  CHECK(winome::end_session_format_title(winome::EndSessionAction::Restart, false) ==
        "Restart");
  CHECK(winome::end_session_format_title(winome::EndSessionAction::PowerOff, false) ==
        "Power Off");
  CHECK(winome::end_session_format_title(winome::EndSessionAction::LogOut, false) ==
        "Log Out");
  CHECK(winome::end_session_format_title(winome::EndSessionAction::Restart, true) ==
        "重启");
  CHECK(winome::end_session_format_title(winome::EndSessionAction::PowerOff, true) ==
        "关机");
  CHECK(winome::end_session_format_title(winome::EndSessionAction::LogOut, true) ==
        "注销");

  CHECK(winome::end_session_confirm_label(winome::EndSessionAction::PowerOff, false) ==
        "Power Off");

  const std::string en_60 = winome::end_session_format_description(
      winome::EndSessionAction::PowerOff, 60, false);
  CHECK(en_60.find("60 seconds.") != std::string::npos);

  const std::string en_1 = winome::end_session_format_description(
      winome::EndSessionAction::Restart, 1, false);
  CHECK(en_1.find("1 second.") != std::string::npos);

  const std::string en_neg = winome::end_session_format_description(
      winome::EndSessionAction::LogOut, -5, false);
  CHECK(en_neg.find("0 seconds.") != std::string::npos);

  const std::string zh_desc = winome::end_session_format_description(
      winome::EndSessionAction::PowerOff, 12, true);
  CHECK(zh_desc.find("12 秒") != std::string::npos);
}

void test_window_theming_hwnd_filter() {
  CHECK(winome::should_apply_dwm_theming_to_hwnd_info(
      L"Notepad", WS_OVERLAPPEDWINDOW, 0, true, false));
  CHECK(!winome::should_apply_dwm_theming_to_hwnd_info(
      L"Notepad", WS_OVERLAPPEDWINDOW, 0, false, false));
  CHECK(!winome::should_apply_dwm_theming_to_hwnd_info(
      L"Notepad", WS_OVERLAPPEDWINDOW, 0, true, true));
  CHECK(!winome::should_apply_dwm_theming_to_hwnd_info(
      L"Notepad", WS_CHILD | WS_CAPTION, 0, true, false));
  CHECK(!winome::should_apply_dwm_theming_to_hwnd_info(
      L"Notepad", WS_OVERLAPPEDWINDOW, WS_EX_TOOLWINDOW, true, false));
  CHECK(!winome::should_apply_dwm_theming_to_hwnd_info(
      L"#32768", WS_POPUP | WS_CAPTION, 0, true, false));
  CHECK(!winome::should_apply_dwm_theming_to_hwnd_info(
      L"Shell_TrayWnd", WS_OVERLAPPEDWINDOW, 0, true, false));
  CHECK(!winome::should_apply_dwm_theming_to_hwnd_info(
      L"WINOME_Panel", WS_OVERLAPPEDWINDOW, 0, true, false));
  CHECK(!winome::should_apply_dwm_theming_to_hwnd_info(
      L"gdkSurfaceToplevel", WS_OVERLAPPEDWINDOW, 0, true, false));
  CHECK(!winome::should_apply_dwm_theming_to_hwnd_info(
      nullptr, WS_OVERLAPPEDWINDOW, 0, true, false));
}

struct IpcTestSink {
  int ov_open = 0;
  int ov_close = 0;
  int ov_toggle = 0;
  int ov_grid = 0;
  int cfg_reload = 0;
  int switch_arg = -1;
  int quit_calls = 0;
  bool is_open = false;
  std::string settings_page;
  std::string notify_title;
  std::string notify_body;
  std::string notify_app;
};

IpcTestSink g_ipc_sink;

void test_ipc_dispatch_and_named_pipe_roundtrip() {
  g_ipc_sink = IpcTestSink{};
  g_ipc_sink.is_open = true;

  winome::IpcCallbacks cbs{};
  cbs.overview_open = +[]() { ++g_ipc_sink.ov_open; };
  cbs.overview_close = +[]() { ++g_ipc_sink.ov_close; };
  cbs.overview_toggle = +[]() { ++g_ipc_sink.ov_toggle; };
  cbs.overview_app_grid = +[]() { ++g_ipc_sink.ov_grid; };
  cbs.overview_is_open = +[]() { return g_ipc_sink.is_open; };
  cbs.settings_open = +[](const std::string& p) { g_ipc_sink.settings_page = p; };
  cbs.config_reload = +[]() { ++g_ipc_sink.cfg_reload; };
  cbs.desktop_switch = +[](int idx) {
    g_ipc_sink.switch_arg = idx;
    return idx >= 0 && idx < 4;
  };
  cbs.desktop_list = +[]() -> std::vector<winome::vd::DesktopInfo> {
    return {{0, GUID{}}, {1, GUID{}}};
  };
  cbs.desktop_active = +[]() { return 1; };
  cbs.notify = +[](const std::string& t, const std::string& b, const std::string& a) {
    g_ipc_sink.notify_title = t;
    g_ipc_sink.notify_body = b;
    g_ipc_sink.notify_app = a;
  };
  cbs.quit = +[]() { ++g_ipc_sink.quit_calls; };

  const std::string ping = winome::ipc_dispatch_request_json("{\"cmd\":\"ping\"}", cbs);
  CHECK(ping.find("\"ok\":true") != std::string::npos);
  CHECK(ping.find("\"pong\":true") != std::string::npos);
  CHECK(ping.find(std::string("\"version\":\"") + WINOME_VERSION + "\"") != std::string::npos);
  CHECK(std::string(WINOME_VERSION) == "0.2.1");

  const std::string st = winome::ipc_dispatch_request_json("{\"cmd\":\"status\"}", cbs);
  CHECK(st.find("\"ok\":true") != std::string::npos);
  CHECK(st.find(std::string("\"version\":\"") + WINOME_VERSION + "\"") != std::string::npos);
  CHECK(st.find("\"overview_open\":true") != std::string::npos);
  CHECK(st.find("\"capabilities\":[") != std::string::npos);
  CHECK(st.find("\"telemetry\":{") != std::string::npos);
  CHECK(st.find("\"startup\":{") != std::string::npos);

  CHECK(winome::ipc_dispatch_request_json("{\"cmd\":\"overview.open\"}", cbs)
            .find("\"ok\":true") != std::string::npos);
  CHECK(winome::ipc_dispatch_request_json("{\"cmd\":\"overview.close\"}", cbs)
            .find("\"ok\":true") != std::string::npos);
  CHECK(winome::ipc_dispatch_request_json("{\"cmd\":\"overview.toggle\"}", cbs)
            .find("\"ok\":true") != std::string::npos);
  CHECK(winome::ipc_dispatch_request_json("{\"cmd\":\"overview.app_grid\"}", cbs)
            .find("\"ok\":true") != std::string::npos);
  CHECK(g_ipc_sink.ov_open == 1 && g_ipc_sink.ov_close == 1 &&
        g_ipc_sink.ov_toggle == 1 && g_ipc_sink.ov_grid == 1);

  CHECK(winome::ipc_dispatch_request_json(
            "{\"cmd\":\"settings.open\",\"page\":\"diagnostics\"}", cbs)
            .find("\"ok\":true") != std::string::npos);
  CHECK(g_ipc_sink.settings_page == "diagnostics");

  const std::string cfg_get = winome::ipc_dispatch_request_json("{\"cmd\":\"config.get\"}", cbs);
  CHECK(cfg_get.find("\"ok\":true") != std::string::npos);
  CHECK(cfg_get.find("\"end_session_confirm\"") != std::string::npos);
  CHECK(cfg_get.find("\"overview_window_blacklist\"") != std::string::npos);
  CHECK(winome::ipc_dispatch_request_json("{\"cmd\":\"config.reload\"}", cbs)
            .find("\"ok\":true") != std::string::npos);
  CHECK(g_ipc_sink.cfg_reload == 1);

  const std::string dlist = winome::ipc_dispatch_request_json("{\"cmd\":\"desktop.list\"}", cbs);
  CHECK(dlist.find("\"count\":2") != std::string::npos);
  CHECK(dlist.find("\"active\":1") != std::string::npos);
  CHECK(winome::ipc_dispatch_request_json(
            "{\"cmd\":\"desktop.switch\",\"index\":2}", cbs)
            .find("\"ok\":true") != std::string::npos);
  CHECK(g_ipc_sink.switch_arg == 2);
  CHECK(winome::ipc_dispatch_request_json(
            "{\"cmd\":\"desktop.switch\",\"index\":-1}", cbs)
            .find("\"ok\":false") != std::string::npos);

  CHECK(winome::ipc_dispatch_request_json(
            "{\"cmd\":\"notify\",\"title\":\"Hi\",\"body\":\"World\",\"app\":\"CLI\"}",
            cbs)
            .find("\"ok\":true") != std::string::npos);
  CHECK(g_ipc_sink.notify_title == "Hi");
  CHECK(g_ipc_sink.notify_body == "World");
  CHECK(g_ipc_sink.notify_app == "CLI");
  CHECK(winome::ipc_dispatch_request_json("{\"cmd\":\"notify\"}", cbs)
            .find("\"ok\":false") != std::string::npos);

  CHECK(winome::ipc_dispatch_request_json("not json", cbs)
            .find("\"ok\":false") != std::string::npos);
  CHECK(winome::ipc_dispatch_request_json("{\"cmd\":\"nonexistent\"}", cbs)
            .find("\"unknown command\"") != std::string::npos);

  // Exercise live Named Pipe server + rapid back-to-back client roundtrips.
  CHECK(winome::ipc_server_start(cbs));
  std::string pipe_reply;
  CHECK(winome::ipc_query_running_shell("{\"cmd\":\"ping\"}", &pipe_reply, 1000));
  CHECK(pipe_reply.find("\"pong\":true") != std::string::npos);

  for (int i = 0; i < 5; ++i) {
    std::string pipe_status;
    CHECK(winome::ipc_query_running_shell("{\"cmd\":\"status\"}", &pipe_status, 1000));
    CHECK(pipe_status.find("\"capabilities\":[") != std::string::npos);
  }
  winome::ipc_server_stop();
}

void test_vd_vtable_profiles_and_cache_trim() {
  const auto p23 = winome::vd::vtable_slots_for_build(22621);
  CHECK(p23.can_manage_desktops);

  const auto p24 = winome::vd::vtable_slots_for_build(26100);
  CHECK(p24.can_manage_desktops);

  const auto p_old = winome::vd::vtable_slots_for_build(19045);
  CHECK(!p_old.can_manage_desktops);

  int cb_hits = 0;
  winome::vd::DesktopChangedCallback dummy_cb = [](int, int, void* ud) {
    ++(*static_cast<int*>(ud));
  };
  winome::vd::add_change_listener(dummy_cb, &cb_hits);
  winome::vd::remove_change_listener(dummy_cb, &cb_hits);

  // Ensure cache trimming handles zero/bounds safely when cache is empty.
  winome::app_icon_cache_trim(16);
  winome::app_icon_cache_trim(0);
  winome::app_icon_cache_clear();
}

void test_crash_recovery_snapshot_and_state() {
  // Config safe mode + developer_mode + disabled_plugins serialization check.
  CHECK(!winome::is_safe_mode());
  winome::set_safe_mode(true);
  CHECK(winome::is_safe_mode());
  winome::set_safe_mode(false);
  CHECK(!winome::is_safe_mode());

  winome::WinomeConfig cfg;
  cfg.developer_mode = true;
  cfg.disabled_plugins = {"com.example.cpu", "sample-weather"};
  const std::string cjson = winome::serialize_config(cfg);
  winome::WinomeConfig cback;
  CHECK(winome::parse_config(cjson, &cback));
  CHECK(cback.developer_mode == true);
  CHECK(cback.disabled_plugins == cfg.disabled_plugins);

  // Versioned JSON recovery snapshot roundtrip.
  winome::RecoverySnapshot snap;
  snap.valid = true;
  snap.taskbar_state = 1;  // ABS_AUTOHIDE
  snap.primary_work = RECT{0, 0, 2560, 1400};
  const std::string sraw = winome::serialize_recovery_snapshot(snap);
  winome::RecoverySnapshot sout;
  CHECK(winome::parse_recovery_snapshot(sraw, &sout));
  CHECK(sout.valid);
  CHECK(sout.taskbar_state == 1);
  CHECK(sout.primary_work.right == 2560 && sout.primary_work.bottom == 1400);

  // Legacy raw 16-byte RECT backward compatibility.
  RECT legacy_rc{10, 20, 1920, 1040};
  std::string legacy_raw(reinterpret_cast<const char*>(&legacy_rc),
                         sizeof(legacy_rc));
  winome::RecoverySnapshot lout;
  CHECK(winome::parse_recovery_snapshot(legacy_raw, &lout));
  CHECK(lout.valid);
  CHECK(lout.primary_work.left == 10 && lout.primary_work.bottom == 1040);

  // Invalid recovery payload cleanly rejected.
  winome::RecoverySnapshot bad_snap;
  CHECK(!winome::parse_recovery_snapshot("corrupt", &bad_snap));

  // CrashState roundtrip & emergency raw writer + log tail reader.
  winome::CrashState cs;
  cs.reason = "CRT Fatal Signal (22)";
  cs.exit_code = 150;
  cs.pid = 4321;
  cs.timestamp_utc = "2026-11-16T13:10:00Z";
  cs.minidump_path = "C:\\Users\\Test\\crashes\\dump.dmp";
  cs.log_tail = "line1\nline2\n";
  cs.taskbar_restored = true;
  cs.safe_mode = true;
  const std::string cs_json = winome::serialize_crash_state(cs);
  winome::CrashState cs_out;
  CHECK(winome::parse_crash_state(cs_json, &cs_out));
  CHECK(cs_out.reason == cs.reason);
  CHECK(cs_out.exit_code == 150);
  CHECK(cs_out.pid == 4321);
  CHECK(cs_out.taskbar_restored);
  CHECK(cs_out.safe_mode);

  wchar_t tmp_dir[MAX_PATH] = {};
  if (GetTempPathW(MAX_PATH, tmp_dir) > 0) {
    const std::wstring crash_file = std::wstring(tmp_dir) + L"winome_test_crash.json";
    winome::write_crash_state_emergency_file(
        crash_file.c_str(), "Win32 Unhandled Exception \"0xC0000005\"",
        0xC0000005u, 9999u, "C:\\dumps\\test.dmp", true);
    HANDLE h = CreateFileW(crash_file.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) {
      char buf[2048] = {};
      DWORD got = 0;
      CHECK(ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr));
      CloseHandle(h);
      winome::CrashState em;
      CHECK(winome::parse_crash_state(std::string(buf, got), &em));
      CHECK(em.exit_code == 0xC0000005u);
      CHECK(em.pid == 9999u);
      CHECK(em.safe_mode);
      CHECK(em.minidump_path == "C:\\dumps\\test.dmp");
    }
    DeleteFileW(crash_file.c_str());

    const std::wstring log_file = std::wstring(tmp_dir) + L"winome_test_tail.log";
    HANDLE lf = CreateFileW(log_file.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lf != INVALID_HANDLE_VALUE) {
      std::string lines;
      for (int i = 0; i < 40; ++i)
        lines += "entry line " + std::to_string(i) + "\n";
      DWORD w = 0;
      WriteFile(lf, lines.data(), static_cast<DWORD>(lines.size()), &w, nullptr);
      CloseHandle(lf);
      const std::string tail = winome::read_log_tail_utf8(log_file, 120);
      CHECK(!tail.empty());
      CHECK(tail.find("entry line 39") != std::string::npos);
      CHECK(tail.find("entry line 0\n") == std::string::npos);
      DeleteFileW(log_file.c_str());
    }
  }
}

void test_debug_runtime_ipc_sddl_and_ctl() {
  // Verify log ring buffer capture, filtering, and seq monotonicity.
  winome::debug_clear_logs();
  const uint64_t base_seq = winome::debug_latest_log_seq();
  winome::debug_append_log(nullptr, "[vd] snapshot changed count=3");
  winome::debug_append_log("perf", "frame time 4.2ms");
  winome::debug_append_log("ipc", "ping handled");
  CHECK(winome::debug_latest_log_seq() == base_seq + 3);
  CHECK(winome::debug_extract_log_tag("[vd] hello") == "vd");
  CHECK(winome::debug_extract_log_tag("plain message") == "misc");

  // Verify timestamp prefixing and consecutive duplicate folding in LogRepeatFolder.
  {
    winome::LogRepeatFolder folder;
    std::vector<std::string> out;
    const std::string ts = "[2026-11-21 23:15:00.000] ";
    CHECK(!winome::LogRepeatFolder::has_existing_timestamp("[workarea] applied"));
    CHECK(winome::LogRepeatFolder::has_existing_timestamp(ts + "[workarea] applied"));
    CHECK(winome::LogRepeatFolder::has_existing_timestamp(
        "(winome.exe:123): Gtk-WARNING **: 02:49:56.424: msg"));

    folder.push("[workarea] applied", ts, &out);
    folder.push("[workarea] applied", ts, &out);
    folder.push("[workarea] applied", ts, &out);
    CHECK(out.size() == 1);
    CHECK(out[0] == ts + "[workarea] applied");

    folder.push("[ov] hide", ts, &out);
    CHECK(out.size() == 3);
    CHECK(out[1] == ts + "[log] previous line repeated 2 times");
    CHECK(out[2] == ts + "[ov] hide");

    folder.flush(ts, &out);
    CHECK(out.size() == 3);

    // Reaching kMaxFoldStreak emits a rolling summary automatically.
    folder.push("spam", ts, &out);
    for (size_t i = 0; i < winome::LogRepeatFolder::kMaxFoldStreak; ++i)
      folder.push("spam", ts, &out);
    CHECK(out.back() ==
          ts + "[log] previous line repeated " +
              std::to_string(winome::LogRepeatFolder::kMaxFoldStreak) + " times");
  }

  const auto vd_logs = winome::debug_get_logs_since(base_seq, "vd", 10);
  CHECK(vd_logs.size() == 1);
  CHECK(vd_logs[0].tag == "vd");
  const auto all_logs = winome::debug_get_logs_since(base_seq, "all", 10);
  CHECK(all_logs.size() == 3);

  // Verify debug flags and IPC commands for runtime control & fault injection.
  CHECK(winome::debug_set_flag_by_name("ipc_trace", true));
  CHECK(!winome::debug_set_flag_by_name("nonexistent_flag", true));
  CHECK(winome::debug_get_flags().ipc_trace);
  winome::debug_set_flag_by_name("ipc_trace", false);

  winome::IpcCallbacks cbs{};
  CHECK(winome::ipc_dispatch_request_json("{\"cmd\":\"debug.flags.get\"}", cbs)
            .find("\"zorder_trace\"") != std::string::npos);
  CHECK(winome::ipc_dispatch_request_json(
            "{\"cmd\":\"debug.flags.set\",\"name\":\"zorder_trace\",\"value\":true}", cbs)
            .find("\"zorder_trace\":true") != std::string::npos);
  CHECK(winome::debug_get_flags().zorder_trace);
  winome::debug_set_flag_by_name("zorder_trace", false);

  const std::string tail_resp = winome::ipc_dispatch_request_json(
      "{\"cmd\":\"logs.tail\",\"tag\":\"perf\",\"count\":5}", cbs);
  CHECK(tail_resp.find("frame time 4.2ms") != std::string::npos);
  CHECK(tail_resp.find("snapshot changed") == std::string::npos);

  const std::string trip_resp = winome::ipc_dispatch_request_json(
      "{\"cmd\":\"subsystem.trip\",\"name\":\"SysTray\"}", cbs);
  CHECK(trip_resp.find("\"state\":\"tripped\"") != std::string::npos);
  const std::string rec_resp = winome::ipc_dispatch_request_json(
      "{\"cmd\":\"subsystem.recover\",\"name\":\"SysTray\"}", cbs);
  CHECK(rec_resp.find("\"state\":\"active\"") != std::string::npos);

  CHECK(winome::ipc_dispatch_request_json(
            "{\"cmd\":\"config.set\",\"name\":\"developer_mode\",\"value\":true}", cbs)
            .find("\"ok\":true") != std::string::npos);
  CHECK(winome::get_config().developer_mode);

  // When developer_mode == false and no callback overrides, looking_glass.open refuses cleanly.
  CHECK(winome::ipc_dispatch_request_json(
            "{\"cmd\":\"looking_glass.open\"}", cbs)
            .find("\"ok\":false") != std::string::npos);

  // Verify SDDL string for current-user + Medium IL pipe access.
  const std::wstring sddl = winome::ipc_build_user_pipe_sddl(L"S-1-5-21-1000-2000-3000-1001");
  CHECK(sddl.find(L"(A;;GA;;;S-1-5-21-1000-2000-3000-1001)") != std::wstring::npos);
  CHECK(sddl.find(L"S:(ML;;NW;;;ME)") != std::wstring::npos);

  // Verify winome-ctl CLI-to-JSON translation.
  std::string req;
  std::string err;
  CHECK(winome::ctl::ctl_build_request_json({"ping"}, &req, &err));
  CHECK(req == "{\"cmd\":\"ping\"}");
  CHECK(winome::ctl::ctl_build_request_json({"overview", "grid"}, &req, &err));
  CHECK(req == "{\"cmd\":\"overview.app_grid\"}");
  CHECK(winome::ctl::ctl_build_request_json({"desktop", "switch", "2"}, &req, &err));
  CHECK(req.find("\"desktop.switch\"") != std::string::npos &&
        req.find("\"index\":2") != std::string::npos);
  CHECK(winome::ctl::ctl_build_request_json({"flags", "set", "ipc_trace", "on"}, &req, &err));
  CHECK(req.find("\"debug.flags.set\"") != std::string::npos &&
        req.find("\"value\":true") != std::string::npos);
  CHECK(winome::ctl::ctl_build_request_json({"subsystem", "trip", "Wlan"}, &req, &err));
  CHECK(req.find("\"subsystem.trip\"") != std::string::npos);
  CHECK(winome::ctl::ctl_build_request_json({"plugins", "restart", "demo"}, &req, &err));
  CHECK(req.find("\"plugin.restart\"") != std::string::npos);
  CHECK(winome::ctl::ctl_build_request_json({"run", "notepad.exe", "a.txt"}, &req, &err));
  CHECK(req.find("\"run.exec\"") != std::string::npos &&
        req.find("notepad.exe a.txt") != std::string::npos);
  CHECK(!winome::ctl::ctl_build_request_json({"nonexistent"}, &req, &err));
}

void test_run_dialog_helpers() {
  winome::RunCommandParts parts;
  CHECK(winome::split_run_command("notepad.exe C:\\temp\\foo.txt", &parts));
  CHECK(parts.executable == "notepad.exe");
  CHECK(parts.arguments == "C:\\temp\\foo.txt");

  CHECK(winome::split_run_command(
      "  \"C:\\Program Files\\My App\\app.exe\" --arg 1  ", &parts));
  CHECK(parts.executable == "C:\\Program Files\\My App\\app.exe");
  CHECK(parts.arguments == "--arg 1");

  CHECK(winome::split_run_command("ms-settings:display", &parts));
  CHECK(parts.executable == "ms-settings:display");
  CHECK(parts.arguments.empty());
  CHECK(!winome::split_run_command("   ", &parts));

  // Internal commands must only trigger when developer_mode == true.
  CHECK(winome::classify_run_internal_command("lg", false) ==
        winome::RunInternalAction::None);
  CHECK(winome::classify_run_internal_command("rt", false) ==
        winome::RunInternalAction::None);
  CHECK(winome::classify_run_internal_command("r", false) ==
        winome::RunInternalAction::None);

  CHECK(winome::classify_run_internal_command("lg", true) ==
        winome::RunInternalAction::OpenLookingGlass);
  CHECK(winome::classify_run_internal_command("rt", true) ==
        winome::RunInternalAction::ReloadTheme);
  CHECK(winome::classify_run_internal_command("r", true) ==
        winome::RunInternalAction::RestartShell);
  CHECK(winome::classify_run_internal_command("debugexit", true) ==
        winome::RunInternalAction::DebugExit);

  // History deduplication & max cap.
  std::vector<std::string> hist;
  hist = winome::run_history_add_item(std::move(hist), "cmd", 3);
  hist = winome::run_history_add_item(std::move(hist), "notepad", 3);
  hist = winome::run_history_add_item(std::move(hist), "calc", 3);
  hist = winome::run_history_add_item(std::move(hist), "cmd", 3);
  CHECK(hist.size() == 3);
  CHECK(hist[0] == "notepad" && hist[2] == "cmd");
  hist = winome::run_history_add_item(std::move(hist), "mspaint", 3);
  CHECK(hist.size() == 3);
  CHECK(hist[0] == "calc" && hist[2] == "mspaint");

  // Prefix common completion.
  CHECK(winome::run_complete_prefix(
            "note", {"notepad.exe", "notepad++.exe"}) == "pad");
  CHECK(winome::run_complete_prefix(
            "calc", {"notepad.exe", "calc.exe"}) == ".exe");
  CHECK(winome::run_complete_prefix("zzz", {"notepad.exe"}).empty());

  // Internal command dispatch executes clean callback when developer_mode == true.
  winome::WinomeConfig prev_cfg = winome::get_config();
  winome::WinomeConfig dev_cfg = prev_cfg;
  dev_cfg.developer_mode = true;
  winome::update_config(dev_cfg);

  static bool s_lg_opened = false;
  static bool s_theme_reloaded = false;
  s_lg_opened = false;
  s_theme_reloaded = false;
  winome::RunDialogCallbacks rcbs{};
  rcbs.open_looking_glass = +[]() {
    s_lg_opened = true;
    return true;
  };
  rcbs.reload_theme = +[]() {
    s_theme_reloaded = true;
    return true;
  };
  std::string err;
  CHECK(winome::execute_run_command_string("lg", false, rcbs, &err));
  CHECK(s_lg_opened);
  CHECK(winome::execute_run_command_string("rt", false, rcbs, &err));
  CHECK(s_theme_reloaded);

  winome::update_config(prev_cfg);
}

void test_plugin_host_and_real_child_process() {
  // 1. Manifest parsing & capability validation.
  {
    winome::PluginManifest m;
    std::string err;
    const std::string valid_json =
        "{\"id\":\"sysmon\",\"name\":\"Sys \\u4e2d\","
        "\"version\":\"1.2.3\",\"exec\":\"python.exe\","
        "\"args\":[\"-u\",\"run.py\"],"
        "\"capabilities\":[\"topbar\",\"desktop.read\"],"
        "\"max_memory_mb\":2048,\"enabled\":true}";
    CHECK(winome::parse_plugin_manifest(valid_json, L"C:\\plugins\\sysmon", &m, &err));
    CHECK(m.id == "sysmon");
    CHECK(m.name == "Sys \xE4\xB8\xAD");
    CHECK(m.version == "1.2.3");
    CHECK(m.exec == "python.exe");
    CHECK(m.args.size() == 2 && m.args[0] == "-u" && m.args[1] == "run.py");
    CHECK(m.max_memory_mb == 1024);  // clamped [8, 1024]
    CHECK(winome::plugin_has_capability(m, "topbar"));
    CHECK(winome::plugin_has_capability(m, "desktop.read"));
    CHECK(!winome::plugin_has_capability(m, "notify"));

    winome::PluginManifest bad;
    CHECK(!winome::parse_plugin_manifest("{\"id\":\"../evil\",\"exec\":\"a.exe\"}",
                                         L"C:\\p", &bad, &err));
    CHECK(!winome::parse_plugin_manifest("{\"id\":\"ok\",\"exec\":\"..\\\\evil.exe\"}",
                                         L"C:\\p", &bad, &err));
  }

  // 2. Rate limiter & crash-loop circuit breaker.
  {
    winome::PluginRateLimiter rl(30, 1000);
    for (int i = 0; i < 30; ++i)
      CHECK(rl.allow(1000 + i));
    CHECK(!rl.allow(1500));
    CHECK(rl.dropped_count() == 1);
    CHECK(rl.allow(2000));

    winome::PluginCrashTracker ct(3, 60000);
    CHECK(!ct.record_crash(1000));
    CHECK(!ct.record_crash(2000));
    // First two expired outside 60s window at t=70000.
    CHECK(!ct.record_crash(70000));
    CHECK(!ct.record_crash(71000));
    CHECK(ct.record_crash(72000));
  }

  // 3. Real child-process lifecycle, Win32 Job Object, JSON stdio, ACL & breaker.
  wchar_t mod_buf[MAX_PATH] = {};
  DWORD mod_len = GetModuleFileNameW(nullptr, mod_buf, MAX_PATH);
  CHECK(mod_len > 0 && mod_len < MAX_PATH);
  std::wstring exe_dir(mod_buf);
  size_t slash = exe_dir.find_last_of(L"\\/");
  exe_dir = (slash == std::wstring::npos) ? L"." : exe_dir.substr(0, slash);
  const std::wstring mock_exe = exe_dir + L"\\winome-mock-plugin.exe";
  CHECK(GetFileAttributesW(mock_exe.c_str()) != INVALID_FILE_ATTRIBUTES);

  int u8_len = WideCharToMultiByte(CP_UTF8, 0, mock_exe.c_str(), -1,
                                   nullptr, 0, nullptr, nullptr);
  std::string mock_utf8(static_cast<size_t>(std::max(0, u8_len - 1)), '\0');
  WideCharToMultiByte(CP_UTF8, 0, mock_exe.c_str(), -1,
                      &mock_utf8[0], u8_len, nullptr, nullptr);
  std::string mock_json_path;
  for (char c : mock_utf8) {
    if (c == '\\') mock_json_path += "\\\\";
    else mock_json_path.push_back(c);
  }

  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring pdir =
      std::wstring(tmp) + L"winome_plugins_test_" + std::to_wstring(GetCurrentProcessId());
  const std::wstring good_dir = pdir + L"\\good";
  const std::wstring crash_dir = pdir + L"\\crasher";
  CreateDirectoryW(pdir.c_str(), nullptr);
  CreateDirectoryW(good_dir.c_str(), nullptr);
  CreateDirectoryW(crash_dir.c_str(), nullptr);

  auto write_text_file = [](const std::wstring& path, const std::string& content) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
      DWORD wr = 0;
      WriteFile(f, content.data(), static_cast<DWORD>(content.size()), &wr, nullptr);
      CloseHandle(f);
    }
  };

  // Note: "good" intentionally lacks "notify" capability.
  write_text_file(
      good_dir + L"\\manifest.json",
      "{\"id\":\"good\",\"name\":\"Good Plugin\",\"version\":\"1.0.0\","
      "\"exec\":\"" + mock_json_path + "\",\"args\":[\"normal\"],"
      "\"capabilities\":[\"topbar\",\"quicksettings\",\"desktop.read\"],"
      "\"max_memory_mb\":64,\"enabled\":true}");
  write_text_file(
      crash_dir + L"\\manifest.json",
      "{\"id\":\"crasher\",\"name\":\"Crasher Plugin\",\"version\":\"0.1.0\","
      "\"exec\":\"" + mock_json_path + "\",\"args\":[\"crash\"],"
      "\"capabilities\":[\"topbar\"],\"max_memory_mb\":64,\"enabled\":true}");

  SetEnvironmentVariableW(L"WINOME_PLUGINS_DIR", pdir.c_str());

  static std::string s_tb_text;
  static int s_tb_removed = 0;
  static std::string s_qs_sub;
  static bool s_qs_active = false;
  static int s_qs_removed = 0;
  static std::vector<std::string> s_notify_titles;
  s_tb_text.clear();
  s_tb_removed = 0;
  s_qs_sub.clear();
  s_qs_active = false;
  s_qs_removed = 0;
  s_notify_titles.clear();

  winome::debug_clear_logs();
  winome::PluginHostCallbacks cbs{};
  cbs.topbar_upsert = +[](const winome::PluginTopBarSpec& spec) {
    if (spec.key == "good:cpu")
      s_tb_text = spec.text;
  };
  cbs.topbar_remove = +[](const std::string& pid, const std::string& key) {
    if (pid == "good" && key == "good:cpu")
      ++s_tb_removed;
  };
  cbs.quicktoggle_upsert = +[](const winome::PluginQuickToggleSpec& spec) {
    if (spec.key == "good:turbo") {
      s_qs_sub = spec.subtitle;
      s_qs_active = spec.active;
    }
  };
  cbs.quicktoggle_remove = +[](const std::string& pid, const std::string& key) {
    if (pid == "good" && key == "good:turbo")
      ++s_qs_removed;
  };
  cbs.notify = +[](const std::string& title, const std::string&, const std::string&) {
    s_notify_titles.push_back(title);
  };

  auto pump_until = [](auto pred, int max_ms) -> bool {
    const ULONGLONG t0 = GetTickCount64();
    while (!pred()) {
      while (g_main_context_pending(nullptr))
        g_main_context_iteration(nullptr, FALSE);
      if (pred())
        return true;
      if (GetTickCount64() - t0 >= static_cast<ULONGLONG>(max_ms))
        return false;
      Sleep(15);
    }
    return true;
  };

  winome::plugin_host_init(cbs);

  // Wait for good plugin startup & UI slots (\u4e2d decoded to UTF-8).
  CHECK(pump_until([&]() {
    return s_tb_text == "42%\xE4\xB8\xAD" && s_qs_active && s_qs_sub == "On";
  }, 3000));

  // Wait briefly for crasher to hit 3 crashes in 60s -> Tripped circuit breaker & idle notify.
  CHECK(pump_until([&]() {
    const std::string lj = winome::plugin_host_list_json();
    return lj.find("\"id\":\"crasher\"") != std::string::npos &&
           lj.find("\"state\":\"Tripped\"") != std::string::npos &&
           !s_notify_titles.empty();
  }, 6000));

  // Capability denial: "Denied" from good MUST NOT trigger notify callback, while
  // breaker tripped notification SHOULD appear.
  bool saw_denied = false;
  bool saw_tripped_notify = false;
  for (const auto& t : s_notify_titles) {
    if (t == "Denied") saw_denied = true;
    if (t.find("Tripped") != std::string::npos ||
        t.find("\xE7\x86\x94\xE6\x96\xAD") != std::string::npos) {
      saw_tripped_notify = true;
    }
  }
  CHECK(!saw_denied);
  CHECK(saw_tripped_notify);

  // Bidirectional events: click TopBar, toggle QuickSettings, notify desktop change.
  winome::plugin_host_emit_topbar_click("good", "cpu");
  CHECK(pump_until([&]() { return s_tb_text == "CLICKED"; }, 2000));

  winome::plugin_host_emit_quicktoggle("good", "turbo", false);
  CHECK(pump_until([&]() { return !s_qs_active && s_qs_sub == "Off"; }, 2000));

  winome::plugin_host_emit_desktop_changed(3, 1);
  CHECK(pump_until([&]() {
    for (const auto& e : winome::debug_get_logs_since(0, "plugin:good", 200)) {
      if (e.tag == "plugin:good" &&
          e.text.find("mock saw desktop.changed") != std::string::npos) {
        return true;
      }
    }
    return false;
  }, 2000));

  std::string err;
  CHECK(winome::plugin_host_action("ping", "good", &err));

  // Verify stderr forwarding and capability denial logged under plugin:good.
  bool saw_stderr = false;
  bool saw_cap_log = false;
  for (const auto& e : winome::debug_get_logs_since(0, "plugin:good", 200)) {
    if (e.tag == "plugin:good") {
      if (e.text.find("mock stderr hello") != std::string::npos) saw_stderr = true;
      if (e.text.find("capability denied") != std::string::npos) saw_cap_log = true;
    }
  }
  CHECK(saw_stderr);
  CHECK(saw_cap_log);

  // Verify disable clears UI slots and persists disabled_plugins, enable restores.
  CHECK(winome::plugin_host_action("disable", "good", &err));
  CHECK(pump_until([&]() { return s_tb_removed >= 1 && s_qs_removed >= 1; }, 2000));
  const auto dis_list = winome::get_config().disabled_plugins;
  CHECK(std::find(dis_list.begin(), dis_list.end(), "good") != dis_list.end());

  s_tb_text.clear();
  CHECK(winome::plugin_host_action("enable", "good", &err));
  CHECK(pump_until([&]() { return s_tb_text == "42%\xE4\xB8\xAD"; }, 3000));

  winome::plugin_host_shutdown();

  // Cleanup temp manifests & restore clean config state.
  winome::WinomeConfig clean_cfg = winome::get_config();
  clean_cfg.disabled_plugins.clear();
  winome::update_config(clean_cfg);
  SetEnvironmentVariableW(L"WINOME_PLUGINS_DIR", nullptr);
  DeleteFileW((good_dir + L"\\manifest.json").c_str());
  DeleteFileW((crash_dir + L"\\manifest.json").c_str());
  RemoveDirectoryW(good_dir.c_str());
  RemoveDirectoryW(crash_dir.c_str());
  RemoveDirectoryW(pdir.c_str());
}

void test_vd_async_and_breaker_recovery() {
  winome::vd::pin_window_async(nullptr);
  winome::capability_set(winome::SubsystemId::VirtualDesktop,
                         winome::CapabilityState::Tripped, "simulated stall");
  winome::vd::reset_circuit_breaker();
  const auto snap = winome::capability_snapshot_all();
  bool found_active = false;
  for (const auto& c : snap) {
    if (c.id == winome::SubsystemId::VirtualDesktop &&
        c.state == winome::CapabilityState::Active) {
      found_active = true;
    }
  }
  CHECK(found_active);

  // VdReconnectPolicy: exponential backoff on transient Explorer COM failures,
  // clean reset on recovery / TaskbarCreated.
  winome::vd::VdReconnectPolicy policy;
  CHECK(policy.can_attempt(1000));
  CHECK(policy.record_failure(1000) == 2000);
  CHECK(!policy.can_attempt(2999));
  CHECK(policy.can_attempt(3000));
  CHECK(policy.record_failure(3000) == 5000);
  CHECK(policy.record_failure(8000) == 15000);
  CHECK(policy.record_failure(23000) == 30000);
  CHECK(policy.record_failure(53000) == 30000);
  policy.record_success();
  CHECK(policy.ever_succeeded && policy.consecutive_failures == 0);
  CHECK(policy.can_attempt(0));
  CHECK(policy.record_failure(60000) == 2000);
  policy.reset_backoff();
  CHECK(policy.can_attempt(60001));

  // Brightness hardware absence should not report Degraded on desktop monitors.
  (void)winome::has_brightness_control();
  CHECK(winome::capability_get(winome::SubsystemId::Brightness).state ==
        winome::CapabilityState::Active);
}

void test_start_menu_pinned_layout_and_folder_sync() {
  // Decrypted start2.bin schema (`pinType: 2`, `name`, `items`, `W~`, `P~`).
  const std::string start2_json =
      R"({"pinnedList":[)"
      R"({"id":"W~Chrome","pinnedBy":32},)"
      R"({"id":"{b9bc0040-0cf1-40c0-8484-db793af3f4cf}","pinnedBy":4,"pinType":2,"name":"\u7231","items":[{"id":"P~Microsoft.WindowsCalculator_8wekyb3d8bbwe!App","pinnedBy":4},{"id":"W~steam://rungameid/1279610","pinnedBy":4}]},)"
      R"({"id":"W~C:\\Tools\\Clip.exe","pinnedBy":4}"
      R"(]})";
  const auto s2_pins = winome::parse_windows_start_layout_json(start2_json);
  CHECK(s2_pins.size() == 3);
  CHECK(s2_pins[0].kind == winome::StartMenuPinnedItem::Kind::DesktopAppId);
  CHECK(s2_pins[0].value == "Chrome");
  CHECK(s2_pins[1].kind == winome::StartMenuPinnedItem::Kind::Folder);
  CHECK(s2_pins[1].value == "\xE7\x88\xB1");
  CHECK(s2_pins[1].children.size() == 2);
  CHECK(s2_pins[1].children[0].kind == winome::StartMenuPinnedItem::Kind::PackagedApp);
  CHECK(s2_pins[1].children[1].value == "steam://rungameid/1279610");

  winome::AppEntry chrome;
  chrome.id = "chrome.exe";
  chrome.name = "Google Chrome";
  chrome.name_lower = "google chrome";
  chrome.aumid = L"Chrome";

  winome::AppEntry uwp_calc;
  uwp_calc.id = "microsoft.windowscalculator_8wekyb3d8bbwe!app";
  uwp_calc.name = "Calculator";
  uwp_calc.name_lower = "calculator";
  uwp_calc.aumid = L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App";
  uwp_calc.is_uwp = true;

  winome::AppEntry wallpaper;
  wallpaper.id = "wallpaper.url";
  wallpaper.name = "Wallpaper Engine";
  wallpaper.name_lower = "wallpaper engine";
  wallpaper.launch_url = "steam://rungameid/1279610";
  wallpaper.folder_name = "Steam";

  winome::AppEntry clip_exe;
  clip_exe.id = "clip-id";
  clip_exe.name = "Clip";
  clip_exe.name_lower = "clip";
  clip_exe.target_exe = L"C:\\Tools\\Clip.exe";

  const auto s2_grid = winome::build_app_grid_items(
      {chrome, uwp_calc, wallpaper, clip_exe}, s2_pins, "");
  CHECK(s2_grid.size() == 3);
  CHECK(s2_grid[0].kind == winome::AppGridItemKind::App);
  CHECK(s2_grid[0].id == "chrome.exe");
  CHECK(s2_grid[1].kind == winome::AppGridItemKind::Folder);
  CHECK(s2_grid[1].title == "\xE7\x88\xB1");
  CHECK(s2_grid[1].children.size() == 2);
  CHECK(s2_grid[2].id == "clip-id");

  // Binary header prefix + escaped UTF-8/Unicode JSON payload + Win11 folder.
  const std::string sample =
      std::string("\x01\x02\x00\x00", 4) +
      R"({"pinnedList":[)"
      R"({"desktopAppLink":"%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\VSCode.lnk"},)"
      R"({"folderName":"\u5de5\u5177","folderItems":[{"packagedAppId":"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"},{"desktopAppLink":"%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\Sub\\Clip.lnk"}]},)"
      R"({"secondaryTile":{"appUserModelId":"Custom.SecTile!App"}})"
      R"(]})";

  const auto pins = winome::parse_windows_start_layout_json(sample);
  CHECK(pins.size() == 3);
  CHECK(pins[0].kind == winome::StartMenuPinnedItem::Kind::DesktopLink);
  CHECK(pins[0].value.find("VSCode.lnk") != std::string::npos);
  CHECK(pins[1].kind == winome::StartMenuPinnedItem::Kind::Folder);
  CHECK(pins[1].value == "\xE5\xB7\xA5\xE5\x85\xB7");
  CHECK(pins[1].children.size() == 2);
  CHECK(pins[2].kind == winome::StartMenuPinnedItem::Kind::PackagedApp);
  CHECK(pins[2].value == "Custom.SecTile!App");

  // Resolve real expanded APPDATA path to match desktopAppLink accurately.
  wchar_t appdata[MAX_PATH] = L"";
  GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
  const std::wstring prefix =
      std::wstring(appdata) + L"\\Microsoft\\Windows\\Start Menu\\Programs\\";

  winome::AppEntry vscode;
  vscode.id = "vscode.exe";
  vscode.name = "VSCode";
  vscode.name_lower = "vscode";
  vscode.launch_path = prefix + L"VSCode.lnk";
  vscode.from_user_programs = true;

  winome::AppEntry calc;
  calc.id = "microsoft.windowscalculator_8wekyb3d8bbwe!app";
  calc.name = "Calculator";
  calc.name_lower = "calculator";
  calc.aumid = L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App";
  calc.is_uwp = true;

  winome::AppEntry clip;
  clip.id = "clip.exe";
  clip.name = "Clip";
  clip.name_lower = "clip";
  clip.launch_path = prefix + L"Sub\\Clip.lnk";
  clip.folder_name = "Sub";
  clip.folder_order = 0;
  clip.from_user_programs = true;

  winome::AppEntry msys_bash;
  msys_bash.id = "msys2-bash";
  msys_bash.name = "MSYS2 Bash";
  msys_bash.name_lower = "msys2 bash";
  msys_bash.folder_name = "MSYS2";
  msys_bash.folder_order = 1;
  msys_bash.item_order = 10;
  msys_bash.from_user_programs = true;

  winome::AppEntry msys_mingw;
  msys_mingw.id = "msys2-ucrt64";
  msys_mingw.name = "MSYS2 UCRT64";
  msys_mingw.name_lower = "msys2 ucrt64";
  msys_mingw.folder_name = "MSYS2";
  msys_mingw.folder_order = 1;
  msys_mingw.item_order = 5;
  msys_mingw.from_user_programs = true;

  winome::AppEntry common_tool;
  common_tool.id = "common-tool";
  common_tool.name = "Common Tool";
  common_tool.name_lower = "common tool";
  common_tool.folder_name = "CommonSuite";
  common_tool.folder_order = 0;
  common_tool.item_order = 2;
  common_tool.from_user_programs = false;

  winome::AppEntry root_extra;
  root_extra.id = "root-extra";
  root_extra.name = "Root Extra";
  root_extra.name_lower = "root extra";
  root_extra.from_user_programs = true;
  root_extra.item_order = 12;

  const std::vector<winome::AppEntry> all{
      vscode, calc, clip, msys_bash, msys_mingw, common_tool, root_extra};
  const auto grid_items = winome::build_app_grid_items(all, pins, "");

  // Expected order:
  // 0: VSCode (pinned app)
  // 1: 工具 (pinned folder: Calculator + Clip)
  // 2: MSYS2 (unpinned user folder, children sorted by item_order: UCRT64 then Bash)
  // 3: CommonSuite (unpinned common folder)
  // 4: Root Extra (unpinned root app)
  CHECK(grid_items.size() == 5);
  CHECK(grid_items[0].kind == winome::AppGridItemKind::App);
  CHECK(grid_items[0].id == "vscode.exe");
  CHECK(grid_items[1].kind == winome::AppGridItemKind::Folder);
  CHECK(grid_items[1].children.size() == 2);
  CHECK(grid_items[2].kind == winome::AppGridItemKind::Folder);
  CHECK(grid_items[2].title == "MSYS2");
  CHECK(grid_items[2].children.size() == 2);
  CHECK(grid_items[2].children[0].id == "msys2-ucrt64");
  CHECK(grid_items[2].children[1].id == "msys2-bash");
  CHECK(grid_items[3].kind == winome::AppGridItemKind::Folder);
  CHECK(grid_items[3].title == "CommonSuite");
  CHECK(grid_items[4].kind == winome::AppGridItemKind::App);
  CHECK(grid_items[4].id == "root-extra");

  // Search query must flatten apps across folders.
  const auto search_items = winome::build_app_grid_items(all, pins, "ucrt64");
  CHECK(search_items.size() == 1);
  CHECK(search_items[0].kind == winome::AppGridItemKind::App);
  CHECK(search_items[0].id == "msys2-ucrt64");
}

void test_systray_v4_and_uia_candidates() {
  const auto v4_right = winome::build_tray_click_messages(
      WM_APP + 5, 77, 4, true, 123, 456);
  CHECK(v4_right.size() == 3);
  CHECK(v4_right[0].msg == WM_APP + 5);
  CHECK(v4_right[0].wparam == static_cast<uintptr_t>(MAKEWPARAM(123, 456)));
  CHECK(LOWORD(v4_right[0].lparam) == WM_RBUTTONDOWN);
  CHECK(HIWORD(v4_right[0].lparam) == 77);
  CHECK(LOWORD(v4_right[2].lparam) == WM_CONTEXTMENU);
  CHECK(HIWORD(v4_right[2].lparam) == 77);

  const auto v4_left = winome::build_tray_click_messages(
      WM_APP + 5, 42, 4, false, 10, 20);
  CHECK(v4_left.size() == 3);
  CHECK(LOWORD(v4_left[2].lparam) == (WM_USER + 0));
  CHECK(HIWORD(v4_left[2].lparam) == 42);

  const auto v3_right = winome::build_tray_click_messages(
      WM_APP + 1, 9, 3, true, 0, 0);
  CHECK(v3_right.size() == 3);
  CHECK(v3_right[0].wparam == 9);
  CHECK(v3_right[0].lparam == static_cast<intptr_t>(WM_RBUTTONDOWN));

  CHECK(winome::is_uia_tray_candidate("Steam", "", true));
  CHECK(!winome::is_uia_tray_candidate("Steam", "", false));
  CHECK(winome::is_uia_tray_candidate("FlClash", "NotifyItemIcon", false));
  CHECK(winome::is_uia_tray_candidate(" \xE6\x89\x93\xE5\xBC\x80 Chrome \xE4\xB8\xAD\xE7\x9A\x84 Gemini",
                                      "NotifyItemIcon", false));
  CHECK(!winome::is_uia_tray_candidate("\xE7\xBD\x91\xE7\xBB\x9C MISAKA_00002\nInternet \xE8\xAE\xBF\xE9\x97\xAE",
                                       "SystemTrayIcon", false));
  CHECK(!winome::is_uia_tray_candidate("\xE9\x9F\xB3\xE9\x87\x8F \xE5\x96\x87\xE5\x8F\xAD: 44%",
                                       "SystemTrayIcon", false));
  CHECK(!winome::is_uia_tray_candidate("\xE7\x94\xB5\xE6\xBA\x90 \xE7\x94\xB5\xE6\xB1\xA0\xE7\x8A\xB6\xE6\x80\x81: 0%",
                                       "SystemTrayIcon", false));
  CHECK(!winome::is_uia_tray_candidate("\xE9\x80\x9A\xE7\x9F\xA5 \xE6\xB2\xA1\xE6\x9C\x89\xE6\x96\xB0\xE9\x80\x9A\xE7\x9F\xA5",
                                       "SystemTrayIcon", false));
  CHECK(!winome::is_uia_tray_candidate("\xE8\xA7\xA6\xE6\x91\xB8\xE9\x94\xAE\xE7\x9B\x98",
                                       "SystemTrayIcon", false));
  CHECK(!winome::is_uia_tray_candidate("\xE6\x89\x98\xE7\x9B\x98\xE8\xBE\x93\xE5\x85\xA5\xE6\x8C\x87\xE7\xA4\xBA\xE5\x99\xA8 \xE4\xB8\xAD\xE6\x96\x87\xE6\xA8\xA1\xE5\xBC\x8F",
                                       "SystemTrayIcon", false));
  CHECK(winome::is_uia_tray_candidate("Clash Verge", "NotifyItemIcon_1", false));
  CHECK(!winome::is_uia_tray_candidate("Show Hidden Icons", "OverflowButton", true));

  const std::string tray_json = winome::systray_debug_inspect_json();
  CHECK(tray_json.find("\"items_count\"") != std::string::npos);
  CHECK(tray_json.find("\"stats\"") != std::string::npos);
  CHECK(tray_json.find("\"inspect_ms\"") != std::string::npos);
}

void test_overview_window_blacklist() {
  // "Windows 输入体验" / "Windows Input Experience" (TextInputHost).
  CHECK(winome::is_overview_default_blacklisted_window_info(
      "Windows.UI.Core.CoreWindow",
      "C:\\Windows\\SystemApps\\MicrosoftWindows.Client.CBS_cw5n1h2txyewy\\TextInputHost.exe",
      "Windows \xE8\xBE\x93\xE5\x85\xA5\xE4\xBD\x93\xE9\xAA\x8C",
      true));
  CHECK(winome::is_overview_default_blacklisted_window_info(
      "Windows.UI.Core.CoreWindow",
      "",
      "Windows\xE8\xBE\x93\xE5\x85\xA5\xE4\xBD\x93\xE9\xAA\x8C",
      false));
  CHECK(winome::is_overview_default_blacklisted_window_info(
      "Windows.UI.Core.CoreWindow",
      "TextInputHost.exe",
      "Windows Input Experience",
      true));

  // Explorer's nameless, blank/ghost windows.
  CHECK(winome::is_overview_default_blacklisted_window_info(
      "ApplicationFrameWindow",
      "C:\\Windows\\explorer.exe",
      "",
      true));
  CHECK(winome::is_overview_default_blacklisted_window_info(
      "WorkerW",
      "explorer.exe",
      "   ",
      false));

  // Suspended / pre-launched / cloaked UWP "设置" / "Settings" ghost windows.
  CHECK(winome::is_overview_default_blacklisted_window_info(
      "ApplicationFrameWindow",
      "C:\\Windows\\System32\\ApplicationFrameHost.exe",
      "\xE8\xAE\xBE\xE7\xBD\xAE",
      true, 0x2u, true));
  CHECK(winome::is_overview_default_blacklisted_window_info(
      "ApplicationFrameWindow",
      "C:\\Windows\\System32\\ApplicationFrameHost.exe",
      "\xE8\xAE\xBE\xE7\xBD\xAE",
      true, 0x1u, false));
  CHECK(winome::is_overview_default_blacklisted_window_info(
      "ApplicationFrameWindow",
      "ApplicationFrameHost.exe",
      "Settings",
      true, 0x2u, true));
  CHECK(winome::is_overview_default_blacklisted_window_info(
      "ApplicationFrameWindow",
      "ApplicationFrameHost.exe",
      "\xE8\xA8\xAD\xE5\xAE\x9A",
      true, 0x2u, true));
  // Uncloaked but coreless splash-only UWP frame is also blacklisted.
  CHECK(winome::is_overview_default_blacklisted_window_info(
      "ApplicationFrameWindow",
      "C:\\Windows\\System32\\ApplicationFrameHost.exe",
      "\xE8\xAE\xBE\xE7\xBD\xAE",
      false, 0u, false));
  // Any app-cloaked (DWM_CLOAKED_APP = 0x1) UWP app is blacklisted.
  CHECK(winome::is_overview_default_blacklisted_window_info(
      "ApplicationFrameWindow",
      "C:\\Windows\\System32\\ApplicationFrameHost.exe",
      "Microsoft Store",
      true, 0x1u, true));

  // Legitimate Explorer folders, live open UWP Settings, other-desktop UWP apps,
  // and regular user windows remain untouched.
  CHECK(!winome::is_overview_default_blacklisted_window_info(
      "CabinetWClass",
      "C:\\Windows\\explorer.exe",
      "WINOME-portable - \xE6\x96\x87\xE4\xBB\xB6\xE8\xB5\x84\xE6\xBA\x90\xE7\xAE\xA1\xE7\x90\x86\xE5\x99\xA8",
      false));
  CHECK(!winome::is_overview_default_blacklisted_window_info(
      "ApplicationFrameWindow",
      "C:\\Windows\\System32\\ApplicationFrameHost.exe",
      "\xE8\xAE\xBE\xE7\xBD\xAE",
      false, 0u, true));
  CHECK(!winome::is_overview_default_blacklisted_window_info(
      "ApplicationFrameWindow",
      "C:\\Windows\\System32\\ApplicationFrameHost.exe",
      "Calculator",
      true, 0x2u, true));
  CHECK(!winome::is_overview_default_blacklisted_window_info(
      "Chrome_WidgetWin_1",
      "chrome.exe",
      "Google Chrome",
      false));
  CHECK(!winome::is_overview_default_blacklisted_hwnd(nullptr, true));
  // Disabled setting must bypass filtering.
  CHECK(!winome::is_overview_default_blacklisted_hwnd(nullptr, false));
}

void test_winrt_safe_com_getter_and_notifications() {
  struct MockVtbl {
    HRESULT (STDMETHODCALLTYPE* QueryInterface)(void*, REFIID, void**);
    ULONG (STDMETHODCALLTYPE* AddRef)(void*);
    ULONG (STDMETHODCALLTYPE* Release)(void*);
  };
  struct MockChild {
    const MockVtbl* vtbl = nullptr;
    ULONG refs = 1;
    int* destroy_count = nullptr;
  };
  static const MockVtbl kChildVtbl = {
      +[](void* self, REFIID, void** out) -> HRESULT {
        auto* c = static_cast<MockChild*>(self);
        ++c->refs;
        *out = self;
        return S_OK;
      },
      +[](void* self) -> ULONG {
        return ++static_cast<MockChild*>(self)->refs;
      },
      +[](void* self) -> ULONG {
        auto* c = static_cast<MockChild*>(self);
        const ULONG r = --c->refs;
        if (r == 0) {
          if (c->destroy_count != nullptr)
            ++(*c->destroy_count);
          delete c;
        }
        return r;
      },
  };

  struct MockOwnerVtbl {
    HRESULT (STDMETHODCALLTYPE* QueryInterface)(void*, REFIID, void**);
    ULONG (STDMETHODCALLTYPE* AddRef)(void*);
    ULONG (STDMETHODCALLTYPE* Release)(void*);
    HRESULT (STDMETHODCALLTYPE* GetChild)(void*, void**);
  };
  enum class GetterMode { BorrowedNoAddRef, AddRefedShared, FreshPerCall };
  struct MockOwner {
    const MockOwnerVtbl* vtbl = nullptr;
    GetterMode mode = GetterMode::BorrowedNoAddRef;
    MockChild* child = nullptr;
    int* child_destroys = nullptr;
  };
  static const MockOwnerVtbl kOwnerVtbl = {
      +[](void*, REFIID, void** out) -> HRESULT {
        if (out != nullptr) *out = nullptr;
        return E_NOINTERFACE;
      },
      +[](void*) -> ULONG { return 1; },
      +[](void*) -> ULONG { return 1; },
      +[](void* self, void** out) -> HRESULT {
        auto* o = static_cast<MockOwner*>(self);
        if (o->mode == GetterMode::FreshPerCall) {
          *out = new MockChild{&kChildVtbl, 1, o->child_destroys};
          return S_OK;
        }
        if (o->mode == GetterMode::AddRefedShared)
          ++o->child->refs;
        *out = o->child;
        return S_OK;
      },
  };

  // Case 1: Buggy getter omitting AddRef() must compensate and prevent premature free.
  int destroys1 = 0;
  auto* child1 = new MockChild{&kChildVtbl, 1, &destroys1};
  MockOwner owner1{&kOwnerVtbl, GetterMode::BorrowedNoAddRef, child1, &destroys1};
  void* got1 = nullptr;
  bool comp1 = false;
  CHECK(SUCCEEDED(winome::winrt_acquire_com_getter_safe(&owner1, 3, &got1, &comp1)));
  CHECK(got1 == child1);
  CHECK(comp1);
  CHECK(child1->refs == 2);
  CHECK(kChildVtbl.Release(got1) == 1);
  CHECK(destroys1 == 0);
  CHECK(kChildVtbl.Release(child1) == 0);
  CHECK(destroys1 == 1);

  // Case 2: Standard getter calling AddRef() must NOT double-AddRef / leak.
  int destroys2 = 0;
  auto* child2 = new MockChild{&kChildVtbl, 1, &destroys2};
  MockOwner owner2{&kOwnerVtbl, GetterMode::AddRefedShared, child2, &destroys2};
  void* got2 = nullptr;
  bool comp2 = true;
  CHECK(SUCCEEDED(winome::winrt_acquire_com_getter_safe(&owner2, 3, &got2, &comp2)));
  CHECK(got2 == child2);
  CHECK(!comp2);
  CHECK(child2->refs == 2);
  CHECK(kChildVtbl.Release(got2) == 1);
  CHECK(destroys2 == 0);
  CHECK(kChildVtbl.Release(child2) == 0);
  CHECK(destroys2 == 1);

  // Case 3: Getter creating fresh object per call cleans up probe copy without leak.
  int destroys3 = 0;
  MockOwner owner3{&kOwnerVtbl, GetterMode::FreshPerCall, nullptr, &destroys3};
  void* got3 = nullptr;
  bool comp3 = true;
  CHECK(SUCCEEDED(winome::winrt_acquire_com_getter_safe(&owner3, 3, &got3, &comp3)));
  CHECK(got3 != nullptr);
  CHECK(!comp3);
  CHECK(destroys3 == 1);  // second probe instance released immediately
  CHECK(static_cast<MockChild*>(got3)->refs == 1);
  CHECK(kChildVtbl.Release(got3) == 0);
  CHECK(destroys3 == 2);

  // Live WinRT notification poll regression check under concurrent 104-byte heap noise.
  for (int i = 0; i < 8; ++i) {
    std::vector<winome::WinomeSystemNotification> notifs;
    bool allowed = false;
    (void)winome::winrt_get_notifications(&notifs, &allowed);
    for (int k = 0; k < 16; ++k) {
      void* p = HeapAlloc(GetProcessHeap(), 0, 104);
      if (p != nullptr)
        HeapFree(GetProcessHeap(), 0, p);
    }
  }
  winome::winrt_shutdown();
}

void test_win_hotkey_passthrough_classification() {
  using winome::WinKeyComboAction;

  // Plain Win+Tab remaps to Start Menu; modified Tab combos pass through.
  CHECK(winome::classify_win_key_combo(VK_TAB, false, false, false, false, false) ==
        WinKeyComboAction::StartMenu);
  CHECK(winome::classify_win_key_combo(VK_TAB, false, false, true, false, false) ==
        WinKeyComboAction::PassthroughCombo);
  CHECK(winome::classify_win_key_combo(VK_TAB, false, true, false, false, false) ==
        WinKeyComboAction::PassthroughCombo);
  CHECK(winome::classify_win_key_combo(VK_TAB, true, false, false, true, false) ==
        WinKeyComboAction::PassthroughCombo);
  CHECK(winome::classify_win_key_combo(VK_TAB, false, false, false, false, true) ==
        WinKeyComboAction::PassthroughCombo);

  // Non-conflicting Windows shortcuts (e.g. Win+V clipboard, Win+E, Win+R, Win+Shift+S).
  CHECK(winome::classify_win_key_combo('V', false, false, false, false, false) ==
        WinKeyComboAction::PassthroughCombo);
  CHECK(winome::classify_win_key_combo('E', false, false, false, false, false) ==
        WinKeyComboAction::PassthroughCombo);
  CHECK(winome::classify_win_key_combo('R', false, false, false, false, false) ==
        WinKeyComboAction::PassthroughCombo);
  CHECK(winome::classify_win_key_combo('S', false, false, true, false, false) ==
        WinKeyComboAction::PassthroughCombo);
  CHECK(winome::classify_win_key_combo('L', false, false, false, false, false) ==
        WinKeyComboAction::PassthroughCombo);

  // Modifiers pass through without prematurely re-injecting Win so chords like
  // Win+Shift+PageUp never leak Win and Win+Shift+S / Win+Ctrl+Left compose cleanly.
  CHECK(winome::classify_win_key_combo(VK_LSHIFT, false, false, false, false, false) ==
        WinKeyComboAction::ModifierPassthrough);
  CHECK(winome::classify_win_key_combo(VK_LCONTROL, false, false, false, false, false) ==
        WinKeyComboAction::ModifierPassthrough);
  CHECK(winome::classify_win_key_combo(VK_LMENU, true, false, false, true, false) ==
        WinKeyComboAction::ModifierPassthrough);
  CHECK(winome::is_win_combo_modifier_vk(VK_RSHIFT));
  CHECK(winome::is_win_combo_modifier_vk(VK_RCONTROL));
  CHECK(winome::is_win_combo_modifier_vk(VK_RMENU));
  CHECK(!winome::is_win_combo_modifier_vk('V'));

  // WINOME workspace switching shortcuts: Win(+Shift)+PageUp/PageDown.
  CHECK(winome::classify_win_key_combo(VK_PRIOR, false, false, false, false, false) ==
        WinKeyComboAction::WorkspacePrev);
  CHECK(winome::classify_win_key_combo(VK_NEXT, false, false, true, false, false) ==
        WinKeyComboAction::WorkspaceNext);
  // Win+Ctrl+PageUp / Win+Alt+PageUp do not conflict with WINOME workspaces.
  CHECK(winome::classify_win_key_combo(VK_PRIOR, false, true, false, false, false) ==
        WinKeyComboAction::PassthroughCombo);
  CHECK(winome::classify_win_key_combo(VK_NEXT, true, false, false, true, false) ==
        WinKeyComboAction::PassthroughCombo);

  // Native Virtual Desktop hotkeys pass through with VD sync hint.
  CHECK(winome::classify_win_key_combo(VK_LEFT, false, true, false, false, false) ==
        WinKeyComboAction::PassthroughWithVdHint);
  CHECK(winome::classify_win_key_combo(VK_RIGHT, false, true, false, false, false) ==
        WinKeyComboAction::PassthroughWithVdHint);
  CHECK(winome::classify_win_key_combo('D', false, true, false, false, false) ==
        WinKeyComboAction::PassthroughWithVdHint);
  CHECK(winome::classify_win_key_combo(VK_F4, false, true, false, false, false) ==
        WinKeyComboAction::PassthroughWithVdHint);
}

void test_workarea_clamp_guard_and_fullscreen_hysteresis() {
  const RECT mon{0, 0, 1920, 1080};

  // Panel height sanitization rejects 0, negative, and bloated values.
  CHECK(winome::clamp_panel_height_for_monitor(mon, 32, 32) == 32);
  CHECK(winome::clamp_panel_height_for_monitor(mon, 0, 32) == 32);
  CHECK(winome::clamp_panel_height_for_monitor(mon, -50, 32) == 32);
  CHECK(winome::clamp_panel_height_for_monitor(mon, 600, 32) == 32);
  CHECK(winome::clamp_panel_height_for_monitor(mon, 1080, 32) == 32);
  CHECK(winome::clamp_panel_height_for_monitor(RECT{0, 0, 0, 0}, 32, 32) == 0);

  const RECT panel_rc = winome::compute_primary_panel_rect(mon, 32);
  CHECK(panel_rc.left == 0);
  CHECK(panel_rc.top == 0);
  CHECK(panel_rc.right == 1920);
  CHECK(panel_rc.bottom == 32);

  // WorkareaAssertGuard: skips unchanged, skips hidden, skips mid-drag, and backs off loops.
  winome::WorkareaAssertGuard guard;
  const RECT want{0, 32, 1920, 1080};
  const RECT reset{0, 0, 1920, 1080};

  CHECK(guard.evaluate(1000, want, want, true, false, false) ==
        winome::WorkareaAssertAction::SkipUnchanged);
  CHECK(guard.evaluate(1100, want, reset, true, true, false) ==
        winome::WorkareaAssertAction::SkipHidden);
  CHECK(guard.pending_after_defer);
  CHECK(guard.evaluate(1200, want, reset, true, false, true) ==
        winome::WorkareaAssertAction::SkipInMoveSize);
  CHECK(guard.evaluate(1300, want, reset, false, false, false) ==
        winome::WorkareaAssertAction::SkipInvalidMonitor);

  // Burst protection: applies at >=120ms intervals up to 8 times in 3s, then backs off 1s.
  CHECK(guard.evaluate(2000, want, reset, true, false, false) ==
        winome::WorkareaAssertAction::Apply);
  CHECK(guard.evaluate(2050, want, reset, true, false, false) ==
        winome::WorkareaAssertAction::DeferCooldown);
  for (int k = 1; k < winome::WorkareaAssertGuard::kMaxBurstApplies; ++k) {
    CHECK(guard.evaluate(2000 + k * 150, want, reset, true, false, false) ==
          winome::WorkareaAssertAction::Apply);
  }
  CHECK(guard.evaluate(2000 + winome::WorkareaAssertGuard::kMaxBurstApplies * 150,
                       want, reset, true, false, false) ==
        winome::WorkareaAssertAction::DeferBackoff);
  // Once 1s backoff expires and workarea stays matching across the window, burst counter clears.
  CHECK(guard.evaluate(6500, want, want, true, false, false) ==
        winome::WorkareaAssertAction::SkipUnchanged);
  CHECK(guard.evaluate(6700, want, reset, true, false, false) ==
        winome::WorkareaAssertAction::Apply);
  // Lifting workarea top enters 600ms settle grace so async window resize never
  // triggers false zoomed fullscreen hides.
  CHECK(guard.in_settle_grace(6700));
  CHECK(guard.in_settle_grace(7299));
  CHECK(!guard.in_settle_grace(7300));

  // Fullscreen bounds check: rejects near-edge undersized windows & unreserved zoomed windows.
  const RECT reserved_wa{0, 32, 1920, 1080};
  const RECT unreserved_wa{0, 0, 1920, 1080};
  // Normal window near screen edge (6px inset, e.g. from DWM shadow without extended frame).
  CHECK(!winome::is_fullscreen_bounds_for_monitor(
      RECT{5, 0, 1915, 1075}, mon, reserved_wa, false));
  // Genuine fullscreen covering full monitor.
  CHECK(winome::is_fullscreen_bounds_for_monitor(
      RECT{0, 0, 1920, 1080}, mon, reserved_wa, false));
  // Normal maximized window stops at reserved workarea top (32) -> NOT fullscreen.
  CHECK(!winome::is_fullscreen_bounds_for_monitor(
      RECT{0, 32, 1920, 1080}, mon, reserved_wa, true));
  // Zoomed window when workarea top was reset to 0 -> still NOT fullscreen.
  CHECK(!winome::is_fullscreen_bounds_for_monitor(
      RECT{0, 0, 1920, 1080}, mon, unreserved_wa, true));
  // Decorated maximized window (WS_CAPTION + IsZoomed) even if touching top=0 -> NOT fullscreen.
  CHECK(!winome::is_fullscreen_bounds_for_monitor(
      RECT{0, 0, 1920, 1080}, mon, reserved_wa, true, 2, true, false));
  // Undecorated zoomed window during post-workarea settle grace -> NOT fullscreen yet.
  CHECK(!winome::is_fullscreen_bounds_for_monitor(
      RECT{0, 0, 1920, 1080}, mon, reserved_wa, true, 2, false, true));
  // Zoomed borderless fullscreen explicitly extending above reserved top (32 -> 0) -> fullscreen.
  CHECK(winome::is_fullscreen_bounds_for_monitor(
      RECT{0, 0, 1920, 1080}, mon, reserved_wa, true));

  // FullscreenHysteresisTracker: holds during transient overlays & 160ms unhide debounce.
  winome::FullscreenHysteresisTracker tracker;
  HMONITOR fake_mon = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(0x1234));

  winome::FullscreenEvalInput in{};
  in.now_ms = 1000;
  in.raw_fullscreen_mon = fake_mon;
  auto res = tracker.step(in);
  CHECK(res.effective_fullscreen_mon == fake_mon);

  // Transient null/IME/owned popup while fullscreen window still covers monitor -> stays hidden!
  in = {};
  in.now_ms = 1200;
  in.currently_hidden = true;
  in.fg_is_transient_or_null = true;
  in.tracked_fs_still_covering = true;
  in.tracked_fs_mon = fake_mon;
  res = tracker.step(in);
  CHECK(res.effective_fullscreen_mon == fake_mon);
  CHECK(!res.schedule_unhide_timer);

  // Brief non-fullscreen transition -> holds during 160ms debounce, then unhides cleanly.
  in = {};
  in.now_ms = 2000;
  in.currently_hidden = true;
  in.tracked_fs_mon = fake_mon;
  res = tracker.step(in);
  CHECK(res.effective_fullscreen_mon == fake_mon);
  CHECK(res.schedule_unhide_timer);
  CHECK(res.unhide_delay_ms == 160);

  in.now_ms = 2100;
  res = tracker.step(in);
  CHECK(res.effective_fullscreen_mon == fake_mon);

  in.now_ms = 2160;
  res = tracker.step(in);
  CHECK(res.effective_fullscreen_mon == nullptr);

  // Opening Overview always unhides immediately (0ms).
  in = {};
  in.now_ms = 3000;
  in.currently_hidden = true;
  in.overview_open = true;
  in.tracked_fs_mon = fake_mon;
  res = tracker.step(in);
  CHECK(res.effective_fullscreen_mon == nullptr);
  CHECK(res.cancel_unhide_timer);

  // Opening a top-bar Popover unhides immediately and never allows fullscreen re-hide.
  in = {};
  in.now_ms = 3500;
  in.currently_hidden = true;
  in.popover_open = true;
  in.raw_fullscreen_mon = fake_mon;
  in.tracked_fs_mon = fake_mon;
  res = tracker.step(in);
  CHECK(res.effective_fullscreen_mon == nullptr);
  CHECK(res.cancel_unhide_timer);
  CHECK(res.cancel_hide_timer);

  // Runtime enter-fullscreen debounce (80ms) suppresses transient 1-frame flash hides.
  winome::FullscreenHysteresisTracker debounced_tracker(80);
  in = {};
  in.now_ms = 4000;
  in.raw_fullscreen_mon = fake_mon;
  res = debounced_tracker.step(in);
  CHECK(res.effective_fullscreen_mon == nullptr);
  CHECK(res.schedule_hide_timer);
  CHECK(res.hide_delay_ms == 80);

  // If transient fullscreen vanishes at 40ms, panel never blinked hidden.
  in = {};
  in.now_ms = 4040;
  res = debounced_tracker.step(in);
  CHECK(res.effective_fullscreen_mon == nullptr);
  CHECK(res.cancel_hide_timer);

  // Sustained fullscreen for >=80ms hides cleanly.
  in = {};
  in.now_ms = 5000;
  in.raw_fullscreen_mon = fake_mon;
  res = debounced_tracker.step(in);
  CHECK(res.effective_fullscreen_mon == nullptr);
  in.now_ms = 5080;
  res = debounced_tracker.step(in);
  CHECK(res.effective_fullscreen_mon == fake_mon);

  // Escalating workarea backoff: repeated overwrites probe once after each
  // backoff and double backoff duration up to 8s, resetting on SkipUnchanged.
  {
    winome::WorkareaAssertGuard esc;
    int64_t t = 1000;
    for (int k = 0; k < winome::WorkareaAssertGuard::kMaxBurstApplies; ++k) {
      CHECK(esc.evaluate(t, want, reset, true, false, false) ==
            winome::WorkareaAssertAction::Apply);
      t += 150;
    }
    // 9th attempt triggers 1s backoff (consecutive_backoff_count = 1).
    CHECK(esc.evaluate(t, want, reset, true, false, false) ==
          winome::WorkareaAssertAction::DeferBackoff);
    CHECK(esc.backoff_until_ms == t + 1000);
    // Waking right after 1s backoff allows 1 probe apply.
    t += 1020;
    CHECK(esc.evaluate(t, want, reset, true, false, false) ==
          winome::WorkareaAssertAction::Apply);
    // Immediate overwrite 220ms later escalates backoff to 2s.
    t += 220;
    CHECK(esc.evaluate(t, want, reset, true, false, false) ==
          winome::WorkareaAssertAction::DeferBackoff);
    CHECK(esc.backoff_until_ms == t + 2000);
    CHECK(esc.remaining_defer_ms(t) >= 2000);
    // Once workarea sticks, escalation resets cleanly.
    t += 2050;
    CHECK(esc.evaluate(t, want, want, true, false, false) ==
          winome::WorkareaAssertAction::SkipUnchanged);
    CHECK(esc.consecutive_backoff_count == 0);
  }

  // Standard vs borderless fullscreen frame style classification.
  {
    CHECK(winome::has_standard_maximized_frame_style(WS_OVERLAPPEDWINDOW));
    CHECK(winome::has_standard_maximized_frame_style(WS_CAPTION));
    // Custom-chrome maximized window without WS_CAPTION but with resize frame + max box.
    CHECK(winome::has_standard_maximized_frame_style(WS_THICKFRAME | WS_MAXIMIZEBOX));
    // Borderless fullscreen popup window has neither WS_CAPTION nor resizable frame.
    CHECK(!winome::has_standard_maximized_frame_style(WS_POPUP | WS_VISIBLE));
  }

  // Per-monitor hysteresis isolation: hiding monitor 2 does not bypass monitor 1's debounce.
  {
    winome::FullscreenHysteresisTracker mon1_tracker(80);
    winome::FullscreenHysteresisTracker mon2_tracker(80);
    HMONITOR fake_mon2 = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(0x5678));

    winome::FullscreenEvalInput in2{};
    in2.now_ms = 1000;
    in2.raw_fullscreen_mon = fake_mon2;
    CHECK(mon2_tracker.step(in2).effective_fullscreen_mon == nullptr);
    in2.now_ms = 1080;
    CHECK(mon2_tracker.step(in2).effective_fullscreen_mon == fake_mon2);

    // Monitor 1 starts visible even though Monitor 2 is hidden -> still waits 80ms.
    winome::FullscreenEvalInput in1{};
    in1.now_ms = 1100;
    in1.currently_hidden = false;
    in1.raw_fullscreen_mon = fake_mon;
    auto r1 = mon1_tracker.step(in1);
    CHECK(r1.effective_fullscreen_mon == nullptr);
    CHECK(r1.schedule_hide_timer);
    CHECK(r1.hide_delay_ms == 80);
  }
}

void test_hook_watchdog_and_lockfree_ring() {
  // Lock-free SPSC ring FIFO, full backpressure, wraparound, and clear.
  winome::AtomicTriggerRing<int, 4> ring;
  int v = -1;
  CHECK(!ring.try_pop(&v));
  CHECK(ring.try_push(10));
  CHECK(ring.try_push(20));
  CHECK(ring.try_push(30));
  CHECK(!ring.try_push(40));  // capacity 4 holds 3 items (1 slot distinguishes full)
  CHECK(ring.try_pop(&v) && v == 10);
  CHECK(ring.try_push(40));
  CHECK(ring.try_pop(&v) && v == 20);
  CHECK(ring.try_pop(&v) && v == 30);
  CHECK(ring.try_pop(&v) && v == 40);
  CHECK(!ring.try_pop(&v));
  CHECK(ring.try_push(99));
  ring.clear();
  CHECK(!ring.try_pop(&v));

  // Freshly held keys (< threshold) do not trigger watchdog reset or reinstall.
  winome::HookWatchdogInput in{};
  in.now_tick = 5000;
  in.win_down = true;
  in.win_last_keydown_tick = 4500;
  auto out = winome::evaluate_hook_watchdog(in);
  CHECK(!out.reset_win);
  CHECK(!out.safe_to_reinstall);

  // Stuck un-injected Win keydown (> 2500ms without keyup/auto-repeat) resets & allows reinstall.
  in.now_tick = 7100;
  out = winome::evaluate_hook_watchdog(in);
  CHECK(out.reset_win);
  CHECK(out.safe_to_reinstall);

  // Stuck injected Win key whose physical key is released (> 200ms) resets cleanly.
  in = {};
  in.now_tick = 8500;
  in.win_down = true;
  in.win_injected = true;
  in.win_physically_down = false;
  in.win_inject_tick = 8200;
  out = winome::evaluate_hook_watchdog(in);
  CHECK(out.reset_win);
  CHECK(out.safe_to_reinstall);

  // Physically held injected Win key stays active.
  in.win_physically_down = true;
  out = winome::evaluate_hook_watchdog(in);
  CHECK(!out.reset_win);
  CHECK(!out.safe_to_reinstall);

  // Stuck Esc / Alt+F2 / Win+Tab swallowed flags (> 1500ms) recover cleanly instead of
  // locking out the periodic WH_KEYBOARD_LL watchdog forever.
  in = {};
  in.now_tick = 12000;
  in.esc_swallowed = true;
  in.esc_swallow_tick = 10000;
  in.alt_f2_down = true;
  in.alt_f2_tick = 10000;
  in.swallowed_combo_vk = VK_TAB;
  in.combo_swallow_tick = 10000;
  out = winome::evaluate_hook_watchdog(in);
  CHECK(out.clear_esc);
  CHECK(out.clear_alt_f2);
  CHECK(out.clear_combo);
  CHECK(out.safe_to_reinstall);

  // Healthy active low-level hooks should not be churned; silent/missing hooks reinstall.
  CHECK(winome::should_reinstall_ll_hook(false, 10000, 9990, 9900, 4000, 2500));
  CHECK(!winome::should_reinstall_ll_hook(true, 10000, 9000, 5000, 4000, 2500));
  CHECK(winome::should_reinstall_ll_hook(true, 15000, 9000, 5000, 4000, 2500));
  CHECK(!winome::should_reinstall_ll_hook(true, 15000, 9000, 14000, 4000, 2500));
  CHECK(winome::should_reinstall_ll_hook(true, 10000, 0, 5000, 4000, 2500));

  // Only visible, uncloaked Start/Search host windows count as open Start Menu.
  CHECK(winome::is_start_menu_window_active(true, 0, true, true));
  CHECK(!winome::is_start_menu_window_active(true, 1, true, true));
  CHECK(!winome::is_start_menu_window_active(true, 2, true, true));
  CHECK(!winome::is_start_menu_window_active(false, 0, true, true));
  CHECK(!winome::is_start_menu_window_active(true, 0, false, true));
  CHECK(!winome::is_start_menu_window_active(true, 0, true, false));
}

void test_overview_search_helpers() {
  // 1. Multi-term splitting (searchController.js getTermsForSearchString).
  {
    auto terms = winome::split_search_terms("   Visual   Studio  CODE  ");
    CHECK(terms.size() == 3);
    CHECK(terms[0] == "visual");
    CHECK(terms[1] == "studio");
    CHECK(terms[2] == "code");

    auto empty_terms = winome::split_search_terms("   \t  ");
    CHECK(empty_terms.empty());

    CHECK(winome::match_all_search_terms("visual studio code editor", terms));
    CHECK(!winome::match_all_search_terms("visual studio only", terms));
  }

  // 2. Pango Highlighter (misc/util.js Highlighter): XML escaping + case-insensitive <b>.
  {
    std::vector<std::string> terms = {"net", "and"};
    std::string hl = winome::highlight_search_terms_markup(
        "Network & Android <Test>", terms);
    CHECK(hl == "<b>Net</b>work &amp; <b>And</b>roid &lt;Test&gt;");

    // Overlapping/adjacent terms merge cleanly without nested <b> tags.
    std::vector<std::string> overlap = {"abc", "bcd"};
    std::string hl2 = winome::highlight_search_terms_markup("xxABCDyy", overlap);
    CHECK(hl2 == "xx<b>ABCD</b>yy");
  }

  // 3. Scientific, arithmetic, hex, and binary Calculator evaluator.
  {
    std::string res;
    CHECK(winome::try_eval_search_math("12*34", &res));
    CHECK(res == "408");

    CHECK(winome::try_eval_search_math("sqrt(144) + 2^3", &res));
    CHECK(res == "20");

    CHECK(winome::try_eval_search_math("(10 + 5) * 2 - 4 / 2", &res));
    CHECK(res == "28");

    CHECK(winome::try_eval_search_math("0xff", &res));
    CHECK(res == "255 (0xFF)");

    CHECK(winome::try_eval_search_math("0b1010", &res));
    CHECK(res == "10 (0xA)");

    CHECK(winome::try_eval_search_math("round(pi * 100)", &res));
    CHECK(res == "314");

    // Plain numbers without operator/function/radix and divide-by-zero must not match.
    CHECK(!winome::try_eval_search_math("2026", &res));
    CHECK(!winome::try_eval_search_math("10 / 0", &res));
    CHECK(!winome::try_eval_search_math("chrome", &res));
  }

  // 4. URL & File-path detection.
  {
    CHECK(winome::is_url_like_query("https://gnome.org"));
    CHECK(winome::is_url_like_query("github.com/GNOME/gnome-shell"));
    CHECK(winome::is_url_like_query("www.example.org"));
    CHECK(!winome::is_url_like_query("hello world"));
    CHECK(!winome::is_url_like_query("notepad"));

    CHECK(winome::is_path_like_query("C:\\Users\\Test"));
    CHECK(winome::is_path_like_query("D:/Projects"));
    CHECK(winome::is_path_like_query("~/Downloads"));
    CHECK(winome::is_path_like_query("%USERPROFILE%\\Desktop"));
    CHECK(!winome::is_path_like_query("settings"));
  }

  // 5. Dash workspace scope filtering (All Workspaces vs Current Workspace Only).
  {
    // Mode 1: All Workspaces (isolate=false).
    CHECK(winome::should_include_dash_window(false, true, 0, 0, true, 0));
    // Shell-cloaked (0x2) window on Workspace 2 MUST appear in All Workspaces mode.
    CHECK(winome::should_include_dash_window(false, true, 0, 1, false, 0x2u));
    // Suspended UWP (DWM_CLOAKED_APP = 0x1) or invalid desktop (-1) stays hidden.
    CHECK(!winome::should_include_dash_window(false, true, 0, 0, false, 0x1u));
    CHECK(!winome::should_include_dash_window(false, true, 0, -1, false, 0x2u));

    // Mode 2: Current Workspace Only (isolate=true).
    // On Workspace 1 (active=0): ws0 included, ws1 excluded (even if stale on_current=true).
    CHECK(winome::should_include_dash_window(true, true, 0, 0, true, 0));
    CHECK(!winome::should_include_dash_window(true, true, 0, 0, true, 0x1u));
    CHECK(!winome::should_include_dash_window(true, true, 0, 1, true, 0x2u));
    // After scrolling to Workspace 2 (active=1): ws1 included, ws0 excluded.
    CHECK(!winome::should_include_dash_window(true, true, 1, 0, false, 0x2u));
    CHECK(winome::should_include_dash_window(true, true, 1, 1, false, 0x2u));
    // Pinned-to-all-desktops window (window_desktop=-1, on_current=true, uncloaked).
    CHECK(winome::should_include_dash_window(true, true, 1, -1, true, 0));
  }

  // 6. Overview type-to-search & IME composition routing helpers.
  {
    CHECK(winome::is_overview_ime_activation_message(WM_IME_STARTCOMPOSITION, 0));
    CHECK(winome::is_overview_ime_activation_message(WM_IME_COMPOSITION, 0));
    CHECK(winome::is_overview_ime_activation_message(WM_KEYDOWN, VK_PROCESSKEY));
    CHECK(!winome::is_overview_ime_activation_message(WM_IME_SETCONTEXT, 0));
    CHECK(!winome::is_overview_ime_activation_message(WM_KEYDOWN, 'A'));
    CHECK(!winome::is_overview_ime_activation_message(WM_KEYUP, VK_PROCESSKEY));

    const GdkModifierType none = static_cast<GdkModifierType>(0);
    CHECK(winome::should_overview_capture_search_key(GDK_KEY_a, none, false, false));
    CHECK(winome::should_overview_capture_search_key(
        GDK_KEY_A, GDK_SHIFT_MASK, false, false));
    CHECK(winome::should_overview_capture_search_key(
        gdk_unicode_to_keyval(0x4E2D), none, false, false));
    CHECK(winome::should_overview_capture_search_key(
        GDK_KEY_v, GDK_CONTROL_MASK, false, false));
    CHECK(winome::should_overview_capture_search_key(
        GDK_KEY_space, none, true, false));
    CHECK(!winome::should_overview_capture_search_key(
        GDK_KEY_space, none, false, false));
    CHECK(!winome::should_overview_capture_search_key(
        GDK_KEY_Escape, none, false, false));
    CHECK(!winome::should_overview_capture_search_key(
        GDK_KEY_Return, none, false, false));
    CHECK(!winome::should_overview_capture_search_key(
        GDK_KEY_Tab, none, false, false));
    CHECK(!winome::should_overview_capture_search_key(
        GDK_KEY_f, GDK_CONTROL_MASK, false, false));
    CHECK(!winome::should_overview_capture_search_key(
        GDK_KEY_a, GDK_ALT_MASK, false, false));

    CHECK(winome::should_overview_consume_dormant_key(
        GDK_KEY_space, none, true, false, false));
    CHECK(winome::should_overview_consume_dormant_key(
        GDK_KEY_BackSpace, none, true, false, false));
    CHECK(winome::should_overview_consume_dormant_key(
        GDK_KEY_Delete, none, true, false, false));
    CHECK(!winome::should_overview_consume_dormant_key(
        GDK_KEY_a, none, true, false, false));
    CHECK(!winome::should_overview_consume_dormant_key(
        GDK_KEY_Tab, none, true, false, false));
    CHECK(!winome::should_overview_consume_dormant_key(
        GDK_KEY_space, none, false, false, false));
    CHECK(!winome::should_overview_consume_dormant_key(
        GDK_KEY_space, none, true, true, false));
    CHECK(!winome::should_overview_consume_dormant_key(
        GDK_KEY_BackSpace, none, true, false, true));
  }
}

void test_live_wallpaper_helpers() {
  // 1. Mode sanitization.
  CHECK(winome::sanitize_live_wallpaper_mode("live") == "live");
  CHECK(winome::sanitize_live_wallpaper_mode("snapshot") == "snapshot");
  CHECK(winome::sanitize_live_wallpaper_mode("off") == "off");
  CHECK(winome::sanitize_live_wallpaper_mode("") == "live");
  CHECK(winome::sanitize_live_wallpaper_mode("auto") == "live");

  // 2. Engine child window detection filter.
  const RECT mon{0, 0, 1920, 1080};
  const RECT full{0, 0, 1920, 1080};
  const RECT small{0, 0, 400, 300};
  const RECT off_mon{1920, 0, 3840, 1080};

  // Wallpaper Engine / Lively / Sucrose covering monitor -> accepted.
  CHECK(winome::is_live_wallpaper_candidate(L"WPWindow", L"wallpaper64.exe", full, mon, true));
  CHECK(winome::is_live_wallpaper_candidate(L"Chrome_WidgetWin_1", L"Lively.Player.exe", full, mon, true));
  CHECK(winome::is_live_wallpaper_candidate(L"SDL_app", L"Sucrose.Player.exe", full, mon, true));

  // Built-in shell / Explorer / WINOME / hidden / tiny / other-monitor -> rejected.
  CHECK(!winome::is_live_wallpaper_candidate(L"SHELLDLL_DefView", L"wallpaper64.exe", full, mon, true));
  CHECK(!winome::is_live_wallpaper_candidate(L"WorkerW", L"wallpaper64.exe", full, mon, true));
  CHECK(!winome::is_live_wallpaper_candidate(L"WPWindow", L"explorer.exe", full, mon, true));
  CHECK(!winome::is_live_wallpaper_candidate(L"WPWindow", L"winome.exe", full, mon, true));
  CHECK(!winome::is_live_wallpaper_candidate(L"WPWindow", L"", full, mon, true));
  CHECK(!winome::is_live_wallpaper_candidate(L"WPWindow", L"wallpaper64.exe", full, mon, false));
  CHECK(!winome::is_live_wallpaper_candidate(L"WPWindow", L"wallpaper64.exe", small, mon, true));
  CHECK(!winome::is_live_wallpaper_candidate(L"WPWindow", L"wallpaper64.exe", off_mon, mon, true));

  // 3. Multi-monitor WGC crop rect computation.
  // Single monitor: full texture.
  {
    RECT c = winome::compute_wgc_crop_rect(RECT{0, 0, 1920, 1080},
                                           RECT{0, 0, 1920, 1080}, 1920, 1080);
    CHECK(c.left == 0 && c.top == 0 && c.right == 1920 && c.bottom == 1080);
  }
  // Dual monitor with left secondary at [-1920, 0] and primary at [0, 0].
  {
    RECT host{-1920, 0, 1920, 1080};
    RECT c_prim = winome::compute_wgc_crop_rect(host, RECT{0, 0, 1920, 1080}, 3840, 1080);
    CHECK(c_prim.left == 1920 && c_prim.top == 0 && c_prim.right == 3840 && c_prim.bottom == 1080);
    RECT c_left = winome::compute_wgc_crop_rect(host, RECT{-1920, 0, 0, 1080}, 3840, 1080);
    CHECK(c_left.left == 0 && c_left.top == 0 && c_left.right == 1920 && c_left.bottom == 1080);
  }
  // Scaled texture (e.g. 2x backing texture vs logical host rect).
  {
    RECT c = winome::compute_wgc_crop_rect(RECT{0, 0, 1920, 1080},
                                           RECT{960, 0, 1920, 1080}, 3840, 2160);
    CHECK(c.left == 1920 && c.top == 0 && c.right == 3840 && c.bottom == 2160);
  }
}

void test_panel_overview_and_workspace_dots() {
  // 1. Upstream GNOME width-multiplier tiers.
  CHECK(std::fabs(winome::workspace_dot_width_multiplier(1) - 3.625) < 1e-6);
  CHECK(std::fabs(winome::workspace_dot_width_multiplier(2) - 3.625) < 1e-6);
  CHECK(std::fabs(winome::workspace_dot_width_multiplier(3) - 3.25) < 1e-6);
  CHECK(std::fabs(winome::workspace_dot_width_multiplier(5) - 3.25) < 1e-6);
  CHECK(std::fabs(winome::workspace_dot_width_multiplier(6) - 2.75) < 1e-6);

  // 2. Continuous morphing preserves invariant total width (zero layout jitter).
  for (int count : {2, 3, 6}) {
    double base_total = 0.0;
    auto m0 = winome::compute_workspace_dot_metrics(count, 0.0, 8, 5, &base_total);
    CHECK(static_cast<int>(m0.size()) == count);
    CHECK(std::fabs(m0[0].expansion - 1.0) < 1e-6);
    CHECK(std::fabs(m0[0].opacity - 1.0) < 1e-6);
    CHECK(std::fabs(m0[0].draw_h - 8.0) < 1e-6);
    CHECK(std::fabs(m0[1].expansion - 0.0) < 1e-6);
    CHECK(std::fabs(m0[1].opacity - 0.5) < 1e-6);
    CHECK(std::fabs(m0[1].draw_h - 6.0) < 1e-6);  // 8 * 0.75

    for (double pos : {0.25, 0.5, 0.75, 1.0, static_cast<double>(count - 1)}) {
      double cur_total = 0.0;
      auto mc = winome::compute_workspace_dot_metrics(count, pos, 8, 5, &cur_total);
      CHECK(std::fabs(cur_total - base_total) < 1e-6);
    }
  }

  // Midpoint between workspace 0 and 1: symmetric expansion/opacity/size.
  {
    auto mid = winome::compute_workspace_dot_metrics(2, 0.5, 8, 5);
    CHECK(std::fabs(mid[0].expansion - 0.5) < 1e-6);
    CHECK(std::fabs(mid[1].expansion - 0.5) < 1e-6);
    CHECK(std::fabs(mid[0].opacity - 0.75) < 1e-6);
    CHECK(std::fabs(mid[1].opacity - 0.75) < 1e-6);
    CHECK(std::fabs(mid[0].slot_w - mid[1].slot_w) < 1e-6);
  }

  // 3. Top bar backdrop (#000000 <-> #222226) and #panelActivities pill alpha.
  {
    double r = 0, g = 0, b = 0, a = 0;
    // Desktop resting.
    winome::compute_panel_overview_colors(0.0, 0.0, false, &r, &g, &b, &a);
    CHECK(std::fabs(r) < 1e-6 && std::fabs(g) < 1e-6 && std::fabs(b) < 1e-6);
    CHECK(std::fabs(a - 0.0) < 1e-6);

    // Desktop hovered.
    winome::compute_panel_overview_colors(0.0, 1.0, false, &r, &g, &b, &a);
    CHECK(std::fabs(a - 0.17) < 1e-6);

    // Overview resting (#222226 = 34,34,38; alpha = 0.28).
    winome::compute_panel_overview_colors(1.0, 0.0, false, &r, &g, &b, &a);
    CHECK(std::fabs(r - 34.0 / 255.0) < 1e-6);
    CHECK(std::fabs(g - 34.0 / 255.0) < 1e-6);
    CHECK(std::fabs(b - 38.0 / 255.0) < 1e-6);
    CHECK(std::fabs(a - 0.28) < 1e-6);

    // Overview hovered (alpha = 0.32).
    winome::compute_panel_overview_colors(1.0, 1.0, false, &r, &g, &b, &a);
    CHECK(std::fabs(a - 0.32) < 1e-6);

    // Mid-transition (p=0.5, no hover): smooth midpoint.
    winome::compute_panel_overview_colors(0.5, 0.0, false, &r, &g, &b, &a);
    CHECK(std::fabs(r - 17.0 / 255.0) < 1e-6);
    CHECK(std::fabs(b - 19.0 / 255.0) < 1e-6);
    CHECK(std::fabs(a - 0.14) < 1e-6);
  }
}

void test_wpn_notifications_and_wifi_bt_sorting() {
  // 1. WPN toast XML parsing + entity decoding + attribution skip.
  {
    const std::string xml =
        "<toast><visual><binding template=\"ToastGeneric\">"
        "<text>Antigravity &amp; Build</text>"
        "<text>Done &lt;OK&gt; &#x4E2D;&#65;</text>"
        "<text placement=\"attribution\">via Windows</text>"
        "<text>Line 2 &quot;ready&quot;</text>"
        "</binding></visual></toast>";
    std::string title, body;
    CHECK(winome::parse_wpn_toast_xml(xml, &title, &body));
    CHECK(title == "Antigravity & Build");
    CHECK(body == "Done <OK> \xE4\xB8\xAD" "A\nLine 2 \"ready\"");

    std::string empty_t, empty_b;
    CHECK(!winome::parse_wpn_toast_xml("<toast><visual/></toast>", &empty_t, &empty_b));
  }

  // 2. WPN handler PrimaryId -> friendly app name formatting.
  {
    CHECK(winome::friendly_app_name_from_wpn_id("electron.app.Antigravity") == "Antigravity");
    CHECK(winome::friendly_app_name_from_wpn_id(
              "Microsoft.OutlookForWindows_8wekyb3d8bbwe!Microsoft.OutlookforWindows") ==
          "Outlook");
    CHECK(winome::friendly_app_name_from_wpn_id("C:\\Program Files\\WeChat\\WeChat.exe") ==
          "WeChat");
    CHECK(winome::friendly_app_name_from_wpn_id("") == "Windows");
  }

  // 3. Wi-Fi network sorting: connected first, then signal descending, then SSID.
  {
    WinomeWifiNetwork nets[4] = {};
    std::strcpy(nets[0].ssid, "Bravo");
    nets[0].signal_quality = 80;
    nets[0].connected = FALSE;

    std::strcpy(nets[1].ssid, "Alpha");
    nets[1].signal_quality = 80;
    nets[1].connected = FALSE;

    std::strcpy(nets[2].ssid, "WeakHome");
    nets[2].signal_quality = 45;
    nets[2].connected = TRUE;

    std::strcpy(nets[3].ssid, "StrongCafe");
    nets[3].signal_quality = 95;
    nets[3].connected = FALSE;

    winome_wifi_sort_networks(nets, 4);
    CHECK(std::strcmp(nets[0].ssid, "WeakHome") == 0);
    CHECK(std::strcmp(nets[1].ssid, "StrongCafe") == 0);
    CHECK(std::strcmp(nets[2].ssid, "Alpha") == 0);
    CHECK(std::strcmp(nets[3].ssid, "Bravo") == 0);
  }

  // 4. Bluetooth device sorting: connected first, then paired, then nearby.
  {
    winome::BluetoothDevice devs[4] = {};
    std::strcpy(devs[0].name, "Nearby Speaker");
    devs[0].connected = false;
    devs[0].paired = false;

    std::strcpy(devs[1].name, "Paired Keyboard");
    devs[1].connected = false;
    devs[1].paired = true;

    std::strcpy(devs[2].name, "AirPods");
    devs[2].connected = true;
    devs[2].paired = true;

    std::strcpy(devs[3].name, "Alpha Mouse");
    devs[3].connected = false;
    devs[3].paired = true;

    winome::sort_bluetooth_devices(devs, 4);
    CHECK(std::strcmp(devs[0].name, "AirPods") == 0);
    CHECK(std::strcmp(devs[1].name, "Alpha Mouse") == 0);
    CHECK(std::strcmp(devs[2].name, "Paired Keyboard") == 0);
    CHECK(std::strcmp(devs[3].name, "Nearby Speaker") == 0);
  }
}

void test_power_modes_overlay_and_legacy() {
  const char* kBalanced = "381b4222-f694-41f0-9685-ff5bb260df2e";
  const char* kBalancedRand = "b9a9a03a-5b6d-40a4-8656-132d3c462975";
  const char* kLegacySaver = "a1841308-3541-4fab-bc81-f71556f20b4a";
  const char* kLegacyPerf = "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c";
  const char* kLegacyUltimate = "e9a42b02-d5df-448d-aa00-03f14749eb61";
  const char* kOverlaySaver = "961cc777-2547-4f9d-8174-7d86181b8a7a";
  const char* kOverlayNull = "00000000-0000-0000-0000-000000000000";
  const char* kOverlayBetterPerf = "3af9b8d9-7c97-431d-ad78-34a85fe439fb";
  const char* kOverlayBestPerf = "ded574b5-45a0-4f42-8737-46345c09c238";

  // 1. Modern Windows 10/11 Balanced base + overlay power modes.
  CHECK(winome_power_mode_from_guid_strings(kBalanced, kOverlaySaver) ==
        WINOME_POWER_SAVER);
  CHECK(winome_power_mode_from_guid_strings(kBalanced, kOverlayNull) ==
        WINOME_POWER_BALANCED);
  CHECK(winome_power_mode_from_guid_strings(kBalanced, kBalanced) ==
        WINOME_POWER_BALANCED);
  CHECK(winome_power_mode_from_guid_strings(kBalanced, kOverlayBestPerf) ==
        WINOME_POWER_PERFORMANCE);
  CHECK(winome_power_mode_from_guid_strings(kBalanced, kOverlayBetterPerf) ==
        WINOME_POWER_PERFORMANCE);
  CHECK(winome_power_mode_from_guid_strings(kBalancedRand, kOverlayBestPerf) ==
        WINOME_POWER_PERFORMANCE);
  CHECK(winome_power_mode_from_guid_strings(kBalanced, nullptr) ==
        WINOME_POWER_BALANCED);

  // 2. Explicit legacy non-Balanced schemes override null/balanced overlay.
  CHECK(winome_power_mode_from_guid_strings(kLegacySaver, kOverlayNull) ==
        WINOME_POWER_SAVER);
  CHECK(winome_power_mode_from_guid_strings(kLegacyPerf, kOverlayNull) ==
        WINOME_POWER_PERFORMANCE);
  CHECK(winome_power_mode_from_guid_strings(kLegacyUltimate, nullptr) ==
        WINOME_POWER_PERFORMANCE);

  // 3. Invalid base GUID returns sentinel; unknown OEM GUID falls back safely.
  CHECK(winome_power_mode_from_guid_strings("not-a-guid", kOverlayNull) ==
        WINOME_POWER_MODE_COUNT);
  CHECK(winome_power_mode_from_guid_strings(
            "11111111-2222-3333-4444-555555555555", nullptr) ==
        WINOME_POWER_BALANCED);

  // 4. Read-only host sanity: active mode is valid and all 3 modes have icons.
  WinomePowerMode active = winome_power_get_active();
  CHECK(active < WINOME_POWER_MODE_COUNT);
  CHECK(winome_power_mode_available(WINOME_POWER_BALANCED) == TRUE);
  CHECK(std::strcmp(winome_power_mode_icon(WINOME_POWER_SAVER),
                    "power-profile-power-saver-symbolic") == 0);
  CHECK(std::strcmp(winome_power_mode_icon(WINOME_POWER_BALANCED),
                    "power-profile-balanced-symbolic") == 0);
  CHECK(std::strcmp(winome_power_mode_icon(WINOME_POWER_PERFORMANCE),
                    "power-profile-performance-symbolic") == 0);
}

void test_notification_activation_xml_and_theme_modes() {
  // 1. Protocol toast XML parsing.
  {
    const std::string xml =
        "<toast launch=\"ms-settings:windowsupdate\" activationType=\"protocol\">"
        "<visual><binding template=\"ToastGeneric\">"
        "<text>Windows Update</text>"
        "<text>Restart required</text>"
        "</binding></visual></toast>";
    winome::WinomeSystemNotification parsed;
    CHECK(winome::parse_wpn_toast_xml_full(xml, &parsed));
    CHECK(parsed.title == "Windows Update");
    CHECK(parsed.body == "Restart required");
    CHECK(parsed.launch_args == "ms-settings:windowsupdate");
    CHECK(parsed.activation_type == "protocol");
    CHECK(parsed.url == "ms-settings:windowsupdate");
    CHECK(parsed.actions.empty());
  }

  // 2. Foreground toast with action buttons, entity unescaping, and filtering.
  {
    const std::string xml =
        "<toast launch=\"action=view&amp;id=42\" activationType=\"foreground\">"
        "<visual><binding template=\"ToastGeneric\">"
        "<text>Mail</text>"
        "<text>Check https://example.com/msg?a=1&amp;b=2.</text>"
        "</binding></visual>"
        "<actions>"
        "<action content=\"Open Link\" arguments=\"https://example.com/open\" activationType=\"protocol\"/>"
        "<action content=\"Reply\" arguments=\"reply=42\" activationType=\"foreground\"/>"
        "<action content=\"Snooze\" arguments=\"snooze\" activationType=\"system\"/>"
        "<action content=\"Settings\" arguments=\"cfg\" placement=\"contextMenu\"/>"
        "</actions></toast>";
    winome::WinomeSystemNotification parsed;
    CHECK(winome::parse_wpn_toast_xml_full(xml, &parsed));
    CHECK(parsed.launch_args == "action=view&id=42");
    CHECK(parsed.activation_type == "foreground");
    CHECK(parsed.url == "https://example.com/msg?a=1&b=2");
    CHECK(parsed.actions.size() == 2);
    CHECK(parsed.actions[0].label == "Open Link");
    CHECK(parsed.actions[0].arguments == "https://example.com/open");
    CHECK(parsed.actions[0].activation_type == "protocol");
    CHECK(parsed.actions[1].label == "Reply");
    CHECK(parsed.actions[1].arguments == "reply=42");
    CHECK(parsed.actions[1].activation_type == "foreground");
  }

  // 3. Protocol URI validation blocks unsafe schemes and non-URI launch args.
  CHECK(winome::is_safe_protocol_uri("https://gnome.org", ""));
  CHECK(winome::is_safe_protocol_uri("ms-settings:colors", ""));
  CHECK(winome::is_safe_protocol_uri("vscode://file/test", ""));
  CHECK(winome::is_safe_protocol_uri("myapp:open?id=1", "protocol"));
  CHECK(!winome::is_safe_protocol_uri("file:///C:/Windows/win.ini", "protocol"));
  CHECK(!winome::is_safe_protocol_uri("javascript:alert(1)", "protocol"));
  CHECK(!winome::is_safe_protocol_uri("powershell:calc", "protocol"));
  CHECK(!winome::is_safe_protocol_uri("conversationId=9813", "foreground"));

  // 4. Fine-grained App / System dark mode query works read-only.
  bool apps_dark = false;
  bool sys_dark = false;
  CHECK(winome::get_theme_modes(&apps_dark, &sys_dark));
}

void test_multi_monitor_interaction_helpers() {
  // 1. Multi-monitor hot-corner hit testing (primary, right-extended, left-negative).
  const winome::HotCornerMonitor mons[3] = {
      {reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(0x1)), RECT{0, 0, 1920, 1080}},
      {reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(0x2)), RECT{1920, 0, 3840, 1080}},
      {reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(0x3)), RECT{-1920, 100, 0, 1180}},
  };
  CHECK(winome::hot_corner_hit_test_monitors(0, 0, mons, 3, 5) == 0);
  CHECK(winome::hot_corner_hit_test_monitors(5, 5, mons, 3, 5) == 0);
  CHECK(winome::hot_corner_hit_test_monitors(6, 5, mons, 3, 5) == -1);
  CHECK(winome::hot_corner_hit_test_monitors(1920, 0, mons, 3, 5) == 1);
  CHECK(winome::hot_corner_hit_test_monitors(1924, 4, mons, 3, 5) == 1);
  CHECK(winome::hot_corner_hit_test_monitors(1926, 0, mons, 3, 5) == -1);
  CHECK(winome::hot_corner_hit_test_monitors(-1920, 100, mons, 3, 5) == 2);
  CHECK(winome::hot_corner_hit_test_monitors(-1915, 105, mons, 3, 5) == 2);
  CHECK(winome::hot_corner_hit_test_monitors(-1920, 99, mons, 3, 5) == -1);
  CHECK(winome::hot_corner_hit_test_monitors(0, 0, nullptr, 0, 5) == -1);

  // 2. Active monitor selection by pointer coordinates.
  const std::vector<RECT> rects = {
      RECT{0, 0, 1920, 1080},
      RECT{1920, 0, 3840, 1080},
      RECT{-1920, 100, 0, 1180},
  };
  CHECK(winome::select_monitor_index_for_point(rects, 500, 500) == 0);
  CHECK(winome::select_monitor_index_for_point(rects, 2500, 300) == 1);
  CHECK(winome::select_monitor_index_for_point(rects, -500, 600) == 2);
  CHECK(winome::select_monitor_index_for_point(rects, -500, 50) == -1);

  // 3. Popover positioning clamps inside secondary monitors (never snaps to primary).
  {
    const RECT right_mon{1920, 0, 3840, 1080};
    const RECT clock_anchor{2800, 0, 2960, 32};
    const POINT cal = winome::clamp_popover_origin_to_monitor(
        right_mon, 32, clock_anchor, 640, 420, true, 2, 12, 8);
    CHECK(cal.x == (2800 + 2960) / 2 - 320);
    CHECK(cal.y == 34);
    CHECK(cal.x >= right_mon.left + 8 && cal.x + 640 <= right_mon.right - 8);

    const RECT qs_anchor{3700, 0, 3840, 32};
    const POINT qs = winome::clamp_popover_origin_to_monitor(
        right_mon, 32, qs_anchor, 400, 500, false, 2, 12, 8);
    CHECK(qs.x == 3840 - 400 - 12);
    CHECK(qs.y == 34);
  }
  {
    const RECT left_mon{-1920, 0, 0, 1080};
    const RECT clock_anchor{-1020, 0, -900, 32};
    const POINT cal = winome::clamp_popover_origin_to_monitor(
        left_mon, 32, clock_anchor, 640, 420, true, 2, 12, 8);
    CHECK(cal.x == (-1020 + -900) / 2 - 320);
    CHECK(cal.y == 34);
    CHECK(cal.x >= left_mon.left + 8 && cal.x + 640 <= left_mon.right - 8);

    const POINT qs = winome::clamp_popover_origin_to_monitor(
        left_mon, 32, RECT{-120, 0, 0, 32}, 400, 500, false, 2, 12, 8);
    CHECK(qs.x == 0 - 400 - 12);
    CHECK(qs.y == 34);
  }

  // 4. Modal/Run dialog centering on extended monitor workareas.
  {
    const RECT ext_wa{1920, 32, 3840, 1080};
    const POINT centered = winome::center_rect_in_bounds(ext_wa, 420, 220, 0.5);
    CHECK(centered.x == 1920 + (1920 - 420) / 2);
    CHECK(centered.y == 32 + (1048 - 220) / 2);

    const POINT upper_third =
        winome::center_rect_in_bounds(ext_wa, 500, 140, 1.0 / 3.0);
    CHECK(upper_third.x == 1920 + (1920 - 500) / 2);
    CHECK(upper_third.y == 32 + static_cast<int>((1048 - 140) * (1.0 / 3.0)));
  }
}

void test_independent_per_monitor_workspace_engine() {
  // 1. Config roundtrip & sanitization for multi_monitor_workspace_mode.
  {
    winome::WinomeConfig cfg;
    CHECK(cfg.multi_monitor_workspace_mode == "independent");
    cfg.multi_monitor_workspace_mode = "primary_only";
    winome::WinomeConfig parsed;
    CHECK(winome::parse_config(winome::serialize_config(cfg), &parsed));
    CHECK(parsed.multi_monitor_workspace_mode == "primary_only");

    cfg.multi_monitor_workspace_mode = "span";
    CHECK(winome::parse_config(winome::serialize_config(cfg), &parsed));
    CHECK(parsed.multi_monitor_workspace_mode == "span");

    winome::WinomeConfig bad;
    CHECK(winome::parse_config("{\"multi_monitor_workspace_mode\":\"bogus\"}", &bad));
    CHECK(bad.multi_monitor_workspace_mode == "independent");
  }

  // 2. Core IndependentWorkspaceModel: Monitor 1 switching 1A -> 1B NEVER touches Monitor 2!
  constexpr uintptr_t kMon1 = 0x101;
  constexpr uintptr_t kMon2 = 0x202;
  constexpr uintptr_t kWin1A = 0xA001;
  constexpr uintptr_t kWin1B = 0xA002;
  constexpr uintptr_t kWin2A = 0xB001;
  constexpr uintptr_t kWin2B = 0xB002;

  winome::ws::IndependentWorkspaceModel model;
  model.ensure_monitor(kMon1);
  model.ensure_monitor(kMon2);
  model.reconcile({
      {kWin1A, kMon1, false},
      {kWin2A, kMon2, false},
  });
  CHECK(model.state_for(kMon1).active == 0 && model.state_for(kMon1).count == 2);
  CHECK(model.state_for(kMon2).active == 0 && model.state_for(kMon2).count == 2);

  // Place kWin1B on Monitor 1 workspace 1 (1B) and kWin2B on Monitor 2 workspace 1 (2B).
  auto mv1 = model.move_window(kWin1B, kMon1, 1);
  CHECK(mv1.size() == 1 && mv1[0].hwnd == kWin1B && mv1[0].native_desktop == 1);
  CHECK(model.state_for(kMon1).count == 3);  // dynamic trailing empty ws appended on Mon1
  CHECK(model.state_for(kMon2).count == 2);  // Mon2 untouched!

  auto mv2 = model.move_window(kWin2B, kMon2, 1);
  CHECK(mv2.size() == 1 && mv2[0].hwnd == kWin2B && mv2[0].native_desktop == 1);
  CHECK(model.state_for(kMon2).count == 3);

  // Switch Monitor 1: 1A (0) -> 1B (1). Must ONLY move kWin1A -> park(1) and kWin1B -> live(0).
  auto sw1 = model.switch_monitor(kMon1, 1);
  CHECK(model.state_for(kMon1).active == 1);
  CHECK(model.state_for(kMon2).active == 0);  // Monitor 2 stays on 2A!
  bool saw_1a_park = false, saw_1b_live = false, saw_mon2_touched = false;
  for (const auto& m : sw1) {
    if (m.hwnd == kWin1A && m.native_desktop == 1) saw_1a_park = true;
    if (m.hwnd == kWin1B && m.native_desktop == 0) saw_1b_live = true;
    if (m.hwnd == kWin2A || m.hwnd == kWin2B) saw_mon2_touched = true;
  }
  CHECK(saw_1a_park && saw_1b_live && !saw_mon2_touched);

  // Switch Monitor 2: 2A (0) -> 2B (1). Monitor 1 must stay on 1B and be untouched.
  auto sw2 = model.switch_monitor(kMon2, 1);
  CHECK(model.state_for(kMon1).active == 1);
  CHECK(model.state_for(kMon2).active == 1);
  bool saw_2a_park = false, saw_2b_live = false, saw_mon1_touched = false;
  for (const auto& m : sw2) {
    if (m.hwnd == kWin2A && m.native_desktop == 1) saw_2a_park = true;
    if (m.hwnd == kWin2B && m.native_desktop == 0) saw_2b_live = true;
    if (m.hwnd == kWin1A || m.hwnd == kWin1B) saw_mon1_touched = true;
  }
  CHECK(saw_2a_park && saw_2b_live && !saw_mon1_touched);

  // Reconcile dragging live window kWin1B from Monitor 1 onto Monitor 2:
  // adopts Monitor 2's active workspace (1).
  model.reconcile({
      {kWin1A, kMon1, true},
      {kWin1B, kMon2, false},
      {kWin2A, kMon2, true},
      {kWin2B, kMon2, false},
  });
  winome::ws::WindowPlacement wp{};
  CHECK(model.find_window(kWin1B, &wp) && wp.monitor == kMon2 && wp.workspace == 1);

  // Shutdown restore plan returns ALL parked windows (kWin1A and kWin2A) to Live Stage (0).
  auto restore = model.build_restore_all_to_live_moves();
  CHECK(restore.size() == 2);
  bool r_1a = false, r_2a = false;
  for (const auto& m : restore) {
    CHECK(m.native_desktop == 0);
    if (m.hwnd == kWin1A) r_1a = true;
    if (m.hwnd == kWin2A) r_2a = true;
  }
  CHECK(r_1a && r_2a);

  // 3. Simultaneous Secondary Overview geometry (1-workspace & multi-workspace).
  {
    const auto g1 = winome::compute_secondary_overview_geometry(1920, 1080, 32, 1, 1);
    CHECK(g1.full_box.left == 0 && g1.full_box.top == 32 &&
          g1.full_box.right == 1920 && g1.full_box.bottom == 1080);
    CHECK(g1.pill_rects.empty());
    CHECK(g1.picker_box.left > 0 && g1.picker_box.right < 1920);
    CHECK(g1.picker_box.top > 32 && g1.picker_box.bottom < 1080);
    const double expected_aspect = 1920.0 / 1048.0;
    const double actual_aspect =
        static_cast<double>(g1.picker_box.right - g1.picker_box.left) /
        static_cast<double>(g1.picker_box.bottom - g1.picker_box.top);
    CHECK(std::fabs(actual_aspect - expected_aspect) < 0.03);

    const auto g3 = winome::compute_secondary_overview_geometry(2560, 1440, 48, 2, 3);
    CHECK(g3.pill_rects.size() == 3);
    CHECK(g3.pill_rects[0].right < g3.pill_rects[1].left);
    CHECK(g3.pill_rects[1].right < g3.pill_rects[2].left);
    CHECK(g3.pill_rects[0].bottom < g3.picker_box.top);
    const RECT left_nb = winome::compute_secondary_workspace_box(g3, 0, 1);
    const RECT mid_nb = winome::compute_secondary_workspace_box(g3, 1, 1);
    const RECT right_nb = winome::compute_secondary_workspace_box(g3, 2, 1);
    CHECK(left_nb.right < mid_nb.left);
    CHECK(right_nb.left > mid_nb.right);
  }

  // 4. Multi-monitor Overview linked vs unlinked simultaneous dispatch & config.
  {
    winome::WinomeConfig cfg;
    CHECK(cfg.multi_monitor_overview_linked == false);  // default: unlinked simultaneous
    cfg.multi_monitor_overview_linked = true;
    winome::WinomeConfig parsed;
    CHECK(winome::parse_config(winome::serialize_config(cfg), &parsed));
    CHECK(parsed.multi_monitor_overview_linked == true);

    using Act = winome::OverviewDispatchAction;
    // Unlinked (default): open Mon1 -> OpenMain; open Mon2 while Mon1 open -> OpenSecondary!
    CHECK(winome::decide_multi_monitor_overview_action(
              false, false, kMon1, false, 0, false, false) == Act::OpenMain);
    CHECK(winome::decide_multi_monitor_overview_action(
              false, false, kMon2, true, kMon1, false, false) == Act::OpenSecondary);
    // Unlinked: toggle Mon2 while both open -> CloseSecondaryOnly (Mon1 stays open!).
    CHECK(winome::decide_multi_monitor_overview_action(
              false, true, kMon2, true, kMon1, true, true) == Act::CloseSecondaryOnly);
    // Unlinked: toggle Mon1 while both open -> CloseMainOnly (Mon2 stays open!).
    CHECK(winome::decide_multi_monitor_overview_action(
              false, true, kMon1, true, kMon1, false, true) == Act::CloseMainOnly);
    // Unlinked: open Mon1 again while Mon2 secondary still open -> OpenMain on Mon1!
    CHECK(winome::decide_multi_monitor_overview_action(
              false, true, kMon1, false, 0, false, true) == Act::OpenMain);
    // Linked (optional): open Mon1 -> OpenLinkedAll; toggle Mon2 -> CloseAll!
    CHECK(winome::decide_multi_monitor_overview_action(
              true, false, kMon1, false, 0, false, false) == Act::OpenLinkedAll);
    CHECK(winome::decide_multi_monitor_overview_action(
              true, true, kMon2, true, kMon1, true, true) == Act::CloseAll);
    CHECK(winome::decide_multi_monitor_overview_action(
              true, false, kMon2, true, kMon1, true, true) == Act::None);

    // CloseAll secondary-closing guard (never strands unlinked/orphan hosts):
    CHECK(winome::should_close_secondary_on_close_all(false, true, true));
    CHECK(winome::should_close_secondary_on_close_all(false, false, true));
    CHECK(winome::should_close_secondary_on_close_all(true, false, true));
    CHECK(!winome::should_close_secondary_on_close_all(true, true, true));
    CHECK(!winome::should_close_secondary_on_close_all(false, true, false));

    // Secondary Overview shows its own bottom Dash in Independent and Span modes.
    CHECK(winome::should_secondary_overview_show_dash(
        winome::ws::MultiMonitorWorkspaceMode::Independent));
    CHECK(!winome::should_secondary_overview_show_dash(
        winome::ws::MultiMonitorWorkspaceMode::PrimaryOnly));
    CHECK(winome::should_secondary_overview_show_dash(
        winome::ws::MultiMonitorWorkspaceMode::Span));

    const auto sec_no_dash = winome::compute_secondary_overview_geometry(
        1920, 1080, 32, 1, 2, 0);
    const auto sec_with_dash = winome::compute_secondary_overview_geometry(
        1920, 1080, 32, 1, 2, 100);
    CHECK(sec_with_dash.picker_box.bottom <= 1080 - 114);
    CHECK(sec_with_dash.picker_box.bottom < sec_no_dash.picker_box.bottom);
  }

  // 4b. Unified per-monitor Overview role, unified dispatch & OS primary sync math.
  {
    using Mode = winome::ws::MultiMonitorWorkspaceMode;
    // Search & App Grid are ALWAYS enabled on every monitor in every mode.
    // Dash & multi-workspace are enabled everywhere EXCEPT non-primary in PrimaryOnly.
    const auto r_ind_p = winome::resolve_monitor_overview_role(Mode::Independent, true);
    const auto r_ind_s = winome::resolve_monitor_overview_role(Mode::Independent, false);
    CHECK(r_ind_p.show_search && r_ind_p.allow_app_grid && r_ind_p.show_dash && r_ind_p.multi_workspace);
    CHECK(r_ind_s.show_search && r_ind_s.allow_app_grid && r_ind_s.show_dash && r_ind_s.multi_workspace);

    const auto r_span_p = winome::resolve_monitor_overview_role(Mode::Span, true);
    const auto r_span_s = winome::resolve_monitor_overview_role(Mode::Span, false);
    CHECK(r_span_p.show_search && r_span_p.allow_app_grid && r_span_p.show_dash && r_span_p.multi_workspace);
    CHECK(r_span_s.show_search && r_span_s.allow_app_grid && r_span_s.show_dash && r_span_s.multi_workspace);

    const auto r_po_p = winome::resolve_monitor_overview_role(Mode::PrimaryOnly, true);
    const auto r_po_s = winome::resolve_monitor_overview_role(Mode::PrimaryOnly, false);
    CHECK(r_po_p.show_search && r_po_p.allow_app_grid && r_po_p.show_dash && r_po_p.multi_workspace);
    CHECK(r_po_s.show_search && r_po_s.allow_app_grid && !r_po_s.show_dash && !r_po_s.multi_workspace);

    using UAct = winome::UnifiedOverviewAction;
    // Unlinked simultaneous: each monitor toggles independently without closing others.
    CHECK(winome::decide_unified_overview_action(false, false, false, false) == UAct::OpenMonitor);
    CHECK(winome::decide_unified_overview_action(false, false, false, true) == UAct::OpenMonitor);
    CHECK(winome::decide_unified_overview_action(false, false, true, true) == UAct::None);
    CHECK(winome::decide_unified_overview_action(false, true, false, true) == UAct::OpenMonitor);
    CHECK(winome::decide_unified_overview_action(false, true, true, true) == UAct::CloseMonitor);
    // Linked: opens all monitors together and closes all together.
    CHECK(winome::decide_unified_overview_action(true, false, false, false) == UAct::OpenAllLinked);
    CHECK(winome::decide_unified_overview_action(true, false, true, true) == UAct::None);
    CHECK(winome::decide_unified_overview_action(true, true, false, false) == UAct::OpenAllLinked);
    CHECK(winome::decide_unified_overview_action(true, true, true, true) == UAct::CloseAll);
    CHECK(winome::decide_unified_overview_action(true, true, false, true) == UAct::CloseAll);

    // Windows OS primary display coordinate re-origin math:
    // Case 1: 2 monitors side-by-side (Mon1 at 0,0; Mon2 at +1920,0) -> promote Mon2.
    std::vector<winome::DisplayPositionSpec> two_right = {
        {L"\\\\.\\DISPLAY1", 0, 0, 1920, 1080, true},
        {L"\\\\.\\DISPLAY2", 1920, 0, 1920, 1080, false},
    };
    CHECK(winome::compute_os_primary_display_positions(&two_right, L"\\\\.\\DISPLAY2"));
    CHECK(two_right[0].device_name == L"\\\\.\\DISPLAY1" && two_right[0].x == -1920 && two_right[0].y == 0 && !two_right[0].is_primary);
    CHECK(two_right[1].device_name == L"\\\\.\\DISPLAY2" && two_right[1].x == 0 && two_right[1].y == 0 && two_right[1].is_primary);

    // Case 2: 2 monitors with left-negative secondary (-2560, 120) -> promote Mon2.
    std::vector<winome::DisplayPositionSpec> two_left = {
        {L"\\\\.\\DISPLAY1", 0, 0, 1920, 1080, true},
        {L"\\\\.\\DISPLAY2", -2560, 120, 2560, 1440, false},
    };
    CHECK(winome::compute_os_primary_display_positions(&two_left, L"\\\\.\\DISPLAY2"));
    CHECK(two_left[0].x == 2560 && two_left[0].y == -120 && !two_left[0].is_primary);
    CHECK(two_left[1].x == 0 && two_left[1].y == 0 && two_left[1].is_primary);

    // Case 3: 3 monitors L-shaped -> promote DISPLAY3, exact relative offsets preserved.
    std::vector<winome::DisplayPositionSpec> three_l = {
        {L"\\\\.\\DISPLAY1", 0, 0, 1920, 1080, true},
        {L"\\\\.\\DISPLAY2", 1920, 0, 1920, 1080, false},
        {L"\\\\.\\DISPLAY3", 1920, -1080, 1920, 1080, false},
    };
    CHECK(winome::compute_os_primary_display_positions(&three_l, L"\\\\.\\DISPLAY3"));
    CHECK(three_l[0].x == -1920 && three_l[0].y == 1080 && !three_l[0].is_primary);
    CHECK(three_l[1].x == 0 && three_l[1].y == 1080 && !three_l[1].is_primary);
    CHECK(three_l[2].x == 0 && three_l[2].y == 0 && three_l[2].is_primary);

    // Unknown device returns false cleanly.
    CHECK(!winome::compute_os_primary_display_positions(&three_l, L"\\\\.\\DISPLAY99"));
    CHECK(!winome::compute_os_primary_display_positions(&three_l, L""));

    // Stale desktop echo guard: suppresses delayed pre-switch poll echoing
    // `from` within 350ms, but NEVER blocks genuine switches to another desktop
    // or switches after the grace window (fixing Span + linked overview desync).
    CHECK(winome::should_ignore_stale_desktop_echo(0, 0, 1, 150000));
    CHECK(winome::should_ignore_stale_desktop_echo(1, 1, 2, 349999));
    CHECK(!winome::should_ignore_stale_desktop_echo(2, 0, 1, 100000));
    CHECK(!winome::should_ignore_stale_desktop_echo(0, 0, 1, 350000));
    CHECK(!winome::should_ignore_stale_desktop_echo(0, 0, 1, 650000));
    CHECK(!winome::should_ignore_stale_desktop_echo(0, -1, -1, 50000));
    CHECK(!winome::should_ignore_stale_desktop_echo(1, 1, 1, 50000));
  }

  // 5. Cross-monitor window drop geometry & IndependentWorkspaceModel transfer.
  {
    const RECT mon1_rc{0, 0, 1920, 1080};
    const RECT mon2_work{-1920, 32, 0, 1080};  // left-side monitor with negative X + panel
    const RECT src_win{200, 140, 1000, 740};   // 800x600 window on Mon1

    // 5a. Drop onto closed-overview monitor at exact cursor drop point (clamped inside workarea).
    const RECT at_pt = winome::ws::compute_cross_monitor_window_rect(
        src_win, mon1_rc, mon2_work, POINT{-1850, 40}, true);
    CHECK(at_pt.left >= mon2_work.left);
    CHECK(at_pt.top >= mon2_work.top);
    CHECK(at_pt.right <= mon2_work.right);
    CHECK(at_pt.bottom <= mon2_work.bottom);
    CHECK((at_pt.right - at_pt.left) == 800);
    CHECK((at_pt.bottom - at_pt.top) == 600);

    // 5b. Drop onto open-overview monitor (proportional mapping, oversized window shrinks to fit).
    const RECT huge_win{0, 0, 3000, 2000};
    const RECT small_work{1920, 32, 3200, 752};  // 1280x720 below panel
    const RECT mapped = winome::ws::compute_cross_monitor_window_rect(
        huge_win, mon1_rc, small_work, POINT{2500, 400}, false);
    CHECK(mapped.left >= small_work.left && mapped.right <= small_work.right);
    CHECK(mapped.top >= small_work.top && mapped.bottom <= small_work.bottom);
    CHECK((mapped.right - mapped.left) <= 1280);
    CHECK((mapped.bottom - mapped.top) <= 720);

    // 5c. IndependentWorkspaceModel cross-monitor move (Screen 1 ws B -> Screen 2 ws A).
    winome::ws::IndependentWorkspaceModel xm;
    constexpr uintptr_t kW1Base = 0x5100;
    constexpr uintptr_t kW1 = 0x5101;
    constexpr uintptr_t kW1Keep = 0x5102;
    constexpr uintptr_t kW2 = 0x5201;
    xm.reconcile({{kW1Base, kMon1, false},
                  {kW1, kMon1, false},
                  {kW1Keep, kMon1, false},
                  {kW2, kMon2, false}});
    // Keep kW1Base on Mon1 ws 0 (A), put kW1 + kW1Keep on Mon1 ws 1 (B, active).
    xm.move_window(kW1, kMon1, 1);
    xm.move_window(kW1Keep, kMon1, 1);
    xm.switch_monitor(kMon1, 1);
    CHECK(xm.state_for(kMon1).active == 1);
    CHECK(xm.state_for(kMon2).active == 0);
    // Drag kW1 from Mon1 ws 1 directly into Mon2 ws 0 (Mon2's active workspace):
    const auto xmoves = xm.move_window(kW1, kMon2, 0);
    CHECK(xm.window_workspace(kW1, kMon2) == 0);
    // Mon2 ws 0 is active, so kW1 must be on Live Stage (native desktop 0).
    CHECK(xmoves.size() == 1 && xmoves[0].hwnd == kW1 &&
          xmoves[0].native_desktop == 0);
    // Mon1 still has kW1Base on ws 0 and kW1Keep on ws 1, so Mon1 stays on active = 1.
    CHECK(xm.state_for(kMon1).active == 1);
    // Drag kW1 from Mon2 ws 0 into Mon1 ws 0 while Mon1 active is 1 -> parked (desktop 1).
    const auto xmoves2 = xm.move_window(kW1, kMon1, 0);
    CHECK(xm.window_workspace(kW1, kMon1) == 0);
    CHECK(xmoves2.size() == 1 && xmoves2[0].hwnd == kW1 &&
          xmoves2[0].native_desktop == 1);
    // Dragging the last remaining window kW1Keep out of Mon1 ws 1 -> Mon2 ws 0
    // prunes Mon1's extra trailing workspace (count 3 -> 2: ws 0 occupied + ws 1 empty).
    CHECK(xm.state_for(kMon1).count == 3);
    xm.move_window(kW1Keep, kMon2, 0);
    CHECK(xm.state_for(kMon1).count == 2);
  }
}

void test_diagnostics_zip_export() {
  wchar_t tmp_base[MAX_PATH] = {};
  CHECK(GetTempPathW(MAX_PATH, tmp_base) > 0);
  std::wstring root = std::wstring(tmp_base) + L"winome-diag-test-" +
                      std::to_wstring(GetCurrentProcessId());
  std::wstring crashes = root + L"\\crashes";
  CreateDirectoryW(root.c_str(), nullptr);
  CreateDirectoryW(crashes.c_str(), nullptr);

  auto write_raw = [](const std::wstring& path, const std::string& content) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      DWORD w = 0;
      WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &w, nullptr);
      CloseHandle(h);
    }
  };

  std::string sample_log;
  for (int i = 0; i < 40; ++i)
    sample_log += "[2026-11-24 23:30:00.000] [ws] independent switch ok line=" +
                  std::to_string(i) + "\n";
  write_raw(root + L"\\winome.log", sample_log);
  write_raw(root + L"\\config.json", "{\"language\":\"zh\",\"multi_monitor_workspace_mode\":\"independent\"}\n");
  write_raw(root + L"\\last-crash.json", "{\"reason\":\"test\"}\n");
  write_raw(crashes + L"\\dump-1.dmp", std::string(512, 'A'));
  write_raw(crashes + L"\\dump-2.dmp", std::string(512, 'B'));
  write_raw(crashes + L"\\dump-3.dmp", std::string(512, 'C'));

  const std::wstring zip_path = root + L"\\out.zip";
  winome::DiagnosticsExportOptions opts{};
  opts.data_dir = root;
  opts.output_zip_path = zip_path;
  opts.extra_summary_text = "UnitTestSummary=1";
  opts.extra_files.push_back({"debug/extra.txt", "hello-extra-payload"});
  opts.max_crash_dumps = 2;
  opts.flush_stdio = false;

  const auto res = winome::export_diagnostics_bundle(opts);
  CHECK(res.ok);
  // Expect: winome.log, config.json, last-crash.json, 2 dumps, debug/extra.txt, system-info.txt = 7 entries.
  CHECK(res.file_count == 7);
  CHECK(res.uncompressed_bytes > 0);
  CHECK(res.compressed_bytes > 0);
  CHECK(res.compressed_bytes < res.uncompressed_bytes);

  // Read back zip bytes and verify PKZIP Local File Headers, EOCD, CRC32 & raw Deflate round-trip.
  std::vector<uint8_t> raw;
  {
    HANDLE h = CreateFileW(zip_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) {
      LARGE_INTEGER sz{};
      GetFileSizeEx(h, &sz);
      raw.resize(static_cast<size_t>(sz.QuadPart));
      DWORD got = 0;
      ReadFile(h, raw.data(), static_cast<DWORD>(raw.size()), &got, nullptr);
      CloseHandle(h);
    }
  }
  CHECK(raw.size() > 64);

  auto rd16 = [&](size_t off) -> uint16_t {
    return static_cast<uint16_t>(raw[off] | (raw[off + 1] << 8));
  };
  auto rd32 = [&](size_t off) -> uint32_t {
    return static_cast<uint32_t>(raw[off]) |
           (static_cast<uint32_t>(raw[off + 1]) << 8) |
           (static_cast<uint32_t>(raw[off + 2]) << 16) |
           (static_cast<uint32_t>(raw[off + 3]) << 24);
  };

  // Verify EOCD at end of archive.
  CHECK(rd32(raw.size() - 22) == 0x06054b50u);
  CHECK(rd16(raw.size() - 12) == 7u);

  // Walk Local File Headers and inflate logs/winome.log + system-info.txt.
  size_t pos = 0;
  int found_entries = 0;
  int dump_entries = 0;
  bool verified_log = false;
  bool verified_sysinfo = false;
  while (pos + 30 <= raw.size() && rd32(pos) == 0x04034b50u) {
    const uint16_t method = rd16(pos + 8);
    const uint32_t expected_crc = rd32(pos + 14);
    const uint32_t comp_sz = rd32(pos + 18);
    const uint32_t uncomp_sz = rd32(pos + 22);
    const uint16_t name_len = rd16(pos + 26);
    const uint16_t extra_len = rd16(pos + 28);
    const std::string name(reinterpret_cast<const char*>(raw.data() + pos + 30),
                           name_len);
    const size_t data_off = pos + 30 + name_len + extra_len;
    CHECK(data_off + comp_sz <= raw.size());

    std::string decoded;
    if (method == 0) {
      decoded.assign(reinterpret_cast<const char*>(raw.data() + data_off), comp_sz);
    } else {
      CHECK(method == 8);
      decoded.resize(uncomp_sz);
      z_stream zs{};
      CHECK(inflateInit2(&zs, -MAX_WBITS) == Z_OK);
      zs.next_in = const_cast<Bytef*>(raw.data() + data_off);
      zs.avail_in = comp_sz;
      zs.next_out = reinterpret_cast<Bytef*>(decoded.data());
      zs.avail_out = uncomp_sz;
      CHECK(inflate(&zs, Z_FINISH) == Z_STREAM_END);
      inflateEnd(&zs);
    }
    const uint32_t actual_crc = static_cast<uint32_t>(
        crc32(crc32(0L, Z_NULL, 0),
              reinterpret_cast<const Bytef*>(decoded.data()),
              static_cast<uInt>(decoded.size())));
    CHECK(actual_crc == expected_crc);

    if (name == "logs/winome.log") {
      CHECK(decoded == sample_log);
      verified_log = true;
    } else if (name == "system-info.txt") {
      CHECK(decoded.find("UnitTestSummary=1") != std::string::npos);
      CHECK(decoded.find(WINOME_VERSION) != std::string::npos);
      verified_sysinfo = true;
    } else if (name.rfind("crashes/", 0) == 0 && name.find(".dmp") != std::string::npos) {
      ++dump_entries;
    }

    pos = data_off + comp_sz;
    ++found_entries;
  }
  CHECK(found_entries == 7);
  CHECK(dump_entries == 2);
  CHECK(verified_log);
  CHECK(verified_sysinfo);

  DeleteFileW(zip_path.c_str());
  DeleteFileW((root + L"\\winome.log").c_str());
  DeleteFileW((root + L"\\config.json").c_str());
  DeleteFileW((root + L"\\last-crash.json").c_str());
  DeleteFileW((crashes + L"\\dump-1.dmp").c_str());
  DeleteFileW((crashes + L"\\dump-2.dmp").c_str());
  DeleteFileW((crashes + L"\\dump-3.dmp").c_str());
  RemoveDirectoryW(crashes.c_str());
  RemoveDirectoryW(root.c_str());
}

}  // namespace

namespace {
constexpr const wchar_t* kTestGuardProp = L"WINOME_TestGuardOrigProc";
int g_test_guard_calls = 0;

LRESULT CALLBACK test_guarded_subclass_proc(HWND hwnd,
                                            UINT msg,
                                            WPARAM wParam,
                                            LPARAM lParam) {
  ++g_test_guard_calls;
  WNDPROC orig = reinterpret_cast<WNDPROC>(GetPropW(hwnd, kTestGuardProp));
  if (msg == WM_NCDESTROY) {
    RemovePropW(hwnd, kTestGuardProp);
    if (orig != nullptr) {
      if (GetWindowLongPtrW(hwnd, GWLP_WNDPROC) ==
          reinterpret_cast<LONG_PTR>(test_guarded_subclass_proc)) {
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
      }
      return CallWindowProcW(orig, hwnd, msg, wParam, lParam);
    }
  }
  return orig != nullptr ? CallWindowProcW(orig, hwnd, msg, wParam, lParam)
                         : DefWindowProcW(hwnd, msg, wParam, lParam);
}

void install_guarded_test_subclass(HWND hwnd) {
  if (hwnd == nullptr || !IsWindow(hwnd) ||
      GetPropW(hwnd, kTestGuardProp) != nullptr) {
    return;
  }
  WNDPROC cur = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
  if (cur != nullptr && cur != test_guarded_subclass_proc) {
    SetPropW(hwnd, kTestGuardProp, reinterpret_cast<HANDLE>(cur));
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(test_guarded_subclass_proc));
  }
}
}  // namespace

void test_overlay_wndproc_chaining_no_recursion() {
  HWND hwnd = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"winome-test-overlay",
      WS_POPUP | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME, 0, 0, 64, 64,
      nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  CHECK(hwnd != nullptr);
  if (hwnd == nullptr)
    return;

  // Apply harden_shell_overlay_hwnd + guarded secondary subclass twice (mirroring
  // on_realize + relayout): the second pass must be an idempotent no-op with zero cycle.
  winome::harden_shell_overlay_hwnd(hwnd);
  install_guarded_test_subclass(hwnd);
  winome::harden_shell_overlay_hwnd(hwnd);
  install_guarded_test_subclass(hwnd);

  g_test_guard_calls = 0;
  const LRESULT len = SendMessageW(hwnd, WM_GETTEXTLENGTH, 0, 0);
  CHECK(len >= 0);
  CHECK(g_test_guard_calls == 1);

  // SC_MINIMIZE must be swallowed cleanly without recursion.
  g_test_guard_calls = 0;
  const LRESULT min_res = SendMessageW(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
  CHECK(min_res == 0);
  CHECK(IsIconic(hwnd) == FALSE);
  CHECK(g_test_guard_calls == 1);

  CHECK(DestroyWindow(hwnd) != FALSE);
}

int main() {
  ScopedTempConfigDir temp_dir;
  test_config_roundtrip();
  test_config_sanitization_and_unknown_keys();
  test_config_persistence_and_i18n(&temp_dir);
  test_wifi_ssid_decoding();
  test_overview_layout_geometry();
  test_wifi_profile_xml();
  test_notification_age_formatting();
  test_systray_filtering_and_fallback();
  test_config_listener_updates();
  test_monitor_manager_geometry();
  test_virtual_desktop_profiles();
  test_app_index_and_lru_cache();
  test_app_grid_pagination();
  test_app_grid_and_running_window_matching();
  test_dash_favorites_and_thumbs_summary();
  test_st_engine_theme_metrics();
  test_perf_metrics_and_isolated_fontmap();
  test_hot_corner_hit_test();
  test_end_session_formatting();
  test_window_theming_hwnd_filter();
  test_ipc_dispatch_and_named_pipe_roundtrip();
  test_vd_vtable_profiles_and_cache_trim();
  test_crash_recovery_snapshot_and_state();
  test_debug_runtime_ipc_sddl_and_ctl();
  test_run_dialog_helpers();
  test_plugin_host_and_real_child_process();
  test_vd_async_and_breaker_recovery();
  test_start_menu_pinned_layout_and_folder_sync();
  test_systray_v4_and_uia_candidates();
  test_overview_window_blacklist();
  test_winrt_safe_com_getter_and_notifications();
  test_win_hotkey_passthrough_classification();

  // Quick Settings secondary-monitor workspace mode visibility & cycling.
  CHECK(!winome::ws::should_show_quick_settings_ws_mode_toggle(0));
  CHECK(!winome::ws::should_show_quick_settings_ws_mode_toggle(1));
  CHECK(winome::ws::should_show_quick_settings_ws_mode_toggle(2));
  CHECK(winome::ws::should_show_quick_settings_ws_mode_toggle(3));
  CHECK(winome::ws::next_multi_monitor_workspace_mode("independent") ==
        "primary_only");
  CHECK(winome::ws::next_multi_monitor_workspace_mode("primary_only") ==
        "span");
  CHECK(winome::ws::next_multi_monitor_workspace_mode("span") ==
        "independent");
  CHECK(winome::ws::next_multi_monitor_workspace_mode("") == "independent");
  CHECK(winome::ws::next_multi_monitor_workspace_mode("unknown") ==
        "independent");

  // Native taskbar ABM_SETSTATE guard (avoids Explorer's duplicate-AppBar popup).
  CHECK(winome::should_send_taskbar_autohide_state(true, false, false));
  CHECK(!winome::should_send_taskbar_autohide_state(false, false, false));
  CHECK(!winome::should_send_taskbar_autohide_state(true, true, false));
  CHECK(!winome::should_send_taskbar_autohide_state(true, false, true));

  // Overlay style hardening preserves non-frame bits and clears min/max/thickframe.
  const LONG_PTR stripped = winome::strip_overlay_minimize_styles(
      static_cast<LONG_PTR>(WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS |
                            WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME));
  CHECK((stripped & WS_MINIMIZEBOX) == 0);
  CHECK((stripped & WS_MAXIMIZEBOX) == 0);
  CHECK((stripped & WS_THICKFRAME) == 0);
  CHECK((stripped & WS_POPUP) != 0);
  CHECK((stripped & WS_VISIBLE) != 0);
  CHECK((stripped & WS_CLIPSIBLINGS) != 0);

  // Launched app placement on secondary monitor centers & clamps to target work area.
  const RECT sec_work{-150, 1112, 2190, 2160};
  const RECT centered = winome::compute_launched_window_rect(
      RECT{100, 100, 900, 700}, sec_work);
  CHECK(centered.right - centered.left == 800);
  CHECK(centered.bottom - centered.top == 600);
  CHECK(centered.left == 620);
  CHECK(centered.top == 1336);
  const RECT clamped = winome::compute_launched_window_rect(
      RECT{0, 0, 3000, 2000}, sec_work);
  CHECK(clamped.left == -150);
  CHECK(clamped.top == 1112);
  CHECK(clamped.right == 2190);
  CHECK(clamped.bottom == 2160);
  const RECT def_sz = winome::compute_launched_window_rect(RECT{0, 0, 0, 0}, sec_work);
  CHECK(def_sz.right - def_sz.left == (2340 * 3) / 4);
  CHECK(def_sz.bottom - def_sz.top == (1048 * 3) / 4);
  test_workarea_clamp_guard_and_fullscreen_hysteresis();
  test_hook_watchdog_and_lockfree_ring();
  test_overview_search_helpers();
  test_live_wallpaper_helpers();
  test_panel_overview_and_workspace_dots();
  test_wpn_notifications_and_wifi_bt_sorting();
  test_power_modes_overlay_and_legacy();
  test_notification_activation_xml_and_theme_modes();
  test_multi_monitor_interaction_helpers();
  test_overlay_wndproc_chaining_no_recursion();
  test_independent_per_monitor_workspace_engine();
  test_diagnostics_zip_export();

  // Developer-mode test.* CLI translation, step expansion, single & aggregated execution.
  {
    std::string req, err;
    CHECK(winome::ctl::ctl_build_request_json(
        {"test", "trigger", "win_tap_os", "--wait", "380", "--tag", "perf,ov"},
        &req, &err));
    CHECK(req.find("\"cmd\":\"test.trigger\"") != std::string::npos);
    CHECK(req.find("\"action\":\"win_tap_os\"") != std::string::npos);
    CHECK(req.find("\"wait_ms\":380") != std::string::npos);

    CHECK(winome::ctl::ctl_build_request_json(
        {"test", "run", "--preset", "slow-double-win", "--gap", "360",
         "--settle", "500", "--repeat", "3"},
        &req, &err));
    CHECK(req.find("\"cmd\":\"test.run\"") != std::string::npos);
    CHECK(req.find("\"preset\":\"slow-double-win\"") != std::string::npos);
    CHECK(req.find("\"gap_ms\":360") != std::string::npos);
    CHECK(req.find("\"repeat\":3") != std::string::npos);

    CHECK(winome::ctl::ctl_build_request_json({"test", "begin"}, &req, &err));
    CHECK(req.find("\"cmd\":\"test.begin\"") != std::string::npos);
    CHECK(winome::ctl::ctl_build_request_json({"test", "end"}, &req, &err));
    CHECK(req.find("\"cmd\":\"test.end\"") != std::string::npos);

    std::vector<std::string> expanded;
    CHECK(winome::ipc_expand_dev_test_steps("", "slow-double-win", 380,
                                            &expanded, &err));
    CHECK(expanded.size() == 3);
    CHECK(expanded[0] == "win_tap_os");
    CHECK(expanded[1] == "sleep:380");
    CHECK(expanded[2] == "win_tap_os");

    // Verify developer_mode gate and single/aggregate/session flows.
    const winome::WinomeConfig saved_cfg = winome::get_config();
    winome::WinomeConfig cfg = saved_cfg;
    cfg.developer_mode = false;
    winome::update_config(cfg);

    winome::IpcCallbacks dev_cbs{};
    dev_cbs.dev_test_action = +[](const std::string& act, std::string*) -> bool {
      if (act == "mock_anim") {
        winome::debug_append_log(
            "perf",
            "[perf] overview anim frames=15 dropped=1 avg=16.40ms "
            "p95=17.20ms max=21.10ms tick_max=1.20ms dwm_max=0.80ms panel_draws=1");
        return true;
      }
      return act == "mock_noop";
    };
    const std::string denied = winome::ipc_dispatch_request_json(
        "{\"cmd\":\"test.trigger\",\"action\":\"mock_anim\",\"wait_ms\":0}",
        dev_cbs);
    CHECK(denied.find("developer_mode_required") != std::string::npos);

    cfg.developer_mode = true;
    winome::update_config(cfg);

    const std::string single = winome::ipc_dispatch_request_json(
        "{\"cmd\":\"test.trigger\",\"action\":\"mock_anim\",\"wait_ms\":0,\"tag\":\"perf\"}",
        dev_cbs);
    CHECK(single.find("\"ok\":true") != std::string::npos);
    CHECK(single.find("\"mode\":\"single\"") != std::string::npos);
    CHECK(single.find("\"anim_runs\":1") != std::string::npos);
    CHECK(single.find("\"total_frames\":15") != std::string::npos);
    CHECK(single.find("\"total_dropped\":1") != std::string::npos);

    const std::string agg = winome::ipc_dispatch_request_json(
        "{\"cmd\":\"test.run\",\"steps\":\"mock_anim,sleep:5,mock_anim\","
        "\"settle_ms\":0,\"repeat\":2,\"tag\":\"perf\"}",
        dev_cbs);
    CHECK(agg.find("\"ok\":true") != std::string::npos);
    CHECK(agg.find("\"mode\":\"aggregate\"") != std::string::npos);
    CHECK(agg.find("\"anim_runs\":4") != std::string::npos);
    CHECK(agg.find("\"total_frames\":60") != std::string::npos);
    CHECK(agg.find("\"total_dropped\":4") != std::string::npos);

    const std::string s_beg = winome::ipc_dispatch_request_json(
        "{\"cmd\":\"test.begin\",\"tag\":\"perf\"}", dev_cbs);
    CHECK(s_beg.find("\"ok\":true") != std::string::npos);
    (void)winome::ipc_dispatch_request_json(
        "{\"cmd\":\"test.trigger\",\"action\":\"mock_anim\",\"wait_ms\":0}",
        dev_cbs);
    (void)winome::ipc_dispatch_request_json(
        "{\"cmd\":\"test.trigger\",\"action\":\"mock_anim\",\"wait_ms\":0}",
        dev_cbs);
    const std::string s_end = winome::ipc_dispatch_request_json(
        "{\"cmd\":\"test.end\"}", dev_cbs);
    CHECK(s_end.find("\"ok\":true") != std::string::npos);
    CHECK(s_end.find("\"mode\":\"session\"") != std::string::npos);
    CHECK(s_end.find("\"anim_runs\":2") != std::string::npos);
    CHECK(s_end.find("\"total_frames\":30") != std::string::npos);

    winome::update_config(saved_cfg);
  }

  // Universal Developer Debug Operation Channel (dev.help / dev.op / dev.batch)
  {
    CHECK(winome::dev_resolve_vk("LWin") == VK_LWIN);
    CHECK(winome::dev_resolve_vk("Esc") == VK_ESCAPE);
    CHECK(winome::dev_resolve_vk("F2") == VK_F2);
    CHECK(winome::dev_resolve_vk("a") == 'A');
    CHECK(winome::dev_resolve_vk("0x5B") == VK_LWIN);
    CHECK(winome::dev_resolve_vk("nonexistent_key_xyz") == 0);

    winome::DevOpRequest parsed;
    std::string perr;
    CHECK(winome::dev_parse_op_shorthand("key.combo Win+Tab hold_ms=35", &parsed, &perr));
    CHECK(parsed.op == "key.combo");
    CHECK(parsed.args["keys"] == "Win+Tab");
    CHECK(parsed.args["hold_ms"] == "35");

    std::vector<winome::DevOpRequest> batch;
    CHECK(winome::dev_parse_batch_shorthand(
        "shell.overview open; sleep 10; perf.snapshot", &batch, &perr));
    CHECK(batch.size() == 3);
    CHECK(batch[0].op == "shell.overview");
    CHECK(batch[0].args["action"] == "open");
    CHECK(batch[1].op == "sleep");
    CHECK(batch[1].args["ms"] == "10");
    CHECK(batch[2].op == "perf.snapshot");

    // CLI translation for winome-ctl dev help / op / batch
    std::string req, err;
    CHECK(winome::ctl::ctl_build_request_json({"dev", "help"}, &req, &err));
    CHECK(req.find("\"cmd\":\"dev.help\"") != std::string::npos);

    CHECK(winome::ctl::ctl_build_request_json(
        {"dev", "op", "mouse.move", "x=10", "y=20", "--wait", "50", "--logs", "--tag", "perf"},
        &req, &err));
    CHECK(req.find("\"cmd\":\"dev.op\"") != std::string::npos);
    CHECK(req.find("\"op\":\"mouse.move\"") != std::string::npos);
    CHECK(req.find("\"x\":\"10\"") != std::string::npos);
    CHECK(req.find("\"capture_logs\":true") != std::string::npos);

    CHECK(winome::ctl::ctl_build_request_json(
        {"dev", "batch", "--ops", "sleep 5; perf.snapshot", "--logs", "--stop-on-error"},
        &req, &err));
    CHECK(req.find("\"cmd\":\"dev.batch\"") != std::string::npos);
    CHECK(req.find("\"stop_on_error\":true") != std::string::npos);

    // Gate check + live IPC dispatch
    const winome::WinomeConfig saved_cfg = winome::get_config();
    winome::WinomeConfig cfg = saved_cfg;
    cfg.developer_mode = false;
    winome::update_config(cfg);

    winome::IpcCallbacks dev_cbs{};
    dev_cbs.dev_shell_op = [](const std::string& op,
                              const std::map<std::string, std::string>& args,
                              std::string* out_json,
                              std::string* out_err) -> bool {
      if (op == "shell.mock") {
        auto it = args.find("v");
        *out_json = "{\"echo\":\"" + (it != args.end() ? it->second : "") + "\"}";
        return true;
      }
      *out_err = "mock_fail";
      return false;
    };
    CHECK(winome::ipc_dispatch_request_json("{\"cmd\":\"dev.help\"}", dev_cbs)
              .find("developer_mode is disabled") != std::string::npos);

    cfg.developer_mode = true;
    winome::update_config(cfg);

    CHECK(winome::ipc_dispatch_request_json("{\"cmd\":\"dev.help\"}", dev_cbs)
              .find("\"dev-universal\"") != std::string::npos);

    const std::string op_res = winome::ipc_dispatch_request_json(
        "{\"cmd\":\"dev.op\",\"op\":\"shell.mock\",\"args\":{\"v\":\"ok42\"},\"capture_logs\":true}",
        dev_cbs);
    CHECK(op_res.find("\"ok\":true") != std::string::npos);
    CHECK(op_res.find("\"echo\":\"ok42\"") != std::string::npos);

    const std::string snap_res = winome::ipc_dispatch_request_json(
        "{\"cmd\":\"dev.op\",\"op\":\"perf.snapshot\"}", dev_cbs);
    CHECK(snap_res.find("\"ok\":true") != std::string::npos);
    CHECK(snap_res.find("\"memory\":") != std::string::npos);

    const std::string batch_res = winome::ipc_dispatch_request_json(
        "{\"cmd\":\"dev.batch\",\"ops\":["
        "{\"op\":\"shell.mock\",\"args\":{\"v\":\"a\"}},"
        "{\"op\":\"sleep\",\"args\":{\"ms\":5}},"
        "{\"op\":\"shell.bad\"},"
        "{\"op\":\"perf.snapshot\"}],"
        "\"stop_on_error\":true,\"capture_logs\":true}",
        dev_cbs);
    CHECK(batch_res.find("\"ok\":false") != std::string::npos);
    CHECK(batch_res.find("\"executed\":3") != std::string::npos);
    CHECK(batch_res.find("\"total\":4") != std::string::npos);

    winome::update_config(saved_cfg);
  }

  // Test sanitize_gdk_gl_backing_windows detaches owner, drops topmost,
  // re-cloaks, and enforces click-through/non-activatable styles on GdkWin32GL.
  {
    HINSTANCE hinst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc_owner{};
    wc_owner.cbSize = sizeof(wc_owner);
    wc_owner.lpfnWndProc = DefWindowProcW;
    wc_owner.hInstance = hinst;
    wc_owner.lpszClassName = L"WinomeSanitizeOwnerTest";
    const ATOM owner_atom = RegisterClassExW(&wc_owner);

    WNDCLASSEXW wc_gl{};
    wc_gl.cbSize = sizeof(wc_gl);
    wc_gl.lpfnWndProc = DefWindowProcW;
    wc_gl.hInstance = hinst;
    wc_gl.lpszClassName = L"GdkWin32GL";
    const ATOM gl_atom = RegisterClassExW(&wc_gl);

    HWND owner = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"WinomeSanitizeOwnerTest", L"Owner",
        WS_POPUP, 0, 0, 120, 120, nullptr, nullptr, hinst, nullptr);
    HWND gl = CreateWindowExW(
        0, L"GdkWin32GL", L"", WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0, 0, 120, 120, owner, nullptr, hinst, nullptr);
    CHECK(owner != nullptr);
    CHECK(gl != nullptr);
    if (owner != nullptr && gl != nullptr) {
      BOOL cloak_true = TRUE;
      BOOL cloak_false = FALSE;
      DwmSetWindowAttribute(gl, DWMWA_CLOAK, &cloak_true, sizeof(cloak_true));
      ShowWindow(gl, SW_SHOWNOACTIVATE);
      // Simulate Overview cloaking owner, showing, uncloaking owner, and
      // promoting owner to topmost (which uncloaks/promotes owned popup).
      DwmSetWindowAttribute(owner, DWMWA_CLOAK, &cloak_true, sizeof(cloak_true));
      ShowWindow(owner, SW_SHOWNOACTIVATE);
      DwmSetWindowAttribute(owner, DWMWA_CLOAK, &cloak_false, sizeof(cloak_false));
      SetWindowPos(owner, HWND_TOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

      winome::sanitize_gdk_gl_backing_windows();

      CHECK(GetWindowLongPtrW(gl, GWLP_HWNDPARENT) == 0);
      const LONG_PTR ex = GetWindowLongPtrW(gl, GWL_EXSTYLE);
      CHECK((ex & WS_EX_TOPMOST) == 0);
      CHECK((ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)) ==
            (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT));
      DWORD cloaked_after = 0;
      DwmGetWindowAttribute(gl, DWMWA_CLOAKED, &cloaked_after,
                            sizeof(cloaked_after));
      CHECK(cloaked_after != 0);

      // Subsequent owner cloak->uncloak must no longer touch detached GL HWND.
      DwmSetWindowAttribute(owner, DWMWA_CLOAK, &cloak_true, sizeof(cloak_true));
      DwmSetWindowAttribute(owner, DWMWA_CLOAK, &cloak_false, sizeof(cloak_false));
      DWORD cloaked_persist = 0;
      DwmGetWindowAttribute(gl, DWMWA_CLOAKED, &cloaked_persist,
                            sizeof(cloaked_persist));
      CHECK(cloaked_persist != 0);
    }
    if (gl != nullptr)
      DestroyWindow(gl);
    if (owner != nullptr)
      DestroyWindow(owner);
    if (gl_atom != 0)
      UnregisterClassW(L"GdkWin32GL", hinst);
    if (owner_atom != 0)
      UnregisterClassW(L"WinomeSanitizeOwnerTest", hinst);
  }

  if (g_failures == 0)
    std::printf("ALL WINOME UNIT TESTS PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
