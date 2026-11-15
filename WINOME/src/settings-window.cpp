// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Native GTK4 WINOME Settings preferences window wired to live WinomeConfig.

#include "settings-window.h"

#include "config.h"
#include "fonts.h"
#include "i18n.h"

#include <gtk/gtk.h>
#include <gdk/win32/gdkwin32.h>
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <cstdio>
#include <string>

namespace winome {
namespace {

struct SettingsUi {
  GtkWidget* window = nullptr;
  GtkWidget* stack = nullptr;
  GtkWidget* sidebar = nullptr;

  // Appearance controls
  GtkWidget* lang_drop = nullptr;
  GtkWidget* clock_fmt_drop = nullptr;
  GtkWidget* clock_sec_switch = nullptr;
  GtkWidget* battery_pct_switch = nullptr;
  GtkWidget* multimon_switch = nullptr;

  // Behavior controls
  GtkWidget* hot_corner_switch = nullptr;
  GtkWidget* hot_cooldown_spin = nullptr;
  GtkWidget* win_key_switch = nullptr;

  // Switches / spins update on config notify.
  bool syncing = false;
  bool css_loaded = false;
};

SettingsUi g_ui;

std::wstring local_app_winome_dir(const wchar_t* subdir) {
  wchar_t buf[MAX_PATH] = {};
  DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  std::wstring dir = (len > 0 && len < MAX_PATH) ? std::wstring(buf) : L".";
  dir += L"\\WINOME";
  if (subdir != nullptr && subdir[0] != L'\0') {
    dir += L"\\";
    dir += subdir;
  }
  return dir;
}

std::string utf8_from_wide(const std::wstring& w) {
  if (w.empty())
    return {};
  int need = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                 nullptr, 0, nullptr, nullptr);
  if (need <= 0)
    return {};
  std::string out(static_cast<size_t>(need), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                      out.data(), need, nullptr, nullptr);
  return out;
}

std::string windows_build_summary() {
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll != nullptr) {
    auto fn = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
    if (fn != nullptr) {
      OSVERSIONINFOEXW osv{};
      osv.dwOSVersionInfoSize = sizeof(osv);
      if (fn(&osv) == 0) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Windows %lu.%lu (Build %lu)",
                      osv.dwMajorVersion, osv.dwMinorVersion,
                      osv.dwBuildNumber);
        return buf;
      }
    }
  }
  return "Windows x86_64";
}

