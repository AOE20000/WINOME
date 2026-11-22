// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Safe Windows System Tray discovery & activation without DLL injection.

#include "systray.h"

#include "native-taskbar.h"
#include "perf-metrics.h"

#include <cstdio>

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <dwmapi.h>
#include <oleauto.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uiautomation.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace winome {
namespace {

constexpr CLSID kClsidCUIAutomation = {
    0xff48dba4, 0x60ef, 0x4201,
    {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
constexpr IID kIidIUIAutomation = {
    0x30cbe57d, 0xd9d0, 0x452a,
    {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};
constexpr IID kIidIUIAutomation2 = {
    0x34723aff, 0x0c9d, 0x49d0,
    {0x98, 0x96, 0x7a, 0xb5, 0x2d, 0xf8, 0xcd, 0x8a}};
constexpr IID kIidIUIAutomationInvokePattern = {
    0xfb377fbe, 0x8ea6, 0x46d5,
    {0x9c, 0x73, 0x64, 0x99, 0x64, 0x2d, 0x30, 0x59}};
constexpr IID kIidIUIAutomationLegacyIAccessiblePattern = {
    0x828055ad, 0x355b, 0x4435,
    {0x86, 0xd5, 0x3b, 0x51, 0xc1, 0x4a, 0x9b, 0x1b}};

double qpc_ms_since(const std::chrono::steady_clock::time_point& t0) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

void configure_uia_timeouts(IUIAutomation* uia,
                            DWORD conn_ms = 800,
                            DWORD tx_ms = 1500) {
  if (uia == nullptr)
    return;
  IUIAutomation2* uia2 = nullptr;
  if (SUCCEEDED(uia->QueryInterface(kIidIUIAutomation2,
                                    reinterpret_cast<void**>(&uia2))) &&
      uia2 != nullptr) {
    uia2->put_ConnectionTimeout(conn_ms);
    uia2->put_TransactionTimeout(tx_ms);
    uia2->Release();
  }
}

IUIAutomationCacheRequest* create_button_cache_request(IUIAutomation* uia) {
  if (uia == nullptr)
    return nullptr;
  IUIAutomationCacheRequest* cr = nullptr;
  if (FAILED(uia->CreateCacheRequest(&cr)) || cr == nullptr)
    return nullptr;
  cr->AddProperty(UIA_NamePropertyId);
  cr->AddProperty(UIA_AutomationIdPropertyId);
  cr->AddProperty(UIA_ClassNamePropertyId);
  return cr;
}

// Matches Explorer's internal classic 64-bit TRAYDATA payload.
struct TrayData64 {
  HWND hwnd;
  UINT uID;
  UINT uCallbackMessage;
  DWORD reserved[2];
  HICON hIcon;
};

struct ListenerEntry {
  SystrayChangedCallback cb;
  void* user_data;
};

struct UniqueHandle {
  HANDLE h = nullptr;
  ~UniqueHandle() {
    if (h != nullptr)
      CloseHandle(h);
  }
  UniqueHandle() = default;
  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;
};

struct RemoteBuffer {
  HANDLE proc = nullptr;
  void* ptr = nullptr;
  ~RemoteBuffer() {
    if (proc != nullptr && ptr != nullptr)
      VirtualFreeEx(proc, ptr, 0, MEM_RELEASE);
  }
  bool alloc(HANDLE process, SIZE_T size) {
    proc = process;
    ptr = VirtualAllocEx(process, nullptr, size,
                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return ptr != nullptr;
  }
};

struct ScopedHBitmap {
  HBITMAP bmp = nullptr;
  ~ScopedHBitmap() {
    if (bmp != nullptr)
      DeleteObject(bmp);
  }
};

std::string lower_ascii(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s)
    out.push_back(static_cast<char>(std::tolower(c)));
  return out;
}

std::string wide_to_utf8(const wchar_t* wstr) {
  if (wstr == nullptr || wstr[0] == L'\0')
    return {};
  int need = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
  if (need <= 1)
    return {};
  std::string out(static_cast<size_t>(need - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out.data(), need, nullptr, nullptr);
  return out;
}

std::string bstr_to_utf8_and_free(BSTR bstr) {
  if (bstr == nullptr)
    return {};
  std::string out = (bstr[0] != L'\0') ? wide_to_utf8(bstr) : std::string();
  SysFreeString(bstr);
  return out;
}

uint64_t fnv1a_hash(const void* data, size_t len, uint64_t seed = 14695981039346656037ULL) {
  const auto* p = static_cast<const uint8_t*>(data);
  uint64_t h = seed;
  for (size_t i = 0; i < len; ++i) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

bool contains_any(const std::string& haystack, const char* const* needles, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (haystack.find(needles[i]) != std::string::npos)
      return true;
  }
  return false;
}

// Convert HICON bitmap planes to packed 32bpp RGBA pixels.
bool hicon_to_rgba(HICON hicon,
                   int* out_w,
                   int* out_h,
                   std::vector<uint8_t>* out_rgba) {
  if (hicon == nullptr || out_w == nullptr || out_h == nullptr || out_rgba == nullptr)
    return false;

  ICONINFO ii{};
  if (!GetIconInfo(hicon, &ii))
    return false;
  ScopedHBitmap color_guard{ii.hbmColor};
  ScopedHBitmap mask_guard{ii.hbmMask};

  BITMAP bm{};
  HBITMAP target_bmp = ii.hbmColor != nullptr ? ii.hbmColor : ii.hbmMask;
  if (target_bmp == nullptr || GetObjectW(target_bmp, sizeof(bm), &bm) == 0)
    return false;

  int w = bm.bmWidth;
  int h = ii.hbmColor != nullptr ? bm.bmHeight : bm.bmHeight / 2;
  if (w <= 0 || h <= 0 || w > 128 || h > 128)
    return false;

  HDC screen_dc = GetDC(nullptr);
  if (screen_dc == nullptr)
    return false;

  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;  // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  const size_t pixel_count = static_cast<size_t>(w) * static_cast<size_t>(h);
  std::vector<uint8_t> bgra(pixel_count * 4);
  if (GetDIBits(screen_dc, target_bmp, 0, static_cast<UINT>(h),
                bgra.data(), &bi, DIB_RGB_COLORS) == 0) {
    ReleaseDC(nullptr, screen_dc);
    return false;
  }

  std::vector<uint8_t> mask_bgra;
  bool has_any_alpha = false;
  for (size_t i = 0; i < pixel_count; ++i) {
    if (bgra[i * 4 + 3] != 0) {
      has_any_alpha = true;
      break;
    }
  }
  if (!has_any_alpha && ii.hbmMask != nullptr && ii.hbmColor != nullptr) {
    mask_bgra.resize(pixel_count * 4);
    GetDIBits(screen_dc, ii.hbmMask, 0, static_cast<UINT>(h),
              mask_bgra.data(), &bi, DIB_RGB_COLORS);
  }
  ReleaseDC(nullptr, screen_dc);

  out_rgba->resize(pixel_count * 4);
  for (size_t i = 0; i < pixel_count; ++i) {
    uint8_t b = bgra[i * 4 + 0];
    uint8_t g = bgra[i * 4 + 1];
    uint8_t r = bgra[i * 4 + 2];
    uint8_t a = bgra[i * 4 + 3];
    if (!has_any_alpha) {
      if (!mask_bgra.empty()) {
        // Mask pixel is 0 for opaque, 255 for transparent.
        a = mask_bgra[i * 4] == 0 ? 255 : 0;
      } else {
        a = 255;
      }
    }
    (*out_rgba)[i * 4 + 0] = r;
    (*out_rgba)[i * 4 + 1] = g;
    (*out_rgba)[i * 4 + 2] = b;
    (*out_rgba)[i * 4 + 3] = a;
  }

  *out_w = w;
  *out_h = h;
  return true;
}

struct ClassicToolbarTarget {
  HWND hwnd = nullptr;
  TraySurface surface = TraySurface::ClassicToolbar;
};

void collect_classic_toolbars(std::vector<ClassicToolbarTarget>* out_bars) {
  auto find_descendant_toolbar = [](HWND root,
                                    TraySurface surf,
                                    std::vector<ClassicToolbarTarget>* list) {
    if (root == nullptr)
      return;
    struct Ctx {
      TraySurface surface;
      std::vector<ClassicToolbarTarget>* out;
    } ctx{surf, list};
    EnumChildWindows(
        root,
        [](HWND hw, LPARAM lp) -> BOOL {
          wchar_t cls[64] = {};
          if (GetClassNameW(hw, cls, 64) > 0 &&
              wcscmp(cls, L"ToolbarWindow32") == 0) {
            auto* c = reinterpret_cast<Ctx*>(lp);
            c->out->push_back(ClassicToolbarTarget{hw, c->surface});
          }
          return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
  };

  if (HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr)) {
    HWND notify = FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr);
    if (notify != nullptr)
      find_descendant_toolbar(notify, TraySurface::ClassicToolbar, out_bars);
  }
  if (HWND overflow = FindWindowW(L"NotifyIconOverflowWindow", nullptr)) {
    find_descendant_toolbar(overflow, TraySurface::ClassicOverflow, out_bars);
  }
}

void scan_classic_toolbar(HWND toolbar,
                          TraySurface surface,
                          std::vector<TrayIconEntry>* out) {
  DWORD pid = 0;
  GetWindowThreadProcessId(toolbar, &pid);
  if (pid == 0)
    return;

  UniqueHandle proc;
  proc.h = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ |
                           PROCESS_QUERY_LIMITED_INFORMATION,
                       FALSE, pid);
  if (proc.h == nullptr)
    return;

  BOOL is_wow64 = FALSE;
  if (IsWow64Process(proc.h, &is_wow64) && is_wow64)
    return;

  const LRESULT count = SendMessageW(toolbar, TB_BUTTONCOUNT, 0, 0);
  if (count <= 0 || count > 256)
    return;

  RemoteBuffer remote_tb;
  RemoteBuffer remote_text;
  if (!remote_tb.alloc(proc.h, sizeof(TBBUTTON)) ||
      !remote_text.alloc(proc.h, sizeof(wchar_t) * 256)) {
    return;
  }

  for (LRESULT i = 0; i < count; ++i) {
    if (!SendMessageW(toolbar, TB_GETBUTTON, static_cast<WPARAM>(i),
                      reinterpret_cast<LPARAM>(remote_tb.ptr))) {
      continue;
    }

    TBBUTTON tb{};
    if (!ReadProcessMemory(proc.h, remote_tb.ptr, &tb, sizeof(tb), nullptr))
      continue;
    if ((tb.fsState & TBSTATE_HIDDEN) != 0 || tb.dwData == 0)
      continue;

    TrayData64 td{};
    if (!ReadProcessMemory(proc.h, reinterpret_cast<LPCVOID>(tb.dwData),
                           &td, sizeof(td), nullptr)) {
      continue;
    }
    if (td.hwnd == nullptr || !IsWindow(td.hwnd))
      continue;

    std::string title;
    LRESULT text_len = SendMessageW(toolbar, TB_GETBUTTONTEXTW,
                                    static_cast<WPARAM>(tb.idCommand),
                                    reinterpret_cast<LPARAM>(remote_text.ptr));
    if (text_len > 0 && text_len < 256) {
      wchar_t buf[256] = {};
      if (ReadProcessMemory(proc.h, remote_text.ptr, buf, sizeof(buf), nullptr)) {
        buf[255] = L'\0';
        title = wide_to_utf8(buf);
      }
    }

    if (is_system_builtin_tray_item(title, ""))
      continue;

    TrayIconEntry entry;
    uint64_t key_parts[2] = {
        reinterpret_cast<uint64_t>(td.hwnd),
        static_cast<uint64_t>(td.uID)};
    entry.id = fnv1a_hash(key_parts, sizeof(key_parts));
    entry.title = title.empty() ? "Tray Application" : title;
    entry.surface = surface;
    entry.target_hwnd = reinterpret_cast<uintptr_t>(td.hwnd);
    entry.callback_msg = td.uCallbackMessage;
    entry.u_id = td.uID;
    entry.version = td.reserved[0];

    if (td.hIcon != nullptr)
      hicon_to_rgba(td.hIcon, &entry.icon_width, &entry.icon_height,
                    &entry.rgba_pixels);

    out->push_back(std::move(entry));
  }
}

std::string get_uia_string_prop(IUIAutomationElement* elem, PROPERTYID prop_id) {
  if (elem == nullptr)
    return {};
  VARIANT v;
  VariantInit(&v);
  std::string result;
  if (SUCCEEDED(elem->GetCurrentPropertyValue(prop_id, &v))) {
    if (v.vt == VT_BSTR && v.bstrVal != nullptr)
      result = wide_to_utf8(v.bstrVal);
  }
  VariantClear(&v);
  return result;
}

std::vector<int> get_uia_runtime_id(IUIAutomationElement* elem) {
  std::vector<int> ids;
  if (elem == nullptr)
    return ids;
  SAFEARRAY* sa = nullptr;
  if (SUCCEEDED(elem->GetRuntimeId(&sa)) && sa != nullptr) {
    LONG lb = 0, ub = -1;
    if (SUCCEEDED(SafeArrayGetLBound(sa, 1, &lb)) &&
        SUCCEEDED(SafeArrayGetUBound(sa, 1, &ub)) && ub >= lb) {
      ids.reserve(static_cast<size_t>(ub - lb + 1));
      for (LONG i = lb; i <= ub; ++i) {
        int val = 0;
        if (SUCCEEDED(SafeArrayGetElement(sa, &i, &val)))
          ids.push_back(val);
      }
    }
    SafeArrayDestroy(sa);
  }
  return ids;
}

constexpr IID kIidIUIAutomationTogglePattern = {
    0x94cf8058, 0x9b8d, 0x4ab9, {0x8b, 0xfd, 0x4c, 0xd0, 0xa3, 0x3c, 0x8c, 0x70}};

void read_uia_button_props(IUIAutomationElement* elem,
                           bool cached,
                           std::string* out_name,
                           std::string* out_aid,
                           std::string* out_cls) {
  if (elem == nullptr)
    return;
  if (cached) {
    BSTR bn = nullptr;
    BSTR ba = nullptr;
    BSTR bc = nullptr;
    if (SUCCEEDED(elem->get_CachedName(&bn)))
      *out_name = bstr_to_utf8_and_free(bn);
    if (SUCCEEDED(elem->get_CachedAutomationId(&ba)))
      *out_aid = bstr_to_utf8_and_free(ba);
    if (SUCCEEDED(elem->get_CachedClassName(&bc)))
      *out_cls = bstr_to_utf8_and_free(bc);
    return;
  }
  *out_name = get_uia_string_prop(elem, UIA_NamePropertyId);
  *out_aid = get_uia_string_prop(elem, UIA_AutomationIdPropertyId);
  *out_cls = get_uia_string_prop(elem, UIA_ClassNamePropertyId);
}

void collect_uia_target_hwnds(HWND root_hwnd, std::vector<HWND>* out) {
  if (root_hwnd == nullptr || out == nullptr)
    return;
  struct ChildBuckets {
    std::vector<HWND> bridges;
    std::vector<HWND> notify_wnds;
  } buckets;

  EnumChildWindows(
      root_hwnd,
      [](HWND child, LPARAM lp) -> BOOL {
        auto* b = reinterpret_cast<ChildBuckets*>(lp);
        wchar_t cls[128] = {};
        if (GetClassNameW(child, cls, 128) > 0) {
          if (wcscmp(cls, L"Windows.UI.Composition.DesktopWindowContentBridge") == 0) {
            b->bridges.push_back(child);
          } else if (wcscmp(cls, L"TrayNotifyWnd") == 0) {
            b->notify_wnds.push_back(child);
          }
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&buckets));

  // On Windows 11 XAML taskbar/overflow, targeting ONLY the XAML Island bridge(s)
  // avoids scanning all taskbar buttons, Win32 toolbars, and nested proxies 2-3x.
  if (!buckets.bridges.empty()) {
    *out = std::move(buckets.bridges);
    return;
  }
  if (!buckets.notify_wnds.empty()) {
    *out = std::move(buckets.notify_wnds);
    return;
  }
  out->push_back(root_hwnd);
}

bool is_overflow_chevron_button(const std::string& name_lower,
                                const std::string& aid_lower,
                                const std::string& cls_lower = {}) {
  return cls_lower.find("chevron") != std::string::npos ||
         cls_lower.find("overflowtoggle") != std::string::npos ||
         aid_lower.find("overflow") != std::string::npos ||
         aid_lower.find("showhidden") != std::string::npos ||
         aid_lower.find("chevron") != std::string::npos ||
         name_lower.find("show hidden") != std::string::npos ||
         name_lower.find("显示隐藏") != std::string::npos ||
         name_lower.find("系统托盘溢出") != std::string::npos;
}

std::wstring expand_known_folder_and_env(const std::wstring& raw) {
  std::wstring s = raw;
  if (raw.find(L'%') != std::wstring::npos) {
    DWORD need = ExpandEnvironmentStringsW(raw.c_str(), nullptr, 0);
    if (need > 0 && need <= 4096) {
      std::wstring buf(need, L'\0');
      DWORD wrote = ExpandEnvironmentStringsW(raw.c_str(), buf.data(), need);
      if (wrote > 0 && wrote <= need) {
        if (!buf.empty() && buf.back() == L'\0')
          buf.pop_back();
        s = std::move(buf);
      }
    }
  }
  if (s.size() >= 38 && s[0] == L'{' && s[37] == L'}') {
    GUID kf{};
    if (SUCCEEDED(CLSIDFromString(s.substr(0, 38).c_str(), &kf))) {
      PWSTR kf_path = nullptr;
      if (SUCCEEDED(SHGetKnownFolderPath(kf, KF_FLAG_DEFAULT, nullptr, &kf_path)) &&
          kf_path != nullptr) {
        std::wstring out(kf_path);
        CoTaskMemFree(kf_path);
        return out + s.substr(38);
      }
    }
  }
  return s;
}

struct CachedTrayIcon {
  int w = 0;
  int h = 0;
  std::vector<uint8_t> rgba;
  ULONGLONG fetched_tick = 0;
};

std::mutex g_icon_cache_mu;
std::unordered_map<std::string, CachedTrayIcon> g_icon_cache;
std::unordered_map<uint64_t, CachedTrayIcon> g_icon_cache_by_id;

void try_populate_uia_icon_from_registry(const std::string& title,
                                         TrayIconEntry* entry) {
  if (entry == nullptr || !entry->rgba_pixels.empty() || title.empty())
    return;

  const std::string title_low = lower_ascii(title);
  const ULONGLONG now = GetTickCount64();
  {
    std::lock_guard<std::mutex> lock(g_icon_cache_mu);
    if (entry->id != 0) {
      auto id_it = g_icon_cache_by_id.find(entry->id);
      if (id_it != g_icon_cache_by_id.end() &&
          (!id_it->second.rgba.empty() ||
           (now - id_it->second.fetched_tick) < 60000ULL)) {
        entry->icon_width = id_it->second.w;
        entry->icon_height = id_it->second.h;
        entry->rgba_pixels = id_it->second.rgba;
        return;
      }
    }
    auto it = g_icon_cache.find(title_low);
    if (it != g_icon_cache.end()) {
      if (!it->second.rgba.empty() || (now - it->second.fetched_tick) < 60000ULL) {
        entry->icon_width = it->second.w;
        entry->icon_height = it->second.h;
        entry->rgba_pixels = it->second.rgba;
        return;
      }
    }
  }

  HKEY root = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0,
                    KEY_READ, &root) != ERROR_SUCCESS) {
    return;
  }

  wchar_t subname[256];
  DWORD sublen = 256;
  std::wstring best_exe;
  int best_score = 0;

  for (DWORD idx = 0;; ++idx) {
    sublen = 256;
    if (RegEnumKeyExW(root, idx, subname, &sublen, nullptr, nullptr, nullptr,
                      nullptr) != ERROR_SUCCESS) {
      break;
    }
    HKEY sub = nullptr;
    if (RegOpenKeyExW(root, subname, 0, KEY_READ, &sub) != ERROR_SUCCESS)
      continue;

    wchar_t exe_buf[512] = {};
    DWORD exe_cb = sizeof(exe_buf);
    wchar_t tip_buf[512] = {};
    DWORD tip_cb = sizeof(tip_buf);
    RegQueryValueExW(sub, L"ExecutablePath", nullptr, nullptr,
                     reinterpret_cast<LPBYTE>(exe_buf), &exe_cb);
    RegQueryValueExW(sub, L"InitialTooltip", nullptr, nullptr,
                     reinterpret_cast<LPBYTE>(tip_buf), &tip_cb);
    RegCloseKey(sub);

    if (exe_buf[0] == L'\0')
      continue;

    const std::string tip_low = lower_ascii(wide_to_utf8(tip_buf));
    std::wstring expanded_exe = expand_known_folder_and_env(exe_buf);
    const std::string exe_utf8 = lower_ascii(wide_to_utf8(expanded_exe.c_str()));
    size_t slash = exe_utf8.find_last_of("\\/");
    size_t dot = exe_utf8.find_last_of('.');
    std::string stem;
    if (slash != std::string::npos) {
      stem = (dot != std::string::npos && dot > slash)
                 ? exe_utf8.substr(slash + 1, dot - slash - 1)
                 : exe_utf8.substr(slash + 1);
    }

    int score = 0;
    if (!tip_low.empty()) {
      if (tip_low == title_low)
        score = 100;
      else if (title_low.find(tip_low) != std::string::npos ||
               tip_low.find(title_low) != std::string::npos)
        score = 70;
    }
    if (score == 0 && stem.size() >= 3 &&
        title_low.find(stem) != std::string::npos) {
      score = 50;
    }
    if (score > best_score &&
        GetFileAttributesW(expanded_exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
      best_score = score;
      best_exe = std::move(expanded_exe);
      if (score == 100)
        break;
    }
  }
  RegCloseKey(root);

  if (!best_exe.empty()) {
    SHFILEINFOW sfi{};
    if (SHGetFileInfoW(best_exe.c_str(), 0, &sfi, sizeof(sfi),
                       SHGFI_ICON | SHGFI_LARGEICON) &&
        sfi.hIcon != nullptr) {
      hicon_to_rgba(sfi.hIcon, &entry->icon_width, &entry->icon_height,
                    &entry->rgba_pixels);
      DestroyIcon(sfi.hIcon);
    }
  }

  CachedTrayIcon cached;
  cached.w = entry->icon_width;
  cached.h = entry->icon_height;
  cached.rgba = entry->rgba_pixels;
  cached.fetched_tick = now;
  std::lock_guard<std::mutex> lock(g_icon_cache_mu);
  if (g_icon_cache.size() > 128)
    g_icon_cache.clear();
  if (g_icon_cache_by_id.size() > 128)
    g_icon_cache_by_id.clear();
  if (entry->id != 0)
    g_icon_cache_by_id[entry->id] = cached;
  g_icon_cache[title_low] = std::move(cached);
}

std::string clean_tray_display_title(const std::string& raw) {
  std::string tmp;
  tmp.reserve(raw.size());
  bool prev_space = false;
  for (char c : raw) {
    if (c == '\r' || c == '\n' || c == '\t') {
      if (!prev_space && !tmp.empty()) {
        tmp.push_back(' ');
        prev_space = true;
      }
      continue;
    }
    if (c == ' ') {
      if (!prev_space && !tmp.empty()) {
        tmp.push_back(' ');
        prev_space = true;
      }
      continue;
    }
    tmp.push_back(c);
    prev_space = false;
  }
  while (!tmp.empty() && tmp.back() == ' ')
    tmp.pop_back();
  return tmp;
}

struct UiaProbeRow {
  long ct = UIA_ButtonControlTypeId;
  std::string name;
  std::string aid;
  std::string cls;
  TraySurface surface = TraySurface::Unknown;
};

template <typename Callback>
bool for_each_uia_button_cached(IUIAutomation* uia,
                                HWND hwnd,
                                Callback&& visitor) {
  if (uia == nullptr || hwnd == nullptr)
    return false;

  IUIAutomationElement* root = nullptr;
  if (FAILED(uia->ElementFromHandle(hwnd, &root)) || root == nullptr)
    return false;

  VARIANT vtype;
  VariantInit(&vtype);
  vtype.vt = VT_I4;
  vtype.lVal = UIA_ButtonControlTypeId;

  IUIAutomationCondition* cond = nullptr;
  bool keep_going = true;
  if (SUCCEEDED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, vtype, &cond)) &&
      cond != nullptr) {
    IUIAutomationCacheRequest* cache_req = create_button_cache_request(uia);
    IUIAutomationElementArray* arr = nullptr;
    const bool use_cache = (cache_req != nullptr);
    HRESULT fhr = use_cache
                      ? root->FindAllBuildCache(TreeScope_Descendants, cond,
                                                cache_req, &arr)
                      : root->FindAll(TreeScope_Descendants, cond, &arr);
    if (SUCCEEDED(fhr) && arr != nullptr) {
      int len = 0;
      arr->get_Length(&len);
      for (int i = 0; i < len && i < 128 && keep_going; ++i) {
        IUIAutomationElement* el = nullptr;
        if (FAILED(arr->GetElement(i, &el)) || el == nullptr)
          continue;

        std::string name;
        std::string aid;
        std::string cls;
        read_uia_button_props(el, use_cache, &name, &aid, &cls);
        keep_going = visitor(el, name, aid, cls);
        el->Release();
      }
      arr->Release();
    }
    if (cache_req != nullptr)
      cache_req->Release();
    cond->Release();
  }
  VariantClear(&vtype);
  root->Release();
  return !keep_going;
}

void scan_uia_single_hwnd(IUIAutomation* uia,
                          HWND hwnd,
                          TraySurface surface,
                          bool in_overflow_window,
                          std::vector<TrayIconEntry>* out,
                          std::vector<UiaProbeRow>* probe_out) {
  for_each_uia_button_cached(
      uia, hwnd,
      [&](IUIAutomationElement* el, const std::string& name,
          const std::string& auto_id, const std::string& cls) {
        if (probe_out != nullptr && probe_out->size() < 64 &&
            (!name.empty() || !auto_id.empty())) {
          probe_out->push_back(
              UiaProbeRow{UIA_ButtonControlTypeId, name, auto_id, cls, surface});
        }

        const std::string cls_low = lower_ascii(cls);
        const std::string name_low = lower_ascii(name);
        const std::string aid_low = lower_ascii(auto_id);

        const bool is_builtin_cls =
            cls_low.find("tasklistbutton") != std::string::npos ||
            cls_low.find("togglebutton") != std::string::npos ||
            cls_low.find("clockbutton") != std::string::npos ||
            cls_low.find("imebutton") != std::string::npos ||
            cls_low.find("notificationbutton") != std::string::npos ||
            cls_low.find("showdesktop") != std::string::npos ||
            cls_low.find("inputindicator") != std::string::npos ||
            cls_low.find("accentbutton") != std::string::npos ||
            cls_low.find("omnibutton") != std::string::npos ||
            is_overflow_chevron_button(name_low, aid_low, cls_low);

        const bool by_cls_notify =
            cls_low.find("notifyicon") != std::string::npos;

        if (!is_builtin_cls &&
            is_uia_tray_candidate(name, auto_id, in_overflow_window || by_cls_notify)) {
          std::vector<int> rid = get_uia_runtime_id(el);
          TrayIconEntry entry;
          std::string clean_name = clean_tray_display_title(name);
          entry.title = clean_name.empty() ? name : std::move(clean_name);
          entry.app_hint = auto_id;
          entry.is_uia = true;
          entry.surface = surface;
          entry.uia_runtime_id = std::move(rid);
          if (!entry.uia_runtime_id.empty()) {
            entry.id = fnv1a_hash(
                entry.uia_runtime_id.data(),
                entry.uia_runtime_id.size() * sizeof(int));
          } else {
            entry.id = fnv1a_hash(entry.title.data(), entry.title.size());
          }
          try_populate_uia_icon_from_registry(entry.title, &entry);
          out->push_back(std::move(entry));
        }
        return true;
      });
}

void scan_uia_root(IUIAutomation* uia,
                   HWND hwnd,
                   TraySurface surface,
                   bool in_overflow_window,
                   std::vector<TrayIconEntry>* out,
                   std::vector<UiaProbeRow>* probe_out) {
  std::vector<HWND> targets;
  collect_uia_target_hwnds(hwnd, &targets);
  for (HWND th : targets)
    scan_uia_single_hwnd(uia, th, surface, in_overflow_window, out, probe_out);
}

bool send_physical_mouse_click(int x, int y, bool right_click) {
  SetCursorPos(x, y);
  INPUT inputs[2]{};
  inputs[0].type = INPUT_MOUSE;
  inputs[0].mi.dwFlags =
      right_click ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
  inputs[1].type = INPUT_MOUSE;
  inputs[1].mi.dwFlags =
      right_click ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
  return SendInput(2, inputs, sizeof(INPUT)) == 2;
}

constexpr IID kIidIUIAutomationElement3 = {
    0x8471df34, 0xaee0, 0x4a01, {0xa7, 0xde, 0x7d, 0xb9, 0xaf, 0x12, 0xc2, 0x96}};

bool send_shift_f10_shortcut() {
  INPUT keys[4]{};
  keys[0].type = INPUT_KEYBOARD;
  keys[0].ki.wVk = VK_SHIFT;
  keys[1].type = INPUT_KEYBOARD;
  keys[1].ki.wVk = VK_F10;
  keys[2].type = INPUT_KEYBOARD;
  keys[2].ki.wVk = VK_F10;
  keys[2].ki.dwFlags = KEYEVENTF_KEYUP;
  keys[3].type = INPUT_KEYBOARD;
  keys[3].ki.wVk = VK_SHIFT;
  keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
  return SendInput(4, keys, sizeof(INPUT)) == 4;
}

bool trigger_uia_element(IUIAutomationElement* el,
                         HWND root_hwnd,
                         bool right_click) {
  if (el == nullptr)
    return false;

  if (right_click) {
    const auto t0 = std::chrono::steady_clock::now();
    if (root_hwnd != nullptr && IsWindowVisible(root_hwnd))
      SetForegroundWindow(root_hwnd);

    // Primary Win11 XAML UIA context menu path: IUIAutomationElement3::ShowContextMenu().
    IUIAutomationElement3* el3 = nullptr;
    HRESULT qi_hr = el->QueryInterface(kIidIUIAutomationElement3,
                                       reinterpret_cast<void**>(&el3));
    if (SUCCEEDED(qi_hr) && el3 != nullptr) {
      HRESULT sc_hr = el3->ShowContextMenu();
      el3->Release();
      g_print("[systray] UIA ShowContextMenu hr=0x%08lx elapsed=%.1fms\n",
              static_cast<unsigned long>(sc_hr), qpc_ms_since(t0));
      if (SUCCEEDED(sc_hr))
        return true;
    }

    // Fallback 1: Focus element cleanly and send Shift+F10.
    if (SUCCEEDED(el->SetFocus())) {
      Sleep(20);
      if (send_shift_f10_shortcut()) {
        g_print("[systray] UIA SetFocus+Shift+F10 elapsed=%.1fms\n",
                qpc_ms_since(t0));
        return true;
      }
    }

    // Fallback 2: Real pointer right-click on temporarily uncloaked target.
    RECT rc{};
    if (SUCCEEDED(el->get_CurrentBoundingRectangle(&rc)) &&
        rc.right > rc.left && rc.bottom > rc.top) {
      const int cx = (rc.left + rc.right) / 2;
      const int cy = (rc.top + rc.bottom) / 2;
      NativeTaskbar::set_primary_cloaked(false);
      if (root_hwnd != nullptr && IsWindowVisible(root_hwnd))
        SetForegroundWindow(root_hwnd);
      Sleep(20);
      const bool clicked = send_physical_mouse_click(cx, cy, true);
      g_print("[systray] physical right-click (%d,%d) dispatched=%d elapsed=%.1fms\n",
              cx, cy, clicked ? 1 : 0, qpc_ms_since(t0));
      return clicked;
    }
    return false;
  }

  bool invoked = false;
  IUIAutomationInvokePattern* inv = nullptr;
  if (SUCCEEDED(el->GetCurrentPatternAs(
          UIA_InvokePatternId, kIidIUIAutomationInvokePattern,
          reinterpret_cast<void**>(&inv))) &&
      inv != nullptr) {
    invoked = SUCCEEDED(inv->Invoke());
    inv->Release();
  }
  if (!invoked) {
    IUIAutomationTogglePattern* tog = nullptr;
    if (SUCCEEDED(el->GetCurrentPatternAs(
            UIA_TogglePatternId, kIidIUIAutomationTogglePattern,
            reinterpret_cast<void**>(&tog))) &&
        tog != nullptr) {
      invoked = SUCCEEDED(tog->Toggle());
      tog->Release();
    }
  }
  if (!invoked) {
    IUIAutomationLegacyIAccessiblePattern* leg = nullptr;
    if (SUCCEEDED(el->GetCurrentPatternAs(
            UIA_LegacyIAccessiblePatternId,
            kIidIUIAutomationLegacyIAccessiblePattern,
            reinterpret_cast<void**>(&leg))) &&
        leg != nullptr) {
      invoked = SUCCEEDED(leg->DoDefaultAction());
      leg->Release();
    }
  }
  if (!invoked) {
    RECT rc{};
    if (SUCCEEDED(el->get_CurrentBoundingRectangle(&rc)) &&
        rc.right > rc.left && rc.bottom > rc.top &&
        root_hwnd != nullptr && IsWindowVisible(root_hwnd)) {
      invoked = send_physical_mouse_click((rc.left + rc.right) / 2,
                                          (rc.top + rc.bottom) / 2, false);
    }
  }
  return invoked;
}

bool invoke_uia_button(IUIAutomation* uia,
                       HWND root_hwnd,
                       const std::vector<int>& runtime_id,
                       const std::string& fallback_title,
                       bool match_overflow_chevron,
                       bool right_click) {
  if (uia == nullptr || root_hwnd == nullptr)
    return false;

  std::vector<HWND> targets;
  collect_uia_target_hwnds(root_hwnd, &targets);

  bool invoked = false;
  for (HWND th : targets) {
    if (invoked)
      break;
    for_each_uia_button_cached(
        uia, th,
        [&](IUIAutomationElement* el, const std::string& raw_name,
            const std::string& raw_aid, const std::string& raw_cls) {
          bool matched = false;
          if (match_overflow_chevron) {
            matched = is_overflow_chevron_button(
                lower_ascii(raw_name), lower_ascii(raw_aid), lower_ascii(raw_cls));
          } else {
            if (!fallback_title.empty() &&
                (raw_name == fallback_title ||
                 clean_tray_display_title(raw_name) == fallback_title)) {
              matched = true;
            } else if (!runtime_id.empty()) {
              // Skip expensive cross-process GetRuntimeId() on taskbar/builtin buttons.
              const std::string aid_low = lower_ascii(raw_aid);
              const bool maybe_tray =
                  aid_low != "systemtrayicon" &&
                  aid_low != "startbutton" &&
                  aid_low.rfind("appid:", 0) != 0;
              if (maybe_tray && get_uia_runtime_id(el) == runtime_id)
                matched = true;
            }
          }

          if (matched)
            invoked = trigger_uia_element(el, root_hwnd, right_click);
          return !invoked;
        });
  }
  return invoked;
}

bool snapshot_equals(const std::vector<TrayIconEntry>& a,
                     const std::vector<TrayIconEntry>& b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].id != b[i].id || a[i].title != b[i].title ||
        a[i].icon_width != b[i].icon_width ||
        a[i].icon_height != b[i].icon_height ||
        a[i].rgba_pixels != b[i].rgba_pixels) {
      return false;
    }
  }
  return true;
}

