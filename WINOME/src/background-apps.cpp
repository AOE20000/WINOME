// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Async Windows Background Apps + System Tray aggregator for Quick Settings.

#include "background-apps.h"

#include "config.h"
#include "i18n.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace winome {
namespace {

struct ListenerEntry {
  BackgroundAppsChangedCallback cb = nullptr;
  void* user_data = nullptr;
};

struct CloseEnumCtx {
  std::unordered_set<DWORD> pids;
  std::vector<HWND> hwnds;
};

std::mutex g_state_mu;
std::vector<BackgroundAppEntry> g_snapshot;
std::string g_snapshot_digest;
std::vector<ListenerEntry> g_listeners;
bool g_has_snapshot = false;

std::atomic<bool> g_running{false};
std::atomic<bool> g_in_flight{false};
std::atomic<bool> g_pending_force{false};
std::atomic<gint64> g_last_refresh_us{0};
std::atomic<uint64_t> g_generation{0};

std::string to_lower_ascii(const std::string& s) {
  std::string out = s;
  for (char& ch : out)
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return out;
}

std::string trim_ascii(const std::string& s) {
  size_t b = 0;
  while (b < s.size() &&
         std::isspace(static_cast<unsigned char>(s[b]))) {
    ++b;
  }
  size_t e = s.size();
  while (e > b &&
         std::isspace(static_cast<unsigned char>(s[e - 1]))) {
    --e;
  }
  return s.substr(b, e - b);
}

std::string exe_basename_lower_from_wide(const std::wstring& path) {
  if (path.empty())
    return {};
  const size_t slash = path.find_last_of(L"\\/");
  std::wstring base =
      (slash == std::wstring::npos) ? path : path.substr(slash + 1);
  return app_utf8_tolower(app_wide_to_utf8(base));
}

bool ascii_icmp(const std::string& a, const std::string& b) {
  const std::string la = to_lower_ascii(a);
  const std::string lb = to_lower_ascii(b);
  if (la != lb)
    return la < lb;
  return a < b;
}

std::string compute_snapshot_digest(
    const std::vector<BackgroundAppEntry>& items) {
  std::string digest;
  digest.reserve(items.size() * 48);
  for (const auto& item : items) {
    digest += (item.kind == BackgroundAppKind::TrayApp) ? "T|" : "B|";
    digest += item.key;
    digest += '|';
    digest += item.title;
    digest += '|';
    digest += item.detail;
    digest += '|';
    digest += std::to_string(item.pids.size());
    digest += '|';
    digest += std::to_string(item.rgba_pixels.size());
    digest += '\n';
  }
  return digest;
}

std::unordered_map<uint64_t, TrayOwnerMeta> resolve_tray_owners(
    const std::vector<TrayIconEntry>& tray_items) {
  std::unordered_map<uint64_t, TrayOwnerMeta> out;
  for (const auto& item : tray_items) {
    if (item.target_hwnd == 0)
      continue;
    HWND hwnd = reinterpret_cast<HWND>(item.target_hwnd);
    if (!IsWindow(hwnd))
      continue;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0)
      continue;
    TrayOwnerMeta meta;
    meta.pid = static_cast<uint32_t>(pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc != nullptr) {
      wchar_t buf[MAX_PATH] = {};
      DWORD len = MAX_PATH;
      if (QueryFullProcessImageNameW(proc, 0, buf, &len) && len > 0)
        meta.exe_base_lower = exe_basename_lower_from_wide(std::wstring(buf, len));
      CloseHandle(proc);
    }
    out.emplace(item.id, std::move(meta));
  }
  return out;
}

