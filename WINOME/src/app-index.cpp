// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors

#include "app-index.h"
#include "config.h"
#include "host-utils.h"

#include <bcrypt.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <list>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>

namespace winome {

namespace {

constexpr size_t kMaxIconCacheEntries = 1024;
constexpr int kShilJumbo = 4;
constexpr UINT kIldTransparent = 0x00000001u;

constexpr GUID kPKEY_AppUserModel_ID_fmt = {
    0x9F4C2855, 0x9F79, 0x4B39,
    {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}};

constexpr GUID kIID_IImageList = {
    0x46eb5926, 0x582e, 0x4017,
    {0x9f, 0xdf, 0xe8, 0x99, 0x8d, 0xaa, 0x09, 0x50}};

struct CacheItem {
  std::string key;
  GdkTexture *texture = nullptr;
};

std::mutex g_cache_mu;
std::list<CacheItem> g_cache_lru; // front = MRU, back = LRU
std::unordered_map<std::string, std::list<CacheItem>::iterator> g_cache_map;

std::mutex g_index_mu;
std::vector<AppEntry> g_entries;
std::vector<StartMenuPinnedItem> g_pinned_layout;
bool g_index_populated = false;
std::atomic<bool> g_refresh_running{false};
std::atomic<uint64_t> g_index_revision{0};

std::mutex g_watch_mu;
std::thread g_watch_thread;
HANDLE g_watch_stop_event = nullptr;
bool g_watch_running = false;

std::wstring
wide_from_utf8_local (const std::string &s)
{
  if (s.empty ())
    return {};
  int need = MultiByteToWideChar (CP_UTF8, 0, s.c_str (), -1, nullptr, 0);
  if (need <= 1)
    return {};
  std::wstring out (static_cast<size_t> (need - 1), L'\0');
  MultiByteToWideChar (CP_UTF8, 0, s.c_str (), -1, out.data (), need);
  return out;
}

std::wstring
expand_env_wide_local (const std::wstring &raw)
{
  if (raw.empty ())
    return {};
  DWORD need = ExpandEnvironmentStringsW (raw.c_str (), nullptr, 0);
  if (need == 0 || need > 4096)
    return raw;
  std::wstring out (need, L'\0');
  DWORD wrote = ExpandEnvironmentStringsW (raw.c_str (), out.data (), need);
  if (wrote == 0 || wrote > need)
    return raw;
  if (!out.empty () && out.back () == L'\0')
    out.pop_back ();
  return out;
}

std::string
normalize_win_path_lower (const std::wstring &raw)
{
  if (raw.empty ())
    return {};
  std::wstring exp = expand_env_wide_local (raw);
  std::string utf8 = app_wide_to_utf8 (exp);
  std::string lower = app_utf8_tolower (utf8);
  std::string out;
  out.reserve (lower.size ());
  char prev = '\0';
  for (char ch : lower) {
    if (ch == '/')
      ch = '\\';
    if (ch == '\\' && prev == '\\')
      continue;
    out.push_back (ch);
    prev = ch;
  }
  while (out.size () > 3 && out.back () == '\\')
    out.pop_back ();
  return out;
}

bool
natural_name_less (const std::wstring &a, const std::wstring &b)
{
  int cmp = CompareStringEx (
      LOCALE_NAME_USER_DEFAULT,
      SORT_DIGITSASNUMBERS | LINGUISTIC_IGNORECASE,
      a.c_str (), static_cast<int> (a.size ()),
      b.c_str (), static_cast<int> (b.size ()),
      nullptr, nullptr, 0);
  if (cmp != 0)
    return cmp == CSTR_LESS_THAN;
  return _wcsicmp (a.c_str (), b.c_str ()) < 0;
}

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
resolve_url_shortcut_info (const std::wstring &url_path, AppEntry *entry)
{
  if (entry == nullptr || url_path.empty ())
    return;
  wchar_t url_buf[1024] = L"";
  if (GetPrivateProfileStringW (L"InternetShortcut", L"URL", L"", url_buf,
                              1024, url_path.c_str ()) > 0 &&
      url_buf[0] != L'\0') {
    entry->launch_url = app_utf8_tolower (app_wide_to_utf8 (url_buf));
  }
  wchar_t icon_buf[MAX_PATH] = L"";
  if (GetPrivateProfileStringW (L"InternetShortcut", L"IconFile", L"", icon_buf,
                              MAX_PATH, url_path.c_str ()) > 0 &&
      icon_buf[0] != L'\0') {
    entry->icon_path = expand_env_wide_local (icon_buf);
    entry->icon_index = static_cast<int> (
        GetPrivateProfileIntW (L"InternetShortcut", L"IconIndex", 0,
                               url_path.c_str ()));
  }
}

bool
is_start_menu_shortcut_ext (const wchar_t *name, size_t len)
{
  return len > 4 && (_wcsicmp (name + len - 4, L".lnk") == 0 ||
                     _wcsicmp (name + len - 4, L".url") == 0);
}

void
collect_start_menu_dir (const std::wstring &dir, int depth, bool is_user,
                        const std::string &top_folder_name,
                        int top_folder_order, int *next_folder_order,
                        int *next_item_order, std::vector<AppEntry> *out)
{
  if (depth > 5 || out == nullptr)
    return;

  std::wstring pattern = dir + L"\\*";
  WIN32_FIND_DATAW ffd{};
  HANDLE h = FindFirstFileW (pattern.c_str (), &ffd);
  if (h == INVALID_HANDLE_VALUE)
    return;

  std::vector<std::wstring> subdirs;
  std::vector<std::wstring> files;
  do {
    if (wcscmp (ffd.cFileName, L".") == 0 || wcscmp (ffd.cFileName, L"..") == 0)
      continue;
    if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      subdirs.emplace_back (ffd.cFileName);
    } else {
      size_t flen = wcslen (ffd.cFileName);
      if (is_start_menu_shortcut_ext (ffd.cFileName, flen))
        files.emplace_back (ffd.cFileName);
    }
  } while (FindNextFileW (h, &ffd));
  FindClose (h);

  std::sort (subdirs.begin (), subdirs.end (), natural_name_less);
  std::sort (files.begin (), files.end (), natural_name_less);

  // Process files in this directory first (preserving root entries before subfolders
  // or per-folder child order).
  for (const std::wstring &fname : files) {
    size_t flen = fname.size ();
    std::wstring stem (fname.c_str (), flen - 4);
    std::string name = app_wide_to_utf8 (stem);
    if (name.empty ())
      continue;
    std::string name_lower = app_utf8_tolower (name);
    if (is_ignored_shortcut_name (name_lower))
      continue;

    std::wstring full = dir + L"\\" + fname;
    AppEntry entry;
    entry.name = std::move (name);
    entry.name_lower = std::move (name_lower);
    entry.launch_path = full;
    if (flen > 4 && _wcsicmp (fname.c_str () + flen - 4, L".url") == 0)
      resolve_url_shortcut_info (full, &entry);
    else
      resolve_shortcut_info (full, &entry);
    entry.id = canonical_app_id (entry.aumid, entry.target_exe, entry.launch_path);
    if (!entry.target_exe.empty ()) {
      entry.target_lower =
          app_utf8_tolower (app_wide_to_utf8 (basename_lower (entry.target_exe)));
    }
    entry.folder_name = top_folder_name;
    entry.folder_order = top_folder_order;
    entry.item_order = next_item_order != nullptr ? (*next_item_order)++ : 0;
    entry.from_user_programs = is_user;
    out->push_back (std::move (entry));
  }