struct PendingUiaAction {
  std::vector<int> runtime_id;
  std::string title;
  TraySurface surface = TraySurface::Unknown;
  bool right_click = false;
  std::chrono::steady_clock::time_point enqueued_at{};
};

struct SystrayState {
  std::mutex mu;
  std::mutex shell_tray_mu;
  std::condition_variable scan_cv;
  std::condition_variable action_cv;
  std::atomic<int> action_in_flight{0};
  bool stop = false;
  bool started = false;
  bool scan_pending = false;
  bool force_probe = false;
  bool toggle_overflow_pending = false;
  std::vector<PendingUiaAction> uia_actions;
  std::thread scan_worker;
  std::thread action_worker;
  std::vector<TrayIconEntry> items;
  std::vector<TrayIconEntry> last_xo_items;
  std::vector<UiaProbeRow> last_uia_probe;
  std::vector<ListenerEntry> listeners;
  std::chrono::steady_clock::time_point last_scan{};
  std::chrono::steady_clock::time_point last_xo_probe{};
  double last_scan_ms = 0.0;
  uint64_t scan_count = 0;
  double last_action_ms = 0.0;
  uint64_t action_count = 0;
  std::string last_action_info;
};

SystrayState g_tray;

const char* tray_surface_label(TraySurface s) {
  switch (s) {
    case TraySurface::ClassicToolbar:  return "classic_toolbar";
    case TraySurface::ShellTray:       return "shell_tray";
    case TraySurface::XamlOverflow:    return "xaml_overflow";
    case TraySurface::ClassicOverflow: return "classic_overflow";
    default:                           return "unknown";
  }
}