std::vector<RawProcessInfo> enumerate_user_session_processes() {
  std::vector<RawProcessInfo> out;
  DWORD my_sid = 0;
  ProcessIdToSessionId(GetCurrentProcessId(), &my_sid);

  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return out;

  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) {
    do {
      if (pe.th32ProcessID == 0)
        continue;
      DWORD sid = 0;
      if (!ProcessIdToSessionId(pe.th32ProcessID, &sid) || sid != my_sid)
        continue;
      std::string exe = exe_basename_lower_from_wide(pe.szExeFile);
      if (exe.empty())
        continue;
      out.push_back(RawProcessInfo{
          static_cast<uint32_t>(pe.th32ProcessID),
          std::move(exe),
      });
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return out;
}

gboolean notify_listeners_idle(gpointer ud) {
  const uint64_t expected_gen = static_cast<uint64_t>(
      reinterpret_cast<uintptr_t>(ud));
  if (!g_running.load(std::memory_order_acquire) ||
      g_generation.load(std::memory_order_acquire) != expected_gen) {
    return G_SOURCE_REMOVE;
  }

  std::vector<ListenerEntry> snapshot;
  {
    std::lock_guard<std::mutex> lock(g_state_mu);
    snapshot = g_listeners;
  }
  for (const auto& entry : snapshot) {
    if (entry.cb != nullptr)
      entry.cb(entry.user_data);
  }
  return G_SOURCE_REMOVE;
}

void on_systray_source_changed(void* /*ud*/) {
  background_apps_refresh_async(false);
}

void on_config_source_changed(void* /*ud*/) {
  background_apps_refresh_async(true);
}

BOOL CALLBACK collect_pid_top_windows_cb(HWND hwnd, LPARAM lparam) {
  auto* ctx = reinterpret_cast<CloseEnumCtx*>(lparam);
  if (ctx == nullptr)
    return TRUE;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != 0 && ctx->pids.count(pid) > 0)
    ctx->hwnds.push_back(hwnd);
  return TRUE;
}

bool is_process_still_alive(DWORD pid) {
  HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (h == nullptr)
    return false;
  DWORD wait_rc = WaitForSingleObject(h, 0);
  CloseHandle(h);
  return wait_rc == WAIT_TIMEOUT;
}

}  // namespace

std::string compact_tray_title_and_detail(const std::string& raw_title,
                                          const std::string& app_hint,
                                          std::string* out_detail) {
  if (out_detail != nullptr)
    out_detail->clear();

  size_t split_pos = std::string::npos;
  size_t sep_len = 0;

  const size_t nl = raw_title.find_first_of("\r\n");
  if (nl != std::string::npos) {
    split_pos = nl;
    sep_len = 1;
    if (raw_title[nl] == '\r' && nl + 1 < raw_title.size() &&
        raw_title[nl + 1] == '\n') {
      sep_len = 2;
    }
  }

  const size_t dash = raw_title.find(" - ");
  if (dash != std::string::npos && dash >= 2 &&
      (split_pos == std::string::npos || dash < split_pos)) {
    split_pos = dash;
    sep_len = 3;
  }

  const size_t emdash = raw_title.find(" \xE2\x80\x94 ");
  if (emdash != std::string::npos && emdash >= 2 &&
      (split_pos == std::string::npos || emdash < split_pos)) {
    split_pos = emdash;
    sep_len = 5;
  }

  std::string base =
      (split_pos == std::string::npos) ? raw_title : raw_title.substr(0, split_pos);
  base = trim_ascii(base);
  while (!base.empty() &&
         (base.back() == ':' || base.back() == ',' || base.back() == ';')) {
    base.pop_back();
    base = trim_ascii(base);
  }

  if (base.empty())
    base = !app_hint.empty() ? trim_ascii(app_hint) : trim_ascii(raw_title);

  if (out_detail != nullptr && split_pos != std::string::npos &&
      split_pos + sep_len < raw_title.size()) {
    std::string detail = raw_title.substr(split_pos + sep_len);
    for (char& ch : detail) {
      if (ch == '\r' || ch == '\n' || ch == '\t')
        ch = ' ';
    }
    detail = trim_ascii(detail);
    while (!detail.empty() &&
           (detail.front() == '-' || detail.front() == ':' ||
            detail.front() == ' ')) {
      detail.erase(detail.begin());
    }
    detail = trim_ascii(detail);
    if (!detail.empty() && to_lower_ascii(detail) != to_lower_ascii(base))
      *out_detail = std::move(detail);
  }

  return base;
}