void ensure_settings_css() {
  if (g_ui.css_loaded)
    return;
  g_ui.css_loaded = true;

  static const char* kCss = R"(
#winomeSettingsWindow {
  background-color: #1e1e1e;
  color: #eeeeec;
}
#winomeSettingsWindow stacksidebar {
  background-color: #242424;
  border-right: 1px solid rgba(255, 255, 255, 0.08);
  min-width: 164px;
}
#winomeSettingsWindow stacksidebar list {
  background-color: transparent;
  padding: 8px;
}
#winomeSettingsWindow stacksidebar row {
  border-radius: 8px;
  padding: 8px 12px;
  margin-bottom: 2px;
  color: #ddddda;
  transition: background-color 120ms ease;
}
#winomeSettingsWindow stacksidebar row:hover {
  background-color: rgba(255, 255, 255, 0.06);
}
#winomeSettingsWindow stacksidebar row:selected {
  background-color: rgba(255, 255, 255, 0.12);
  color: #ffffff;
  font-weight: 600;
}
#winomeSettingsWindow stacksidebar row:selected:hover {
  background-color: rgba(255, 255, 255, 0.16);
}
#winomeSettingsWindow stacksidebar row:focus:not(:focus-visible) {
  outline-style: none;
  box-shadow: none;
}
#winomeSettingsPage {
  padding: 20px 24px;
}
#winomeSettingsCard {
  background-color: #2c2c2c;
  border: 1px solid rgba(255, 255, 255, 0.08);
  border-radius: 12px;
  padding: 4px 0;
}
#winomeSettingsRow {
  padding: 10px 16px;
}
#winomeSettingsSectionTitle {
  font-size: 13px;
  font-weight: 700;
  color: #9a9996;
  margin-top: 8px;
  margin-bottom: 6px;
}
#winomeSettingsDim {
  font-size: 12px;
  color: #9a9996;
}
#winomeSettingsHeaderTitle {
  font-size: 18px;
  font-weight: 700;
  color: #ffffff;
}
#winomeSettingsButton {
  background-color: #383838;
  background-image: none;
  color: #ffffff;
  border-radius: 8px;
  padding: 6px 14px;
  border: 1px solid rgba(255, 255, 255, 0.1);
  transition: background-color 120ms ease;
}
#winomeSettingsButton:hover {
  background-color: #454545;
}
#winomeSettingsButton:active {
  background-color: #525252;
}
#winomeSettingsButton:focus:not(:focus-visible) {
  outline-style: none;
  box-shadow: none;
}
#winomeSettingsCard separator {
  background-color: rgba(255, 255, 255, 0.08);
  min-height: 1px;
}
#winomeSettingsWindow switch {
  min-width: 46px;
  min-height: 26px;
  padding: 0;
  border: none;
  box-shadow: none;
  background-image: none;
  background-color: rgba(255, 255, 255, 0.18);
  border-radius: 999px;
  transition: background-color 150ms ease;
}
#winomeSettingsWindow switch:checked {
  background-color: #3584e4;
}
#winomeSettingsWindow switch > slider {
  min-width: 20px;
  min-height: 20px;
  margin: 3px;
  padding: 0;
  border: none;
  border-radius: 999px;
  background-image: none;
  background-color: #ffffff;
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.35);
}
#winomeSettingsWindow switch:focus:not(:focus-visible) {
  outline-style: none;
  box-shadow: none;
}
#winomeSettingsWindow entry,
#winomeSettingsWindow spinbutton,
#winomeSettingsWindow dropdown > button {
  background-color: #383838;
  background-image: none;
  color: #eeeeec;
  border-radius: 8px;
  border: 1px solid rgba(255, 255, 255, 0.1);
}
#winomeSettingsWindow entry:hover,
#winomeSettingsWindow spinbutton:hover,
#winomeSettingsWindow dropdown > button:hover {
  background-color: #424242;
}
#winomeSettingsWindow entry:focus-within,
#winomeSettingsWindow spinbutton:focus-within {
  border-color: #3584e4;
}
#winomeSettingsWindow dropdown popover > contents {
  background-color: #2c2c2c;
  color: #eeeeec;
  border-radius: 10px;
  border: 1px solid rgba(255, 255, 255, 0.12);
}
#winomeSettingsWindow scrollbar {
  background-color: transparent;
  background-image: none;
  border: none;
}
#winomeSettingsWindow scrollbar slider {
  min-width: 6px;
  min-height: 24px;
  border-radius: 8px;
  border: none;
  background-image: none;
  background-color: rgba(255, 255, 255, 0.28);
}
#winomeSettingsWindow scrollbar slider:hover {
  background-color: rgba(255, 255, 255, 0.42);
}
)";

  GtkCssProvider* provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, kCss);
  gtk_style_context_add_provider_for_display(
      gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

// Enable Win32 dark title bar chrome (Win11 / Win10 2004+, with 1903 fallback)
// so the native window frame matches the dark #1e1e1e GTK content area.
void apply_dark_titlebar(GtkWidget* win) {
  if (win == nullptr)
    return;
  GdkSurface* surf = gtk_native_get_surface(GTK_NATIVE(win));
  if (surf == nullptr)
    return;
  HWND hwnd = gdk_win32_surface_get_handle(surf);
  if (hwnd == nullptr)
    return;
  BOOL dark = TRUE;
  if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark))))
    DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
}

GtkWidget* make_section_label(const char* text) {
  GtkWidget* lbl = gtk_label_new(text);
  gtk_widget_set_name(lbl, "winomeSettingsSectionTitle");
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
  return lbl;
}

GtkWidget* make_card() {
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(box, "winomeSettingsCard");
  return box;
}