gboolean idle_notify_tray_listeners(gpointer G_GNUC_UNUSED data) {
  std::vector<ListenerEntry> copy;
  {
    std::lock_guard<std::mutex> lock(g_tray.mu);
    copy = g_tray.listeners;
  }
  for (const auto& l : copy) {
    if (l.cb != nullptr)
      l.cb(l.user_data);
  }
  return G_SOURCE_REMOVE;
}

struct ShellTrayCloakGuardState {
  bool was_hidden = false;
  RECT orig_rect{};
  bool moved_onscreen = false;
};

bool ensure_shell_tray_accessible_cloaked(HWND shell_tray,
                                          ShellTrayCloakGuardState* st,
                                          DWORD settle_ms = 25) {
  if (shell_tray == nullptr)
    return false;
  const bool hidden = (IsWindowVisible(shell_tray) == FALSE);
  if (st != nullptr) {
    st->was_hidden = hidden;
    GetWindowRect(shell_tray, &st->orig_rect);
    st->moved_onscreen = false;
  }

  NativeTaskbar::suspend_keeper_ms(2000);
  NativeTaskbar::set_primary_cloaked(true);

  if (hidden)
    ShowWindowAsync(shell_tray, SW_SHOWNOACTIVATE);

  // If Explorer parked Shell_TrayWnd off-screen (e.g. ABS_AUTOHIDE bottom strip
  // y >= mon.bottom - 8), temporarily move it back onscreen while DWM-cloaked so
  // XAML buttons & TopLevelWindowForOverflowXamlIsland lay out inside screen bounds.
  HMONITOR mon = MonitorFromWindow(shell_tray, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  RECT rc{};
  if (GetMonitorInfoW(mon, &mi) && GetWindowRect(shell_tray, &rc)) {
    const int bar_h = std::max(36L, rc.bottom - rc.top);
    const int bar_w = std::max(400L, mi.rcMonitor.right - mi.rcMonitor.left);
    if (rc.top >= mi.rcMonitor.bottom - 8) {
      SetWindowPos(shell_tray, HWND_TOPMOST, mi.rcMonitor.left,
                   mi.rcMonitor.bottom - bar_h, bar_w, bar_h,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
      if (st != nullptr)
        st->moved_onscreen = true;
    }
  }
  const DWORD wait_ms =
      (st != nullptr && (st->was_hidden || st->moved_onscreen)) ? settle_ms : 0;
  if (wait_ms > 0)
    Sleep(wait_ms);
  return true;
}

void restore_shell_tray_if_hidden(HWND shell_tray,
                                  const ShellTrayCloakGuardState& st) {
  if (shell_tray == nullptr)
    return;
  if (st.moved_onscreen && st.orig_rect.right > st.orig_rect.left) {
    SetWindowPos(shell_tray, nullptr, st.orig_rect.left, st.orig_rect.top,
                 st.orig_rect.right - st.orig_rect.left,
                 st.orig_rect.bottom - st.orig_rect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
  }
  if (st.was_hidden) {
    ShowWindowAsync(shell_tray, SW_HIDE);
    Sleep(15);
  }
  NativeTaskbar::set_primary_cloaked(false);
  NativeTaskbar::suspend_keeper_ms(0);
}

bool has_xaml_tray_bridge(HWND shell_tray) {
  if (shell_tray == nullptr)
    return false;
  bool found = false;
  EnumChildWindows(
      shell_tray,
      [](HWND child, LPARAM lp) -> BOOL {
        wchar_t cls[128] = {};
        if (GetClassNameW(child, cls, 128) > 0 &&
            wcscmp(cls, L"Windows.UI.Composition.DesktopWindowContentBridge") == 0) {
          *reinterpret_cast<bool*>(lp) = true;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&found));
  return found;
}

bool toggle_native_overflow_impl(IUIAutomation* uia) {
  HWND xaml_overflow = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr);
  HWND classic_overflow = FindWindowW(L"NotifyIconOverflowWindow", nullptr);

  if (xaml_overflow != nullptr && IsWindowVisible(xaml_overflow)) {
    g_print("[systray] closing visible XAML overflow\n");
    PostMessageW(xaml_overflow, WM_KEYDOWN, VK_ESCAPE, 0);
    PostMessageW(xaml_overflow, WM_KEYUP, VK_ESCAPE, 0);
    NativeTaskbar::suspend_keeper_ms(0);
    return true;
  }
  if (classic_overflow != nullptr && IsWindowVisible(classic_overflow)) {
    g_print("[systray] closing visible classic overflow\n");
    ShowWindowAsync(classic_overflow, SW_HIDE);
    NativeTaskbar::suspend_keeper_ms(0);
    return true;
  }

  HWND shell_tray = FindWindowW(L"Shell_TrayWnd", nullptr);
  if (shell_tray == nullptr) {
    g_print("[systray] toggle_native_overflow: Shell_TrayWnd not found\n");
    NativeTaskbar::suspend_keeper_ms(0);
    return false;
  }

  const bool is_win11_xaml =
      (xaml_overflow != nullptr) || has_xaml_tray_bridge(shell_tray);
  if (!is_win11_xaml && classic_overflow != nullptr) {
    NativeTaskbar::suspend_keeper_ms(4000);
    ShowWindowAsync(classic_overflow, SW_SHOWNORMAL);
    SetForegroundWindow(classic_overflow);
    g_print("[systray] opened classic NotifyIconOverflowWindow\n");
    return true;
  }

  std::lock_guard<std::mutex> shell_lock(g_tray.shell_tray_mu);
  ShellTrayCloakGuardState st;
  ensure_shell_tray_accessible_cloaked(shell_tray, &st, 25);
  SetForegroundWindow(shell_tray);
  Sleep(15);

  bool opened = false;
  if (uia != nullptr && invoke_uia_button(uia, shell_tray, {}, "", true, false)) {
    for (int wait_step = 0; wait_step < 10; ++wait_step) {
      Sleep(25);
      if (NativeTaskbar::is_tray_overflow_open()) {
        opened = true;
        break;
      }
    }
  }
  g_print("[systray] UIA chevron invoke opened=%d\n", opened ? 1 : 0);

  if (!opened && uia != nullptr) {
    // Fallback: locate chevron rectangle while cloaked & send pointer click.
    POINT saved_pt{};
    GetCursorPos(&saved_pt);
    std::vector<HWND> targets;
    collect_uia_target_hwnds(shell_tray, &targets);
    bool clicked_rect = false;
    for (HWND th : targets) {
      if (clicked_rect)
        break;
      for_each_uia_button_cached(
          uia, th,
          [&](IUIAutomationElement* el, const std::string& nm,
              const std::string& aid, const std::string& cls) {
            if (is_overflow_chevron_button(lower_ascii(nm), lower_ascii(aid),
                                           lower_ascii(cls))) {
              RECT r{};
              if (SUCCEEDED(el->get_CurrentBoundingRectangle(&r)) &&
                  r.right > r.left && r.bottom > r.top) {
                clicked_rect = send_physical_mouse_click(
                    (r.left + r.right) / 2, (r.top + r.bottom) / 2, false);
              }
            }
            return !clicked_rect;
          });
    }
    SetCursorPos(saved_pt.x, saved_pt.y);
    if (clicked_rect) {
      for (int wait_step = 0; wait_step < 8; ++wait_step) {
        Sleep(25);
        if (NativeTaskbar::is_tray_overflow_open()) {
          opened = true;
          break;
        }
      }
      g_print("[systray] chevron physical click opened=%d\n", opened ? 1 : 0);
    }
  }

  if (!opened) {
    // Win+B focuses system tray overflow chevron only if taskbar receives focus.
    SetForegroundWindow(shell_tray);
    Sleep(30);
    if (GetForegroundWindow() == shell_tray) {
      INPUT keys[4]{};
      keys[0].type = INPUT_KEYBOARD;
      keys[0].ki.wVk = VK_LWIN;
      keys[1].type = INPUT_KEYBOARD;
      keys[1].ki.wVk = 'B';
      keys[2].type = INPUT_KEYBOARD;
      keys[2].ki.wVk = 'B';
      keys[2].ki.dwFlags = KEYEVENTF_KEYUP;
      keys[3].type = INPUT_KEYBOARD;
      keys[3].ki.wVk = VK_LWIN;
      keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
      SendInput(4, keys, sizeof(INPUT));
      Sleep(50);
      if (GetForegroundWindow() == shell_tray) {
        INPUT sp[2]{};
        sp[0].type = INPUT_KEYBOARD;
        sp[0].ki.wVk = VK_SPACE;
        sp[1].type = INPUT_KEYBOARD;
        sp[1].ki.wVk = VK_SPACE;
        sp[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, sp, sizeof(INPUT));
        Sleep(60);
      }
      opened = NativeTaskbar::is_tray_overflow_open();
      g_print("[systray] Win+B+Space fallback opened=%d\n", opened ? 1 : 0);
    }
  }

  if (opened) {
    if (HWND xo = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr)) {
      if (IsWindowVisible(xo))
        SetForegroundWindow(xo);
    }
  } else {
    restore_shell_tray_if_hidden(shell_tray, st);
  }
  return opened;
}

void record_action_telemetry(const PendingUiaAction& act,
                             bool ok,
                             double exec_ms,
                             const char* path_tag) {
  const double total_from_enqueue =
      (act.enqueued_at.time_since_epoch().count() != 0)
          ? qpc_ms_since(act.enqueued_at)
          : exec_ms;
  char msg[256];
  std::snprintf(msg, sizeof(msg),
                "ok=%d right=%d surf=%s path=%s exec=%.1fms total=%.1fms title=%s",
                ok ? 1 : 0, act.right_click ? 1 : 0,
                tray_surface_label(act.surface),
                path_tag ? path_tag : "",
                exec_ms, total_from_enqueue, act.title.c_str());
  g_print("[systray] action %s\n", msg);

  std::lock_guard<std::mutex> lock(g_tray.mu);
  ++g_tray.action_count;
  g_tray.last_action_ms = total_from_enqueue;
  g_tray.last_action_info = msg;
}

void activate_uia_item_impl(IUIAutomation* uia, const PendingUiaAction& act) {
  if (uia == nullptr)
    return;
  const auto act_t0 = std::chrono::steady_clock::now();

  auto try_open_overflow = [&]() -> bool {
    if (HWND xo = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr)) {
      if (IsWindowVisible(xo) &&
          invoke_uia_button(uia, xo, act.runtime_id, act.title, false,
                            act.right_click)) {
        record_action_telemetry(act, true, qpc_ms_since(act_t0), "xo-visible");
        return true;
      }
    }
    if (HWND co = FindWindowW(L"NotifyIconOverflowWindow", nullptr)) {
      if (IsWindowVisible(co) &&
          invoke_uia_button(uia, co, act.runtime_id, act.title, false,
                            act.right_click)) {
        record_action_telemetry(act, true, qpc_ms_since(act_t0), "co-visible");
        return true;
      }
    }
    return false;
  };

  auto open_and_invoke_overflow = [&]() -> bool {
    if (toggle_native_overflow_impl(uia)) {
      for (int i = 0; i < 10; ++i) {
        if (HWND xo = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr)) {
          if (IsWindowVisible(xo) &&
              invoke_uia_button(uia, xo, act.runtime_id, act.title, false,
                                act.right_click)) {
            record_action_telemetry(act, true, qpc_ms_since(act_t0), "xo-opened");
            return true;
          }
        }
        if (HWND co = FindWindowW(L"NotifyIconOverflowWindow", nullptr)) {
          if (IsWindowVisible(co) &&
              invoke_uia_button(uia, co, act.runtime_id, act.title, false,
                                act.right_click)) {
            record_action_telemetry(act, true, qpc_ms_since(act_t0), "co-opened");
            return true;
          }
        }
        Sleep(25);
      }
    }
    return false;
  };

  if (try_open_overflow())
    return;

  // Overflow-resident icons skip scanning Shell_TrayWnd and go straight to overflow.
  if (act.surface == TraySurface::XamlOverflow ||
      act.surface == TraySurface::ClassicOverflow) {
    if (!open_and_invoke_overflow())
      record_action_telemetry(act, false, qpc_ms_since(act_t0), "xo-miss");
    return;
  }

  HWND shell_tray = FindWindowW(L"Shell_TrayWnd", nullptr);
  if (shell_tray != nullptr) {
    bool hit = false;
    {
      std::lock_guard<std::mutex> shell_lock(g_tray.shell_tray_mu);
      ShellTrayCloakGuardState st;
      ensure_shell_tray_accessible_cloaked(shell_tray, &st, 25);
      hit = invoke_uia_button(uia, shell_tray, act.runtime_id, act.title,
                              false, act.right_click);
      if (hit) {
        if (!act.right_click) {
          restore_shell_tray_if_hidden(shell_tray, st);
        } else {
          // Allow brief window for Explorer/app to dispatch context menu, while
          // NativeTaskbar::is_tray_overflow_open() holds keeper if #32768 opens.
          NativeTaskbar::suspend_keeper_ms(1500);
        }
      } else {
        restore_shell_tray_if_hidden(shell_tray, st);
      }
    }
    if (hit) {
      record_action_telemetry(act, true, qpc_ms_since(act_t0), "shell-tray");
      return;
    }
  }

  if (open_and_invoke_overflow())
    return;

  record_action_telemetry(act, false, qpc_ms_since(act_t0), "miss");
}