std::string format_background_app_subtitle(BackgroundAppKind kind,
                                           const std::string& detail,
                                           bool zh) {
  const char* tag = (kind == BackgroundAppKind::TrayApp)
                        ? (zh ? "\xE6\x89\x98\xE7\x9B\x98\xE5\xBA\x94\xE7\x94\xA8"
                              : "Tray App")
                        : (zh ? "\xE5\x90\x8E\xE5\x8F\xB0\xE5\xBA\x94\xE7\x94\xA8"
                              : "Background App");
  if (detail.empty())
    return tag;
  return std::string(tag) + " \xC2\xB7 " + detail;
}

std::string format_background_apps_toggle_title(size_t total_count, bool zh) {
  char buf[96];
  if (zh) {
    if (total_count == 0)
      return "\xE6\x97\xA0\xE5\x90\x8E\xE5\x8F\xB0\xE5\xBA\x94\xE7\x94\xA8";
    std::snprintf(buf, sizeof(buf),
                  "%zu \xE4\xB8\xAA\xE5\x90\x8E\xE5\x8F\xB0\xE5\xBA\x94\xE7\x94\xA8",
                  total_count);
    return buf;
  }
  if (total_count == 0)
    return "No Background Apps";
  if (total_count == 1)
    return "1 Background App";
  std::snprintf(buf, sizeof(buf), "%zu Background Apps", total_count);
  return buf;
}

std::string format_background_apps_header_subtitle(size_t tray_count,
                                                   size_t bg_count,
                                                   bool zh) {
  char buf[128];
  if (tray_count == 0 && bg_count == 0) {
    return zh ? "\xE6\x9A\x82\xE6\x97\xA0\xE8\xBF\x90\xE8\xA1\x8C\xE4\xB8\xAD\xE5\xBA\x94\xE7\x94\xA8"
              : "None running";
  }
  if (tray_count > 0 && bg_count > 0) {
    if (zh) {
      std::snprintf(buf, sizeof(buf),
                    "%zu \xE6\x89\x98\xE7\x9B\x98 \xC2\xB7 %zu \xE5\x90\x8E\xE5\x8F\xB0",
                    tray_count, bg_count);
    } else {
      std::snprintf(buf, sizeof(buf),
                    "%zu tray \xC2\xB7 %zu background",
                    tray_count, bg_count);
    }
    return buf;
  }
  if (tray_count > 0) {
    if (zh) {
      std::snprintf(buf, sizeof(buf),
                    "%zu \xE4\xB8\xAA\xE6\x89\x98\xE7\x9B\x98\xE5\xBA\x94\xE7\x94\xA8",
                    tray_count);
    } else {
      std::snprintf(buf, sizeof(buf),
                    tray_count == 1 ? "1 tray app" : "%zu tray apps",
                    tray_count);
    }
    return buf;
  }
  if (zh) {
    std::snprintf(buf, sizeof(buf),
                  "%zu \xE4\xB8\xAA\xE5\x90\x8E\xE5\x8F\xB0\xE5\xBA\x94\xE7\x94\xA8",
                  bg_count);
  } else {
    std::snprintf(buf, sizeof(buf),
                  bg_count == 1 ? "1 background app" : "%zu background apps",
                  bg_count);
  }
  return buf;
}