GtkWidget* make_row(const char* title, const char* subtitle, GtkWidget* control) {
  GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
  gtk_widget_set_name(row, "winomeSettingsRow");

  GtkWidget* text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(text_box, TRUE);
  gtk_widget_set_valign(text_box, GTK_ALIGN_CENTER);

  GtkWidget* title_lbl = gtk_label_new(title);
  gtk_label_set_xalign(GTK_LABEL(title_lbl), 0.0f);
  gtk_box_append(GTK_BOX(text_box), title_lbl);

  if (subtitle != nullptr && subtitle[0] != '\0') {
    GtkWidget* sub_lbl = gtk_label_new(subtitle);
    gtk_widget_set_name(sub_lbl, "winomeSettingsDim");
    gtk_label_set_xalign(GTK_LABEL(sub_lbl), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(sub_lbl), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(sub_lbl), PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(text_box), sub_lbl);
  }

  gtk_box_append(GTK_BOX(row), text_box);
  if (control != nullptr) {
    gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), control);
  }
  return row;
}

void append_row_to_card(GtkWidget* card, GtkWidget* row, bool add_separator) {
  if (add_separator)
    gtk_box_append(GTK_BOX(card), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append(GTK_BOX(card), row);
}

void sync_ui_from_config() {
  if (g_ui.window == nullptr)
    return;
  g_ui.syncing = true;
  const WinomeConfig& cfg = get_config();

  if (g_ui.lang_drop != nullptr) {
    guint sel = 0;
    if (cfg.language == "zh")
      sel = 1;
    else if (cfg.language == "en")
      sel = 2;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(g_ui.lang_drop), sel);
  }

  if (g_ui.clock_fmt_drop != nullptr) {
    guint sel = 0;
    if (cfg.clock_format == "24h")
      sel = 1;
    else if (cfg.clock_format == "12h")
      sel = 2;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(g_ui.clock_fmt_drop), sel);
  }

  if (g_ui.clock_sec_switch != nullptr) {
    gtk_switch_set_active(GTK_SWITCH(g_ui.clock_sec_switch),
                          cfg.clock_show_seconds ? TRUE : FALSE);
  }
  if (g_ui.battery_pct_switch != nullptr) {
    gtk_switch_set_active(GTK_SWITCH(g_ui.battery_pct_switch),
                          cfg.show_battery_percentage ? TRUE : FALSE);
  }
  if (g_ui.multimon_switch != nullptr) {
    gtk_switch_set_active(GTK_SWITCH(g_ui.multimon_switch),
                          cfg.multi_monitor_panels ? TRUE : FALSE);
  }
  if (g_ui.hot_corner_switch != nullptr) {
    gtk_switch_set_active(GTK_SWITCH(g_ui.hot_corner_switch),
                          cfg.hot_corner_enabled ? TRUE : FALSE);
  }
  if (g_ui.hot_cooldown_spin != nullptr) {
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_ui.hot_cooldown_spin),
                              static_cast<double>(cfg.hot_corner_cooldown_ms));
    gtk_widget_set_sensitive(g_ui.hot_cooldown_spin,
                             cfg.hot_corner_enabled ? TRUE : FALSE);
  }
  if (g_ui.win_key_switch != nullptr) {
    gtk_switch_set_active(GTK_SWITCH(g_ui.win_key_switch),
                          cfg.win_key_overview ? TRUE : FALSE);
  }

  g_ui.syncing = false;
}

gboolean idle_reopen_settings(gpointer data) {
  std::string* page = static_cast<std::string*>(data);
  if (g_ui.window != nullptr) {
    close_settings_window();
    open_settings_window();
    if (g_ui.stack != nullptr && page != nullptr)
      gtk_stack_set_visible_child_name(GTK_STACK(g_ui.stack), page->c_str());
  }
  delete page;
  return G_SOURCE_REMOVE;
}

