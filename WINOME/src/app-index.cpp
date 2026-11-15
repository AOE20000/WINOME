// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors

#include "app-index.h"
#include "config.h"

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
bool g_index_populated = false;
std::atomic<bool> g_refresh_running{false};
std::atomic<uint64_t> g_index_revision{0};

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
      std::unordered_map<std::string, bool> lnk_names;
      std::unordered_map<std::string, bool> lnk_ids;
      for (const AppEntry &e : raw) {
        if (!e.id.empty ())
          lnk_ids[e.id] = true;
        if (!e.name_lower.empty ())
          lnk_names[e.name_lower] = true;
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
                !id.empty () && lnk_ids.find (id) == lnk_ids.end () &&
                lnk_names.find (name_lower) == lnk_names.end ()) {
              AppEntry uwp;
              uwp.id = std::move (id);
              uwp.name = std::move (name);
              uwp.name_lower = std::move (name_lower);
              uwp.aumid = app_id;
              uwp.launch_path = L"shell:AppsFolder\\" + app_id;
              uwp.is_uwp = true;
              lnk_ids[uwp.id] = true;
              lnk_names[uwp.name_lower] = true;
              raw.push_back (std::move (uwp));
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
  {
    std::lock_guard<std::mutex> lock (g_index_mu);
    if (!g_index_populated) {
      g_entries = fresh;
      g_index_populated = true;
      g_index_revision.fetch_add (1, std::memory_order_release);
    }
    return g_entries;
  }
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
  if (target.empty () || target.rfind (L"shell:", 0) == 0)
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