bool is_ignored_background_exe(const std::string& exe_base_lower) {
  if (exe_base_lower.empty())
    return true;
  static const char* const kIgnored[] = {
      "winome.exe",
      "explorer.exe",
      "cmd.exe",
      "powershell.exe",
      "pwsh.exe",
      "conhost.exe",
      "rundll32.exe",
      "msiexec.exe",
      "regedit.exe",
      "mmc.exe",
      "taskmgr.exe",
      "svchost.exe",
      "dllhost.exe",
      "applicationframehost.exe",
      "runtimebroker.exe",
      "searchhost.exe",
      "shellexperiencehost.exe",
      "startmenuexperiencehost.exe",
      "textinputhost.exe",
      "systemsettings.exe",
      "msedgewebview2.exe",
      "crashpad_handler.exe",
      "update.exe",
      "updater.exe",
      "elevation_service.exe",
      "backgroundtaskhost.exe",
      "wscript.exe",
      "cscript.exe",
      "control.exe",
      "werfault.exe",
  };
  const std::string lower = to_lower_ascii(exe_base_lower);
  for (const char* name : kIgnored) {
    if (lower == name)
      return true;
  }
  return false;
}

std::vector<BackgroundAppEntry> merge_background_and_tray_apps(
    const std::vector<TrayIconEntry>& tray_items,
    const std::unordered_map<uint64_t, TrayOwnerMeta>& tray_owners,
    const std::vector<AppEntry>& installed_apps,
    const std::vector<RawProcessInfo>& running_procs,
    const std::vector<RunningWindow>& visible_windows,
    bool include_tray,
    uint32_t self_pid) {
  std::unordered_set<std::string> visible_exes;
  for (const auto& w : visible_windows) {
    if (!w.exe_base_lower.empty())
      visible_exes.insert(to_lower_ascii(w.exe_base_lower));
  }

  // Index installed apps by exe basename so both tray items (for richer icons/IDs)
  // and windowless background processes resolve consistently.
  std::unordered_map<std::string, const AppEntry*> exe_to_app;
  for (const auto& app : installed_apps) {
    if (app.target_lower.empty() || is_ignored_background_exe(app.target_lower))
      continue;
    auto it = exe_to_app.find(app.target_lower);
    if (it == exe_to_app.end()) {
      exe_to_app.emplace(app.target_lower, &app);
    } else {
      const AppEntry* prev = it->second;
      const bool prefer =
          (prev->is_uwp && !app.is_uwp) ||
          (prev->is_uwp == app.is_uwp && app.name.size() < prev->name.size());
      if (prefer)
        it->second = &app;
    }
  }

  // Always record tray identities so tray apps are never duplicated as generic
  // Background Apps even when tray_location == "panel".
  std::unordered_set<uint32_t> tray_pids;
  std::unordered_set<std::string> tray_exes;
  std::unordered_set<std::string> tray_names_lower;

  std::vector<BackgroundAppEntry> tray_entries;
  if (include_tray)
    tray_entries.reserve(tray_items.size());

  for (const auto& item : tray_items) {
    std::string detail;
    std::string title = compact_tray_title_and_detail(
        item.title, item.app_hint, &detail);
    if (!title.empty())
      tray_names_lower.insert(to_lower_ascii(title));
    if (!item.app_hint.empty())
      tray_names_lower.insert(to_lower_ascii(item.app_hint));

    uint32_t owner_pid = 0;
    std::string owner_exe;
    auto owner_it = tray_owners.find(item.id);
    if (owner_it != tray_owners.end()) {
      owner_pid = owner_it->second.pid;
      owner_exe = to_lower_ascii(owner_it->second.exe_base_lower);
      if (owner_pid != 0)
        tray_pids.insert(owner_pid);
      if (!owner_exe.empty())
        tray_exes.insert(owner_exe);
    }

    if (!include_tray)
      continue;

    BackgroundAppEntry entry;
    entry.kind = BackgroundAppKind::TrayApp;
    entry.key = "tray:" + std::to_string(item.id);
    entry.title = std::move(title);
    entry.detail = std::move(detail);
    entry.tooltip = item.title;
    entry.exe_lower = owner_exe;
    entry.tray_id = item.id;
    entry.pid = owner_pid;
    entry.hwnd = item.target_hwnd;
    if (owner_pid != 0)
      entry.pids.push_back(owner_pid);
    entry.icon_width = item.icon_width;
    entry.icon_height = item.icon_height;
    entry.rgba_pixels = item.rgba_pixels;
    entry.fallback_icon_name = tray_fallback_icon_name(item.title, item.app_hint);

    if (!owner_exe.empty()) {
      auto app_it = exe_to_app.find(owner_exe);
      if (app_it != exe_to_app.end())
        entry.app_id = app_it->second->id;
    }

    tray_entries.push_back(std::move(entry));
  }

  std::sort(tray_entries.begin(), tray_entries.end(),
            [](const BackgroundAppEntry& a, const BackgroundAppEntry& b) {
              return ascii_icmp(a.title, b.title);
            });

  // Aggregate running processes without visible windows that match an installed
  // desktop application and are not already represented as a tray app.
  std::unordered_map<std::string, std::vector<uint32_t>> bg_pids_by_app_id;
  std::unordered_map<std::string, const AppEntry*> bg_apps_by_id;

  for (const auto& proc : running_procs) {
    if (proc.pid == 0 || proc.pid == self_pid)
      continue;
    if (tray_pids.count(proc.pid) > 0)
      continue;
    const std::string exe = to_lower_ascii(proc.exe_base_lower);
    if (is_ignored_background_exe(exe))
      continue;
    if (visible_exes.count(exe) > 0 || tray_exes.count(exe) > 0)
      continue;

    auto app_it = exe_to_app.find(exe);
    if (app_it == exe_to_app.end())
      continue;
    const AppEntry* app = app_it->second;
    if (!app->name_lower.empty() && tray_names_lower.count(app->name_lower) > 0)
      continue;
    if (!match_entry_running_windows(*app, visible_windows).empty())
      continue;

    auto& pids = bg_pids_by_app_id[app->id];
    if (std::find(pids.begin(), pids.end(), proc.pid) == pids.end())
      pids.push_back(proc.pid);
    bg_apps_by_id.emplace(app->id, app);
  }

  std::vector<BackgroundAppEntry> bg_entries;
  bg_entries.reserve(bg_apps_by_id.size());
  for (const auto& kv : bg_apps_by_id) {
    const AppEntry* app = kv.second;
    BackgroundAppEntry entry;
    entry.kind = BackgroundAppKind::BackgroundApp;
    entry.key = "bg:" + app->id;
    entry.title = app->name;
    entry.tooltip = app->name;
    entry.app_id = app->id;
    entry.exe_lower = app->target_lower;
    entry.pids = bg_pids_by_app_id[app->id];
    entry.pid = entry.pids.empty() ? 0 : entry.pids.front();
    entry.fallback_icon_name = "application-x-executable-symbolic";
    bg_entries.push_back(std::move(entry));
  }

  std::sort(bg_entries.begin(), bg_entries.end(),
            [](const BackgroundAppEntry& a, const BackgroundAppEntry& b) {
              return ascii_icmp(a.title, b.title);
            });

  std::vector<BackgroundAppEntry> merged;
  merged.reserve(tray_entries.size() + bg_entries.size());
  for (auto& e : tray_entries)
    merged.push_back(std::move(e));
  for (auto& e : bg_entries)
    merged.push_back(std::move(e));
  return merged;
}