void on_lang_changed(GObject* obj, GParamSpec* pspec G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  if (g_ui.syncing)
    return;
  WinomeConfig cfg = get_config();
  guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
  cfg.language = (sel == 1) ? "zh" : (sel == 2 ? "en" : "auto");
  update_config(cfg);

  if (g_ui.window != nullptr) {
    const char* name =
        g_ui.stack ? gtk_stack_get_visible_child_name(GTK_STACK(g_ui.stack))
                   : nullptr;
    auto* page = new std::string(name != nullptr ? name : "appearance");
    g_idle_add(idle_reopen_settings, page);
  }
}

void on_clock_fmt_changed(GObject* obj, GParamSpec* pspec G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  if (g_ui.syncing)
    return;
  WinomeConfig cfg = get_config();
  guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
  cfg.clock_format = (sel == 1) ? "24h" : (sel == 2 ? "12h" : "auto");
  update_config(cfg);
}

gboolean on_clock_sec_set(GtkSwitch* sw G_GNUC_UNUSED, gboolean state, gpointer ud G_GNUC_UNUSED) {
  if (g_ui.syncing)
    return FALSE;
  WinomeConfig cfg = get_config();
  cfg.clock_show_seconds = state != FALSE;
  update_config(cfg);
  return FALSE;
}

gboolean on_battery_pct_set(GtkSwitch* sw G_GNUC_UNUSED, gboolean state, gpointer ud G_GNUC_UNUSED) {
  if (g_ui.syncing)
    return FALSE;
  WinomeConfig cfg = get_config();
  cfg.show_battery_percentage = state != FALSE;
  update_config(cfg);
  return FALSE;
}

gboolean on_multimon_set(GtkSwitch* sw G_GNUC_UNUSED, gboolean state, gpointer ud G_GNUC_UNUSED) {
  if (g_ui.syncing)
    return FALSE;
  WinomeConfig cfg = get_config();
  cfg.multi_monitor_panels = state != FALSE;
  update_config(cfg);
  return FALSE;
}

gboolean on_hot_corner_set(GtkSwitch* sw G_GNUC_UNUSED, gboolean state, gpointer ud G_GNUC_UNUSED) {
  if (g_ui.syncing)
    return FALSE;
  WinomeConfig cfg = get_config();
  cfg.hot_corner_enabled = state != FALSE;
  update_config(cfg);
  return FALSE;
}

void on_hot_cooldown_changed(GtkSpinButton* spin, gpointer ud G_GNUC_UNUSED) {
  if (g_ui.syncing)
    return;
  WinomeConfig cfg = get_config();
  cfg.hot_corner_cooldown_ms = gtk_spin_button_get_value_as_int(spin);
  update_config(cfg);
}

gboolean on_win_key_set(GtkSwitch* sw G_GNUC_UNUSED, gboolean state, gpointer ud G_GNUC_UNUSED) {
  if (g_ui.syncing)
    return FALSE;
  WinomeConfig cfg = get_config();
  cfg.win_key_overview = state != FALSE;
  update_config(cfg);
  return FALSE;
}

void on_open_win_settings(GtkButton* btn G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  ShellExecuteW(nullptr, L"open", L"ms-settings:", nullptr, nullptr, SW_SHOWNORMAL);
}