  for (const std::wstring &sub : subdirs) {
    std::wstring full = dir + L"\\" + sub;
    std::string folder_name = top_folder_name;
    int folder_order = top_folder_order;
    if (depth == 0) {
      folder_name = app_wide_to_utf8 (sub);
      // Skip autostart-only folder if empty or standard Windows Startup utilities?
      // Still collect non-ignored shortcuts if user placed any in Startup.
      folder_order = next_folder_order != nullptr ? (*next_folder_order)++ : 0;
    }
    collect_start_menu_dir (full, depth + 1, is_user, folder_name, folder_order,
                            next_folder_order, next_item_order, out);
  }
}

std::vector<AppEntry>
scan_start_menu_sync ()
{
  HRESULT co_hr = CoInitializeEx (
      nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  bool did_co = SUCCEEDED (co_hr);

  std::vector<AppEntry> raw;
  int user_folder_order = 0;
  int common_folder_order = 0;
  int next_item_order = 0;
  const wchar_t *vars[] = {L"APPDATA", L"ProgramData"};
  for (const wchar_t *var : vars) {
    wchar_t base[MAX_PATH] = L"";
    if (GetEnvironmentVariableW (var, base, MAX_PATH) > 0) {
      bool is_user = (wcscmp (var, L"APPDATA") == 0);
      std::wstring programs = std::wstring (base) +
                              L"\\Microsoft\\Windows\\Start Menu\\Programs";
      collect_start_menu_dir (
          programs, 0, is_user, "", -1,
          is_user ? &user_folder_order : &common_folder_order,
          &next_item_order, &raw);
    }
  }

  // Append Store / UWP / Windows inbox apps exposed via FOLDERID_AppsFolder
  // (e.g. Settings, Calculator, Photos, Snipping Tool) when not already covered
  // by a Start Menu .lnk entry.
  constexpr GUID kFOLDERID_AppsFolder = {
      0x1e87508d, 0x89c2, 0x42f0,
      {0x8a, 0x7e, 0x64, 0x5a, 0x0f, 0x50, 0xca, 0x58}};
  constexpr GUID kBHID_EnumItems = {
      0x94f60519, 0x2850, 0x4924,
      {0xaa, 0x5a, 0xd1, 0x5e, 0x84, 0x86, 0x80, 0x39}};

  IShellItem *apps_folder = nullptr;
  if (SUCCEEDED (SHGetKnownFolderItem (kFOLDERID_AppsFolder, KF_FLAG_DEFAULT,
                                       nullptr, IID_PPV_ARGS (&apps_folder))) &&
      apps_folder != nullptr) {
    IEnumShellItems *enum_items = nullptr;
    if (SUCCEEDED (apps_folder->BindToHandler (nullptr, kBHID_EnumItems,
                                               IID_PPV_ARGS (&enum_items))) &&
        enum_items != nullptr) {
      std::unordered_map<std::string, std::vector<size_t>> lnk_names;
      std::unordered_map<std::string, bool> lnk_ids;
      for (size_t i = 0; i < raw.size (); ++i) {
        const AppEntry &e = raw[i];
        if (!e.id.empty ())
          lnk_ids[e.id] = true;
        if (!e.name_lower.empty ())
          lnk_names[e.name_lower].push_back (i);
      }

      IShellItem *child = nullptr;
      while (enum_items->Next (1, &child, nullptr) == S_OK &&
             child != nullptr) {
        PWSTR disp = nullptr;
        PWSTR parsing = nullptr;
        if (SUCCEEDED (child->GetDisplayName (SIGDN_NORMALDISPLAY, &disp)) &&
            disp != nullptr &&
            SUCCEEDED (
                child->GetDisplayName (SIGDN_PARENTRELATIVEPARSING, &parsing)) &&
            parsing != nullptr) {
          std::wstring app_id (parsing);
          bool has_path_sep =
              app_id.find (L'\\') != std::wstring::npos ||
              app_id.find (L'/') != std::wstring::npos;
          bool is_raw_guid =
              app_id.size () >= 2 && app_id.front () == L'{' &&
              app_id.back () == L'}';
          if (!app_id.empty () && !has_path_sep && !is_raw_guid) {
            std::string name = app_wide_to_utf8 (disp);
            std::string name_lower = app_utf8_tolower (name);
            std::string id = canonical_app_id (app_id, L"", L"");
            if (!name.empty () && !is_ignored_shortcut_name (name_lower) &&
                !id.empty ()) {
              auto nit = lnk_names.find (name_lower);
              if (nit != lnk_names.end ()) {
                // Attach Windows AppsFolder AUMID onto matching .lnk / .url
                // shortcuts so start2.bin W~<AUMID> pins match cleanly.
                for (size_t idx : nit->second) {
                  if (raw[idx].aumid.empty ())
                    raw[idx].aumid = app_id;
                }
              } else if (lnk_ids.find (id) == lnk_ids.end ()) {
                AppEntry uwp;
                uwp.id = std::move (id);
                uwp.name = std::move (name);
                uwp.name_lower = std::move (name_lower);
                uwp.aumid = app_id;
                uwp.launch_path = L"shell:AppsFolder\\" + app_id;
                uwp.is_uwp = true;
                lnk_ids[uwp.id] = true;
                lnk_names[uwp.name_lower].push_back (raw.size ());
                raw.push_back (std::move (uwp));
              }
            }
          }
        }
        if (disp != nullptr)
          CoTaskMemFree (disp);
        if (parsing != nullptr)
          CoTaskMemFree (parsing);
        child->Release ();
        child = nullptr;
      }
      enum_items->Release ();
    }
    apps_folder->Release ();
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

bool
extract_hbitmap_raw (HBITMAP bmp, RawDecodedIcon *out)
{
  if (bmp == nullptr || out == nullptr)
    return false;

  BITMAP bm{};
  if (GetObjectW (bmp, sizeof (bm), &bm) == 0 || bm.bmWidth <= 0 ||
      bm.bmHeight <= 0)
    return false;

  int w = bm.bmWidth;
  int h = bm.bmHeight;
  size_t byte_len = (size_t)w * (size_t)h * 4;
  auto *buf = static_cast<uint8_t *> (g_malloc (byte_len));

  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  HDC screen = GetDC (nullptr);
  int lines = GetDIBits (screen, bmp, 0, (UINT)h, buf, &bi, DIB_RGB_COLORS);
  ReleaseDC (nullptr, screen);
  if (lines <= 0) {
    g_free (buf);
    return false;
  }

  // IShellItemImageFactory::GetImage frequently outputs straight (unpremultiplied)
  // BGRA where color channels exceed alpha; opaque 32-bpp HBITMAPs can also
  // leave all alpha bytes at 0 despite non-zero RGB.
  bool any_nonzero_alpha = false;
  bool any_nonzero_rgb = false;
  bool has_rgb_gt_a = false;
  for (size_t i = 0; i < byte_len; i += 4) {
    uint8_t b = buf[i + 0];
    uint8_t g = buf[i + 1];
    uint8_t r = buf[i + 2];
    uint8_t a = buf[i + 3];
    if (a != 0)
      any_nonzero_alpha = true;
    if (r != 0 || g != 0 || b != 0)
      any_nonzero_rgb = true;
    if (r > a || g > a || b > a)
      has_rgb_gt_a = true;
  }
  if (!any_nonzero_alpha && any_nonzero_rgb) {
    for (size_t i = 0; i < byte_len; i += 4)
      buf[i + 3] = 0xff;
    has_rgb_gt_a = true;
  }

  out->width = w;
  out->height = h;
  out->unpremultiplied = has_rgb_gt_a;
  if (out->bytes != nullptr)
    g_bytes_unref (out->bytes);
  out->bytes = g_bytes_new_take (buf, (gsize)byte_len);
  return true;
}

bool
extract_hicon_raw (HICON icon, RawDecodedIcon *out)
{
  if (icon == nullptr || out == nullptr)
    return false;

  ICONINFO info{};
  if (!GetIconInfo (icon, &info))
    return false;

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
    return false;
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
  bool ok = false;
  if (dib != nullptr && bits != nullptr) {
    HDC mem = CreateCompatibleDC (screen);
    HGDIOBJ old = SelectObject (mem, dib);
    BOOL drawn = DrawIconEx (mem, 0, 0, icon, w, h, 0, nullptr, DI_NORMAL);
    SelectObject (mem, old);
    DeleteDC (mem);
    if (drawn) {
      size_t byte_len = (size_t)w * (size_t)h * 4;
      const auto *src = static_cast<const uint8_t *> (bits);
      bool any_nonzero_alpha = false;
      bool has_rgb_gt_a = false;
      for (size_t i = 0; i < byte_len; i += 4) {
        uint8_t a = src[i + 3];
        if (a != 0)
          any_nonzero_alpha = true;
        if (src[i + 0] > a || src[i + 1] > a || src[i + 2] > a)
          has_rgb_gt_a = true;
      }
      out->width = w;
      out->height = h;
      out->unpremultiplied = any_nonzero_alpha && has_rgb_gt_a;
      if (out->bytes != nullptr)
        g_bytes_unref (out->bytes);
      out->bytes = g_bytes_new (bits, (gsize)byte_len);
      ok = true;
    }
  }
  if (dib != nullptr)
    DeleteObject (dib);
  ReleaseDC (nullptr, screen);

  if (info.hbmColor != nullptr)
    DeleteObject (info.hbmColor);
  if (info.hbmMask != nullptr)
    DeleteObject (info.hbmMask);
  return ok;
}

bool
load_shell_image_raw_for_path (const std::wstring &path, int size,
                               RawDecodedIcon *out)
{
  if (path.empty () || size <= 0 || out == nullptr)
    return false;

  HRESULT hr = CoInitializeEx (
      nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  bool did_co = SUCCEEDED (hr);

  bool ok = false;
  IShellItem *item = nullptr;
  if (SUCCEEDED (SHCreateItemFromParsingName (path.c_str (), nullptr,
                                              IID_PPV_ARGS (&item))) &&
      item != nullptr) {
    IShellItemImageFactory *factory = nullptr;
    if (SUCCEEDED (item->QueryInterface (IID_PPV_ARGS (&factory))) &&
        factory != nullptr) {
      HBITMAP bmp = nullptr;
      if (SUCCEEDED (factory->GetImage (SIZE{size, size},
                                        SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK,
                                        &bmp)) &&
          bmp != nullptr) {
        ok = extract_hbitmap_raw (bmp, out);
        DeleteObject (bmp);
      } else if (SUCCEEDED (factory->GetImage (SIZE{size, size},
                                               SIIGBF_BIGGERSIZEOK, &bmp)) &&
                 bmp != nullptr) {
        ok = extract_hbitmap_raw (bmp, out);
        DeleteObject (bmp);
      }
      factory->Release ();
    }
    item->Release ();
  }

  if (!ok) {
    SHFILEINFOW sfi{};
    if (SHGetFileInfoW (path.c_str (), 0, &sfi, sizeof (sfi),
                        SHGFI_SYSICONINDEX)) {
      IImageList *iml = nullptr;
      if (SUCCEEDED (SHGetImageList (kShilJumbo, kIID_IImageList,
                                     reinterpret_cast<void **> (&iml))) &&
          iml != nullptr) {
        HICON hi = nullptr;
        if (SUCCEEDED (iml->GetIcon (sfi.iIcon, kIldTransparent, &hi)) &&
            hi != nullptr) {
          ok = extract_hicon_raw (hi, out);
          DestroyIcon (hi);
        }
        iml->Release ();
      }
    }
  }

  if (did_co)
    CoUninitialize ();

  if (ok)
    return true;

  SHFILEINFOW sfi{};
  if (SHGetFileInfoW (path.c_str (), 0, &sfi, sizeof (sfi),
                      SHGFI_ICON | SHGFI_LARGEICON) &&
      sfi.hIcon != nullptr) {
    ok = extract_hicon_raw (sfi.hIcon, out);
    DestroyIcon (sfi.hIcon);
  }
  return ok;
}

GdkTexture *
load_shell_image_for_path (const std::wstring &path, int size)
{
  RawDecodedIcon raw;
  if (!load_shell_image_raw_for_path (path, size, &raw))
    return nullptr;
  GdkTexture *tex = app_icon_texture_from_raw (raw);
  app_icon_raw_clear (raw);
  return tex;
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

namespace {

bool
get_file_write_stamp (const std::wstring &path, std::string *out_stamp)
{
  if (path.empty () || out_stamp == nullptr)
    return false;
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (!GetFileAttributesExW (path.c_str (), GetFileExInfoStandard, &fad))
    return false;
  *out_stamp = std::to_string (fad.ftLastWriteTime.dwHighDateTime) + ":" +
               std::to_string (fad.ftLastWriteTime.dwLowDateTime) + ":" +
               std::to_string (fad.nFileSizeHigh) + ":" +
               std::to_string (fad.nFileSizeLow);
  return true;
}

bool
write_file_bytes_local (const std::wstring &path, const std::string &data)
{
  if (path.empty ())
    return false;
  size_t slash = path.find_last_of (L"\\/");
  if (slash != std::wstring::npos) {
    std::wstring dir = path.substr (0, slash);
    CreateDirectoryW (dir.c_str (), nullptr);
  }
  HANDLE hf = CreateFileW (path.c_str (), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
  if (hf == INVALID_HANDLE_VALUE)
    return false;
  DWORD wrote = 0;
  BOOL ok = WriteFile (hf, data.data (), static_cast<DWORD> (data.size ()),
                       &wrote, nullptr);
  CloseHandle (hf);
  return ok != FALSE && wrote == data.size ();
}

void
skip_json_ws (const std::string &s, size_t *pos)
{
  while (*pos < s.size ()) {
    unsigned char c = static_cast<unsigned char> (s[*pos]);
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
      ++(*pos);
    else
      break;
  }
}

void
append_codepoint_utf8 (uint32_t cp, std::string *out)
{
  if (cp <= 0x7F) {
    out->push_back (static_cast<char> (cp));
  } else if (cp <= 0x7FF) {
    out->push_back (static_cast<char> (0xC0 | (cp >> 6)));
    out->push_back (static_cast<char> (0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out->push_back (static_cast<char> (0xE0 | (cp >> 12)));
    out->push_back (static_cast<char> (0x80 | ((cp >> 6) & 0x3F)));
    out->push_back (static_cast<char> (0x80 | (cp & 0x3F)));
  } else {
    out->push_back (static_cast<char> (0xF0 | (cp >> 18)));
    out->push_back (static_cast<char> (0x80 | ((cp >> 12) & 0x3F)));
    out->push_back (static_cast<char> (0x80 | ((cp >> 6) & 0x3F)));
    out->push_back (static_cast<char> (0x80 | (cp & 0x3F)));
  }
}

bool
parse_hex4 (const std::string &s, size_t pos, uint32_t *out)
{
  if (pos + 4 > s.size ())
    return false;
  uint32_t v = 0;
  for (size_t i = 0; i < 4; ++i) {
    char c = s[pos + i];
    uint32_t d = 0;
    if (c >= '0' && c <= '9')
      d = static_cast<uint32_t> (c - '0');
    else if (c >= 'a' && c <= 'f')
      d = 10u + static_cast<uint32_t> (c - 'a');
    else if (c >= 'A' && c <= 'F')
      d = 10u + static_cast<uint32_t> (c - 'A');
    else
      return false;
    v = (v << 4) | d;
  }
  *out = v;
  return true;
}

bool
parse_json_string_local (const std::string &s, size_t *pos, std::string *out)
{
  skip_json_ws (s, pos);
  if (*pos >= s.size () || s[*pos] != '"')
    return false;
  ++(*pos);
  std::string buf;
  while (*pos < s.size ()) {
    char c = s[*pos];
    ++(*pos);
    if (c == '"') {
      *out = std::move (buf);
      return true;
    }
    if (c == '\\') {
      if (*pos >= s.size ())
        return false;
      char esc = s[*pos];
      ++(*pos);
      switch (esc) {
        case '"': buf.push_back ('"'); break;
        case '\\': buf.push_back ('\\'); break;
        case '/': buf.push_back ('/'); break;
        case 'b': buf.push_back ('\b'); break;
        case 'f': buf.push_back ('\f'); break;
        case 'n': buf.push_back ('\n'); break;
        case 'r': buf.push_back ('\r'); break;
        case 't': buf.push_back ('\t'); break;
        case 'u': {
          uint32_t cp = 0;
          if (!parse_hex4 (s, *pos, &cp))
            return false;
          *pos += 4;
          if (cp >= 0xD800 && cp <= 0xDBFF && *pos + 6 <= s.size () &&
              s[*pos] == '\\' && s[*pos + 1] == 'u') {
            uint32_t lo = 0;
            if (parse_hex4 (s, *pos + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
              *pos += 6;
              cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
            }
          }
          append_codepoint_utf8 (cp, &buf);
          break;
        }
        default:
          buf.push_back (esc);
          break;
      }
    } else {
      buf.push_back (c);
    }
  }
  return false;
}

bool skip_json_value_local (const std::string &s, size_t *pos, int depth);

bool
skip_json_array_or_obj_local (const std::string &s, size_t *pos,
                              char open_ch, char close_ch, int depth)
{
  if (depth > 32 || *pos >= s.size () || s[*pos] != open_ch)
    return false;
  ++(*pos);
  skip_json_ws (s, pos);
  if (*pos < s.size () && s[*pos] == close_ch) {
    ++(*pos);
    return true;
  }
  while (*pos < s.size ()) {
    if (open_ch == '{') {
      std::string k;
      if (!parse_json_string_local (s, pos, &k))
        return false;
      skip_json_ws (s, pos);
      if (*pos >= s.size () || s[*pos] != ':')
        return false;
      ++(*pos);
    }
    if (!skip_json_value_local (s, pos, depth + 1))
      return false;
    skip_json_ws (s, pos);
    if (*pos >= s.size ())
      return false;
    if (s[*pos] == ',') {
      ++(*pos);
      continue;
    }
    if (s[*pos] == close_ch) {
      ++(*pos);
      return true;
    }
    return false;
  }
  return false;
}

bool
skip_json_value_local (const std::string &s, size_t *pos, int depth)
{
  if (depth > 32)
    return false;
  skip_json_ws (s, pos);
  if (*pos >= s.size ())
    return false;
  char c = s[*pos];
  if (c == '"') {
    std::string dummy;
    return parse_json_string_local (s, pos, &dummy);
  }
  if (c == '{')
    return skip_json_array_or_obj_local (s, pos, '{', '}', depth);
  if (c == '[')
    return skip_json_array_or_obj_local (s, pos, '[', ']', depth);
  while (*pos < s.size ()) {
    char ch = s[*pos];
    if (ch == ',' || ch == '}' || ch == ']' || ch == ' ' || ch == '\t' ||
        ch == '\r' || ch == '\n') {
      return true;
    }
    ++(*pos);
  }
  return true;
}

bool
parse_secondary_tile_id (const std::string &s, size_t *pos, std::string *out_id)
{
  skip_json_ws (s, pos);
  if (*pos >= s.size () || s[*pos] != '{')
    return skip_json_value_local (s, pos, 0);
  ++(*pos);
  skip_json_ws (s, pos);
  if (*pos < s.size () && s[*pos] == '}') {
    ++(*pos);
    return true;
  }
  while (*pos < s.size ()) {
    std::string key;
    if (!parse_json_string_local (s, pos, &key))
      return false;
    skip_json_ws (s, pos);
    if (*pos >= s.size () || s[*pos] != ':')
      return false;
    ++(*pos);
    skip_json_ws (s, pos);
    if (*pos < s.size () && s[*pos] == '"' &&
        (key == "appUserModelId" || key == "packageFamilyName" ||
         key == "packagedAppId")) {
      std::string v;
      if (!parse_json_string_local (s, pos, &v))
        return false;
      if (out_id != nullptr && out_id->empty () && !v.empty ())
        *out_id = std::move (v);
    } else {
      if (!skip_json_value_local (s, pos, 1))
        return false;
    }
    skip_json_ws (s, pos);
    if (*pos < s.size () && s[*pos] == ',') {
      ++(*pos);
      continue;
    }
    if (*pos < s.size () && s[*pos] == '}') {
      ++(*pos);
      return true;
    }
    return false;
  }
  return false;
}

bool parse_pinned_array (const std::string &s, size_t *pos, int depth,
                         std::vector<StartMenuPinnedItem> *out);

bool
parse_pinned_object (const std::string &s, size_t *pos, int depth,
                     StartMenuPinnedItem *out_item, bool *has_item)
{
  if (depth > 16)
    return false;
  skip_json_ws (s, pos);
  if (*pos >= s.size () || s[*pos] != '{')
    return false;
  ++(*pos);
  skip_json_ws (s, pos);
  if (*pos < s.size () && s[*pos] == '}') {
    ++(*pos);
    *has_item = false;
    return true;
  }

  std::string link_val;
  std::string uwp_val;
  std::string appid_val;
  std::string sec_val;
  std::string start2_id;
  std::string folder_name;
  std::vector<StartMenuPinnedItem> folder_children;
  bool has_folder_items = false;

  while (*pos < s.size ()) {
    std::string key;
    if (!parse_json_string_local (s, pos, &key))
      return false;
    skip_json_ws (s, pos);
    if (*pos >= s.size () || s[*pos] != ':')
      return false;
    ++(*pos);
    skip_json_ws (s, pos);

    if (key == "id" && *pos < s.size () && s[*pos] == '"') {
      if (!parse_json_string_local (s, pos, &start2_id))
        return false;
    } else if (key == "desktopAppLink" && *pos < s.size () && s[*pos] == '"') {
      if (!parse_json_string_local (s, pos, &link_val))
        return false;
    } else if (key == "packagedAppId" && *pos < s.size () && s[*pos] == '"') {
      if (!parse_json_string_local (s, pos, &uwp_val))
        return false;
    } else if (key == "desktopAppId" && *pos < s.size () && s[*pos] == '"') {
      if (!parse_json_string_local (s, pos, &appid_val))
        return false;
    } else if (key == "secondaryTile") {
      if (!parse_secondary_tile_id (s, pos, &sec_val))
        return false;
    } else if ((key == "folderName" || key == "name" || key == "title") &&
               *pos < s.size () && s[*pos] == '"') {
      if (!parse_json_string_local (s, pos, &folder_name))
        return false;
    } else if ((key == "folderItems" || key == "items") &&
               *pos < s.size () && s[*pos] == '[') {
      if (!parse_pinned_array (s, pos, depth + 1, &folder_children))
        return false;
      has_folder_items = true;
    } else {
      if (!skip_json_value_local (s, pos, depth + 1))
        return false;
    }

    skip_json_ws (s, pos);
    if (*pos < s.size () && s[*pos] == ',') {
      ++(*pos);
      continue;
    }
    if (*pos < s.size () && s[*pos] == '}') {
      ++(*pos);
      break;
    }
    return false;
  }

  *has_item = false;
  if (has_folder_items && !folder_children.empty ()) {
    out_item->kind = StartMenuPinnedItem::Kind::Folder;
    out_item->value = std::move (folder_name);
    out_item->children = std::move (folder_children);
    *has_item = true;
  } else if (start2_id.rfind ("P~", 0) == 0 && start2_id.size () > 2) {
    out_item->kind = StartMenuPinnedItem::Kind::PackagedApp;
    out_item->value = start2_id.substr (2);
    *has_item = true;
  } else if (start2_id.rfind ("W~", 0) == 0 && start2_id.size () > 2) {
    out_item->kind = StartMenuPinnedItem::Kind::DesktopAppId;
    out_item->value = start2_id.substr (2);
    *has_item = true;
  } else if (!link_val.empty ()) {
    out_item->kind = StartMenuPinnedItem::Kind::DesktopLink;
    out_item->value = std::move (link_val);
    *has_item = true;
  } else if (!uwp_val.empty ()) {
    out_item->kind = StartMenuPinnedItem::Kind::PackagedApp;
    out_item->value = std::move (uwp_val);
    *has_item = true;
  } else if (!appid_val.empty ()) {
    out_item->kind = StartMenuPinnedItem::Kind::DesktopAppId;
    out_item->value = std::move (appid_val);
    *has_item = true;
  } else if (!sec_val.empty ()) {
    out_item->kind = StartMenuPinnedItem::Kind::PackagedApp;
    out_item->value = std::move (sec_val);
    *has_item = true;
  }
  return true;
}

bool
parse_pinned_array (const std::string &s, size_t *pos, int depth,
                    std::vector<StartMenuPinnedItem> *out)
{
  if (depth > 16)
    return false;
  skip_json_ws (s, pos);
  if (*pos >= s.size () || s[*pos] != '[')
    return false;
  ++(*pos);
  skip_json_ws (s, pos);
  if (*pos < s.size () && s[*pos] == ']') {
    ++(*pos);
    return true;
  }
  while (*pos < s.size ()) {
    skip_json_ws (s, pos);
    if (*pos < s.size () && s[*pos] == '{') {
      StartMenuPinnedItem item;
      bool has_item = false;
      if (!parse_pinned_object (s, pos, depth + 1, &item, &has_item))
        return false;
      if (has_item)
        out->push_back (std::move (item));
    } else {
      if (!skip_json_value_local (s, pos, depth + 1))
        return false;
    }
    skip_json_ws (s, pos);
    if (*pos < s.size () && s[*pos] == ',') {
      ++(*pos);
      continue;
    }
    if (*pos < s.size () && s[*pos] == ']') {
      ++(*pos);
      return true;
    }
    return false;
  }
  return false;
}

std::string
read_file_bytes_local (const std::wstring &path)
{
  if (path.empty ())
    return {};
  HANDLE hf = CreateFileW (path.c_str (), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
  if (hf == INVALID_HANDLE_VALUE)
    return {};
  LARGE_INTEGER sz{};
  std::string out;
  if (GetFileSizeEx (hf, &sz) && sz.QuadPart > 0 && sz.QuadPart <= 4 * 1024 * 1024) {
    out.resize (static_cast<size_t> (sz.QuadPart));
    DWORD read_bytes = 0;
    if (!ReadFile (hf, out.data (), static_cast<DWORD> (sz.QuadPart), &read_bytes,
                   nullptr)) {
      out.clear ();
    } else {
      out.resize (read_bytes);
    }
  }
  CloseHandle (hf);
  return out;
}

void
watch_thread_proc ()
{
  std::vector<HANDLE> handles;
  if (g_watch_stop_event != nullptr)
    handles.push_back (g_watch_stop_event);

  auto add_dir_watch = [&handles] (const std::wstring &dir) {
    if (dir.empty () || GetFileAttributesW (dir.c_str ()) == INVALID_FILE_ATTRIBUTES)
      return;
    HANDLE h = FindFirstChangeNotificationW (
        dir.c_str (), TRUE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_LAST_WRITE);
    if (h != nullptr && h != INVALID_HANDLE_VALUE)
      handles.push_back (h);
  };

  wchar_t buf[MAX_PATH] = L"";
  if (GetEnvironmentVariableW (L"APPDATA", buf, MAX_PATH) > 0) {
    add_dir_watch (std::wstring (buf) +
                   L"\\Microsoft\\Windows\\Start Menu\\Programs");
  }
  if (GetEnvironmentVariableW (L"ProgramData", buf, MAX_PATH) > 0) {
    add_dir_watch (std::wstring (buf) +
                   L"\\Microsoft\\Windows\\Start Menu\\Programs");
  }
  if (GetEnvironmentVariableW (L"LOCALAPPDATA", buf, MAX_PATH) > 0) {
    add_dir_watch (std::wstring (buf) +
                   L"\\Packages\\Microsoft.Windows.StartMenuExperienceHost_cw5n1h2txyewy\\LocalState");
  }

  if (handles.size () <= 1)
    return;

  for (;;) {
    DWORD wait = WaitForMultipleObjects (
        static_cast<DWORD> (handles.size ()), handles.data (), FALSE, INFINITE);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED)
      break;
    DWORD idx = wait - WAIT_OBJECT_0;
    if (idx < handles.size ())
      FindNextChangeNotification (handles[idx]);

    // Coalesce burst file writes (shortcut installers / Start Menu saves).
    if (WaitForSingleObject (handles[0], 500) == WAIT_OBJECT_0)
      break;
    for (size_t i = 1; i < handles.size (); ++i) {
      while (WaitForSingleObject (handles[i], 0) == WAIT_OBJECT_0)
        FindNextChangeNotification (handles[i]);
    }
    app_index_refresh_async (nullptr, nullptr);
  }

  for (size_t i = 1; i < handles.size (); ++i)
    FindCloseChangeNotification (handles[i]);
}

}  // namespace

std::vector<StartMenuPinnedItem>
parse_windows_start_layout_json (const std::string &json_text)
{
  std::vector<StartMenuPinnedItem> result;
  size_t pos = 0;
  // Strip UTF-8 BOM if present, or scan to first '{' (handles binary headers).
  if (json_text.size () >= 3 &&
      static_cast<unsigned char> (json_text[0]) == 0xEF &&
      static_cast<unsigned char> (json_text[1]) == 0xBB &&
      static_cast<unsigned char> (json_text[2]) == 0xBF) {
    pos = 3;
  }
  size_t brace = json_text.find ("{\"pinnedList\"", pos);
  if (brace == std::string::npos)
    brace = json_text.find ('{', pos);
  if (brace == std::string::npos)
    return result;
  pos = brace + 1;
  skip_json_ws (json_text, &pos);
  if (pos < json_text.size () && json_text[pos] == '}')
    return result;

  while (pos < json_text.size ()) {
    std::string key;
    if (!parse_json_string_local (json_text, &pos, &key))
      break;
    skip_json_ws (json_text, &pos);
    if (pos >= json_text.size () || json_text[pos] != ':')
      break;
    ++pos;
    skip_json_ws (json_text, &pos);
    if (key == "pinnedList" && pos < json_text.size () &&
        json_text[pos] == '[') {
      parse_pinned_array (json_text, &pos, 0, &result);
      break;
    }
    if (!skip_json_value_local (json_text, &pos, 0))
      break;
    skip_json_ws (json_text, &pos);
    if (pos < json_text.size () && json_text[pos] == ',') {
      ++pos;
      continue;
    }
    break;
  }
  return result;
}

bool
decrypt_start2_bin_bytes (const std::string &raw_bytes, std::string *out_json)
{
  if (out_json == nullptr)
    return false;
  out_json->clear ();

  // Minimal 44-byte header + 512-byte random padding split + AES ciphertext.
  if (raw_bytes.size () <= 44u + 512u)
    return false;

  auto read_u32_le = [&raw_bytes] (size_t off) -> uint32_t {
    const auto *p = reinterpret_cast<const uint8_t *> (raw_bytes.data () + off);
    return static_cast<uint32_t> (p[0]) |
           (static_cast<uint32_t> (p[1]) << 8) |
           (static_cast<uint32_t> (p[2]) << 16) |
           (static_cast<uint32_t> (p[3]) << 24);
  };

  const uint32_t ft_low = read_u32_le (0x20);
  const uint32_t ft_high = read_u32_le (0x24);
  const uint32_t total_len = read_u32_le (0x28);
  if (total_len <= 0x200u ||
      44u + static_cast<size_t> (total_len) > raw_bytes.size ()) {
    return false;
  }

  const uint32_t ct_len = total_len - 0x200u;
  if ((ct_len % 16u) != 0u)
    return false;

  auto mt_uniform = [] (std::mt19937 &mt, uint32_t r) -> uint32_t {
    if (r == 0u)
      return 0u;
    if (r == 0xFFFFFFFFu)
      return mt ();
    const uint32_t b = r + 1u;
    const uint32_t limit_q = 0xFFFFFFFFu / b;
    const uint32_t rem = 0xFFFFFFFFu % b;
    for (;;) {
      const uint32_t v = mt ();
      if (!(limit_q <= (v / b) && rem != r))
        return v % b;
    }
  };

  auto alpha_numeric = [] (uint32_t seed, size_t len) -> std::vector<uint8_t> {
    std::mt19937 mt (seed);
    uint32_t v = 0;
    do {
      v = mt ();
    } while ((v / 0x3E9u) > 0x417873u);
    mt.discard (v % 0x3E9u);
    std::vector<uint8_t> out (len);
    for (size_t i = 0; i < len; ++i) {
      do {
        v = mt ();
      } while ((v / 0x60u) > 0x2AAAAA9u);
      out[i] = static_cast<uint8_t> ((v % 0x60u) + 0x20u);
    }
    return out;
  };

  std::mt19937 pad_mt (0x00AF4D97u ^ ft_low);
  const uint32_t pre_pad = pad_mt () & 0x1FFu;
  const size_t ct_off = 0x2Cu + static_cast<size_t> (pre_pad);
  if (ct_off + ct_len > raw_bytes.size ())
    return false;

  std::mt19937 sym_mt (ft_high ^ ft_low ^ 0x3B21D91Eu);
  const uint32_t key_seed = sym_mt ();
  const uint32_t key_len = 64u + mt_uniform (sym_mt, 64u);
  const uint32_t iv_seed = sym_mt ();

  std::vector<uint8_t> key = alpha_numeric (key_seed, key_len);
  std::vector<uint8_t> iv = alpha_numeric (iv_seed, 16u);
  if (key.size () < 32u || iv.size () != 16u)
    return false;

  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_KEY_HANDLE hkey = nullptr;
  bool ok = false;
  if (BCryptOpenAlgorithmProvider (&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) >= 0) {
    if (BCryptSetProperty (
            alg, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR> (const_cast<wchar_t *> (BCRYPT_CHAIN_MODE_CBC)),
            sizeof (BCRYPT_CHAIN_MODE_CBC), 0) >= 0 &&
        BCryptGenerateSymmetricKey (alg, &hkey, nullptr, 0, key.data (), 32u, 0) >=
            0) {
      std::vector<uint8_t> ct_buf (
          reinterpret_cast<const uint8_t *> (raw_bytes.data () + ct_off),
          reinterpret_cast<const uint8_t *> (raw_bytes.data () + ct_off + ct_len));
      std::vector<uint8_t> iv_work = iv;
      std::string plain (ct_len, '\0');
      ULONG plain_len = 0;
      LONG st = BCryptDecrypt (
          hkey, ct_buf.data (), ct_len, nullptr, iv_work.data (),
          static_cast<ULONG> (iv_work.size ()),
          reinterpret_cast<PUCHAR> (plain.data ()), ct_len, &plain_len,
          BCRYPT_BLOCK_PADDING);
      if (st >= 0 && plain_len <= ct_len) {
        plain.resize (plain_len);
        *out_json = std::move (plain);
        ok = true;
      }
      BCryptDestroyKey (hkey);
    }
    BCryptCloseAlgorithmProvider (alg, 0);
  }
  return ok;
}

std::vector<StartMenuPinnedItem>
load_windows_start_menu_pinned ()
{
  wchar_t override_path[MAX_PATH] = L"";
  if (GetEnvironmentVariableW (L"WINOME_START_LAYOUT_JSON", override_path,
                             MAX_PATH) > 0 &&
      override_path[0] != L'\0') {
    std::string raw = read_file_bytes_local (override_path);
    if (!raw.empty ())
      return parse_windows_start_layout_json (raw);
  }

  wchar_t local_app[MAX_PATH] = L"";
  std::wstring start_dir;
  std::wstring start2_path;
  std::wstring cache_json_path;
  std::wstring cache_meta_path;
  std::string current_stamp;
  if (GetEnvironmentVariableW (L"LOCALAPPDATA", local_app, MAX_PATH) > 0) {
    start_dir =
        std::wstring (local_app) +
        L"\\Packages\\Microsoft.Windows.StartMenuExperienceHost_cw5n1h2txyewy\\LocalState\\";
    start2_path = start_dir + L"start2.bin";
    if (!get_file_write_stamp (start2_path, &current_stamp))
      get_file_write_stamp (start_dir + L"start.bin", &current_stamp);

    // Primary path on Windows 11: directly decrypt live start2.bin / start.bin
    // so manually pinned items AND Start Menu folder groups (`pinType: 2`)
    // remain 100% intact.
    for (const wchar_t *fname : {L"start2.bin", L"start.bin"}) {
      std::string raw = read_file_bytes_local (start_dir + fname);
      if (raw.empty ())
        continue;
      std::string decrypted;
      if (decrypt_start2_bin_bytes (raw, &decrypted)) {
        auto parsed = parse_windows_start_layout_json (decrypted);
        if (!parsed.empty ())
          return parsed;
      }
      auto plain_parsed = parse_windows_start_layout_json (raw);
      if (!plain_parsed.empty ())
        return plain_parsed;
    }

    std::wstring winome_dir = std::wstring (local_app) + L"\\winome";
    CreateDirectoryW (winome_dir.c_str (), nullptr);
    cache_json_path = winome_dir + L"\\start-layout-cache.json";
    cache_meta_path = winome_dir + L"\\start-layout-cache.meta";

    if (!current_stamp.empty ()) {
      std::string saved_stamp = read_file_bytes_local (cache_meta_path);
      if (saved_stamp == current_stamp) {
        std::string cached = read_file_bytes_local (cache_json_path);
        if (!cached.empty ())
          return parse_windows_start_layout_json (cached);
      }
    }
  }

  // Fallback for builds where start2.bin encryption differs: invoke Windows
  // `Export-StartLayout` cmdlet quietly and cache by start2.bin timestamp.
  std::string json_raw;
  wchar_t tmp_dir[MAX_PATH] = L"";
  if (GetTempPathW (MAX_PATH, tmp_dir) > 0) {
    std::wstring tmp_file =
        std::wstring (tmp_dir) + L"winome-startlayout-" +
        std::to_wstring (GetCurrentProcessId ()) + L"-" +
        std::to_wstring (GetCurrentThreadId ()) + L".json";
    std::wstring ps_cmd =
        L"powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden "
        L"-ExecutionPolicy Bypass -Command \"Export-StartLayout -Path '";
    for (wchar_t ch : tmp_file) {
      if (ch == L'\'')
        ps_cmd += L"''";
      else
        ps_cmd.push_back (ch);
    }
    ps_cmd += L"'\"";

    STARTUPINFOW si{};
    si.cb = sizeof (si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW (nullptr, ps_cmd.data (), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
      DWORD wait = WaitForSingleObject (pi.hProcess, 5000);
      if (wait == WAIT_OBJECT_0) {
        DWORD code = 1;
        if (GetExitCodeProcess (pi.hProcess, &code) && code == 0)
          json_raw = read_file_bytes_local (tmp_file);
      } else {
        TerminateProcess (pi.hProcess, 1);
      }
      CloseHandle (pi.hThread);
      CloseHandle (pi.hProcess);
    }
    DeleteFileW (tmp_file.c_str ());
  }

  if (!json_raw.empty ()) {
    auto parsed = parse_windows_start_layout_json (json_raw);
    if (!parsed.empty () && !cache_json_path.empty () &&
        !current_stamp.empty ()) {
      write_file_bytes_local (cache_json_path, json_raw);
      write_file_bytes_local (cache_meta_path, current_stamp);
    }
    if (!parsed.empty ())
      return parsed;
  }
  return {};
}

namespace {

std::wstring
expand_known_folder_and_env_wide (const std::wstring &raw)
{
  std::wstring s = expand_env_wide_local (raw);
  if (s.size () >= 38 && s[0] == L'{' && s[37] == L'}') {
    std::wstring guid_str = s.substr (0, 38);
    GUID kf_id{};
    if (SUCCEEDED (CLSIDFromString (guid_str.c_str (), &kf_id))) {
      PWSTR kf_path = nullptr;
      if (SUCCEEDED (SHGetKnownFolderPath (kf_id, KF_FLAG_DEFAULT, nullptr,
                                           &kf_path)) &&
          kf_path != nullptr) {
        std::wstring expanded (kf_path);
        CoTaskMemFree (kf_path);
        return expanded + s.substr (38);
      }
    }
  }
  return s;
}

}  // namespace

std::vector<AppGridItem>
build_app_grid_items (const std::vector<AppEntry> &apps,
                      const std::vector<StartMenuPinnedItem> &pinned_items,
                      const std::string &query_lower)
{
  std::vector<AppGridItem> items;
  if (!query_lower.empty ()) {
    std::vector<AppEntry> matched = filter_and_sort_apps (apps, query_lower);
    items.reserve (matched.size ());
    for (AppEntry &e : matched) {
      AppGridItem gi;
      gi.kind = AppGridItemKind::App;
      gi.id = e.id;
      gi.title = e.name;
      gi.app = std::move (e);
      items.push_back (std::move (gi));
    }
    return items;
  }

  std::unordered_map<std::string, size_t> by_launch;
  std::unordered_map<std::string, size_t> by_target;
  std::unordered_map<std::string, size_t> by_url;
  std::unordered_map<std::string, size_t> by_aumid;
  std::unordered_map<std::string, size_t> by_id;
  for (size_t i = 0; i < apps.size (); ++i) {
    if (!apps[i].launch_path.empty ()) {
      std::string lp = normalize_win_path_lower (apps[i].launch_path);
      if (!lp.empty () && by_launch.find (lp) == by_launch.end ())
        by_launch[lp] = i;
    }
    if (!apps[i].target_exe.empty ()) {
      std::string tp = normalize_win_path_lower (apps[i].target_exe);
      if (!tp.empty () && by_target.find (tp) == by_target.end ())
        by_target[tp] = i;
    }
    if (!apps[i].launch_url.empty ()) {
      if (by_url.find (apps[i].launch_url) == by_url.end ())
        by_url[apps[i].launch_url] = i;
    }
    if (!apps[i].aumid.empty ()) {
      std::string au = normalize_app_id (app_wide_to_utf8 (apps[i].aumid));
      if (!au.empty () && by_aumid.find (au) == by_aumid.end ())
        by_aumid[au] = i;
    }
    if (!apps[i].id.empty ()) {
      std::string id = normalize_app_id (apps[i].id);
      if (!id.empty () && by_id.find (id) == by_id.end ())
        by_id[id] = i;
    }
  }

  std::vector<bool> used (apps.size (), false);
  std::unordered_map<std::string, bool> placed_ids;

  auto synthesize_disk_entry = [&] (const std::wstring &wval,
                                    AppEntry *out_entry) -> bool {
    DWORD attrs = wval.empty () ? INVALID_FILE_ATTRIBUTES
                                : GetFileAttributesW (wval.c_str ());
    if (attrs == INVALID_FILE_ATTRIBUTES)
      return false;

    std::wstring clean_path = wval;
    while (clean_path.size () > 3 &&
           (clean_path.back () == L'\\' || clean_path.back () == L'/')) {
      clean_path.pop_back ();
    }
    size_t slash = clean_path.find_last_of (L"\\/");
    std::wstring fname = slash != std::wstring::npos
                             ? clean_path.substr (slash + 1)
                             : clean_path;
    bool is_dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    std::wstring stem = fname;
    if (!is_dir) {
      size_t dot = fname.find_last_of (L'.');
      if (dot != std::wstring::npos) {
        std::wstring ext = fname.substr (dot);
        if (_wcsicmp (ext.c_str (), L".lnk") == 0 ||
            _wcsicmp (ext.c_str (), L".url") == 0 ||
            _wcsicmp (ext.c_str (), L".exe") == 0) {
          stem = fname.substr (0, dot);
        }
      }
    }
    std::string name = app_wide_to_utf8 (stem);
    if (name.empty ())
      return false;

    AppEntry e;
    e.name = std::move (name);
    e.name_lower = app_utf8_tolower (e.name);
    e.launch_path = clean_path;
    if (!is_dir) {
      size_t dot = fname.find_last_of (L'.');
      if (dot != std::wstring::npos &&
          _wcsicmp (fname.c_str () + dot, L".url") == 0) {
        resolve_url_shortcut_info (clean_path, &e);
      } else if (dot != std::wstring::npos &&
                 _wcsicmp (fname.c_str () + dot, L".lnk") == 0) {
        resolve_shortcut_info (clean_path, &e);
      } else {
        e.target_exe = clean_path;
      }
    }
    e.id = canonical_app_id (e.aumid, e.target_exe, e.launch_path);
    if (!e.target_exe.empty ()) {
      e.target_lower = app_utf8_tolower (
          app_wide_to_utf8 (basename_lower (e.target_exe)));
    }
    if (e.id.empty () || placed_ids[e.id])
      return false;
    placed_ids[e.id] = true;
    *out_entry = std::move (e);
    return true;
  };

  auto claim_index = [&] (size_t idx, AppEntry *out_entry) -> bool {
    used[idx] = true;
    if (placed_ids[apps[idx].id])
      return false;
    placed_ids[apps[idx].id] = true;
    *out_entry = apps[idx];
    return true;
  };

  auto resolve_pin_entry = [&] (const StartMenuPinnedItem &pin,
                                AppEntry *out_entry) -> bool {
    std::wstring wval =
        expand_known_folder_and_env_wide (wide_from_utf8_local (pin.value));
    std::string lp = normalize_win_path_lower (wval);

    if (pin.kind == StartMenuPinnedItem::Kind::DesktopLink) {
      if (auto li = by_launch.find (lp); li != by_launch.end ())
        return claim_index (li->second, out_entry);
      if (auto ti = by_target.find (lp); ti != by_target.end ())
        return claim_index (ti->second, out_entry);
      return synthesize_disk_entry (wval, out_entry);
    }

    if (auto li = by_launch.find (lp); li != by_launch.end ())
      return claim_index (li->second, out_entry);
    if (auto ti = by_target.find (lp); ti != by_target.end ())
      return claim_index (ti->second, out_entry);

    std::string raw_low = app_utf8_tolower (pin.value);
    if (auto ui = by_url.find (raw_low); ui != by_url.end ())
      return claim_index (ui->second, out_entry);

    std::string norm = normalize_app_id (pin.value);
    if (!norm.empty ()) {
      if (auto ai = by_aumid.find (norm); ai != by_aumid.end ())
        return claim_index (ai->second, out_entry);
      if (auto ii = by_id.find (norm); ii != by_id.end ())
        return claim_index (ii->second, out_entry);
    }

    return synthesize_disk_entry (wval, out_entry);
  };

  // Tier 1: Manually pinned Windows Start Menu items & folders in exact order.
  int pinned_folder_seq = 0;
  for (const StartMenuPinnedItem &pin : pinned_items) {
    if (pin.kind == StartMenuPinnedItem::Kind::Folder) {
      std::vector<AppEntry> children;
      for (const StartMenuPinnedItem &child : pin.children) {
        AppEntry ce;
        if (resolve_pin_entry (child, &ce))
          children.push_back (std::move (ce));
      }
      if (!children.empty ()) {
        AppGridItem fi;
        fi.kind = AppGridItemKind::Folder;
        fi.id = "pinned-folder:" + std::to_string (pinned_folder_seq++);
        fi.title = pin.value.empty () ? "Folder" : pin.value;
        fi.children = std::move (children);
        items.push_back (std::move (fi));
      }
    } else {
      AppEntry ae;
      if (resolve_pin_entry (pin, &ae)) {
        AppGridItem ai;
        ai.kind = AppGridItemKind::App;
        ai.id = ae.id;
        ai.title = ae.name;
        ai.app = std::move (ae);
        items.push_back (std::move (ai));
      }
    }
  }

  // Tier 2: One-way sync grouping for remaining unpinned apps following Windows
  // Start Menu\Programs folder directory order.
  struct FolderGroup {
    std::string key;
    std::string display_name;
    bool from_user = false;
    int folder_order = 0x7fffffff;
    std::vector<AppEntry> children;
  };
  std::unordered_map<std::string, size_t> group_idx;
  std::vector<FolderGroup> groups;
  std::vector<AppEntry> ungrouped;

  for (size_t i = 0; i < apps.size (); ++i) {
    if (used[i] || placed_ids[apps[i].id])
      continue;
    placed_ids[apps[i].id] = true;
    if (!apps[i].folder_name.empty ()) {
      std::string fkey = app_utf8_tolower (apps[i].folder_name);
      auto git = group_idx.find (fkey);
      if (git == group_idx.end ()) {
        size_t idx = groups.size ();
        group_idx[fkey] = idx;
        FolderGroup fg;
        fg.key = fkey;
        fg.display_name = apps[i].folder_name;
        fg.from_user = apps[i].from_user_programs;
        fg.folder_order =
            apps[i].folder_order >= 0 ? apps[i].folder_order : 0x7fffffff;
        fg.children.push_back (apps[i]);
        groups.push_back (std::move (fg));
      } else {
        FolderGroup &fg = groups[git->second];
        if (apps[i].from_user_programs && !fg.from_user) {
          fg.from_user = true;
          fg.display_name = apps[i].folder_name;
          if (apps[i].folder_order >= 0)
            fg.folder_order = apps[i].folder_order;
        } else if (apps[i].from_user_programs == fg.from_user &&
                   apps[i].folder_order >= 0) {
          fg.folder_order = std::min (fg.folder_order, apps[i].folder_order);
        }
        fg.children.push_back (apps[i]);
      }
    } else {
      ungrouped.push_back (apps[i]);
    }
  }

  std::sort (groups.begin (), groups.end (),
             [] (const FolderGroup &a, const FolderGroup &b) {
               if (a.from_user != b.from_user)
                 return a.from_user > b.from_user;
               if (a.folder_order != b.folder_order)
                 return a.folder_order < b.folder_order;
               return a.key < b.key;
             });

  for (FolderGroup &fg : groups) {
    std::sort (fg.children.begin (), fg.children.end (),
               [] (const AppEntry &a, const AppEntry &b) {
                 if (a.from_user_programs != b.from_user_programs)
                   return a.from_user_programs > b.from_user_programs;
                 if (a.item_order != b.item_order)
                   return a.item_order < b.item_order;
                 return a.name_lower < b.name_lower;
               });
    AppGridItem fi;
    fi.kind = AppGridItemKind::Folder;
    fi.id = "folder:" + fg.key;
    fi.title = std::move (fg.display_name);
    fi.children = std::move (fg.children);
    items.push_back (std::move (fi));
  }

  std::sort (ungrouped.begin (), ungrouped.end (),
             [] (const AppEntry &a, const AppEntry &b) {
               if (a.from_user_programs != b.from_user_programs)
                 return a.from_user_programs > b.from_user_programs;
               bool a_uwp = a.is_uwp && a.launch_path.empty ();
               bool b_uwp = b.is_uwp && b.launch_path.empty ();
               if (a_uwp != b_uwp)
                 return !a_uwp;
               if (a.item_order != b.item_order)
                 return a.item_order < b.item_order;
               return a.name_lower < b.name_lower;
             });

  for (AppEntry &e : ungrouped) {
    AppGridItem ai;
    ai.kind = AppGridItemKind::App;
    ai.id = e.id;
    ai.title = e.name;
    ai.app = std::move (e);
    items.push_back (std::move (ai));
  }

  return items;
}

void
app_index_start_watch ()
{
  std::lock_guard<std::mutex> lock (g_watch_mu);
  if (g_watch_running)
    return;
  g_watch_stop_event = CreateEventW (nullptr, TRUE, FALSE, nullptr);
  if (g_watch_stop_event == nullptr)
    return;
  g_watch_running = true;
  g_watch_thread = std::thread (watch_thread_proc);
}

void
app_index_stop_watch ()
{
  std::thread to_join;
  HANDLE ev = nullptr;
  {
    std::lock_guard<std::mutex> lock (g_watch_mu);
    if (!g_watch_running)
      return;
    g_watch_running = false;
    ev = g_watch_stop_event;
    if (ev != nullptr)
      SetEvent (ev);
    to_join = std::move (g_watch_thread);
  }
  if (to_join.joinable ())
    to_join.join ();
  std::lock_guard<std::mutex> lock (g_watch_mu);
  if (g_watch_stop_event != nullptr) {
    CloseHandle (g_watch_stop_event);
    g_watch_stop_event = nullptr;
  }
}

std::string
app_index_debug_dump_json ()
{
  const std::vector<AppEntry> apps = app_index_get_all ();
  const std::vector<StartMenuPinnedItem> pins = app_index_get_pinned_layout ();
  const std::vector<AppGridItem> items = build_app_grid_items (apps, pins, "");

  auto escape_json = [] (const std::string &s, std::string *out) {
    out->push_back ('"');
    for (unsigned char c : s) {
      if (c == '"')
        *out += "\\\"";
      else if (c == '\\')
        *out += "\\\\";
      else if (c < 0x20) {
        char buf[8];
        std::snprintf (buf, sizeof (buf), "\\u%04x", c);
        *out += buf;
      } else {
        out->push_back (static_cast<char> (c));
      }
    }
    out->push_back ('"');
  };

  size_t pin_folders = 0;
  for (const auto &p : pins) {
    if (p.kind == StartMenuPinnedItem::Kind::Folder)
      ++pin_folders;
  }
  size_t grid_folders = 0;
  for (const auto &g : items) {
    if (g.kind == AppGridItemKind::Folder)
      ++grid_folders;
  }

  std::string out = "{";
  out += "\"apps_total\":" + std::to_string (apps.size ());
  out += ",\"pinned_top_count\":" + std::to_string (pins.size ());
  out += ",\"pinned_folder_count\":" + std::to_string (pin_folders);
  out += ",\"grid_items_total\":" + std::to_string (items.size ());
  out += ",\"grid_folders_total\":" + std::to_string (grid_folders);
  out += ",\"items\":[";
  size_t limit = std::min<size_t> (items.size (), 60);
  for (size_t i = 0; i < limit; ++i) {
    if (i > 0)
      out.push_back (',');
    out += "{\"kind\":";
    out += (items[i].kind == AppGridItemKind::Folder ? "\"folder\"" : "\"app\"");
    out += ",\"title\":";
    escape_json (items[i].title, &out);
    if (items[i].kind == AppGridItemKind::Folder) {
      out += ",\"children_count\":" + std::to_string (items[i].children.size ());
    }
    out.push_back ('}');
  }
  out += "]}";
  return out;
}

void
app_index_refresh_async (void (*on_ready) (void *), void *user_data)
{
  app_index_start_watch ();
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
    std::vector<StartMenuPinnedItem> fresh_pinned =
        load_windows_start_menu_pinned ();
    {
      std::lock_guard<std::mutex> lock (g_index_mu);
      g_entries = std::move (fresh);
      g_pinned_layout = std::move (fresh_pinned);
      g_index_populated = true;
      g_index_revision.fetch_add (1, std::memory_order_release);
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
  std::vector<StartMenuPinnedItem> fresh_pinned =
      load_windows_start_menu_pinned ();
  {
    std::lock_guard<std::mutex> lock (g_index_mu);
    if (!g_index_populated) {
      g_entries = std::move (fresh);
      g_pinned_layout = std::move (fresh_pinned);
      g_index_populated = true;
      g_index_revision.fetch_add (1, std::memory_order_release);
    }
    return g_entries;
  }
}

std::vector<StartMenuPinnedItem>
app_index_get_pinned_layout ()
{
  {
    std::lock_guard<std::mutex> lock (g_index_mu);
    if (g_index_populated)
      return g_pinned_layout;
  }
  app_index_get_all ();
  std::lock_guard<std::mutex> lock (g_index_mu);
  return g_pinned_layout;
}

uint64_t
app_index_revision (void)
{
  return g_index_revision.load (std::memory_order_acquire);
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

namespace {

void
shell_execute_async (std::wstring verb, std::wstring file, std::wstring params)
{
  std::thread ([v = std::move (verb), f = std::move (file),
                p = std::move (params)] () {
    HRESULT hr = CoInitializeEx (
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SHELLEXECUTEINFOW sei {};
    sei.cbSize = sizeof (sei);
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = v.empty () ? L"open" : v.c_str ();
    sei.lpFile = f.c_str ();
    sei.lpParameters = p.empty () ? nullptr : p.c_str ();
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW (&sei);
    if (SUCCEEDED (hr))
      CoUninitialize ();
  }).detach ();
}

}  // namespace

bool
app_index_launch (const AppEntry &entry, bool as_admin)
{
  const std::wstring &target =
      !entry.launch_path.empty () ? entry.launch_path : entry.target_exe;
  if (target.empty ())
    return false;
  shell_execute_async (as_admin ? L"runas" : L"open", target, L"");
  return true;
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
    shell_execute_async (as_admin ? L"runas" : L"open", wide, L"");
    return true;
  }
  if (id_or_aumid.find ('!') != std::string::npos) {
    shell_execute_async (L"open", L"shell:AppsFolder\\" + wide, L"");
    return true;
  }
  return false;
}

bool
app_index_open_location (const AppEntry &entry)
{
  std::wstring target =
      !entry.target_exe.empty () ? entry.target_exe : entry.launch_path;
  if (target.empty () || target.rfind (L"shell:", 0) == 0)
    return false;
  shell_execute_async (L"open", L"explorer.exe", L"/select,\"" + target + L"\"");
  return true;
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

void
app_icon_raw_clear (RawDecodedIcon &raw)
{
  if (raw.bytes != nullptr) {
    g_bytes_unref (raw.bytes);
    raw.bytes = nullptr;
  }
  raw.width = 0;
  raw.height = 0;
  raw.unpremultiplied = false;
}

GdkTexture *
app_icon_texture_from_raw (const RawDecodedIcon &raw)
{
  if (raw.bytes == nullptr || raw.width <= 0 || raw.height <= 0)
    return nullptr;
  GdkMemoryFormat fmt = raw.unpremultiplied
                            ? GDK_MEMORY_B8G8R8A8
                            : GDK_MEMORY_B8G8R8A8_PREMULTIPLIED;
  return gdk_memory_texture_new (raw.width, raw.height, fmt, raw.bytes,
                                 (gsize)raw.width * 4);
}

RawDecodedIcon
app_icon_decode_entry_raw (const AppEntry &entry, int size)
{
  RawDecodedIcon raw;
  raw.id = entry.id;
  raw.size = size;
  if (size <= 0)
    return raw;

  bool ok = false;
  if (!entry.launch_path.empty ())
    ok = load_shell_image_raw_for_path (entry.launch_path, size, &raw);
  if (!ok && !entry.target_exe.empty ())
    ok = load_shell_image_raw_for_path (entry.target_exe, size, &raw);
  if (!ok && !entry.icon_path.empty ()) {
    HICON hi = nullptr;
    UINT icon_id = 0;
    UINT extracted = PrivateExtractIconsW (
        entry.icon_path.c_str (), entry.icon_index, size, size, &hi, &icon_id,
        1, 0);
    if (extracted > 0 && extracted != 0xFFFFFFFFu && hi != nullptr) {
      ok = extract_hicon_raw (hi, &raw);
      DestroyIcon (hi);
    }
    if (!ok) {
      HICON fallback = nullptr;
      if (ExtractIconExW (entry.icon_path.c_str (), entry.icon_index, &fallback,
                          nullptr, 1) > 0 &&
          fallback != nullptr) {
        ok = extract_hicon_raw (fallback, &raw);
        DestroyIcon (fallback);
      }
    }
  }
  if (!ok)
    app_icon_raw_clear (raw);
  return raw;
}

GdkTexture *
hicon_to_texture (HICON icon)
{
  RawDecodedIcon raw;
  if (!extract_hicon_raw (icon, &raw))
    return nullptr;
  GdkTexture *texture = app_icon_texture_from_raw (raw);
  app_icon_raw_clear (raw);
  return texture;
}

GdkTexture *
texture_from_hbitmap (HBITMAP bmp)
{
  RawDecodedIcon raw;
  if (!extract_hbitmap_raw (bmp, &raw))
    return nullptr;
  GdkTexture *texture = app_icon_texture_from_raw (raw);
  app_icon_raw_clear (raw);
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

void
app_icon_cache_trim (size_t max_keep)
{
  std::lock_guard<std::mutex> lock (g_cache_mu);
  while (g_cache_lru.size () > max_keep) {
    auto last = std::prev (g_cache_lru.end ());
    g_cache_map.erase (last->key);
    g_clear_object (&last->texture);
    g_cache_lru.erase (last);
  }
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

  RawDecodedIcon raw = app_icon_decode_entry_raw (entry, size);
  GdkTexture *tex = app_icon_texture_from_raw (raw);
  app_icon_raw_clear (raw);
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

namespace {

bool
is_running_alt_tab_window (HWND hwnd)
{
  if (!IsWindowVisible (hwnd))
    return false;

  DWORD pid = 0;
  GetWindowThreadProcessId (hwnd, &pid);
  if (pid == GetCurrentProcessId ())
    return false;

  HWND try_ = GetAncestor (hwnd, GA_ROOTOWNER);
  HWND walk = nullptr;
  while (try_ != walk) {
    walk = try_;
    try_ = GetLastActivePopup (walk);
    if (IsWindowVisible (try_))
      break;
  }
  if (walk != hwnd)
    return false;

  if (GetWindowLongPtrW (hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
    return false;

  // Dynamic lookup avoids adding dwmapi link requirements to test binaries.
  using DwmGetAttrFn = HRESULT (WINAPI *) (HWND, DWORD, PVOID, DWORD);
  static DwmGetAttrFn fn = [] () -> DwmGetAttrFn {
    HMODULE mod = GetModuleHandleW (L"dwmapi.dll");
    if (mod == nullptr)
      mod = LoadLibraryW (L"dwmapi.dll");
    FARPROC proc =
        mod != nullptr ? GetProcAddress (mod, "DwmGetWindowAttribute")
                       : nullptr;
    DwmGetAttrFn resolved = nullptr;
    if (proc != nullptr)
      std::memcpy (&resolved, &proc, sizeof (resolved));
    return resolved;
  }();
  if (fn != nullptr) {
    BOOL cloaked = FALSE;
    constexpr DWORD kDwmwaCloaked = 14;
    if (SUCCEEDED (fn (hwnd, kDwmwaCloaked, &cloaked, sizeof (cloaked))) &&
        cloaked != FALSE) {
      return false;
    }
  }
  return true;
}

std::wstring
running_window_aumid (HWND hwnd)
{
  std::wstring out;
  IPropertyStore *store = nullptr;
  struct WinomePropKey {
    GUID fmtid;
    DWORD pid;
  };
  WinomePropKey key{kPKEY_AppUserModel_ID_fmt, 5};
  if (SUCCEEDED (SHGetPropertyStoreForWindow (hwnd, IID_PPV_ARGS (&store))) &&
      store != nullptr) {
    PROPVARIANT pv;
    PropVariantInit (&pv);
    if (SUCCEEDED (store->GetValue (*reinterpret_cast<PROPERTYKEY *> (&key),
                                    &pv)) &&
        pv.vt == VT_LPWSTR && pv.pwszVal != nullptr) {
      out = pv.pwszVal;
    }
    PropVariantClear (&pv);
    store->Release ();
  }
  return out;
}

std::wstring
running_window_exe (HWND hwnd)
{
  DWORD pid = 0;
  GetWindowThreadProcessId (hwnd, &pid);
  if (pid == 0)
    return {};
  HANDLE proc = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (proc == nullptr)
    return {};
  wchar_t buf[MAX_PATH] = L"";
  DWORD len = MAX_PATH;
  std::wstring out;
  if (QueryFullProcessImageNameW (proc, 0, buf, &len))
    out.assign (buf, len);
  CloseHandle (proc);
  return out;
}

BOOL CALLBACK
collect_running_window_cb (HWND hwnd, LPARAM lparam)
{
  auto *out = reinterpret_cast<std::vector<RunningWindow> *> (lparam);
  if (!is_running_alt_tab_window (hwnd))
    return TRUE;
  if (is_overview_default_blacklisted_hwnd (
          hwnd, winome::get_config ().overview_window_blacklist)) {
    return TRUE;
  }

  std::wstring aumid = running_window_aumid (hwnd);
  std::wstring exe = running_window_exe (hwnd);
  if (aumid.empty () && exe.empty ())
    return TRUE;

  wchar_t wtitle[256] = L"";
  GetWindowTextW (hwnd, wtitle, 256);

  RunningWindow rw;
  rw.hwnd = hwnd;
  rw.title = app_wide_to_utf8 (wtitle);
  rw.aumid_norm = normalize_app_id (app_wide_to_utf8 (aumid));
  rw.exe_norm = normalize_app_id (app_wide_to_utf8 (exe));
  size_t slash = rw.exe_norm.find_last_of ('\\');
  rw.exe_base_lower =
      (slash != std::string::npos) ? rw.exe_norm.substr (slash + 1) : rw.exe_norm;
  rw.canonical_id = canonical_app_id (aumid, exe, L"");
  out->push_back (std::move (rw));
  return TRUE;
}

}  // namespace

std::vector<RunningWindow>
match_entry_running_windows (const AppEntry &entry,
                             const std::vector<RunningWindow> &windows)
{
  std::string entry_aumid =
      normalize_app_id (app_wide_to_utf8 (entry.aumid));
  std::string entry_exe =
      normalize_app_id (app_wide_to_utf8 (entry.target_exe));
  bool is_frame_host =
      entry_exe.find ("\\applicationframehost.exe") != std::string::npos;

  std::vector<RunningWindow> matched;
  for (const RunningWindow &w : windows) {
    bool hit = false;
    if (!entry_aumid.empty () && !w.aumid_norm.empty ()) {
      hit = (entry_aumid == w.aumid_norm);
    } else if (!entry.id.empty () && !w.canonical_id.empty () &&
               entry.id == w.canonical_id) {
      hit = true;
    } else if (!entry_aumid.empty () && w.canonical_id == entry_aumid) {
      hit = true;
    } else if (!entry_exe.empty () && !is_frame_host &&
               w.exe_base_lower != "applicationframehost.exe" &&
               !w.exe_norm.empty () && entry_exe == w.exe_norm &&
               (w.aumid_norm.empty () || entry_aumid.empty ())) {
      hit = true;
    }
    if (hit)
      matched.push_back (w);
  }
  return matched;
}

std::vector<RunningWindow>
app_index_enumerate_running_windows (void)
{
  std::vector<RunningWindow> out;
  EnumWindows (collect_running_window_cb, reinterpret_cast<LPARAM> (&out));
  return out;
}

std::vector<RunningWindow>
app_index_running_windows_for_entry (const AppEntry &entry)
{
  std::vector<RunningWindow> all = app_index_enumerate_running_windows ();
  return match_entry_running_windows (entry, all);
}

bool
app_index_activate_hwnd (HWND hwnd)
{
  if (hwnd == nullptr || !IsWindow (hwnd))
    return false;
  if (IsIconic (hwnd))
    ShowWindow (hwnd, SW_RESTORE);
  return SetForegroundWindow (hwnd) != FALSE;
}

void
app_index_close_windows (const std::vector<HWND> &hwnds)
{
  for (HWND hwnd : hwnds) {
    if (hwnd != nullptr && IsWindow (hwnd))
      PostMessageW (hwnd, WM_CLOSE, 0, 0);
  }
}

}  // namespace winome
