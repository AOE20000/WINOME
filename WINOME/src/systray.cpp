// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Safe Windows System Tray discovery & activation without DLL injection.

#include "systray.h"
#include "perf-metrics.h"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <oleauto.h>
#include <shellapi.h>
#include <uiautomation.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

namespace winome {
namespace {

constexpr CLSID kClsidCUIAutomation = {
    0xff48dba4, 0x60ef, 0x4201,
    {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
constexpr IID kIidIUIAutomation = {
    0x30cbe57d, 0xd9d0, 0x452a,
    {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};
constexpr IID kIidIUIAutomationInvokePattern = {
    0xfb377fbe, 0x8ea6, 0x46d5,
    {0x9c, 0x73, 0x64, 0x99, 0x64, 0x2d, 0x30, 0x59}};
constexpr IID kIidIUIAutomationLegacyIAccessiblePattern = {
    0x828055ad, 0x355b, 0x4435,
    {0x86, 0xd5, 0x3b, 0x51, 0xc1, 0x4a, 0x9b, 0x1b}};

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

void collect_classic_toolbars(std::vector<HWND>* out_bars) {
  auto find_descendant_toolbar = [](HWND root, std::vector<HWND>* list) {
    if (root == nullptr)
      return;
    EnumChildWindows(
        root,
        [](HWND hw, LPARAM lp) -> BOOL {
          wchar_t cls[64] = {};
          if (GetClassNameW(hw, cls, 64) > 0 &&
              wcscmp(cls, L"ToolbarWindow32") == 0) {
            auto* out = reinterpret_cast<std::vector<HWND>*>(lp);
            out->push_back(hw);
          }
          return TRUE;
        },
        reinterpret_cast<LPARAM>(list));
  };

  if (HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr)) {
    HWND notify = FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr);
    if (notify != nullptr)
      find_descendant_toolbar(notify, out_bars);
  }
  if (HWND overflow = FindWindowW(L"NotifyIconOverflowWindow", nullptr)) {
    find_descendant_toolbar(overflow, out_bars);
  }
}

void scan_classic_toolbar(HWND toolbar, std::vector<TrayIconEntry>* out) {
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

void scan_uia_root(IUIAutomation* uia,
                   HWND hwnd,
                   std::vector<TrayIconEntry>* out) {
  if (uia == nullptr || hwnd == nullptr)
    return;

  IUIAutomationElement* root = nullptr;
  if (FAILED(uia->ElementFromHandle(hwnd, &root)) || root == nullptr)
    return;

  VARIANT vtype;
  VariantInit(&vtype);
  vtype.vt = VT_I4;
  vtype.lVal = UIA_ButtonControlTypeId;

  IUIAutomationCondition* cond = nullptr;
  if (SUCCEEDED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, vtype, &cond)) &&
      cond != nullptr) {
    IUIAutomationElementArray* arr = nullptr;
    if (SUCCEEDED(root->FindAll(TreeScope_Descendants, cond, &arr)) &&
        arr != nullptr) {
      int len = 0;
      arr->get_Length(&len);
      for (int i = 0; i < len && i < 128; ++i) {
        IUIAutomationElement* el = nullptr;
        if (FAILED(arr->GetElement(i, &el)) || el == nullptr)
          continue;

        std::string name = get_uia_string_prop(el, UIA_NamePropertyId);
        std::string auto_id = get_uia_string_prop(el, UIA_AutomationIdPropertyId);

        // Windows 11 tray icons typically sit inside NotifyIcon items or
        // TopLevelWindowForOverflowXamlIsland without shell chrome IDs.
        const bool looks_like_tray_item =
            auto_id.find("Notify") != std::string::npos ||
            auto_id.find("SystemTrayIcon") != std::string::npos ||
            auto_id.find("NotifyItem") != std::string::npos;

        if (!name.empty() && looks_like_tray_item &&
            !is_system_builtin_tray_item(name, auto_id)) {
          std::vector<int> rid = get_uia_runtime_id(el);
          TrayIconEntry entry;
          entry.title = name;
          entry.app_hint = auto_id;
          entry.is_uia = true;
          entry.uia_runtime_id = std::move(rid);
          if (!entry.uia_runtime_id.empty()) {
            entry.id = fnv1a_hash(
                entry.uia_runtime_id.data(),
                entry.uia_runtime_id.size() * sizeof(int));
          } else {
            entry.id = fnv1a_hash(entry.title.data(), entry.title.size());
          }
          out->push_back(std::move(entry));
        }

        el->Release();
      }
      arr->Release();
    }
    cond->Release();
  }
  VariantClear(&vtype);
  root->Release();
}

bool invoke_uia_button(IUIAutomation* uia,
                       HWND root_hwnd,
                       const std::vector<int>& runtime_id,
                       bool match_overflow_chevron) {
  if (uia == nullptr || root_hwnd == nullptr)
    return false;

  IUIAutomationElement* root = nullptr;
  if (FAILED(uia->ElementFromHandle(root_hwnd, &root)) || root == nullptr)
    return false;

  bool invoked = false;
  VARIANT vtype;
  VariantInit(&vtype);
  vtype.vt = VT_I4;
  vtype.lVal = UIA_ButtonControlTypeId;

  IUIAutomationCondition* cond = nullptr;
  if (SUCCEEDED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, vtype, &cond)) &&
      cond != nullptr) {
    IUIAutomationElementArray* arr = nullptr;
    if (SUCCEEDED(root->FindAll(TreeScope_Descendants, cond, &arr)) &&
        arr != nullptr) {
      int len = 0;
      arr->get_Length(&len);
      for (int i = 0; i < len && i < 128 && !invoked; ++i) {
        IUIAutomationElement* el = nullptr;
        if (FAILED(arr->GetElement(i, &el)) || el == nullptr)
          continue;

        bool matched = false;
        if (match_overflow_chevron) {
          std::string name = lower_ascii(get_uia_string_prop(el, UIA_NamePropertyId));
          std::string aid = lower_ascii(get_uia_string_prop(el, UIA_AutomationIdPropertyId));
          if (aid.find("overflow") != std::string::npos ||
              aid.find("showhidden") != std::string::npos ||
              name.find("show hidden") != std::string::npos ||
              name.find("显示隐藏的图标") != std::string::npos) {
            matched = true;
          }
        } else if (!runtime_id.empty()) {
          if (get_uia_runtime_id(el) == runtime_id)
            matched = true;
        }

        if (matched) {
          IUIAutomationInvokePattern* inv = nullptr;
          if (SUCCEEDED(el->GetCurrentPatternAs(
                  UIA_InvokePatternId, kIidIUIAutomationInvokePattern,
                  reinterpret_cast<void**>(&inv))) &&
              inv != nullptr) {
            invoked = SUCCEEDED(inv->Invoke());
            inv->Release();
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
        }
        el->Release();
      }
      arr->Release();
    }
    cond->Release();
  }

  VariantClear(&vtype);
  root->Release();
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

struct SystrayState {
  std::mutex mu;
  std::condition_variable cv;
  bool stop = false;
  bool started = false;
  bool scan_pending = false;
  bool toggle_overflow_pending = false;
  std::thread worker;
  std::vector<TrayIconEntry> items;
  std::vector<ListenerEntry> listeners;
  std::chrono::steady_clock::time_point last_scan{};
};

SystrayState g_tray;

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

std::vector<TrayIconEntry> collect_tray_snapshot(IUIAutomation* uia) {
  std::vector<TrayIconEntry> items;

  std::vector<HWND> toolbars;
  collect_classic_toolbars(&toolbars);
  for (HWND tb : toolbars)
    scan_classic_toolbar(tb, &items);

  // If no classic ToolbarWindow32 items exist (standard Windows 11 XAML taskbar),
  // fall back to read-only UI Automation inspection of Explorer's tray surfaces.
  if (items.empty() && uia != nullptr) {
    if (HWND shell_tray = FindWindowW(L"Shell_TrayWnd", nullptr))
      scan_uia_root(uia, shell_tray, &items);
    if (HWND xaml_overflow =
            FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr)) {
      scan_uia_root(uia, xaml_overflow, &items);
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
  return unique;
}

void worker_main() {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool com_ok = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;

  IUIAutomation* uia = nullptr;
  if (com_ok) {
    CoCreateInstance(kClsidCUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                     kIidIUIAutomation, reinterpret_cast<void**>(&uia));
  }
  if (!com_ok) {
    capability_set(SubsystemId::SysTray, CapabilityState::Degraded,
                   "COM init failed; UIA tray disabled");
  } else if (uia == nullptr) {
    capability_set(SubsystemId::SysTray, CapabilityState::Degraded,
                   "IUIAutomation unavailable; classic toolbar fallback only");
  } else {
    capability_set(SubsystemId::SysTray, CapabilityState::Active);
  }

  for (;;) {
    bool do_scan = false;
    bool do_overflow = false;
    {
      std::unique_lock<std::mutex> lock(g_tray.mu);
      g_tray.cv.wait(lock, [] {
        return g_tray.stop || g_tray.scan_pending || g_tray.toggle_overflow_pending;
      });
      if (g_tray.stop)
        break;
      do_scan = g_tray.scan_pending;
      do_overflow = g_tray.toggle_overflow_pending;
      g_tray.scan_pending = false;
      g_tray.toggle_overflow_pending = false;
    }

    if (do_overflow && uia != nullptr) {
      if (HWND shell_tray = FindWindowW(L"Shell_TrayWnd", nullptr))
        invoke_uia_button(uia, shell_tray, {}, true);
    }

    if (do_scan) {
      std::vector<TrayIconEntry> fresh = collect_tray_snapshot(uia);
      bool changed = false;
      {
        std::lock_guard<std::mutex> lock(g_tray.mu);
        if (!snapshot_equals(g_tray.items, fresh)) {
          g_tray.items = std::move(fresh);
          changed = true;
        }
      }
      if (changed)
        g_idle_add(idle_notify_tray_listeners, nullptr);
    }
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
  g_tray.worker = std::thread(worker_main);
}

void systray_shutdown() {
  std::thread to_join;
  {
    std::lock_guard<std::mutex> lock(g_tray.mu);
    if (!g_tray.started)
      return;
    g_tray.stop = true;
    g_tray.listeners.clear();
    g_tray.items.clear();
    to_join = std::move(g_tray.worker);
    g_tray.started = false;
  }
  g_tray.cv.notify_all();
  if (to_join.joinable())
    to_join.join();
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
  g_tray.cv.notify_all();
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

    if (right_click) {
      SendNotifyMessageW(target, match.callback_msg,
                         static_cast<WPARAM>(match.u_id), WM_RBUTTONUP);
      SendNotifyMessageW(target, match.callback_msg,
                         static_cast<WPARAM>(match.u_id),
                         static_cast<LPARAM>(WM_CONTEXTMENU));
      (void)screen_x;
      (void)screen_y;
    } else {
      SendNotifyMessageW(target, match.callback_msg,
                         static_cast<WPARAM>(match.u_id), WM_LBUTTONDOWN);
      SendNotifyMessageW(target, match.callback_msg,
                         static_cast<WPARAM>(match.u_id), WM_LBUTTONUP);
      if (match.version >= NOTIFYICON_VERSION) {
        SendNotifyMessageW(target, match.callback_msg,
                           static_cast<WPARAM>(match.u_id), NIN_SELECT);
      }
    }
    return;
  }

  if (match.is_uia && !match.uia_runtime_id.empty()) {
    std::vector<int> rid = std::move(match.uia_runtime_id);
    std::thread([rid]() {
      HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      IUIAutomation* uia = nullptr;
      if ((SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) &&
          SUCCEEDED(CoCreateInstance(kClsidCUIAutomation, nullptr,
                                     CLSCTX_INPROC_SERVER, kIidIUIAutomation,
                                     reinterpret_cast<void**>(&uia))) &&
          uia != nullptr) {
        bool done = false;
        if (HWND shell_tray = FindWindowW(L"Shell_TrayWnd", nullptr))
          done = invoke_uia_button(uia, shell_tray, rid, false);
        if (!done) {
          if (HWND overflow =
                  FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr)) {
            invoke_uia_button(uia, overflow, rid, false);
          }
        }
        uia->Release();
      }
      if (SUCCEEDED(hr))
        CoUninitialize();
    }).detach();
  }
}

void systray_toggle_native_overflow() {
  std::lock_guard<std::mutex> lock(g_tray.mu);
  if (!g_tray.started)
    return;
  g_tray.toggle_overflow_pending = true;
  g_tray.cv.notify_all();
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

bool is_system_builtin_tray_item(const std::string& title,
                                 const std::string& app_hint) {
  const std::string t = lower_ascii(title);
  const std::string a = lower_ascii(app_hint);

  static constexpr const char* kBuiltinIds[] = {
      "showhiddenicons", "notifyiconoverflowbutton",
      "systemtrayicon",  "showdesktop",
      "clockbutton",     "notificationcenterbutton",
      "controlcenterbutton",
  };
  if (contains_any(a, kBuiltinIds, sizeof(kBuiltinIds) / sizeof(kBuiltinIds[0])))
    return true;

  static constexpr const char* kExactTitles[] = {
      "show hidden icons",
      "显示隐藏的图标",
      "speakers",
      "扬声器",
      "volume",
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
  };
  for (const char* exact : kExactTitles) {
    if (t == exact)
      return true;
  }

  // Match Windows' compound clock/status tooltips such as "Network: Connected"
  // or "Battery status: 98% remaining" without catching apps like "BatteryMon".
  if (t.rfind("network:", 0) == 0 || t.rfind("battery status:", 0) == 0 ||
      t.rfind("speakers:", 0) == 0 || t.rfind("扬声器:", 0) == 0 ||
      t.rfind("扬声器：", 0) == 0 || t.rfind("电源:", 0) == 0 ||
      t.rfind("电源：", 0) == 0 || t.rfind("时钟", 0) == 0) {
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

}  // namespace winome