std::vector<TrayIconEntry> collect_tray_snapshot(
    IUIAutomation* uia,
    bool allow_cloak_probe,
    std::vector<UiaProbeRow>* out_probe,
    double* out_ms) {
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<TrayIconEntry> items;
  std::vector<UiaProbeRow> probes;

  std::vector<ClassicToolbarTarget> toolbars;
  collect_classic_toolbars(&toolbars);
  for (const auto& tb : toolbars)
    scan_classic_toolbar(tb.hwnd, tb.surface, &items);

  if (uia != nullptr) {
    if (HWND xaml_overflow =
            FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr)) {
      const size_t before_xo = items.size();
      const bool xo_visible = (IsWindowVisible(xaml_overflow) != FALSE);
      scan_uia_root(uia, xaml_overflow, TraySurface::XamlOverflow, true,
                    &items, &probes);

      if (items.size() == before_xo && allow_cloak_probe && !xo_visible &&
          g_tray.action_in_flight.load(std::memory_order_acquire) == 0) {
        std::lock_guard<std::mutex> shell_lock(g_tray.shell_tray_mu);
        BOOL cloak = TRUE;
        DwmSetWindowAttribute(xaml_overflow, DWMWA_CLOAK, &cloak, sizeof(cloak));
        ShowWindowAsync(xaml_overflow, SW_SHOWNOACTIVATE);
        Sleep(35);
        scan_uia_root(uia, xaml_overflow, TraySurface::XamlOverflow, true,
                      &items, &probes);
        ShowWindowAsync(xaml_overflow, SW_HIDE);
        Sleep(15);
        cloak = FALSE;
        DwmSetWindowAttribute(xaml_overflow, DWMWA_CLOAK, &cloak, sizeof(cloak));
      }

      if (items.size() > before_xo) {
        std::vector<TrayIconEntry> xo_only(items.begin() + before_xo, items.end());
        std::lock_guard<std::mutex> lock(g_tray.mu);
        g_tray.last_xo_items = std::move(xo_only);
        g_tray.last_xo_probe = std::chrono::steady_clock::now();
      } else if (allow_cloak_probe || xo_visible) {
        std::lock_guard<std::mutex> lock(g_tray.mu);
        g_tray.last_xo_items.clear();
        g_tray.last_xo_probe = std::chrono::steady_clock::now();
      } else if (!xo_visible && !allow_cloak_probe) {
        // Keep last known overflow items when TopLevelWindowForOverflowXamlIsland
        // stays hidden on routine scans so menu entries do not flicker out.
        std::lock_guard<std::mutex> lock(g_tray.mu);
        for (const auto& prev_xo : g_tray.last_xo_items)
          items.push_back(prev_xo);
      }
    }

    if (HWND classic_overflow =
            FindWindowW(L"NotifyIconOverflowWindow", nullptr)) {
      scan_uia_root(uia, classic_overflow, TraySurface::ClassicOverflow, true,
                    &items, &probes);
    }

    if (HWND shell_tray = FindWindowW(L"Shell_TrayWnd", nullptr)) {
      const size_t before = items.size();
      scan_uia_root(uia, shell_tray, TraySurface::ShellTray, false, &items,
                    &probes);
      if (items.size() == before && allow_cloak_probe &&
          g_tray.action_in_flight.load(std::memory_order_acquire) == 0) {
        std::lock_guard<std::mutex> shell_lock(g_tray.shell_tray_mu);
        ShellTrayCloakGuardState st;
        ensure_shell_tray_accessible_cloaked(shell_tray, &st, 25);
        scan_uia_root(uia, shell_tray, TraySurface::ShellTray, false, &items,
                      &probes);
        restore_shell_tray_if_hidden(shell_tray, st);
      }
    }
  }

  // Deduplicate by stable entry id.
  std::vector<TrayIconEntry> unique;
  unique.reserve(items.size());
  for (auto& item : items) {
    bool dup = false;
    for (const auto& existing : unique) {
      if (existing.id == item.id) {
        dup = true;
        break;
      }
    }
    if (!dup)
      unique.push_back(std::move(item));
  }

  if (out_probe != nullptr)
    *out_probe = std::move(probes);
  if (out_ms != nullptr)
    *out_ms = qpc_ms_since(t0);
  return unique;
}

