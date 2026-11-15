// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Automated unit tests for WINOME core modules.

#include "app-grid.h"
#include "app-index.h"
#include "config.h"
#include "i18n.h"
#include "monitor-manager.h"
#include "notifications.h"
#include "overview-dash.h"
#include "overview-layout.h"
#include "overview-thumbs.h"
#include "st-engine.h"
#include "systray.h"
#include "virtual-desktop.h"
#include "wifi-networks.h"

#include <cmath>

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

  if (g_failures == 0)
    std::printf("ALL WINOME UNIT TESTS PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