void on_open_config_folder(GtkButton* btn G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  std::wstring dir = local_app_winome_dir(nullptr);
  CreateDirectoryW(dir.c_str(), nullptr);
  ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void on_open_crashes_folder(GtkButton* btn G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  std::wstring root = local_app_winome_dir(nullptr);
  CreateDirectoryW(root.c_str(), nullptr);
  std::wstring crashes = local_app_winome_dir(L"crashes");
  CreateDirectoryW(crashes.c_str(), nullptr);
  ShellExecuteW(nullptr, L"open", crashes.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void on_reset_defaults(GtkButton* btn G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  WinomeConfig defaults;
  update_config(defaults);
}

void on_settings_config_changed(void* ud G_GNUC_UNUSED) {
  sync_ui_from_config();
}

void on_window_destroy(GtkWidget* widget G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  remove_config_listener(on_settings_config_changed, nullptr);
  g_ui.window = nullptr;
  g_ui.stack = nullptr;
  g_ui.sidebar = nullptr;
  g_ui.lang_drop = nullptr;
  g_ui.clock_fmt_drop = nullptr;
  g_ui.clock_sec_switch = nullptr;
  g_ui.battery_pct_switch = nullptr;
  g_ui.multimon_switch = nullptr;
  g_ui.hot_corner_switch = nullptr;
  g_ui.hot_cooldown_spin = nullptr;
  g_ui.win_key_switch = nullptr;
}

GtkWidget* build_appearance_page() {
  GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_name(page, "winomeSettingsPage");

  gtk_box_append(GTK_BOX(page),
                 make_section_label(tr("Language & Panel", "语言与顶栏")));

  GtkWidget* panel_card = make_card();

  const char* const langs[] = {
      tr("Auto (System)", "自动（跟随系统）"),
      "简体中文",
      "English",
      nullptr};
  g_ui.lang_drop = gtk_drop_down_new_from_strings(langs);
  g_signal_connect(g_ui.lang_drop, "notify::selected",
                   G_CALLBACK(on_lang_changed), nullptr);
  append_row_to_card(
      panel_card,
      make_row(tr("Display Language", "界面语言"),
               tr("Applies to panel, quick settings, calendar, and preferences",
                  "即时应用到顶栏、快速设置、日历及偏好设置界面"),
               g_ui.lang_drop),
      false);

  const char* const clock_fmts[] = {
      tr("Auto (Windows)", "自动（跟随系统）"),
      tr("24-hour", "24 小时制"),
      tr("12-hour", "12 小时制"),
      nullptr};
  g_ui.clock_fmt_drop = gtk_drop_down_new_from_strings(clock_fmts);
  g_signal_connect(g_ui.clock_fmt_drop, "notify::selected",
                   G_CALLBACK(on_clock_fmt_changed), nullptr);
  append_row_to_card(
      panel_card,
      make_row(tr("Clock Format", "时间格式"), nullptr, g_ui.clock_fmt_drop),
      true);

  g_ui.clock_sec_switch = gtk_switch_new();
  g_signal_connect(g_ui.clock_sec_switch, "state-set",
                   G_CALLBACK(on_clock_sec_set), nullptr);
  append_row_to_card(
      panel_card,
      make_row(tr("Show Seconds", "显示秒数"),
               tr("Include seconds in the top-center clock label",
                  "在顶部中央时钟中显示秒数"),
               g_ui.clock_sec_switch),
      true);

  g_ui.battery_pct_switch = gtk_switch_new();
  g_signal_connect(g_ui.battery_pct_switch, "state-set",
                   G_CALLBACK(on_battery_pct_set), nullptr);
  append_row_to_card(
      panel_card,
      make_row(tr("Battery Percentage", "显示电量百分比"),
               tr("Show numeric battery percentage next to the panel icon",
                  "在顶栏电池图标旁显示剩余电量数值"),
               g_ui.battery_pct_switch),
      true);

  g_ui.multimon_switch = gtk_switch_new();
  g_signal_connect(g_ui.multimon_switch, "state-set",
                   G_CALLBACK(on_multimon_set), nullptr);
  append_row_to_card(
      panel_card,
      make_row(tr("Multi-Monitor Panels", "多显示器顶栏"),
               tr("Display top bar on secondary monitors when connected",
                  "连接多个显示器时同步在副屏显示顶部状态栏"),
               g_ui.multimon_switch),
      true);

  gtk_box_append(GTK_BOX(page), panel_card);
  return page;
}

GtkWidget* build_behavior_page() {
  GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_name(page, "winomeSettingsPage");

  gtk_box_append(GTK_BOX(page),
                 make_section_label(tr("Overview & Shortcuts", "概览与快捷键")));

  GtkWidget* trig_card = make_card();

  g_ui.hot_corner_switch = gtk_switch_new();
  g_signal_connect(g_ui.hot_corner_switch, "state-set",
                   G_CALLBACK(on_hot_corner_set), nullptr);
  append_row_to_card(
      trig_card,
      make_row(tr("Hot Corner", "左上角热角"),
               tr("Move pointer to the top-left corner to toggle Activities",
                  "将鼠标移至屏幕左上角以切换概览视图"),
               g_ui.hot_corner_switch),
      false);

  g_ui.hot_cooldown_spin = gtk_spin_button_new_with_range(100, 3000, 50);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(g_ui.hot_cooldown_spin), 0);
  g_signal_connect(g_ui.hot_cooldown_spin, "value-changed",
                   G_CALLBACK(on_hot_cooldown_changed), nullptr);
  append_row_to_card(
      trig_card,
      make_row(tr("Hot Corner Cooldown (ms)", "热角触发冷却时间 (毫秒)"),
               tr("Minimum interval between repeated hot-corner triggers",
                  "避免指针停留在角落时连续重复触发"),
               g_ui.hot_cooldown_spin),
      true);

  g_ui.win_key_switch = gtk_switch_new();
  g_signal_connect(g_ui.win_key_switch, "state-set",
                   G_CALLBACK(on_win_key_set), nullptr);
  append_row_to_card(
      trig_card,
      make_row(tr("Super / Win Key Opens Overview", "Win 键打开概览"),
               tr("Map a single tap of the Windows key to Activities Overview",
                  "单按 Windows 徽标键打开或关闭概览界面"),
               g_ui.win_key_switch),
      true);

  gtk_box_append(GTK_BOX(page), trig_card);

  gtk_box_append(GTK_BOX(page),
                 make_section_label(tr("System & Configuration", "系统与配置")));

  GtkWidget* sys_card = make_card();

  GtkWidget* win_btn =
      gtk_button_new_with_label(tr("Open", "打开"));
  gtk_widget_set_name(win_btn, "winomeSettingsButton");
  g_signal_connect(win_btn, "clicked", G_CALLBACK(on_open_win_settings), nullptr);
  append_row_to_card(
      sys_card,
      make_row(tr("Windows Settings", "Windows 系统设置"),
               tr("Launch built-in Windows Settings (ms-settings:)",
                  "打开 Windows 原生系统设置应用"),
               win_btn),
      false);

  GtkWidget* cfg_btn =
      gtk_button_new_with_label(tr("Open Folder", "打开目录"));
  gtk_widget_set_name(cfg_btn, "winomeSettingsButton");
  g_signal_connect(cfg_btn, "clicked", G_CALLBACK(on_open_config_folder), nullptr);
  append_row_to_card(
      sys_card,
      make_row(tr("Configuration Directory", "配置目录"),
               tr("Inspect or back up config.json under %LOCALAPPDATA%\\WINOME",
                  "查看或备份 %LOCALAPPDATA%\\WINOME\\config.json"),
               cfg_btn),
      true);

  GtkWidget* reset_btn =
      gtk_button_new_with_label(tr("Reset", "恢复默认"));
  gtk_widget_set_name(reset_btn, "winomeSettingsButton");
  g_signal_connect(reset_btn, "clicked", G_CALLBACK(on_reset_defaults), nullptr);
  append_row_to_card(
      sys_card,
      make_row(tr("Reset Preferences", "恢复默认设置"),
               tr("Restore all WINOME configuration options to defaults",
                  "将所有 WINOME 配置项还原为出厂默认值"),
               reset_btn),
      true);

  gtk_box_append(GTK_BOX(page), sys_card);
  return page;
}

GtkWidget* build_about_page() {
  GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
  gtk_widget_set_name(page, "winomeSettingsPage");

  GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
  GtkWidget* logo = gtk_image_new_from_icon_name("computer-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(logo), 48);

  GtkWidget* titles = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget* app_name = gtk_label_new("WINOME Shell 0.1.0");
  gtk_widget_set_name(app_name, "winomeSettingsHeaderTitle");
  gtk_label_set_xalign(GTK_LABEL(app_name), 0.0f);

  GtkWidget* sub = gtk_label_new(
      tr("GNOME Shell desktop experience for Windows (GTK4)",
         "面向 Windows 的 GNOME Shell 桌面环境（GTK4）"));
  gtk_widget_set_name(sub, "winomeSettingsDim");
  gtk_label_set_xalign(GTK_LABEL(sub), 0.0f);
  gtk_box_append(GTK_BOX(titles), app_name);
  gtk_box_append(GTK_BOX(titles), sub);
  gtk_box_append(GTK_BOX(header), logo);
  gtk_box_append(GTK_BOX(header), titles);
  gtk_box_append(GTK_BOX(page), header);

  GtkWidget* info_card = make_card();
  std::string win_ver = windows_build_summary();
  std::string cfg_path = utf8_from_wide(local_app_winome_dir(L"config.json"));

  append_row_to_card(
      info_card,
      make_row(tr("Host Operating System", "宿主操作系统"),
               win_ver.c_str(), nullptr),
      false);
  append_row_to_card(
      info_card,
      make_row(tr("Configuration File", "配置文件路径"),
               cfg_path.c_str(), nullptr),
      true);

  GtkWidget* crash_btn =
      gtk_button_new_with_label(tr("Open Folder", "打开目录"));
  gtk_widget_set_name(crash_btn, "winomeSettingsButton");
  g_signal_connect(crash_btn, "clicked",
                   G_CALLBACK(on_open_crashes_folder), nullptr);
  append_row_to_card(
      info_card,
      make_row(tr("Crash Minidumps", "崩溃转储目录"),
               tr("Minidumps saved on unhandled exceptions (%LOCALAPPDATA%\\WINOME\\crashes)",
                  "发生未捕获异常时自动保存的小型转储路径"),
               crash_btn),
      true);

  append_row_to_card(
      info_card,
      make_row(tr("Recovery Command", "桌面恢复命令"),
               "winome.exe --restore", nullptr),
      true);

  gtk_box_append(GTK_BOX(page), info_card);
  return page;
}

}  // namespace

void open_settings_window() {
  if (g_ui.window != nullptr) {
    gtk_window_present(GTK_WINDOW(g_ui.window));
    sync_ui_from_config();
    return;
  }

  ensure_settings_css();

  GtkWidget* win = gtk_window_new();
  g_ui.window = win;
  gtk_widget_set_name(win, "winomeSettingsWindow");
  gtk_window_set_title(GTK_WINDOW(win), tr("WINOME Settings", "WINOME 设置"));
  gtk_window_set_default_size(GTK_WINDOW(win), 620, 520);
  gtk_window_set_resizable(GTK_WINDOW(win), TRUE);
  install_bundled_fonts(win);

  GtkWidget* root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_window_set_child(GTK_WINDOW(win), root);

  GtkWidget* stack = gtk_stack_new();
  g_ui.stack = stack;
  gtk_stack_set_transition_type(GTK_STACK(stack),
                                GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_widget_set_hexpand(stack, TRUE);
  gtk_widget_set_vexpand(stack, TRUE);

  auto wrap_scroll = [](GtkWidget* child) {
    GtkWidget* sc = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), child);
    return sc;
  };

  gtk_stack_add_titled(GTK_STACK(stack), wrap_scroll(build_appearance_page()),
                       "appearance", tr("Appearance", "外观与顶栏"));
  gtk_stack_add_titled(GTK_STACK(stack), wrap_scroll(build_behavior_page()),
                       "behavior", tr("Behavior", "行为与交互"));
  gtk_stack_add_titled(GTK_STACK(stack), wrap_scroll(build_about_page()),
                       "about", tr("About", "关于 WINOME"));

  GtkWidget* sidebar = gtk_stack_sidebar_new();
  g_ui.sidebar = sidebar;
  gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar), GTK_STACK(stack));

  gtk_box_append(GTK_BOX(root), sidebar);
  gtk_box_append(GTK_BOX(root), stack);

  add_config_listener(on_settings_config_changed, nullptr);
  g_signal_connect(win, "destroy", G_CALLBACK(on_window_destroy), nullptr);

  sync_ui_from_config();
  gtk_widget_realize(win);
  apply_dark_titlebar(win);
  gtk_window_present(GTK_WINDOW(win));
}

void close_settings_window() {
  if (g_ui.window != nullptr)
    gtk_window_destroy(GTK_WINDOW(g_ui.window));
}

}  // namespace winome