IUIAutomation* create_configured_uia(bool com_ok) {
  if (!com_ok)
    return nullptr;
  IUIAutomation* uia = nullptr;
  if (SUCCEEDED(CoCreateInstance(kClsidCUIAutomation, nullptr,
                                 CLSCTX_INPROC_SERVER, kIidIUIAutomation,
                                 reinterpret_cast<void**>(&uia))) &&
      uia != nullptr) {
    configure_uia_timeouts(uia, 800, 1500);
  }
  return uia;
}

void action_worker_main() {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool com_ok = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
  IUIAutomation* uia = create_configured_uia(com_ok);

  struct InFlightGuard {
    std::atomic<int>& counter;
    explicit InFlightGuard(std::atomic<int>& c) : counter(c) {
      counter.fetch_add(1, std::memory_order_acq_rel);
    }
    ~InFlightGuard() {
      counter.fetch_sub(1, std::memory_order_acq_rel);
    }
  };

  for (;;) {
    bool do_overflow = false;
    std::vector<PendingUiaAction> uia_actions;
    {
      std::unique_lock<std::mutex> lock(g_tray.mu);
      g_tray.action_cv.wait(lock, [] {
        return g_tray.stop || g_tray.toggle_overflow_pending ||
               !g_tray.uia_actions.empty();
      });
      if (g_tray.stop)
        break;
      do_overflow = g_tray.toggle_overflow_pending;
      uia_actions.swap(g_tray.uia_actions);
      g_tray.toggle_overflow_pending = false;
    }

    InFlightGuard guard(g_tray.action_in_flight);
    for (const auto& act : uia_actions)
      activate_uia_item_impl(uia, act);

    if (do_overflow) {
      toggle_native_overflow_impl(uia);
      systray_refresh_async(false);
    }
  }

  if (uia != nullptr)
    uia->Release();
  if (SUCCEEDED(hr))
    CoUninitialize();
}

