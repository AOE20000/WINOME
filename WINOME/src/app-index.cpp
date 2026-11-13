// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors

#include "app-index.h"
#include "config.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cwctype>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace winome {

namespace {

constexpr size_t kMaxIconCacheEntries = 96;

constexpr GUID kPKEY_AppUserModel_ID_fmt = {
    0x9F4C2855, 0x9F79, 0x4B39,
    {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}};

constexpr GUID kBHID_SFUIObject = {
    0x3981e225, 0xf559, 0x11d3,
    {0x8e, 0x3a, 0x00, 0xc0, 0x4f, 0x68, 0x37, 0xd5}};

struct CacheItem {
  std::string key;
  GdkTexture *texture = nullptr;
};

std::mutex g_cache_mu;
std::list<CacheItem> g_cache_lru; // front = MRU, back = LRU
std::unordered_map<std::string, std::list<CacheItem>::iterator> g_cache_map;

std::mutex g_index_mu;
std::vector<AppEntry> g_entries;
bool g_index_populated = false;
std::atomic<bool> g_refresh_running{false};

std::string
make_sized_key (const std::string &key, int size)
{
  return normalize_app_id (key) + "#" + std::to_string (size);
}

std::wstring
basename_lower (const std::wstring &path)
{
  size_t sep = path.find_last_of (L"\\/");
  std::wstring base = (sep != std::wstring::npos) ? path.substr (sep + 1) : path;
  std::transform (base.begin (), base.end (), base.begin (), towlower);
  return base;
}

bool
resolve_shortcut_info (const std::wstring &lnk_path, AppEntry *entry)
{
  if (entry == nullptr)
    return false;

  IShellLinkW *link = nullptr;
  HRESULT hr = CoCreateInstance (CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS (&link));
  if (FAILED (hr) || link == nullptr)
    return false;

  IPersistFile *pf = nullptr;
  hr = link->QueryInterface (IID_PPV_ARGS (&pf));
  if (SUCCEEDED (hr) && pf != nullptr) {
    if (SUCCEEDED (pf->Load (lnk_path.c_str (), STGM_READ))) {
      wchar_t target[MAX_PATH] = L"";
      if (SUCCEEDED (link->GetPath (target, MAX_PATH, nullptr, SLGP_RAWPATH)) &&
          target[0] != L'\0') {
        wchar_t expanded[MAX_PATH] = L"";
        DWORD n = ExpandEnvironmentStringsW (target, expanded, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
          entry->target_exe = expanded;
        else
          entry->target_exe = target;
      }

      wchar_t icon_buf[MAX_PATH] = L"";
      int icon_idx = 0;
      if (SUCCEEDED (link->GetIconLocation (icon_buf, MAX_PATH, &icon_idx)) &&
          icon_buf[0] != L'\0') {
        wchar_t expanded[MAX_PATH] = L"";
        DWORD n = ExpandEnvironmentStringsW (icon_buf, expanded, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
          entry->icon_path = expanded;
        else
          entry->icon_path = icon_buf;
        entry->icon_index = icon_idx;
      }
    }
    pf->Release ();
  }

  IPropertyStore *store = nullptr;
  if (SUCCEEDED (link->QueryInterface (IID_PPV_ARGS (&store))) &&
      store != nullptr) {
    struct WinomePropKey {
      GUID fmtid;
      DWORD pid;
    };
    WinomePropKey pkey{kPKEY_AppUserModel_ID_fmt, 5};
    PROPVARIANT pv;
    PropVariantInit (&pv);
    if (SUCCEEDED (store->GetValue (
            *reinterpret_cast<PROPERTYKEY *> (&pkey), &pv)) &&
        pv.vt == VT_LPWSTR && pv.pwszVal != nullptr) {
      entry->aumid = pv.pwszVal;
      entry->is_uwp = entry->target_exe.empty ();
    }
    PropVariantClear (&pv);
    store->Release ();
  }

  link->Release ();
  return true;
}

void
collect_lnk_recursive (const std::wstring &dir, int depth,
                       std::vector<AppEntry> *out)
{
  if (depth > 5 || out == nullptr)
    return;

  std::wstring pattern = dir + L"\\*";
  WIN32_FIND_DATAW ffd{};
  HANDLE h = FindFirstFileW (pattern.c_str (), &ffd);
  if (h == INVALID_HANDLE_VALUE)
    return;

  do {
    if (wcscmp (ffd.cFileName, L".") == 0 || wcscmp (ffd.cFileName, L"..") == 0)
      continue;

    std::wstring full = dir + L"\\" + ffd.cFileName;
    if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      collect_lnk_recursive (full, depth + 1, out);
      continue;
    }

    size_t flen = wcslen (ffd.cFileName);
    if (flen <= 4 || _wcsicmp (ffd.cFileName + flen - 4, L".lnk") != 0)
      continue;

    std::wstring stem (ffd.cFileName, flen - 4);
    std::string name = app_wide_to_utf8 (stem);
    if (name.empty ())
      continue;
    std::string name_lower = app_utf8_tolower (name);
    if (is_ignored_shortcut_name (name_lower))
      continue;

    AppEntry entry;
    entry.name = std::move (name);
    entry.name_lower = std::move (name_lower);
    entry.launch_path = full;
    resolve_shortcut_info (full, &entry);
    entry.id = canonical_app_id (entry.aumid, entry.target_exe, entry.launch_path);
    if (!entry.target_exe.empty ()) {
      entry.target_lower =
          app_utf8_tolower (app_wide_to_utf8 (basename_lower (entry.target_exe)));
    }
    out->push_back (std::move (entry));
  } while (FindNextFileW (h, &ffd));

  FindClose (h);
}

std::vector<AppEntry>
scan_start_menu_sync ()
{
  HRESULT co_hr = CoInitializeEx (
      nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  bool did_co = SUCCEEDED (co_hr);

  std::vector<AppEntry> raw;
  const wchar_t *vars[] = {L"APPDATA", L"ProgramData"};
  for (const wchar_t *var : vars) {
    wchar_t base[MAX_PATH] = L"";
    if (GetEnvironmentVariableW (var, base, MAX_PATH) > 0) {
      std::wstring programs = std::wstring (base) +
                              L"\\Microsoft\\Windows\\Start Menu\\Programs";
      collect_lnk_recursive (programs, 0, &raw);
    }
  }

  if (did_co)
    CoUninitialize ();

  // Deduplicate by canonical ID while preserving the first (user Start Menu)
  // match, and sort alphabetically for App Grid display.
  std::unordered_map<std::string, size_t> seen;
  std::vector<AppEntry> dedup;
  dedup.reserve (raw.size ());
  for (AppEntry &e : raw) {
    if (e.id.empty ())
      continue;
    auto it = seen.find (e.id);
    if (it == seen.end ()) {
      seen[e.id] = dedup.size ();
      dedup.push_back (std::move (e));
    } else {
      // Also keep entries with a distinct launch name if same host exe (e.g.
      // different PWAs or scripts sharing cmd/pwsh/msedge).
      AppEntry &existing = dedup[it->second];
      if (existing.name_lower != e.name_lower) {
        e.id = normalize_app_id (app_wide_to_utf8 (e.launch_path));
        if (seen.find (e.id) == seen.end ()) {
          seen[e.id] = dedup.size ();
          dedup.push_back (std::move (e));
        }
      }
    }
  }

  std::sort (dedup.begin (), dedup.end (),
             [] (const AppEntry &a, const AppEntry &b) {
               return a.name_lower < b.name_lower;
             });
  return dedup;
}

struct ReadyDispatch {
  void (*cb) (void *) = nullptr;
  void *user_data = nullptr;
};

gboolean
dispatch_ready_idle (gpointer user_data)
{
  auto *d = static_cast<ReadyDispatch *> (user_data);
  if (d->cb != nullptr)
    d->cb (d->user_data);
  delete d;
  return G_SOURCE_REMOVE;
}

GdkTexture *
load_shell_image_for_path (const std::wstring &path, int size)
{
  if (path.empty ())
    return nullptr;

  HRESULT hr = CoInitializeEx (
      nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  bool did_co = SUCCEEDED (hr);

  GdkTexture *texture = nullptr;
  IShellItem *item = nullptr;
  if (SUCCEEDED (SHCreateItemFromParsingName (path.c_str (), nullptr,
                                              IID_PPV_ARGS (&item))) &&
      item != nullptr) {
    IShellItemImageFactory *factory = nullptr;
    if (SUCCEEDED (item->BindToHandler (nullptr, kBHID_SFUIObject,
                                        IID_PPV_ARGS (&factory))) &&
        factory != nullptr) {
      HBITMAP bmp = nullptr;
      if (SUCCEEDED (factory->GetImage (SIZE{size, size},
                                        SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK,
                                        &bmp)) &&
          bmp != nullptr) {
        texture = texture_from_hbitmap (bmp);
        DeleteObject (bmp);
      }
      factory->Release ();
    }
    item->Release ();
  }

  if (did_co)
    CoUninitialize ();

  if (texture != nullptr)
    return texture;

  SHFILEINFOW sfi{};
  if (SHGetFileInfoW (path.c_str (), 0, &sfi, sizeof (sfi),
                      SHGFI_ICON | SHGFI_LARGEICON) &&
      sfi.hIcon != nullptr) {
    texture = hicon_to_texture (sfi.hIcon);
    DestroyIcon (sfi.hIcon);
  }
  return texture;
}

}  // namespace

std::string
app_wide_to_utf8 (const std::wstring &w)
{
  if (w.empty ())
    return {};
  int n = WideCharToMultiByte (CP_UTF8, 0, w.c_str (), (int)w.size (),
                               nullptr, 0, nullptr, nullptr);
  if (n <= 0)
    return {};
  std::string out ((size_t)n, '\0');
  WideCharToMultiByte (CP_UTF8, 0, w.c_str (), (int)w.size (), out.data (), n,
                       nullptr, nullptr);
  return out;
}

std::wstring
app_utf8_to_wide (const std::string &s)
{
  if (s.empty ())
    return {};
  int n = MultiByteToWideChar (CP_UTF8, 0, s.c_str (), (int)s.size (), nullptr,
                               0);
  if (n <= 0)
    return {};
  std::wstring out ((size_t)n, L'\0');
  MultiByteToWideChar (CP_UTF8, 0, s.c_str (), (int)s.size (), out.data (), n);
  return out;
}

std::string
app_utf8_tolower (const std::string &s)
{
  if (s.empty ())
    return {};
  gchar *down = g_utf8_strdown (s.c_str (), static_cast<gssize> (s.size ()));
  if (down == nullptr)
    return {};
  std::string out (down);
  g_free (down);
  return out;
}

bool
is_ignored_shortcut_name (const std::string &name_lower)
{
  if (name_lower.empty ())
    return true;
  if (name_lower.find ("uninstall") != std::string::npos ||
      name_lower.find ("unins000") != std::string::npos ||
      name_lower.find ("卸载") != std::string::npos) {
    return true;
  }
  return false;
}

std::string
normalize_app_id (const std::string &id)
{
  std::string norm = app_utf8_tolower (id);
  for (char &ch : norm) {
    if (ch == '/')
      ch = '\\';
  }
  while (!norm.empty () &&
         (norm.front () == ' ' || norm.front () == '\t')) {
    norm.erase (norm.begin ());
  }
  while (!norm.empty () &&
         (norm.back () == ' ' || norm.back () == '\t')) {
    norm.pop_back ();
  }
  return norm;
}

std::string
canonical_app_id (const std::wstring &aumid, const std::wstring &target_exe,
                  const std::wstring &lnk_path)
{
  if (!aumid.empty ())
    return normalize_app_id (app_wide_to_utf8 (aumid));
  if (!target_exe.empty ())
    return normalize_app_id (app_wide_to_utf8 (target_exe));
  return normalize_app_id (app_wide_to_utf8 (lnk_path));
}

int
score_app_search_match (const AppEntry &entry, const std::string &query_lower)
{
  if (query_lower.empty ())
    return 1;
  size_t pos = entry.name_lower.find (query_lower);
  if (pos == 0)
    return 100;
  if (pos != std::string::npos) {
    bool word_start =
        entry.name_lower[pos - 1] == ' ' || entry.name_lower[pos - 1] == '-';
    return word_start ? 80 : 60;
  }
  if (!entry.target_lower.empty ()) {
    size_t tpos = entry.target_lower.find (query_lower);
    if (tpos == 0)
      return 50;
    if (tpos != std::string::npos)
      return 30;
  }
  return 0;
}

std::vector<AppEntry>
filter_and_sort_apps (const std::vector<AppEntry> &entries,
                      const std::string &query_lower)
{
  if (query_lower.empty ())
    return entries;

  struct Scored {
    int score;
    size_t index;
  };
  std::vector<Scored> matches;
  matches.reserve (entries.size ());
  for (size_t i = 0; i < entries.size (); ++i) {
    int s = score_app_search_match (entries[i], query_lower);
    if (s > 0)
      matches.push_back ({s, i});
  }

  std::sort (matches.begin (), matches.end (),
             [&entries] (const Scored &a, const Scored &b) {
               if (a.score != b.score)
                 return a.score > b.score;
               return entries[a.index].name_lower < entries[b.index].name_lower;
             });

  std::vector<AppEntry> out;
  out.reserve (matches.size ());
  for (const Scored &m : matches)
    out.push_back (entries[m.index]);
  return out;
}

void
app_index_refresh_async (void (*on_ready) (void *), void *user_data)
{
  bool expected = false;
  if (!g_refresh_running.compare_exchange_strong (expected, true)) {
    if (on_ready != nullptr) {
      std::lock_guard<std::mutex> lock (g_index_mu);
      if (g_index_populated) {
        auto *d = new ReadyDispatch{on_ready, user_data};
        g_idle_add (dispatch_ready_idle, d);
      }
    }
    return;
  }

  std::thread ([on_ready, user_data] () {
    std::vector<AppEntry> fresh = scan_start_menu_sync ();
    {
      std::lock_guard<std::mutex> lock (g_index_mu);
      g_entries = std::move (fresh);
      g_index_populated = true;
    }
    g_refresh_running.store (false);
    if (on_ready != nullptr) {
      auto *d = new ReadyDispatch{on_ready, user_data};
      g_idle_add (dispatch_ready_idle, d);
    }
  }).detach ();
}

std::vector<AppEntry>
app_index_get_all (void)
{
  {
    std::lock_guard<std::mutex> lock (g_index_mu);
    if (g_index_populated)
      return g_entries;
  }
  std::vector<AppEntry> fresh = scan_start_menu_sync ();
  {
    std::lock_guard<std::mutex> lock (g_index_mu);
    if (!g_index_populated) {
      g_entries = fresh;
      g_index_populated = true;
    }
    return g_entries;
  }
}

bool
app_index_find_by_id (const std::string &id_or_aumid, AppEntry *out)
{
  std::string norm = normalize_app_id (id_or_aumid);
  if (norm.empty ())
    return false;

  std::vector<AppEntry> all = app_index_get_all ();
  for (const AppEntry &e : all) {
    if (e.id == norm || normalize_app_id (app_wide_to_utf8 (e.aumid)) == norm ||
        normalize_app_id (app_wide_to_utf8 (e.target_exe)) == norm ||
        normalize_app_id (app_wide_to_utf8 (e.launch_path)) == norm) {
      if (out != nullptr)
        *out = e;
      return true;
    }
  }
  return false;
}

bool
is_app_id_pinned (const std::vector<std::string> &pinned_apps,
                  const std::string &id_or_aumid)
{
  std::string norm = normalize_app_id (id_or_aumid);
  if (norm.empty ())
    return false;
  for (const std::string &p : pinned_apps) {
    if (normalize_app_id (p) == norm)
      return true;
  }
  return false;
}

bool
set_app_id_pinned (const std::string &id_or_aumid, bool pinned)
{
  std::string norm = normalize_app_id (id_or_aumid);
  if (norm.empty ())
    return false;

  bool changed = false;
  WinomeConfig cfg = get_config ();
  auto it = std::find_if (
      cfg.pinned_apps.begin (), cfg.pinned_apps.end (),
      [&norm] (const std::string &existing) {
        return normalize_app_id (existing) == norm;
      });
  if (pinned) {
    if (it == cfg.pinned_apps.end ()) {
      cfg.pinned_apps.push_back (norm);
      changed = true;
    }
  } else {
    if (it != cfg.pinned_apps.end ()) {
      cfg.pinned_apps.erase (it);
      changed = true;
    }
  }
  if (changed)
    update_config (cfg);
  return changed;
}

bool
toggle_app_id_pinned (const std::string &id_or_aumid)
{
  WinomeConfig cfg = get_config ();
  bool now_pinned = !is_app_id_pinned (cfg.pinned_apps, id_or_aumid);
  set_app_id_pinned (id_or_aumid, now_pinned);
  return now_pinned;
}

bool
app_index_launch (const AppEntry &entry, bool as_admin)
{
  const std::wstring &target =
      !entry.launch_path.empty () ? entry.launch_path : entry.target_exe;
  if (target.empty ())
    return false;
  const wchar_t *verb = as_admin ? L"runas" : L"open";
  HINSTANCE res = ShellExecuteW (nullptr, verb, target.c_str (), nullptr,
                                 nullptr, SW_SHOWNORMAL);
  return reinterpret_cast<INT_PTR> (res) > 32;
}

bool
app_index_launch_id (const std::string &id_or_aumid, bool as_admin)
{
  AppEntry entry;
  if (app_index_find_by_id (id_or_aumid, &entry))
    return app_index_launch (entry, as_admin);

  std::wstring wide = app_utf8_to_wide (id_or_aumid);
  if (wide.empty ())
    return false;
  if (GetFileAttributesW (wide.c_str ()) != INVALID_FILE_ATTRIBUTES) {
    const wchar_t *verb = as_admin ? L"runas" : L"open";
    HINSTANCE res = ShellExecuteW (nullptr, verb, wide.c_str (), nullptr,
                                   nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR> (res) > 32;
  }
  if (id_or_aumid.find ('!') != std::string::npos) {
    std::wstring uri = L"shell:AppsFolder\\" + wide;
    HINSTANCE res = ShellExecuteW (nullptr, L"open", uri.c_str (), nullptr,
                                   nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR> (res) > 32;
  }
  return false;
}

bool
app_index_open_location (const AppEntry &entry)
{
  std::wstring target =
      !entry.target_exe.empty () ? entry.target_exe : entry.launch_path;
  if (target.empty ())
    return false;
  std::wstring arg = L"/select,\"" + target + L"\"";
  HINSTANCE res = ShellExecuteW (nullptr, L"open", L"explorer.exe", arg.c_str (),
                                 nullptr, SW_SHOWNORMAL);
  return reinterpret_cast<INT_PTR> (res) > 32;
}

bool
app_index_launch_best_match (const std::string &query_lower)
{
  std::string q = app_utf8_tolower (query_lower);
  if (q.empty ())
    return false;
  std::vector<AppEntry> matches =
      filter_and_sort_apps (app_index_get_all (), q);
  if (matches.empty ())
    return false;
  return app_index_launch (matches[0], false);
}

GdkTexture *
hicon_to_texture (HICON icon)
{
  if (icon == nullptr)
    return nullptr;

  ICONINFO info{};
  if (!GetIconInfo (icon, &info))
    return nullptr;

  int w = 0;
  int h = 0;
  if (info.hbmColor != nullptr) {
    BITMAP bm{};
    GetObjectW (info.hbmColor, sizeof (bm), &bm);
    w = bm.bmWidth;
    h = bm.bmHeight;
  } else if (info.hbmMask != nullptr) {
    BITMAP bm{};
    GetObjectW (info.hbmMask, sizeof (bm), &bm);
    w = bm.bmWidth;
    h = bm.bmHeight / 2;
  }
  if (w <= 0 || h <= 0) {
    if (info.hbmColor != nullptr)
      DeleteObject (info.hbmColor);
    if (info.hbmMask != nullptr)
      DeleteObject (info.hbmMask);
    return nullptr;
  }

  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  HDC screen = GetDC (nullptr);
  void *bits = nullptr;
  HBITMAP dib =
      CreateDIBSection (screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  GdkTexture *texture = nullptr;
  if (dib != nullptr && bits != nullptr) {
    HDC mem = CreateCompatibleDC (screen);
    HGDIOBJ old = SelectObject (mem, dib);
    BOOL ok = DrawIconEx (mem, 0, 0, icon, w, h, 0, nullptr, DI_NORMAL);
    SelectObject (mem, old);
    DeleteDC (mem);
    if (ok) {
      GBytes *bytes = g_bytes_new (bits, (gsize)w * h * 4);
      texture = gdk_memory_texture_new (w, h, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED,
                                        bytes, (gsize)w * 4);
      g_bytes_unref (bytes);
    }
  }
  if (dib != nullptr)
    DeleteObject (dib);
  ReleaseDC (nullptr, screen);

  if (info.hbmColor != nullptr)
    DeleteObject (info.hbmColor);
  if (info.hbmMask != nullptr)
    DeleteObject (info.hbmMask);
  return texture;
}

GdkTexture *
texture_from_hbitmap (HBITMAP bmp)
{
  if (bmp == nullptr)
    return nullptr;

  BITMAP bm{};
  GetObjectW (bmp, sizeof (bm), &bm);
  if (bm.bmWidth <= 0 || bm.bmHeight <= 0)
    return nullptr;

  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = bm.bmWidth;
  bi.bmiHeader.biHeight = -bm.bmHeight;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  HDC screen = GetDC (nullptr);
  void *bits = nullptr;
  HBITMAP dib =
      CreateDIBSection (screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  GdkTexture *texture = nullptr;
  if (dib != nullptr && bits != nullptr) {
    HDC src = CreateCompatibleDC (screen);
    HDC dst = CreateCompatibleDC (screen);
    HGDIOBJ old_src = SelectObject (src, bmp);
    HGDIOBJ old_dst = SelectObject (dst, dib);
    if (BitBlt (dst, 0, 0, bm.bmWidth, bm.bmHeight, src, 0, 0, SRCCOPY)) {
      GBytes *bytes =
          g_bytes_new (bits, (gsize)bm.bmWidth * bm.bmHeight * 4);
      texture = gdk_memory_texture_new (
          bm.bmWidth, bm.bmHeight, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED, bytes,
          (gsize)bm.bmWidth * 4);
      g_bytes_unref (bytes);
    }
    SelectObject (src, old_src);
    SelectObject (dst, old_dst);
    DeleteDC (src);
    DeleteDC (dst);
  }
  if (dib != nullptr)
    DeleteObject (dib);
  ReleaseDC (nullptr, screen);
  return texture;
}

GdkTexture *
app_icon_cache_lookup (const std::string &cache_key, int size)
{
  if (cache_key.empty () || size <= 0)
    return nullptr;
  std::string skey = make_sized_key (cache_key, size);
  std::lock_guard<std::mutex> lock (g_cache_mu);
  auto it = g_cache_map.find (skey);
  if (it == g_cache_map.end ())
    return nullptr;
  g_cache_lru.splice (g_cache_lru.begin (), g_cache_lru, it->second);
  if (it->second->texture == nullptr)
    return nullptr;
  return GDK_TEXTURE (g_object_ref (it->second->texture));
}

void
app_icon_cache_store (const std::string &cache_key, int size,
                      GdkTexture *texture)
{
  if (cache_key.empty () || size <= 0 || texture == nullptr)
    return;
  std::string skey = make_sized_key (cache_key, size);
  std::lock_guard<std::mutex> lock (g_cache_mu);
  auto it = g_cache_map.find (skey);
  if (it != g_cache_map.end ()) {
    g_clear_object (&it->second->texture);
    it->second->texture = GDK_TEXTURE (g_object_ref (texture));
    g_cache_lru.splice (g_cache_lru.begin (), g_cache_lru, it->second);
    return;
  }

  while (g_cache_lru.size () >= kMaxIconCacheEntries) {
    auto last = std::prev (g_cache_lru.end ());
    g_cache_map.erase (last->key);
    g_clear_object (&last->texture);
    g_cache_lru.erase (last);
  }

  g_cache_lru.push_front (
      CacheItem{skey, GDK_TEXTURE (g_object_ref (texture))});
  g_cache_map[skey] = g_cache_lru.begin ();
}

void
app_icon_cache_clear (void)
{
  std::lock_guard<std::mutex> lock (g_cache_mu);
  for (CacheItem &item : g_cache_lru)
    g_clear_object (&item.texture);
  g_cache_lru.clear ();
  g_cache_map.clear ();
}

size_t
app_icon_cache_count (void)
{
  std::lock_guard<std::mutex> lock (g_cache_mu);
  return g_cache_lru.size ();
}

size_t
app_icon_cache_capacity (void)
{
  return kMaxIconCacheEntries;
}

GdkTexture *
app_icon_for_path (const std::wstring &path, int size,
                   const std::string &cache_key)
{
  if (size <= 0)
    return nullptr;
  std::string effective_key =
      !cache_key.empty () ? cache_key : app_wide_to_utf8 (path);
  if (GdkTexture *cached = app_icon_cache_lookup (effective_key, size))
    return cached;

  GdkTexture *tex = load_shell_image_for_path (path, size);
  if (tex != nullptr)
    app_icon_cache_store (effective_key, size, tex);
  return tex;
}

GdkTexture *
app_icon_for_entry (const AppEntry &entry, int size)
{
  if (size <= 0)
    return nullptr;
  if (GdkTexture *cached = app_icon_cache_lookup (entry.id, size))
    return cached;

  GdkTexture *tex = nullptr;
  if (!entry.launch_path.empty ())
    tex = load_shell_image_for_path (entry.launch_path, size);
  if (tex == nullptr && !entry.target_exe.empty ())
    tex = load_shell_image_for_path (entry.target_exe, size);
  if (tex == nullptr && !entry.icon_path.empty ()) {
    HICON hi = nullptr;
    if (ExtractIconExW (entry.icon_path.c_str (), entry.icon_index, &hi,
                        nullptr, 1) > 0 &&
        hi != nullptr) {
      tex = hicon_to_texture (hi);
      DestroyIcon (hi);
    }
  }
  if (tex != nullptr)
    app_icon_cache_store (entry.id, size, tex);
  return tex;
}

GdkTexture *
app_icon_for_window (HWND hwnd, const std::wstring &exe, int size,
                     const std::string &cache_key)
{
  std::string key = !cache_key.empty () ? cache_key : app_wide_to_utf8 (exe);
  if (!key.empty ()) {
    if (GdkTexture *cached = app_icon_cache_lookup (key, size))
      return cached;
  }

  GdkTexture *texture = nullptr;
  if (hwnd != nullptr && IsWindow (hwnd)) {
    HICON icon = (HICON)SendMessageTimeoutW (hwnd, WM_GETICON, ICON_BIG, 0,
                                             SMTO_ABORTIFHUNG, 100, nullptr);
    bool owned = false;
    if (icon == nullptr)
      icon = (HICON)GetClassLongPtrW (hwnd, GCLP_HICON);
    else
      owned = false; // WM_GETICON returns window-owned icon handle

    texture = hicon_to_texture (icon);
    (void)owned;
    if (texture != nullptr && gdk_texture_get_width (texture) >= size) {
      if (!key.empty ())
        app_icon_cache_store (key, size, texture);
      return texture;
    }
    g_clear_object (&texture);
  }

  if (!exe.empty ())
    texture = app_icon_for_path (exe, size, key);
  return texture;
}

}  // namespace winome
