// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Native GTK4 WINOME Settings preferences window wired to live WinomeConfig.

#include "settings-window.h"

#include "config.h"
#include "debug-runtime.h"
#include "diagnostics-export.h"
#include "fonts.h"
#include "i18n.h"
#include "looking-glass.h"
#include "perf-metrics.h"
#include "virtual-desktop.h"
#include "window-theming.h"
#include "winome-version.h"

#include <gtk/gtk.h>
#include <gdk/win32/gdkwin32.h>
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

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
  GtkWidget* dark_titlebars_switch = nullptr;
  GtkWidget* rounded_corners_switch = nullptr;

  // Behavior controls
  GtkWidget* hot_corner_switch = nullptr;
  GtkWidget* hot_cooldown_spin = nullptr;
  GtkWidget* win_key_switch = nullptr;
  GtkWidget* overview_blacklist_switch = nullptr;
  GtkWidget* live_wallpaper_drop = nullptr;
  GtkWidget* dash_scope_drop = nullptr;
  GtkWidget* ws_mode_drop = nullptr;
  GtkWidget* overview_linked_drop = nullptr;
  GtkWidget* end_session_confirm_switch = nullptr;
  GtkWidget* auto_elevate_switch = nullptr;
  GtkWidget* dev_mode_switch = nullptr;
  GtkWidget* dev_console_btn = nullptr;

  // Diagnostics readout container
  GtkWidget* diag_box = nullptr;
  GtkWidget* diag_export_status = nullptr;

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
  if (g_ui.dark_titlebars_switch != nullptr) {
    gtk_switch_set_active(GTK_SWITCH(g_ui.dark_titlebars_switch),
                          cfg.unify_dark_titlebars ? TRUE : FALSE);
  }
  if (g_ui.rounded_corners_switch != nullptr) {
    gtk_switch_set_active(GTK_SWITCH(g_ui.rounded_corners_switch),
                          cfg.unify_rounded_corners ? TRUE : FALSE);
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
  if (g_ui.overview_blacklist_switch != nullptr) {
    gtk_switch_set_active(GTK_SWITCH(g_ui.overview_blacklist_switch),
                          cfg.overview_window_blacklist ? TRUE : FALSE);
  }
  if (g_ui.live_wallpaper_drop != nullptr) {
    guint sel = 0;
    if (cfg.live_wallpaper_mode == "snapshot")
      sel = 1;
    else if (cfg.live_wallpaper_mode == "off")
      sel = 2;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(g_ui.live_wallpaper_drop), sel);
  }
  if (g_ui.dash_scope_drop != nullptr) {
    gtk_drop_down_set_selected(GTK_DROP_DOWN(g_ui.dash_scope_drop),
                               cfg.dash_isolate_workspaces ? 1u : 0u);
  }
  if (g_ui.ws_mode_drop != nullptr) {
    guint sel = 0;
    if (cfg.multi_monitor_workspace_mode == "primary_only")
      sel = 1;
    else if (cfg.multi_monitor_workspace_mode == "span")
      sel = 2;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(g_ui.ws_mode_drop), sel);
  }
  if (g_ui.overview_linked_drop != nullptr) {
    gtk_drop_down_set_selected(GTK_DROP_DOWN(g_ui.overview_linked_drop),
                               cfg.multi_monitor_overview_linked ? 1u : 0u);
  }
  if (g_ui.end_session_confirm_switch != nullptr) {
    gtk_switch_set_active(GTK_SWITCH(g_ui.end_session_confirm_switch),
                          cfg.end_session_confirm ? TRUE : FALSE);
  }
  if (g_ui.auto_elevate_switch != nullptr) {
    gtk_switch_set_active(GTK_SWITCH(g_ui.auto_elevate_switch),
                          cfg.auto_elevate ? TRUE : FALSE);
  }
  if (g_ui.dev_mode_switch != nullptr) {
    gtk_switch_set_active(GTK_SWITCH(g_ui.dev_mode_switch),
                          cfg.developer_mode ? TRUE : FALSE);
  }
  if (g_ui.dev_console_btn != nullptr) {
    gtk_widget_set_sensitive(g_ui.dev_console_btn,
                             cfg.developer_mode ? TRUE : FALSE);
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

gboolean on_overview_blacklist_set(GtkSwitch* sw G_GNUC_UNUSED,
                                   gboolean state,
                                   gpointer ud G_GNUC_UNUSED) {
  if (g_ui.syncing)
    return FALSE;
  WinomeConfig cfg = get_config();
  cfg.overview_window_blacklist = state != FALSE;
  update_config(cfg);
  return FALSE;
}

void on_live_wallpaper_mode_changed(GObject* obj, GParamSpec* pspec G_GNUC_UNUSED,
                                    gpointer ud G_GNUC_UNUSED) {
  if (g_ui.syncing)
    return;
  WinomeConfig cfg = get_config();
  guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
  cfg.live_wallpaper_mode = (sel == 1) ? "snapshot" : (sel == 2 ? "off" : "live");
  update_config(cfg);
}

void on_dash_scope_changed(GObject* obj, GParamSpec* pspec G_GNUC_UNUSED,
                           gpointer ud G_GNUC_UNUSED) {
  if (g_ui.syncing)
    return;
  WinomeConfig cfg = get_config();
  guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
  cfg.dash_isolate_workspaces = (sel == 1);
  update_config(cfg);
}

void on_ws_mode_changed(GObject* obj, GParamSpec* pspec G_GNUC_UNUSED,
                        gpointer ud G_GNUC_UNUSED) {
  if (g_ui.syncing)
    return;
  WinomeConfig cfg = get_config();
  guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
  cfg.multi_monitor_workspace_mode =
      (sel == 1) ? "primary_only" : (sel == 2 ? "span" : "independent");
  update_config(cfg);
}

void on_overview_linked_changed(GObject* obj, GParamSpec* pspec G_GNUC_UNUSED,
                                gpointer ud G_GNUC_UNUSED) {
  if (g_ui.syncing)
    return;
  WinomeConfig cfg = get_config();
  guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
  cfg.multi_monitor_overview_linked = (sel == 1);
  update_config(cfg);
}

gboolean on_dark_titlebars_set(GtkSwitch* sw G_GNUC_UNUSED, gboolean state, gpointer ud G_GNUC_UNUSED) {
  if (g_ui.syncing)
    return FALSE;
  WinomeConfig cfg = get_config();
  cfg.unify_dark_titlebars = state != FALSE;
  update_config(cfg);
  window_theming_sync_all();
  return FALSE;
}

gboolean on_rounded_corners_set(GtkSwitch* sw G_GNUC_UNUSED, gboolean state, gpointer ud G_GNUC_UNUSED) {
  if (g_ui.syncing)
    return FALSE;
  WinomeConfig cfg = get_config();
  cfg.unify_rounded_corners = state != FALSE;
  update_config(cfg);
  window_theming_sync_all();
  return FALSE;
}

gboolean on_end_session_confirm_set(GtkSwitch* sw G_GNUC_UNUSED, gboolean state, gpointer ud G_GNUC_UNUSED) {
  if (g_ui.syncing)
    return FALSE;
  WinomeConfig cfg = get_config();
  cfg.end_session_confirm = state != FALSE;
  update_config(cfg);
  return FALSE;
}

gboolean on_auto_elevate_set(GtkSwitch* sw G_GNUC_UNUSED, gboolean state, gpointer ud G_GNUC_UNUSED) {
  if (g_ui.syncing)
    return FALSE;
  WinomeConfig cfg = get_config();
  cfg.auto_elevate = state != FALSE;
  update_config(cfg);
  return FALSE;
}

gboolean on_dev_mode_set(GtkSwitch* sw G_GNUC_UNUSED, gboolean state, gpointer ud G_GNUC_UNUSED) {
  if (g_ui.syncing)
    return FALSE;
  WinomeConfig cfg = get_config();
  cfg.developer_mode = state != FALSE;
  update_config(cfg);
  if (!cfg.developer_mode)
    close_looking_glass();
  if (g_ui.dev_console_btn != nullptr) {
    gtk_widget_set_sensitive(g_ui.dev_console_btn,
                             cfg.developer_mode ? TRUE : FALSE);
  }
  return FALSE;
}

void on_open_dev_console(GtkButton* btn G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  open_looking_glass();
}

void populate_diagnostics_box() {
  if (g_ui.diag_box == nullptr)
    return;
  while (GtkWidget* ch = gtk_widget_get_first_child(g_ui.diag_box))
    gtk_box_remove(GTK_BOX(g_ui.diag_box), ch);

  const ProcessTelemetry tel = perf_sample_process_telemetry();
  const StartupTimings st = perf_get_startup_timings();
  const FrameTimeTracker::Stats fs = perf_get_overview_frame_stats();
  const vd::DesktopSnapshot ds = vd::cached_snapshot();
  const auto caps = capability_snapshot_all();

  auto format_mib = [](uint64_t bytes, char* buf, size_t n) {
    std::snprintf(buf, n, "%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  };

  gtk_box_append(GTK_BOX(g_ui.diag_box),
                 make_section_label(tr("Process & Render Telemetry", "进程与渲染遥测")));
  GtkWidget* perf_card = make_card();
  char buf[256];

  if (is_safe_mode()) {
    append_row_to_card(
        perf_card,
        make_row(tr("Runtime Mode: SAFE MODE (--safe-mode)", "运行模式：安全模式（--safe-mode）"),
                 tr("Third-party plugins and opt-in DWM theming are temporarily disabled",
                    "第三方外部插件与实验性 DWM 窗口外观统一已临时停用"),
                 nullptr),
        false);
  }

  char ws_buf[64];
  char priv_buf[64];
  format_mib(tel.working_set_bytes, ws_buf, sizeof(ws_buf));
  format_mib(tel.private_bytes, priv_buf, sizeof(priv_buf));
  std::snprintf(buf, sizeof(buf), "RSS: %s | Private Commit: %s | GDI: %lu | USER: %lu",
                ws_buf, priv_buf,
                static_cast<unsigned long>(tel.gdi_objects),
                static_cast<unsigned long>(tel.user_objects));
  append_row_to_card(
      perf_card,
      make_row(tr("Memory & Win32 Handles", "内存与句柄占用"), buf, nullptr),
      is_safe_mode());

  std::snprintf(buf, sizeof(buf),
                "Total %.1f ms | Fontconfig %.1f ms (%d files / %d fontset) | "
                "Panel %.1f ms | Overview %.1f ms | IPC %.1f ms",
                st.total_startup_ms, st.fontmap_ms, st.fonts_loaded_files,
                st.fonts_family_count, st.panel_ms, st.overview_init_ms,
                st.ipc_ms);
  append_row_to_card(
      perf_card,
      make_row(tr("Startup Phase Timings", "启动阶段耗时"), buf, nullptr),
      true);

  std::snprintf(buf, sizeof(buf),
                "Frames: %llu | Dropped (>20ms): %llu | Avg %.2f ms | P95 %.2f ms | Max %.2f ms",
                static_cast<unsigned long long>(fs.frames),
                static_cast<unsigned long long>(fs.dropped_frames),
                fs.avg_frame_ms, fs.p95_frame_ms, fs.max_frame_ms);
  append_row_to_card(
      perf_card,
      make_row(tr("Overview Animation Frame Budget", "概览动画帧耗时统计"), buf, nullptr),
      true);

  std::snprintf(buf, sizeof(buf),
                "Observer: %s | Desktops: %d | Active: %d",
                ds.valid ? "ready" : "pending/fallback",
                ds.count, ds.active_index + 1);
  append_row_to_card(
      perf_card,
      make_row(tr("Virtual Desktop Cache", "虚拟桌面缓存状态"), buf, nullptr),
      true);
  gtk_box_append(GTK_BOX(g_ui.diag_box), perf_card);

  gtk_box_append(GTK_BOX(g_ui.diag_box),
                 make_section_label(tr("Subsystem Capabilities & Circuit Breakers",
                                       "子系统能力与熔断状态")));
  GtkWidget* cap_card = make_card();
  for (size_t i = 0; i < caps.size(); ++i) {
    const CapabilityStatus& c = caps[i];
    std::snprintf(buf, sizeof(buf), "%s",
                  c.detail.empty() ? "-" : c.detail.c_str());
    char title_buf[128];
    std::snprintf(title_buf, sizeof(title_buf), "%s  [%s | err=%lu]",
                  c.name != nullptr ? c.name : "?",
                  capability_state_label(c.state),
                  static_cast<unsigned long>(c.failure_count));
    append_row_to_card(cap_card, make_row(title_buf, buf, nullptr), i > 0);
  }
  gtk_box_append(GTK_BOX(g_ui.diag_box), cap_card);
}

void on_diag_refresh_clicked(GtkButton* btn G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  populate_diagnostics_box();
}

void on_export_diagnostics_clicked(GtkButton* btn G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  debug_flush_log_repeats();

  const ProcessTelemetry tel = perf_sample_process_telemetry();
  const StartupTimings st = perf_get_startup_timings();
  const FrameTimeTracker::Stats fs = perf_get_overview_frame_stats();
  const vd::DesktopSnapshot ds = vd::cached_snapshot();
  const auto caps = capability_snapshot_all();

  std::string summary;
  char line[512];
  std::snprintf(
      line, sizeof(line),
      "Safe Mode      : %s\n"
      "Memory         : RSS=%.1f MiB, Private=%.1f MiB, GDI=%lu, USER=%lu\n"
      "Startup        : Total=%.1fms (Font=%.1fms [%d/%d], Panel=%.1fms, Overview=%.1fms, IPC=%.1fms)\n"
      "Overview Anim  : Frames=%llu, Dropped=%llu, Avg=%.2fms, P95=%.2fms, Max=%.2fms\n"
      "VD Cache       : %s (count=%d, active=%d)\n",
      is_safe_mode() ? "true" : "false",
      static_cast<double>(tel.working_set_bytes) / (1024.0 * 1024.0),
      static_cast<double>(tel.private_bytes) / (1024.0 * 1024.0),
      static_cast<unsigned long>(tel.gdi_objects),
      static_cast<unsigned long>(tel.user_objects),
      st.total_startup_ms, st.fontmap_ms, st.fonts_loaded_files,
      st.fonts_family_count, st.panel_ms, st.overview_init_ms, st.ipc_ms,
      static_cast<unsigned long long>(fs.frames),
      static_cast<unsigned long long>(fs.dropped_frames),
      fs.avg_frame_ms, fs.p95_frame_ms, fs.max_frame_ms,
      ds.valid ? "ready" : "pending/fallback", ds.count, ds.active_index + 1);
  summary += line;
  summary += "\nSubsystems:\n";
  for (const auto& c : caps) {
    std::snprintf(line, sizeof(line), "  - %s [%s | err=%lu]: %s\n",
                  c.name != nullptr ? c.name : "?",
                  capability_state_label(c.state),
                  static_cast<unsigned long>(c.failure_count),
                  c.detail.empty() ? "-" : c.detail.c_str());
    summary += line;
  }

  DiagnosticsExportOptions opts{};
  opts.data_dir = local_app_winome_dir(nullptr);
  opts.extra_summary_text = std::move(summary);
  opts.flush_stdio = true;

  const auto live_logs = debug_get_logs_since(0, "", 600);
  if (!live_logs.empty()) {
    opts.extra_files.push_back(
        {"debug/live-logs.json", debug_logs_to_json(live_logs)});
  }
  const auto wins = debug_inspect_windows();
  if (!wins.empty()) {
    opts.extra_files.push_back(
        {"debug/zorder-windows.json", debug_windows_to_json(wins)});
  }

  const DiagnosticsExportResult res = export_diagnostics_bundle(opts);
  if (res.ok) {
    reveal_file_in_explorer(res.zip_path);
    if (g_ui.diag_export_status != nullptr) {
      std::string msg = std::string(tr("Exported (", "已导出排障包（共 ")) +
                        std::to_string(res.file_count) +
                        tr(" files): ", " 个文件）：") +
                        utf8_from_wide(res.zip_path);
      gtk_label_set_text(GTK_LABEL(g_ui.diag_export_status), msg.c_str());
      gtk_widget_set_visible(g_ui.diag_export_status, TRUE);
    }
  } else if (g_ui.diag_export_status != nullptr) {
    std::string msg = std::string(tr("Export failed: ", "导出失败：")) + res.error;
    gtk_label_set_text(GTK_LABEL(g_ui.diag_export_status), msg.c_str());
    gtk_widget_set_visible(g_ui.diag_export_status, TRUE);
  }
}

void on_stack_page_changed(GObject* obj, GParamSpec* pspec G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  const char* name = gtk_stack_get_visible_child_name(GTK_STACK(obj));
  if (name != nullptr && std::string(name) == "diagnostics")
    populate_diagnostics_box();
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
  g_ui.dark_titlebars_switch = nullptr;
  g_ui.rounded_corners_switch = nullptr;
  g_ui.hot_corner_switch = nullptr;
  g_ui.hot_cooldown_spin = nullptr;
  g_ui.win_key_switch = nullptr;
  g_ui.overview_blacklist_switch = nullptr;
  g_ui.live_wallpaper_drop = nullptr;
  g_ui.dash_scope_drop = nullptr;
  g_ui.ws_mode_drop = nullptr;
  g_ui.overview_linked_drop = nullptr;
  g_ui.end_session_confirm_switch = nullptr;
  g_ui.auto_elevate_switch = nullptr;
  g_ui.dev_mode_switch = nullptr;
  g_ui.dev_console_btn = nullptr;
  g_ui.diag_box = nullptr;
  g_ui.diag_export_status = nullptr;
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

  gtk_box_append(GTK_BOX(page),
                 make_section_label(tr("Window Unification (Opt-in)",
                                       "应用窗口统一外观（实验性可选）")));
  GtkWidget* win_card = make_card();

  g_ui.dark_titlebars_switch = gtk_switch_new();
  g_signal_connect(g_ui.dark_titlebars_switch, "state-set",
                   G_CALLBACK(on_dark_titlebars_set), nullptr);
  append_row_to_card(
      win_card,
      make_row(tr("Dark Titlebars for Desktop Apps", "强制第三方窗口使用暗色标题栏"),
               tr("Apply DWM dark titlebar hint cleanly via SetWinEventHook (no injection)",
                  "通过 SetWinEventHook 应用 DWM 暗色标题栏提示（零 DLL 注入）"),
               g_ui.dark_titlebars_switch),
      false);

  g_ui.rounded_corners_switch = gtk_switch_new();
  g_signal_connect(g_ui.rounded_corners_switch, "state-set",
                   G_CALLBACK(on_rounded_corners_set), nullptr);
  append_row_to_card(
      win_card,
      make_row(tr("Unify Windows 11 Rounded Corners", "统一 Windows 11 窗口圆角"),
               tr("Prefer standard DWM rounded corners on external app windows",
                  "为外部桌面窗口优先请求标准 Win11 DWM 圆角外观"),
               g_ui.rounded_corners_switch),
      true);

  gtk_box_append(GTK_BOX(page), win_card);
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

  g_ui.overview_blacklist_switch = gtk_switch_new();
  g_signal_connect(g_ui.overview_blacklist_switch, "state-set",
                   G_CALLBACK(on_overview_blacklist_set), nullptr);
  append_row_to_card(
      trig_card,
      make_row(tr("Overview System Blacklist", "概览默认黑名单"),
               tr("Exclude \"Windows Input Experience\", suspended/ghost \"Settings\" UWP windows, and nameless blank Explorer windows from Overview",
                  "默认在概览中过滤“Windows输入体验”、后台挂起的“设置”幽灵窗口及资源管理器的无名无画面窗口"),
               g_ui.overview_blacklist_switch),
      true);

  const char* const live_wp_modes[] = {
      tr("Live (30 FPS in Overview)", "实时动态（概览中 30 FPS）"),
      tr("Snapshot on Open", "打开概览时抓帧"),
      tr("Off (Static Wallpaper)", "关闭（仅静态壁纸）"),
      nullptr};
  g_ui.live_wallpaper_drop = gtk_drop_down_new_from_strings(live_wp_modes);
  g_signal_connect(g_ui.live_wallpaper_drop, "notify::selected",
                   G_CALLBACK(on_live_wallpaper_mode_changed), nullptr);
  append_row_to_card(
      trig_card,
      make_row(tr("Live Wallpaper Engine Support", "第三方动态壁纸兼容"),
               tr("Automatically mirror Wallpaper Engine / Lively / Sucrose inside Overview via borderless Windows.Graphics.Capture (zero overhead when inactive)",
                  "自动检测 Wallpaper Engine / Lively / Sucrose 等壁纸引擎并通过无边框 WGC 在概览内实时同步（未运行时零开销）"),
               g_ui.live_wallpaper_drop),
      true);

  const char* const dash_scopes[] = {
      tr("All Workspaces", "所有工作区"),
      tr("Current Workspace Only", "仅当前工作区"),
      nullptr};
  g_ui.dash_scope_drop = gtk_drop_down_new_from_strings(dash_scopes);
  g_signal_connect(g_ui.dash_scope_drop, "notify::selected",
                   G_CALLBACK(on_dash_scope_changed), nullptr);
  append_row_to_card(
      trig_card,
      make_row(tr("Dash Applications", "程序坞应用范围"),
               tr("Show running applications from all workspaces or only the active workspace",
                  "选择在程序坞中显示所有工作区的应用，或仅显示当前工作区的应用"),
               g_ui.dash_scope_drop),
      true);

  const char* const ws_modes[] = {
      tr("Independent per Display (WINOME)", "每显示器独立工作区（WINOME 引擎）"),
      tr("Primary Display Only (GNOME Default)", "仅主显示器使用工作区（GNOME 默认）"),
      tr("Workspaces Span Displays", "工作区跨越所有显示器（全局同步）"),
      nullptr};
  g_ui.ws_mode_drop = gtk_drop_down_new_from_strings(ws_modes);
  g_signal_connect(g_ui.ws_mode_drop, "notify::selected",
                   G_CALLBACK(on_ws_mode_changed), nullptr);
  append_row_to_card(
      trig_card,
      make_row(tr("Multi-Monitor Workspaces", "多显示器工作区模式"),
               tr("Switch each display independently (built-in engine), keep secondary displays fixed (GNOME default), or switch all displays together",
                  "独立切换每块显示器的工作区（1屏切换不影响2屏），或采用 GNOME 默认（仅主屏切换、副屏固定）及全局同步切换"),
               g_ui.ws_mode_drop),
      true);

  const char* const ov_link_modes[] = {
      tr("Independent (Simultaneous, Unlinked)", "允许同时打开（各屏独立开关，不联动）"),
      tr("Linked (Open / Close All Together)", "允许同时打开（全屏联动开关）"),
      nullptr};
  g_ui.overview_linked_drop = gtk_drop_down_new_from_strings(ov_link_modes);
  g_signal_connect(g_ui.overview_linked_drop, "notify::selected",
                   G_CALLBACK(on_overview_linked_changed), nullptr);
  append_row_to_card(
      trig_card,
      make_row(tr("Multi-Monitor Overview", "多显示器概览行为"),
               tr("Allow each display to open/close Overview independently at the same time, or link all displays to open/close together",
                  "默认允许在各显示器上分别同时打开或关闭概览（互不联动），也可切换为触发任一屏时全屏联动展开/关闭"),
               g_ui.overview_linked_drop),
      true);

  g_ui.end_session_confirm_switch = gtk_switch_new();
  g_signal_connect(g_ui.end_session_confirm_switch, "state-set",
                   G_CALLBACK(on_end_session_confirm_set), nullptr);
  append_row_to_card(
      trig_card,
      make_row(tr("End-Session 60s Confirmation", "注销 / 重启 / 关机前 60 秒确认弹窗"),
               tr("Show GNOME-style 60-second countdown modal before Restart, Power Off, or Log Out",
                  "在执行重启、关机或注销前显示 GNOME 风格 60 秒倒计时确认弹窗"),
               g_ui.end_session_confirm_switch),
      true);

  g_ui.auto_elevate_switch = gtk_switch_new();
  g_signal_connect(g_ui.auto_elevate_switch, "state-set",
                   G_CALLBACK(on_auto_elevate_set), nullptr);
  append_row_to_card(
      trig_card,
      make_row(tr("Auto-Elevate to Administrator", "启动时自动以管理员权限运行"),
               tr("Automatically relaunch elevated so Win / Alt+F2 hotkeys work over administrator windows (applies on next start)",
                  "自动以管理员权限启动，确保 Win / Alt+F2 全局快捷键在管理员窗口前台时也稳定生效（下次启动生效）"),
               g_ui.auto_elevate_switch),
      true);

  gtk_box_append(GTK_BOX(page), trig_card);

  gtk_box_append(GTK_BOX(page),
                 make_section_label(tr("Keyboard Shortcuts", "常用快捷键一览")));
  GtkWidget* keys_card = make_card();
  append_row_to_card(
      keys_card,
      make_row("Super (Win)  /  2x Win",
               tr("Toggle Activities Overview / double-tap smoothly transitions to App Grid",
                  "单按切换概览视图 / 快速双击平滑进入全部应用网格"),
               nullptr),
      false);
  append_row_to_card(
      keys_card,
      make_row("Win + PageUp / PageDown",
               tr("Switch to previous or next virtual workspace",
                  "切换到上一个或下一个虚拟桌面工作区"),
               nullptr),
      true);
  append_row_to_card(
      keys_card,
      make_row("Win + Shift + PageUp / PageDown",
               tr("Move active foreground window to adjacent workspace and follow it",
                  "将当前活动窗口移动到相邻工作区并跟随切换"),
               nullptr),
      true);
  append_row_to_card(
      keys_card,
      make_row("Alt + F2",
               tr("Open GNOME Run Dialog (commands, URLs, or Dev Mode lg/rt/r)",
                  "打开 GNOME 运行命令弹窗（程序/网址，开发者模式下支持 lg/rt/r）"),
               nullptr),
      true);
  append_row_to_card(
      keys_card,
      make_row("Win + Tab  /  Win + Ctrl + Arrow",
               tr("Open Windows Start Menu  /  Native Windows desktop shortcuts",
                  "Win+Tab 打开系统开始菜单 / Win+Ctrl+方向键同步通知虚拟桌面切换"),
               nullptr),
      true);
  gtk_box_append(GTK_BOX(page), keys_card);

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

  gtk_box_append(GTK_BOX(page),
                 make_section_label(tr("Developer Mode & Runtime Debugging",
                                       "开发者模式与运行时调试")));
  GtkWidget* dev_card = make_card();

  g_ui.dev_mode_switch = gtk_switch_new();
  g_signal_connect(g_ui.dev_mode_switch, "state-set",
                   G_CALLBACK(on_dev_mode_set), nullptr);
  append_row_to_card(
      dev_card,
      make_row(tr("Developer Mode", "开发者模式"),
               tr("Unlock in-host Looking Glass console, internal Run commands (lg/rt/r), and user-debug.css",
                  "开启后可在当前界面直接打开 Looking Glass 运行时控制台、启用 Alt+F2 内部指令 (lg/rt/r) 与 user-debug.css 覆盖"),
               g_ui.dev_mode_switch),
      false);

  g_ui.dev_console_btn =
      gtk_button_new_with_label(tr("Open Console", "打开控制台"));
  gtk_widget_set_name(g_ui.dev_console_btn, "winomeSettingsButton");
  g_signal_connect(g_ui.dev_console_btn, "clicked",
                   G_CALLBACK(on_open_dev_console), nullptr);
  append_row_to_card(
      dev_card,
      make_row(tr("Runtime Developer Console (Looking Glass)", "Looking Glass 运行时控制台"),
               tr("Execute IPC commands, stream live logs, inspect Z-order/CSS, and control plugins & circuit breakers",
                  "直接执行运行时指令、订阅实时日志、检测控件 CSS 与窗口 Z-Order、管理插件与故障注入（无需全局快捷键）"),
               g_ui.dev_console_btn),
      true);

  gtk_box_append(GTK_BOX(page), dev_card);
  return page;
}

GtkWidget* build_about_page() {
  GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
  gtk_widget_set_name(page, "winomeSettingsPage");

  GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
  GtkWidget* logo = gtk_image_new_from_icon_name("computer-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(logo), 48);

  GtkWidget* titles = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget* app_name = gtk_label_new("WINOME Shell " WINOME_VERSION);
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

  GtkWidget* about_export_btn =
      gtk_button_new_with_label(tr("Export .zip", "导出排障包"));
  gtk_widget_set_name(about_export_btn, "winomeSettingsButton");
  g_signal_connect(about_export_btn, "clicked",
                   G_CALLBACK(on_export_diagnostics_clicked), nullptr);
  append_row_to_card(
      info_card,
      make_row(tr("Troubleshooting Bundle", "一键导出排障信息压缩包"),
               tr("Package logs, crash minidumps, config, and display/telemetry report into a .zip on Desktop",
                  "将运行日志、崩溃转储、配置文件与多屏/遥测摘要打包为桌面 .zip 文件，方便反馈问题"),
               about_export_btn),
      true);

  append_row_to_card(
      info_card,
      make_row(tr("Recovery Command", "桌面恢复命令"),
               "winome.exe --restore", nullptr),
      true);

  gtk_box_append(GTK_BOX(page), info_card);
  return page;
}

GtkWidget* build_diagnostics_page() {
  GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_name(page, "winomeSettingsPage");

  GtkWidget* hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget* title = make_section_label(
      tr("Runtime Health & Performance", "运行时健康状态与性能遥测"));
  gtk_widget_set_hexpand(title, TRUE);
  GtkWidget* export_btn =
      gtk_button_new_with_label(tr("Export Diagnostics (.zip)", "一键导出排障包"));
  gtk_widget_set_name(export_btn, "winomeSettingsButton");
  g_signal_connect(export_btn, "clicked",
                   G_CALLBACK(on_export_diagnostics_clicked), nullptr);
  GtkWidget* refresh_btn = gtk_button_new_with_label(tr("Refresh", "刷新"));
  gtk_widget_set_name(refresh_btn, "winomeSettingsButton");
  g_signal_connect(refresh_btn, "clicked",
                   G_CALLBACK(on_diag_refresh_clicked), nullptr);
  gtk_box_append(GTK_BOX(hdr), title);
  gtk_box_append(GTK_BOX(hdr), export_btn);
  gtk_box_append(GTK_BOX(hdr), refresh_btn);
  gtk_box_append(GTK_BOX(page), hdr);

  g_ui.diag_export_status = gtk_label_new("");
  gtk_widget_set_name(g_ui.diag_export_status, "winomeSettingsDim");
  gtk_label_set_xalign(GTK_LABEL(g_ui.diag_export_status), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(g_ui.diag_export_status), TRUE);
  gtk_label_set_selectable(GTK_LABEL(g_ui.diag_export_status), TRUE);
  gtk_widget_set_visible(g_ui.diag_export_status, FALSE);
  gtk_box_append(GTK_BOX(page), g_ui.diag_export_status);

  g_ui.diag_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_box_append(GTK_BOX(page), g_ui.diag_box);
  populate_diagnostics_box();
  return page;
}

}  // namespace

void open_settings_window() {
  if (g_ui.window != nullptr) {
    gtk_window_present(GTK_WINDOW(g_ui.window));
    sync_ui_from_config();
    populate_diagnostics_box();
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
  gtk_stack_add_titled(GTK_STACK(stack), wrap_scroll(build_diagnostics_page()),
                       "diagnostics", tr("Diagnostics", "诊断与遥测"));
  gtk_stack_add_titled(GTK_STACK(stack), wrap_scroll(build_about_page()),
                       "about", tr("About", "关于 WINOME"));
  g_signal_connect(stack, "notify::visible-child-name",
                   G_CALLBACK(on_stack_page_changed), nullptr);

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

void open_settings_window_page(const char* page_name) {
  open_settings_window();
  if (g_ui.stack != nullptr && page_name != nullptr && page_name[0] != '\0') {
    const std::string target =
        (std::string(page_name) == "developer") ? "behavior" : std::string(page_name);
    if (gtk_stack_get_child_by_name(GTK_STACK(g_ui.stack), target.c_str()) != nullptr) {
      gtk_stack_set_visible_child_name(GTK_STACK(g_ui.stack), target.c_str());
      if (target == "diagnostics")
        populate_diagnostics_box();
    }
  }
}

void close_settings_window() {
  if (g_ui.window != nullptr)
    gtk_window_destroy(GTK_WINDOW(g_ui.window));
}

}  // namespace winome
