// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Automated unit tests for WINOME core modules.

#include "app-grid.h"
#include "app-index.h"
#include "config.h"
#include "crash-dialog.h"
#include "debug-runtime.h"
#include "end-session-dialog.h"
#include "../ctl/ctl-protocol.h"
#include "fonts.h"
#include "host-utils.h"
#include "hot-corner.h"
#include "i18n.h"
#include "monitor-manager.h"
#include "notifications.h"
#include "overview-dash.h"
#include "overview-layout.h"
#include "overview-thumbs.h"
#include "overview-trigger.h"
#include "fullscreen-watcher.h"
#include "perf-metrics.h"
#include "plugin-host.h"
#include "run-dialog.h"
#include "shell-ipc.h"
#include "st-engine.h"
#include "systray.h"
#include "virtual-desktop.h"
#include "wifi-networks.h"
#include "window-theming.h"
#include "winrt-bridge.h"

#include <cmath>

#include <windows.h>
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
  CHECK(def.unify_dark_titlebars == false);
  CHECK(def.unify_rounded_corners == false);

  winome::WinomeConfig src;
  src.language = "zh";
  src.clock_format = "24h";
  src.clock_show_seconds = true;
  src.show_battery_percentage = true;
  src.hot_corner_enabled = false;
  src.hot_corner_cooldown_ms = 1250;
  src.win_key_overview = false;
  src.overview_window_blacklist = false;
  src.multi_monitor_panels = false;
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
  CHECK(dst.overview_window_blacklist == src.overview_window_blacklist);
  CHECK(dst.multi_monitor_panels == src.multi_monitor_panels);
  CHECK(dst.end_session_confirm == src.end_session_confirm);
  CHECK(dst.unify_dark_titlebars == src.unify_dark_titlebars);
  CHECK(dst.unify_rounded_corners == src.unify_rounded_corners);
  CHECK(dst.pinned_apps == src.pinned_apps);

  // Legacy configs without new keys preserve safe defaults.
  winome::WinomeConfig legacy;
  CHECK(winome::parse_config("{\"language\":\"en\"}", &legacy));
  CHECK(legacy.end_session_confirm == true);
  CHECK(legacy.overview_window_blacklist == true);
  CHECK(legacy.unify_dark_titlebars == false);
  CHECK(legacy.unify_rounded_corners == false);
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
  CHECK(d_md.cols == 6 && d_md.rows == 4 && d_md.icon_size == 72 &&
        d_md.page_size == 24);

  auto d_mid = winome::compute_app_grid_dimensions(1000, 560);
  CHECK(d_mid.cols == 5 && d_mid.rows == 3 && d_mid.icon_size == 80 &&
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
      winome::app_grid_set_query(grid, "");
      winome::app_grid_rebuild(grid, 1320, 670);
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
  CHECK(t_drag.visible && t_drag.desktop_pills == 1 && t_drag.has_add_pill);

  auto t_multi = winome::compute_thumbs_summary(2, true, false);
  CHECK(t_multi.visible && t_multi.desktop_pills == 2 && t_multi.has_add_pill);
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

  const std::string st = winome::ipc_dispatch_request_json("{\"cmd\":\"status\"}", cbs);
  CHECK(st.find("\"ok\":true") != std::string::npos);
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

  // Legitimate Explorer folders, UWP Settings, and regular user windows remain untouched.
  CHECK(!winome::is_overview_default_blacklisted_window_info(
      "CabinetWClass",
      "C:\\Windows\\explorer.exe",
      "WINOME-portable - \xE6\x96\x87\xE4\xBB\xB6\xE8\xB5\x84\xE6\xBA\x90\xE7\xAE\xA1\xE7\x90\x86\xE5\x99\xA8",
      false));
  CHECK(!winome::is_overview_default_blacklisted_window_info(
      "ApplicationFrameWindow",
      "C:\\Windows\\System32\\ApplicationFrameHost.exe",
      "\xE8\xAE\xBE\xE7\xBD\xAE",
      true));
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

  // Burst protection: applies at >=500ms intervals up to 4 times in 5s, then backs off.
  CHECK(guard.evaluate(2000, want, reset, true, false, false) ==
        winome::WorkareaAssertAction::Apply);
  CHECK(guard.evaluate(2100, want, reset, true, false, false) ==
        winome::WorkareaAssertAction::DeferCooldown);
  CHECK(guard.evaluate(2600, want, reset, true, false, false) ==
        winome::WorkareaAssertAction::Apply);
  CHECK(guard.evaluate(3200, want, reset, true, false, false) ==
        winome::WorkareaAssertAction::Apply);
  CHECK(guard.evaluate(3800, want, reset, true, false, false) ==
        winome::WorkareaAssertAction::Apply);
  CHECK(guard.evaluate(4400, want, reset, true, false, false) ==
        winome::WorkareaAssertAction::DeferBackoff);
  // Once backoff expires and workarea stays matching long enough, burst counter clears.
  CHECK(guard.evaluate(9500, want, want, true, false, false) ==
        winome::WorkareaAssertAction::SkipUnchanged);
  CHECK(guard.evaluate(10100, want, reset, true, false, false) ==
        winome::WorkareaAssertAction::Apply);

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
}

}  // namespace

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
  test_workarea_clamp_guard_and_fullscreen_hysteresis();

  if (g_failures == 0)
    std::printf("ALL WINOME UNIT TESTS PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