void scan_worker_main() {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool com_ok = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
  IUIAutomation* uia = create_configured_uia(com_ok);

  if (!com_ok) {
    capability_set(SubsystemId::SysTray, CapabilityState::Degraded,
                   "COM init failed; UIA tray disabled");
  } else if (uia == nullptr) {
    capability_set(SubsystemId::SysTray, CapabilityState::Degraded,
                   "IUIAutomation unavailable; classic toolbar fallback only");
  } else {
    capability_set(SubsystemId::SysTray, CapabilityState::Active);
  }

  bool first_scan = true;
  for (;;) {
    bool do_scan = false;
    bool do_probe = false;
    {
      std::unique_lock<std::mutex> lock(g_tray.mu);
      g_tray.scan_cv.wait_for(lock, std::chrono::milliseconds(5000), [&] {
        return g_tray.stop || g_tray.scan_pending || first_scan;
      });
      if (g_tray.stop)
        break;
      do_scan = g_tray.scan_pending || first_scan;
      const auto now = std::chrono::steady_clock::now();
      const bool xo_stale =
          g_tray.last_xo_probe.time_since_epoch().count() == 0 ||
          (now - g_tray.last_xo_probe) >= std::chrono::seconds(30);
      do_probe = g_tray.force_probe || first_scan || xo_stale;
      g_tray.scan_pending = false;
      g_tray.force_probe = false;
      if (!do_scan)
        do_scan = true;
    }
    first_scan = false;

    if (!do_probe &&
        g_tray.action_in_flight.load(std::memory_order_acquire) > 0) {
      continue;
    }

    std::vector<UiaProbeRow> probes;
    double elapsed_ms = 0.0;
    std::vector<TrayIconEntry> fresh =
        collect_tray_snapshot(uia, do_probe, &probes, &elapsed_ms);

    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(g_tray.mu);
      ++g_tray.scan_count;
      g_tray.last_scan_ms = elapsed_ms;
      if (!probes.empty())
        g_tray.last_uia_probe = std::move(probes);
      if (!snapshot_equals(g_tray.items, fresh)) {
        g_tray.items = std::move(fresh);
        changed = true;
      }
    }
    if (changed)
      g_idle_add(idle_notify_tray_listeners, nullptr);
  }

  if (uia != nullptr)
    uia->Release();
  if (SUCCEEDED(hr))
    CoUninitialize();
}

}  // namespace