void background_apps_init() {
  bool expected = false;
  if (!g_running.compare_exchange_strong(expected, true,
                                         std::memory_order_acq_rel)) {
    return;
  }
  g_generation.fetch_add(1, std::memory_order_acq_rel);
  g_last_refresh_us.store(0, std::memory_order_release);
  systray_add_listener(on_systray_source_changed, nullptr);
  add_config_listener(on_config_source_changed, nullptr);
  background_apps_refresh_async(true);
}

void background_apps_shutdown() {
  if (!g_running.exchange(false, std::memory_order_acq_rel))
    return;
  g_generation.fetch_add(1, std::memory_order_acq_rel);
  systray_remove_listener(on_systray_source_changed, nullptr);
  remove_config_listener(on_config_source_changed, nullptr);
  std::lock_guard<std::mutex> lock(g_state_mu);
  g_listeners.clear();
  g_snapshot.clear();
  g_snapshot_digest.clear();
  g_has_snapshot = false;
}

void background_apps_refresh_async(bool force) {
  if (!g_running.load(std::memory_order_acquire))
    return;

  const gint64 now_us = g_get_monotonic_time();
  const gint64 last_us = g_last_refresh_us.load(std::memory_order_acquire);
  if (!force && last_us > 0 && (now_us - last_us) < 1500LL * 1000LL)
    return;

  if (force)
    g_pending_force.store(true, std::memory_order_release);

  bool expected = false;
  if (!g_in_flight.compare_exchange_strong(expected, true,
                                           std::memory_order_acq_rel)) {
    return;
  }

  const uint64_t gen = g_generation.load(std::memory_order_acquire);
  const bool include_tray = (get_config().tray_location != "panel");

  std::thread([gen, include_tray]() {
    auto finish = [&]() {
      g_last_refresh_us.store(g_get_monotonic_time(), std::memory_order_release);
      g_in_flight.store(false, std::memory_order_release);
      if (g_pending_force.exchange(false, std::memory_order_acq_rel) &&
          g_running.load(std::memory_order_acquire) &&
          g_generation.load(std::memory_order_acquire) == gen) {
        g_idle_add(
            [](gpointer) -> gboolean {
              background_apps_refresh_async(true);
              return G_SOURCE_REMOVE;
            },
            nullptr);
      }
    };

    if (!g_running.load(std::memory_order_acquire) ||
        g_generation.load(std::memory_order_acquire) != gen) {
      finish();
      return;
    }

    const std::vector<TrayIconEntry> tray_items = systray_get_items();
    const std::unordered_map<uint64_t, TrayOwnerMeta> tray_owners =
        resolve_tray_owners(tray_items);
    const std::vector<AppEntry> installed_apps = app_index_get_all();
    const std::vector<RunningWindow> visible_windows =
        app_index_enumerate_running_windows();
    const std::vector<RawProcessInfo> running_procs =
        enumerate_user_session_processes();

    std::vector<BackgroundAppEntry> next = merge_background_and_tray_apps(
        tray_items, tray_owners, installed_apps, running_procs, visible_windows,
        include_tray, static_cast<uint32_t>(GetCurrentProcessId()));
    std::string next_digest = compute_snapshot_digest(next);

    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(g_state_mu);
      if (!g_has_snapshot || g_snapshot_digest != next_digest) {
        g_snapshot = std::move(next);
        g_snapshot_digest = std::move(next_digest);
        g_has_snapshot = true;
        changed = true;
      }
    }

    if (changed && g_running.load(std::memory_order_acquire) &&
        g_generation.load(std::memory_order_acquire) == gen) {
      g_idle_add(notify_listeners_idle,
                 reinterpret_cast<gpointer>(static_cast<uintptr_t>(gen)));
    }

    finish();
  }).detach();
}

