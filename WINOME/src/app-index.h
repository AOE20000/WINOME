// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Start Menu application index, search matching, launching, and bounded LRU
// GdkTexture icon cache shared between Overview Dash and the in-host App Grid.

#pragma once

#include <gdk/gdk.h>
#include <windows.h>

#include <cstddef>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace winome {

struct AppEntry {
  std::string id;           // canonical lowercased key (AUMID, exe, or lnk)
  std::string name;         // display name (UTF-8)
  std::string name_lower;   // lowercased display name
  std::string target_lower; // lowercased target exe basename
  std::wstring launch_path; // .lnk / .url / .exe path passed to ShellExecuteW
  std::wstring target_exe;  // resolved target .exe path (may be empty for UWP)
  std::wstring icon_path;   // explicit custom icon path from .lnk / .url (optional)
  int icon_index = 0;
  std::wstring aumid;       // PKEY_AppUserModel_ID or AppsFolder AUMID
  std::string launch_url;   // [InternetShortcut] URL (for .url shortcuts, lowercased)
  bool is_uwp = false;
  std::string folder_name;  // Start Menu\Programs subfolder display name (empty if root/UWP)
  int folder_order = -1;    // directory order index of folder_name (-1 if none)
  int item_order = 0;       // directory order index within folder/root
  bool from_user_programs = false;
};

enum class AppGridItemKind { App, Folder };

struct AppGridItem {
  AppGridItemKind kind = AppGridItemKind::App;
  std::string id;
  std::string title;
  AppEntry app;
  std::vector<AppEntry> children;
};

struct StartMenuPinnedItem {
  enum class Kind { DesktopLink, PackagedApp, DesktopAppId, Folder };
  Kind kind = Kind::DesktopLink;
  std::string value;
  std::vector<StartMenuPinnedItem> children;
};

// Pure helpers exposed for deterministic unit testing and UI consumers.
bool decrypt_start2_bin_bytes(const std::string& raw_bytes,
                              std::string* out_json);
std::vector<StartMenuPinnedItem> parse_windows_start_layout_json(
    const std::string& json_text);
std::vector<StartMenuPinnedItem> load_windows_start_menu_pinned();
std::vector<StartMenuPinnedItem> app_index_get_pinned_layout();
std::vector<AppGridItem> build_app_grid_items(
    const std::vector<AppEntry>& apps,
    const std::vector<StartMenuPinnedItem>& pinned_items,
    const std::string& query_lower);
std::string app_index_debug_dump_json();
void app_index_start_watch();
void app_index_stop_watch();

// Pure helpers (unit-tested in winome-tests).
bool is_ignored_shortcut_name (const std::string &name_lower);
std::string normalize_app_id (const std::string &id);
std::string canonical_app_id (const std::wstring &aumid,
                              const std::wstring &target_exe,
                              const std::wstring &lnk_path);
int score_app_search_match (const AppEntry &entry,
                            const std::string &query_lower);
std::vector<AppEntry> filter_and_sort_apps (
    const std::vector<AppEntry> &entries, const std::string &query_lower);

// UTF-8 / UTF-16 conversion helpers shared across overview modules.
std::string app_wide_to_utf8 (const std::wstring &w);
std::wstring app_utf8_to_wide (const std::string &s);
std::string app_utf8_tolower (const std::string &s);

// Index lifecycle & lookup.
void app_index_refresh_async (void (*on_ready) (void *) = nullptr,
                              void *user_data = nullptr);
std::vector<AppEntry> app_index_get_all (void);
bool app_index_find_by_id (const std::string &id_or_aumid, AppEntry *out);
uint64_t app_index_revision (void);

// Pin helpers shared between Dash and App Grid (backed by WinomeConfig).
bool is_app_id_pinned (const std::vector<std::string> &pinned_apps,
                       const std::string &id_or_aumid);
bool set_app_id_pinned (const std::string &id_or_aumid, bool pinned);
bool toggle_app_id_pinned (const std::string &id_or_aumid);

// Launching & shell actions.
bool app_index_launch (const AppEntry &entry,
                       bool as_admin = false,
                       HMONITOR target_monitor = nullptr);
bool app_index_launch_id (const std::string &id_or_aumid,
                          bool as_admin = false,
                          HMONITOR target_monitor = nullptr);
bool app_index_open_location (const AppEntry &entry,
                              HMONITOR target_monitor = nullptr);
bool app_index_launch_best_match (const std::string &query_lower,
                                  HMONITOR target_monitor = nullptr);

// Running-window inspection & activation shared between App Grid and Dash.
struct RunningWindow {
  HWND hwnd = nullptr;
  std::string title;
  std::string canonical_id;
  std::string aumid_norm;
  std::string exe_norm;
  std::string exe_base_lower;
};

std::vector<RunningWindow> match_entry_running_windows (
    const AppEntry &entry, const std::vector<RunningWindow> &windows);
std::vector<RunningWindow> app_index_enumerate_running_windows (void);
std::vector<RunningWindow> app_index_running_windows_for_entry (
    const AppEntry &entry);
bool app_index_activate_hwnd (HWND hwnd);
void app_index_close_windows (const std::vector<HWND> &hwnds);

// Thread-safe decoded BGRA pixel buffer ready to materialize into a GdkTexture
// on the GTK main thread.
struct RawDecodedIcon {
  std::string id;
  int size = 0;
  int width = 0;
  int height = 0;
  bool unpremultiplied = false;
  GBytes *bytes = nullptr;
};

void app_icon_raw_clear (RawDecodedIcon &raw);
GdkTexture *app_icon_texture_from_raw (const RawDecodedIcon &raw);
RawDecodedIcon app_icon_decode_entry_raw (const AppEntry &entry, int size);

// GdkTexture conversion & bounded LRU icon cache.
GdkTexture *hicon_to_texture (HICON icon);
GdkTexture *texture_from_hbitmap (HBITMAP bmp);

GdkTexture *app_icon_cache_lookup (const std::string &cache_key, int size);
void app_icon_cache_store (const std::string &cache_key, int size,
                           GdkTexture *texture);
void app_icon_cache_clear (void);
void app_icon_cache_trim (size_t max_keep);
size_t app_icon_cache_count (void);
size_t app_icon_cache_capacity (void);

GdkTexture *app_icon_for_path (const std::wstring &path, int size,
                               const std::string &cache_key = {});
GdkTexture *app_icon_for_entry (const AppEntry &entry, int size);
GdkTexture *app_icon_for_window (HWND hwnd, const std::wstring &exe, int size,
                                 const std::string &cache_key = {});

}  // namespace winome