void systray_init() {
  std::lock_guard<std::mutex> lock(g_tray.mu);
  if (g_tray.started)
    return;
  g_tray.stop = false;
  g_tray.started = true;
  g_tray.scan_pending = true;
  g_tray.action_worker = std::thread(action_worker_main);
  g_tray.scan_worker = std::thread(scan_worker_main);
}

void systray_shutdown() {
  std::thread join_scan;
  std::thread join_action;
  {
    std::lock_guard<std::mutex> lock(g_tray.mu);
    if (!g_tray.started)
      return;
    g_tray.stop = true;
    g_tray.listeners.clear();
    g_tray.items.clear();
    g_tray.last_xo_items.clear();
    g_tray.last_uia_probe.clear();
    join_scan = std::move(g_tray.scan_worker);
    join_action = std::move(g_tray.action_worker);
    g_tray.started = false;
  }
  g_tray.scan_cv.notify_all();
  g_tray.action_cv.notify_all();
  if (join_action.joinable())
    join_action.join();
  if (join_scan.joinable())
    join_scan.join();
}

void systray_refresh_async(bool force) {
  std::lock_guard<std::mutex> lock(g_tray.mu);
  if (!g_tray.started)
    return;
  auto now = std::chrono::steady_clock::now();
  if (!force && g_tray.last_scan.time_since_epoch().count() != 0 &&
      (now - g_tray.last_scan) < std::chrono::milliseconds(1500)) {
    return;
  }
  g_tray.last_scan = now;
  g_tray.scan_pending = true;
  if (force)
    g_tray.force_probe = true;
  g_tray.scan_cv.notify_all();
}

std::vector<TrayIconEntry> systray_get_items() {
  std::lock_guard<std::mutex> lock(g_tray.mu);
  return g_tray.items;
}

void systray_activate_item(uint64_t id,
                           bool right_click,
                           int screen_x,
                           int screen_y) {
  TrayIconEntry match;
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(g_tray.mu);
    for (const auto& item : g_tray.items) {
      if (item.id == id) {
        match = item;
        found = true;
        break;
      }
    }
  }
  if (!found)
    return;

  if (!match.is_uia && match.target_hwnd != 0) {
    HWND target = reinterpret_cast<HWND>(match.target_hwnd);
    if (!IsWindow(target))
      return;

    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    if (pid != 0)
      AllowSetForegroundWindow(pid);
    SetForegroundWindow(target);

    const std::vector<TrayNotifyParams> msgs = build_tray_click_messages(
        match.callback_msg, match.u_id, match.version, right_click, screen_x,
        screen_y);
    for (const auto& p : msgs) {
      if (!SendNotifyMessageW(target, p.msg, static_cast<WPARAM>(p.wparam),
                              static_cast<LPARAM>(p.lparam))) {
        PostMessageW(target, p.msg, static_cast<WPARAM>(p.wparam),
                     static_cast<LPARAM>(p.lparam));
      }
    }
    return;
  }

  if (match.is_uia) {
    std::lock_guard<std::mutex> lock(g_tray.mu);
    if (!g_tray.started)
      return;
    g_tray.uia_actions.push_back(PendingUiaAction{
        match.uia_runtime_id, match.title, match.surface, right_click,
        std::chrono::steady_clock::now()});
    g_tray.action_cv.notify_all();
  }
}

void systray_toggle_native_overflow() {
  std::lock_guard<std::mutex> lock(g_tray.mu);
  if (!g_tray.started)
    return;
  g_tray.toggle_overflow_pending = true;
  g_tray.action_cv.notify_all();
}

void systray_add_listener(SystrayChangedCallback cb, void* user_data) {
  if (cb == nullptr)
    return;
  std::lock_guard<std::mutex> lock(g_tray.mu);
  g_tray.listeners.push_back({cb, user_data});
}

void systray_remove_listener(SystrayChangedCallback cb, void* user_data) {
  std::lock_guard<std::mutex> lock(g_tray.mu);
  g_tray.listeners.erase(
      std::remove_if(g_tray.listeners.begin(), g_tray.listeners.end(),
                     [&](const ListenerEntry& e) {
                       return e.cb == cb && e.user_data == user_data;
                     }),
      g_tray.listeners.end());
}

GdkTexture* systray_create_texture(const TrayIconEntry& entry) {
  if (entry.icon_width <= 0 || entry.icon_height <= 0)
    return nullptr;
  const size_t expected =
      static_cast<size_t>(entry.icon_width) *
      static_cast<size_t>(entry.icon_height) * 4;
  if (entry.rgba_pixels.size() != expected)
    return nullptr;

  GBytes* bytes = g_bytes_new(entry.rgba_pixels.data(), entry.rgba_pixels.size());
  GdkTexture* tex = gdk_memory_texture_new(
      entry.icon_width, entry.icon_height, GDK_MEMORY_R8G8B8A8,
      bytes, static_cast<gsize>(entry.icon_width) * 4);
  g_bytes_unref(bytes);
  return tex;
}

std::vector<TrayNotifyParams> build_tray_click_messages(uint32_t callback_msg,
                                                        uint32_t u_id,
                                                        uint32_t version,
                                                        bool right_click,
                                                        int screen_x,
                                                        int screen_y) {
  std::vector<TrayNotifyParams> out;
  if (callback_msg == 0)
    return out;
  constexpr uint32_t kNinSelect = WM_USER + 0;  // 0x0400
  const uint32_t msgs_left_v4[] = {WM_LBUTTONDOWN, WM_LBUTTONUP, kNinSelect};
  const uint32_t msgs_left_v3[] = {WM_LBUTTONDOWN, WM_LBUTTONUP};
  const uint32_t msgs_right[] = {WM_RBUTTONDOWN, WM_RBUTTONUP, WM_CONTEXTMENU};

  if (version >= 4) {
    const uintptr_t wp = static_cast<uintptr_t>(
        MAKEWPARAM(static_cast<WORD>(screen_x), static_cast<WORD>(screen_y)));
    if (right_click) {
      for (uint32_t m : msgs_right) {
        out.push_back(TrayNotifyParams{
            callback_msg, wp,
            static_cast<intptr_t>(
                MAKELPARAM(static_cast<WORD>(m), static_cast<WORD>(u_id)))});
      }
    } else {
      for (uint32_t m : msgs_left_v4) {
        out.push_back(TrayNotifyParams{
            callback_msg, wp,
            static_cast<intptr_t>(
                MAKELPARAM(static_cast<WORD>(m), static_cast<WORD>(u_id)))});
      }
    }
    return out;
  }

  const uintptr_t wp = static_cast<uintptr_t>(u_id);
  if (right_click) {
    for (uint32_t m : msgs_right)
      out.push_back(TrayNotifyParams{callback_msg, wp, static_cast<intptr_t>(m)});
  } else {
    for (uint32_t m : msgs_left_v3)
      out.push_back(TrayNotifyParams{callback_msg, wp, static_cast<intptr_t>(m)});
  }
  return out;
}

bool is_uia_tray_candidate(const std::string& name,
                           const std::string& auto_id,
                           bool in_overflow_window) {
  std::string trimmed = clean_tray_display_title(name);
  if (trimmed.empty())
    return false;
  if (is_system_builtin_tray_item(trimmed, auto_id))
    return false;
  const std::string name_low = lower_ascii(trimmed);
  const std::string aid_low = lower_ascii(auto_id);
  if (is_overflow_chevron_button(name_low, aid_low))
    return false;
  if (aid_low.rfind("appid:", 0) == 0 || aid_low == "startbutton")
    return false;
  if (in_overflow_window)
    return true;
  if (aid_low == "systemtrayicon")
    return false;
  return auto_id.find("Notify") != std::string::npos ||
         auto_id.find("NotifyItem") != std::string::npos ||
         auto_id.find("TrayIcon") != std::string::npos;
}