std::vector<BackgroundAppEntry> background_apps_get_snapshot() {
  std::lock_guard<std::mutex> lock(g_state_mu);
  return g_snapshot;
}

void background_apps_add_listener(BackgroundAppsChangedCallback cb,
                                  void* user_data) {
  if (cb == nullptr)
    return;
  std::lock_guard<std::mutex> lock(g_state_mu);
  g_listeners.push_back(ListenerEntry{cb, user_data});
}

void background_apps_remove_listener(BackgroundAppsChangedCallback cb,
                                     void* user_data) {
  std::lock_guard<std::mutex> lock(g_state_mu);
  g_listeners.erase(
      std::remove_if(g_listeners.begin(), g_listeners.end(),
                     [cb, user_data](const ListenerEntry& e) {
                       return e.cb == cb && e.user_data == user_data;
                     }),
      g_listeners.end());
}

GdkTexture* background_apps_create_icon_texture(
    const BackgroundAppEntry& entry,
    int px) {
  if (entry.icon_width > 0 && entry.icon_height > 0) {
    const size_t expected = static_cast<size_t>(entry.icon_width) *
                            static_cast<size_t>(entry.icon_height) * 4;
    if (entry.rgba_pixels.size() == expected) {
      GBytes* bytes =
          g_bytes_new(entry.rgba_pixels.data(), entry.rgba_pixels.size());
      GdkTexture* tex = gdk_memory_texture_new(
          entry.icon_width, entry.icon_height, GDK_MEMORY_R8G8B8A8, bytes,
          static_cast<gsize>(entry.icon_width) * 4);
      g_bytes_unref(bytes);
      return tex;
    }
  }

  if (!entry.app_id.empty()) {
    AppEntry app;
    if (app_index_find_by_id(entry.app_id, &app))
      return app_icon_for_entry(app, px);
  }
  return nullptr;
}

