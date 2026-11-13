// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Start Menu application index, search matching, launching, and bounded LRU
// GdkTexture icon cache shared between Overview Dash and the in-host App Grid.

#pragma once

#include <gdk/gdk.h>
#include <windows.h>

#include <cstddef>
#include <string>
#include <vector>

namespace winome {

struct AppEntry {
  std::string id;           // canonical lowercased key (AUMID, exe, or lnk)
  std::string name;         // display name (UTF-8)
  std::string name_lower;   // lowercased display name
  std::string target_lower; // lowercased target exe basename
  std::wstring launch_path; // .lnk / .exe path passed to ShellExecuteW
  std::wstring target_exe;  // resolved target .exe path (may be empty for UWP)
  std::wstring icon_path;   // explicit custom icon path from .lnk (optional)
  int icon_index = 0;
  std::wstring aumid;       // PKEY_AppUserModel_ID if set on the shortcut
  bool is_uwp = false;
};

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

// Pin helpers shared between Dash and App Grid (backed by WinomeConfig).
bool is_app_id_pinned (const std::vector<std::string> &pinned_apps,
                       const std::string &id_or_aumid);
bool set_app_id_pinned (const std::string &id_or_aumid, bool pinned);
bool toggle_app_id_pinned (const std::string &id_or_aumid);

// Launching & shell actions.
bool app_index_launch (const AppEntry &entry, bool as_admin = false);
bool app_index_launch_id (const std::string &id_or_aumid,
                          bool as_admin = false);
bool app_index_open_location (const AppEntry &entry);
bool app_index_launch_best_match (const std::string &query_lower);

// GdkTexture conversion & bounded LRU icon cache.
GdkTexture *hicon_to_texture (HICON icon);
GdkTexture *texture_from_hbitmap (HBITMAP bmp);

GdkTexture *app_icon_cache_lookup (const std::string &cache_key, int size);
void app_icon_cache_store (const std::string &cache_key, int size,
                           GdkTexture *texture);
void app_icon_cache_clear (void);
size_t app_icon_cache_count (void);
size_t app_icon_cache_capacity (void);

GdkTexture *app_icon_for_path (const std::wstring &path, int size,
                               const std::string &cache_key = {});
GdkTexture *app_icon_for_entry (const AppEntry &entry, int size);
GdkTexture *app_icon_for_window (HWND hwnd, const std::wstring &exe, int size,
                                 const std::string &cache_key = {});

}  // namespace winome