bool is_system_builtin_tray_item(const std::string& title,
                                 const std::string& app_hint) {
  const std::string t = lower_ascii(clean_tray_display_title(title));
  const std::string a = lower_ascii(app_hint);

  static constexpr const char* kBuiltinIds[] = {
      "showhiddenicons",
      "notifyiconoverflowbutton",
      "systemtrayicon",
      "showdesktop",
      "clockbutton",
      "notificationcenterbutton",
      "controlcenterbutton",
      "startbutton",
  };
  if (contains_any(a, kBuiltinIds, sizeof(kBuiltinIds) / sizeof(kBuiltinIds[0])))
    return true;

  static constexpr const char* kExactTitles[] = {
      "show hidden icons",
      "显示隐藏的图标",
      "显示隐藏图标",
      "speakers",
      "扬声器",
      "volume",
      "音量",
      "网络",
      "network",
      "internet access",
      "battery",
      "电池",
      "notification center",
      "通知中心",
      "quick settings",
      "快速设置",
      "show desktop",
      "显示桌面",
      "系统托盘",
      "system tray",
      "输入法",
      "麦克风",
      "触摸键盘",
      "touch keyboard",
  };
  for (const char* exact : kExactTitles) {
    if (t == exact)
      return true;
  }

  // Match Windows' compound clock/status tooltips such as "Network: Connected",
  // "网络 MISAKA_00002...", "电源 电池状态: 0% 剩余" without catching third-party apps.
  static constexpr const char* kBuiltinPrefixes[] = {
      "network:",
      "network ",
      "battery status:",
      "speakers:",
      "扬声器:",
      "扬声器：",
      "音量 ",
      "音量:",
      "音量：",
      "网络 ",
      "网络:",
      "网络：",
      "电源 ",
      "电源:",
      "电源：",
      "时钟 ",
      "时钟:",
      "时钟：",
      "通知 ",
      "通知:",
      "通知：",
      "托盘输入指示器",
      "input indicator",
  };
  for (const char* pfx : kBuiltinPrefixes) {
    if (t.rfind(pfx, 0) == 0)
      return true;
  }
  return false;
}

const char* tray_fallback_icon_name(const std::string& title,
                                    const std::string& app_hint) {
  const std::string key = lower_ascii(title + " " + app_hint);

  static constexpr const char* kDisplay[] = {"nvidia", "radeon", "amd", "intel", "display", "显卡"};
  if (contains_any(key, kDisplay, sizeof(kDisplay) / sizeof(kDisplay[0])))
    return "video-display-symbolic";

  static constexpr const char* kGames[] = {"steam", "epic", "ubisoft", "xbox", "game"};
  if (contains_any(key, kGames, sizeof(kGames) / sizeof(kGames[0])))
    return "applications-games-symbolic";

  static constexpr const char* kCloud[] = {"onedrive", "dropbox", "icloud", "cloud", "sync", "网盘"};
  if (contains_any(key, kCloud, sizeof(kCloud) / sizeof(kCloud[0])))
    return "weather-overcast-symbolic";

  static constexpr const char* kIme[] = {"ime", "input", "keyboard", "输入法", "拼音", "五笔"};
  if (contains_any(key, kIme, sizeof(kIme) / sizeof(kIme[0])))
    return "input-keyboard-symbolic";

  static constexpr const char* kNetwork[] = {"clash", "v2ray", "vpn", "proxy", "wireguard", "tailscale"};
  if (contains_any(key, kNetwork, sizeof(kNetwork) / sizeof(kNetwork[0])))
    return "network-wired-symbolic";

  static constexpr const char* kAudio[] = {"spotify", "music", "audio", "realtek", "nahimic", "音乐"};
  if (contains_any(key, kAudio, sizeof(kAudio) / sizeof(kAudio[0])))
    return "audio-x-generic-symbolic";

  return "application-x-executable-symbolic";
}

std::string systray_debug_inspect_json() {
  const auto t0 = std::chrono::steady_clock::now();
  auto escape_json = [](const std::string& s, std::string* out) {
    out->push_back('"');
    for (unsigned char c : s) {
      if (c == '"')
        *out += "\\\"";
      else if (c == '\\')
        *out += "\\\\";
      else if (c < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
        *out += buf;
      } else {
        out->push_back(static_cast<char>(c));
      }
    }
    out->push_back('"');
  };

  std::string out = "{";
  HWND shell_tray = FindWindowW(L"Shell_TrayWnd", nullptr);
  HWND xo = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr);
  HWND co = FindWindowW(L"NotifyIconOverflowWindow", nullptr);

  char hex[32];
  std::snprintf(hex, sizeof(hex), "\"0x%llx\"",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(shell_tray)));
  out += "\"shell_tray_hwnd\":";
  out += hex;
  out += ",\"shell_tray_visible\":";
  out += (shell_tray && IsWindowVisible(shell_tray)) ? "true" : "false";

  RECT sr{};
  if (shell_tray)
    GetWindowRect(shell_tray, &sr);
  out += ",\"shell_tray_rect\":[" + std::to_string(sr.left) + "," +
         std::to_string(sr.top) + "," + std::to_string(sr.right) + "," +
         std::to_string(sr.bottom) + "]";

  out += ",\"xaml_overflow_exists\":";
  out += (xo != nullptr) ? "true" : "false";
  out += ",\"xaml_overflow_visible\":";
  out += (xo && IsWindowVisible(xo)) ? "true" : "false";
  out += ",\"classic_overflow_exists\":";
  out += (co != nullptr) ? "true" : "false";
  out += ",\"classic_overflow_visible\":";
  out += (co && IsWindowVisible(co)) ? "true" : "false";

  out += ",\"children\":[";
  bool first_c = true;
  if (shell_tray) {
    std::vector<HWND> children;
    EnumChildWindows(
        shell_tray,
        [](HWND c, LPARAM lp) -> BOOL {
          reinterpret_cast<std::vector<HWND>*>(lp)->push_back(c);
          return TRUE;
        },
        reinterpret_cast<LPARAM>(&children));
    for (HWND c : children) {
      wchar_t cls[128] = {};
      GetClassNameW(c, cls, 128);
      RECT cr{};
      GetWindowRect(c, &cr);
      if (!first_c)
        out.push_back(',');
      first_c = false;
      out += "{\"class\":";
      escape_json(wide_to_utf8(cls), &out);
      out += ",\"vis\":";
      out += IsWindowVisible(c) ? "true" : "false";
      out += ",\"rect\":[" + std::to_string(cr.left) + "," +
             std::to_string(cr.top) + "," + std::to_string(cr.right) + "," +
             std::to_string(cr.bottom) + "]";
      if (_wcsicmp(cls, L"ToolbarWindow32") == 0) {
        DWORD_PTR tb_cnt = 0;
        SendMessageTimeoutW(c, TB_BUTTONCOUNT, 0, 0, SMTO_ABORTIFHUNG, 50, &tb_cnt);
        out += ",\"tb_count\":" + std::to_string(static_cast<long>(tb_cnt));
      }
      out.push_back('}');
    }
  }
  out += "]";

  std::vector<TrayIconEntry> cur;
  std::vector<UiaProbeRow> cached_probe;
  double last_scan_ms = 0.0;
  uint64_t scan_count = 0;
  double last_action_ms = 0.0;
  uint64_t action_count = 0;
  std::string last_action_info;
  {
    std::lock_guard<std::mutex> lock(g_tray.mu);
    cur = g_tray.items;
    cached_probe = g_tray.last_uia_probe;
    last_scan_ms = g_tray.last_scan_ms;
    scan_count = g_tray.scan_count;
    last_action_ms = g_tray.last_action_ms;
    action_count = g_tray.action_count;
    last_action_info = g_tray.last_action_info;
  }

  out += ",\"items_count\":" + std::to_string(cur.size()) + ",\"items\":[";
  for (size_t i = 0; i < cur.size(); ++i) {
    if (i > 0)
      out.push_back(',');
    out += "{\"title\":";
    escape_json(cur[i].title, &out);
    out += ",\"aid\":";
    escape_json(cur[i].app_hint, &out);
    out += ",\"surface\":";
    escape_json(tray_surface_label(cur[i].surface), &out);
    out += ",\"is_uia\":";
    out += cur[i].is_uia ? "true" : "false";
    out += ",\"rgba\":" + std::to_string(cur[i].rgba_pixels.size()) + "}";
  }
  out += "]";

  char sbuf[160];
  std::snprintf(sbuf, sizeof(sbuf),
                ",\"stats\":{\"scans\":%llu,\"last_scan_ms\":%.1f,"
                "\"actions\":%llu,\"last_action_ms\":%.1f,\"inspect_ms\":%.1f,"
                "\"last_action\":",
                static_cast<unsigned long long>(scan_count), last_scan_ms,
                static_cast<unsigned long long>(action_count), last_action_ms,
                qpc_ms_since(t0));
  out += sbuf;
  escape_json(last_action_info, &out);
  out += "}";

  out += ",\"uia_probe\":[";
  for (size_t i = 0; i < cached_probe.size(); ++i) {
    if (i > 0)
      out.push_back(',');
    out += "{\"ct\":" + std::to_string(cached_probe[i].ct) + ",\"surface\":";
    escape_json(tray_surface_label(cached_probe[i].surface), &out);
    out += ",\"name\":";
    escape_json(cached_probe[i].name, &out);
    out += ",\"aid\":";
    escape_json(cached_probe[i].aid, &out);
    out += ",\"cls\":";
    escape_json(cached_probe[i].cls, &out);
    out += "}";
  }
  out += "]}";
  return out;
}

}  // namespace winome