void background_apps_activate(const BackgroundAppEntry& entry,
                              bool secondary_menu,
                              int screen_x,
                              int screen_y) {
  AllowSetForegroundWindow(ASFW_ANY);
  if (entry.kind == BackgroundAppKind::TrayApp) {
    systray_activate_item(entry.tray_id, secondary_menu, screen_x, screen_y);
    return;
  }

  if (!entry.app_id.empty()) {
    AppEntry app;
    if (app_index_find_by_id(entry.app_id, &app)) {
      app_index_launch(app);
      return;
    }
  }

  if (entry.hwnd != 0)
    app_index_activate_hwnd(reinterpret_cast<HWND>(entry.hwnd));
}

void background_apps_quit_async(const BackgroundAppEntry& entry,
                                void (*on_done)(void*),
                                void* user_data) {
  std::vector<uint32_t> pids = entry.pids;
  if (entry.pid != 0 &&
      std::find(pids.begin(), pids.end(), entry.pid) == pids.end()) {
    pids.push_back(entry.pid);
  }
  const uintptr_t hwnd_hint = entry.hwnd;

  std::thread([pids = std::move(pids), hwnd_hint, on_done, user_data]() {
    CloseEnumCtx ctx;
    for (uint32_t pid : pids) {
      if (pid != 0)
        ctx.pids.insert(static_cast<DWORD>(pid));
    }

    if (hwnd_hint != 0) {
      HWND hw = reinterpret_cast<HWND>(hwnd_hint);
      if (IsWindow(hw))
        PostMessageW(hw, WM_CLOSE, 0, 0);
    }

    if (!ctx.pids.empty()) {
      EnumWindows(collect_pid_top_windows_cb, reinterpret_cast<LPARAM>(&ctx));
      for (HWND hw : ctx.hwnds) {
        if (IsWindow(hw))
          PostMessageW(hw, WM_CLOSE, 0, 0);
      }

      for (int attempt = 0; attempt < 12; ++attempt) {
        bool any_alive = false;
        for (DWORD pid : ctx.pids) {
          if (is_process_still_alive(pid)) {
            any_alive = true;
            break;
          }
        }
        if (!any_alive)
          break;
        Sleep(100);
      }

      for (DWORD pid : ctx.pids) {
        if (!is_process_still_alive(pid))
          continue;
        HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (proc != nullptr) {
          TerminateProcess(proc, 0);
          CloseHandle(proc);
        }
      }
    }

    struct DonePayload {
      void (*on_done)(void*) = nullptr;
      void* user_data = nullptr;
    };
    auto* done = new DonePayload{on_done, user_data};
    g_idle_add(
        [](gpointer ud) -> gboolean {
          std::unique_ptr<DonePayload> p(static_cast<DonePayload*>(ud));
          if (p->on_done != nullptr)
            p->on_done(p->user_data);
          background_apps_refresh_async(true);
          return G_SOURCE_REMOVE;
        },
        done);
  }).detach();
}

}  // namespace winome
