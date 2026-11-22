// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Dynamic combase.dll + vtable-slot WinRT bridge executed on a dedicated MTA
// worker thread so it never deadlocks or conflicts with GTK's STA thread.

#include "winrt-bridge.h"
#include "perf-metrics.h"

#include <sqlite3.h>
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace winome {
namespace {

using WINOME_HSTRING = void*;

struct ComBaseApi {
  HMODULE dll = nullptr;
  HRESULT (WINAPI* RoInitialize)(int init_type) = nullptr;
  void    (WINAPI* RoUninitialize)() = nullptr;
  HRESULT (WINAPI* RoGetActivationFactory)(WINOME_HSTRING class_id, REFIID iid,
                                           void** factory) = nullptr;
  HRESULT (WINAPI* WindowsCreateString)(const wchar_t* src, UINT32 len,
                                        WINOME_HSTRING* out) = nullptr;
  const wchar_t* (WINAPI* WindowsGetStringRawBuffer)(WINOME_HSTRING str,
                                                     UINT32* len) = nullptr;
  HRESULT (WINAPI* WindowsDeleteString)(WINOME_HSTRING str) = nullptr;
  bool loaded = false;
  bool probed = false;
};

ComBaseApi& combase() {
  static ComBaseApi api;
  if (!api.probed) {
    api.probed = true;
    api.dll = LoadLibraryW(L"combase.dll");
    if (api.dll != nullptr) {
      auto load = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::decay_t<decltype(fn)>>(
            reinterpret_cast<void*>(GetProcAddress(api.dll, name)));
      };
      load(api.RoInitialize, "RoInitialize");
      load(api.RoUninitialize, "RoUninitialize");
      load(api.RoGetActivationFactory, "RoGetActivationFactory");
      load(api.WindowsCreateString, "WindowsCreateString");
      load(api.WindowsGetStringRawBuffer, "WindowsGetStringRawBuffer");
      load(api.WindowsDeleteString, "WindowsDeleteString");
      api.loaded = api.RoInitialize && api.RoUninitialize &&
                   api.RoGetActivationFactory && api.WindowsCreateString &&
                   api.WindowsGetStringRawBuffer && api.WindowsDeleteString;
    }
  }
  return api;
}

template <typename R, typename... Args>
R vcall(void* obj, size_t slot, Args... args) {
  auto vtbl = *reinterpret_cast<void***>(obj);
  auto fn = reinterpret_cast<R (STDMETHODCALLTYPE*)(void*, Args...)>(vtbl[slot]);
  return fn(obj, args...);
}

struct ComPtr {
  void* ptr = nullptr;
  ComPtr() = default;
  explicit ComPtr(void* p) : ptr(p) {}
  ~ComPtr() { reset(); }
  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;
  ComPtr(ComPtr&& o) noexcept : ptr(o.ptr) { o.ptr = nullptr; }
  ComPtr& operator=(ComPtr&& o) noexcept {
    if (this != &o) {
      reset();
      ptr = o.ptr;
      o.ptr = nullptr;
    }
    return *this;
  }
  void reset(void* p = nullptr) {
    void* old = ptr;
    ptr = p;
    if (old != nullptr) {
      vcall<ULONG>(old, 2);  // IUnknown::Release
    }
  }
  void** put() {
    reset();
    return &ptr;
  }
  explicit operator bool() const { return ptr != nullptr; }
};

std::string wide_to_utf8(const wchar_t* wide, UINT32 len) {
  if (wide == nullptr || len == 0)
    return {};
  int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(len),
                                  nullptr, 0, nullptr, nullptr);
  if (bytes <= 0)
    return {};
  std::string out(static_cast<size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(len),
                      out.data(), bytes, nullptr, nullptr);
  return out;
}

std::wstring utf8_to_wide(const std::string& utf8) {
  if (utf8.empty())
    return {};
  int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                 static_cast<int>(utf8.size()), nullptr, 0);
  if (wlen <= 0)
    return {};
  std::wstring out(static_cast<size_t>(wlen), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                      out.data(), wlen);
  return out;
}

std::string ascii_lower(std::string s) {
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string consume_hstring(WINOME_HSTRING hstr) {
  if (hstr == nullptr)
    return {};
  const ComBaseApi& api = combase();
  UINT32 len = 0;
  const wchar_t* buf = api.WindowsGetStringRawBuffer(hstr, &len);
  std::string res = wide_to_utf8(buf, len);
  api.WindowsDeleteString(hstr);
  return res;
}

class HStringRef {
 public:
  explicit HStringRef(const wchar_t* str) {
    if (str != nullptr && combase().loaded) {
      combase().WindowsCreateString(
          str, static_cast<UINT32>(std::wcslen(str)), &h_);
    }
  }
  ~HStringRef() {
    if (h_ != nullptr && combase().loaded)
      combase().WindowsDeleteString(h_);
  }
  HStringRef(const HStringRef&) = delete;
  HStringRef& operator=(const HStringRef&) = delete;
  WINOME_HSTRING get() const { return h_; }

 private:
  WINOME_HSTRING h_ = nullptr;
};

// Verified GUIDs from Windows SDK 10.0.26100.0 winrt headers.
constexpr IID kIID_IAsyncInfo = {
    0x00000036, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr IID kIID_IRadioStatics = {
    0x5fb6a12e, 0x67cb, 0x46ae, {0xaa, 0xe9, 0x65, 0x91, 0x9f, 0x86, 0xef, 0xf4}};
constexpr IID kIID_IRadio = {
    0x252118df, 0xb33e, 0x416a, {0x87, 0x5f, 0x1c, 0xf3, 0x8a, 0xe2, 0xd8, 0x3e}};

constexpr IID kIID_IGSMTCManagerStatics = {
    0x2050c4ee, 0x11a0, 0x57de, {0xae, 0xd7, 0xc9, 0x7c, 0x70, 0x33, 0x82, 0x45}};
constexpr IID kIID_IGSMTCManager = {
    0xcace8eac, 0xe86e, 0x504a, {0xab, 0x31, 0x5f, 0xf8, 0xff, 0x1b, 0xce, 0x49}};
constexpr IID kIID_IGSMTCSession = {
    0x7148c835, 0x9b14, 0x5ae2, {0xab, 0x85, 0xdc, 0x9b, 0x1c, 0x14, 0xe1, 0xa8}};
constexpr IID kIID_IGSMTCMediaProps = {
    0x68856cf6, 0xadb4, 0x54b2, {0xac, 0x16, 0x05, 0x83, 0x79, 0x07, 0xac, 0xb6}};
constexpr IID kIID_IGSMTCPlaybackInfo = {
    0x94b4b6cf, 0xe8ba, 0x51ad, {0x87, 0xa7, 0xc1, 0x0a, 0xde, 0x10, 0x61, 0x27}};

constexpr IID kIID_IUserNotificationListenerStatics = {
    0xff6123cf, 0x4386, 0x4aa3, {0xb7, 0x3d, 0xb8, 0x04, 0xe5, 0xb6, 0x3b, 0x23}};
constexpr IID kIID_IUserNotificationListener = {
    0x62553e41, 0x8a06, 0x4cef, {0x82, 0x15, 0x60, 0x33, 0xa5, 0xbe, 0x4b, 0x03}};
constexpr IID kIID_IUserNotification = {
    0xadf7e52f, 0x4e53, 0x42d5, {0x9c, 0x33, 0xeb, 0x5e, 0xa5, 0x15, 0xb2, 0x3e}};
constexpr IID kIID_IAppInfo = {
    0xcf7f59b3, 0x6a09, 0x4de8, {0xa6, 0xc0, 0x57, 0x92, 0xd5, 0x68, 0x80, 0xd1}};
constexpr IID kIID_IAppDisplayInfo = {
    0x1aeb1103, 0xe4d4, 0x41aa, {0xa4, 0xf6, 0xc4, 0xa2, 0x76, 0xe7, 0x9e, 0xac}};
constexpr IID kIID_INotification = {
    0x108037fe, 0xeb76, 0x4f82, {0x97, 0xbc, 0xda, 0x07, 0x53, 0x0a, 0x2e, 0x20}};
constexpr IID kIID_INotificationVisual = {
    0x68835b8e, 0xaa56, 0x4e11, {0x86, 0xd3, 0x5f, 0x9a, 0x69, 0x57, 0xbc, 0x5b}};
constexpr IID kIID_INotificationBinding = {
    0xf29e4b85, 0x0370, 0x4ad3, {0xb4, 0xea, 0xda, 0x9e, 0x35, 0xe7, 0xea, 0xbf}};
constexpr IID kIID_IAdaptiveNotificationText = {
    0x46d4a3be, 0x609a, 0x4326, {0xa4, 0x0b, 0xbf, 0xde, 0x87, 0x20, 0x34, 0xa3}};

constexpr int32_t kAsyncStarted = 0;
constexpr int32_t kAsyncCompleted = 1;
constexpr int32_t kRadioKindBluetooth = 3;
constexpr int32_t kRadioStateOn = 1;
constexpr int32_t kRadioStateOff = 2;
constexpr int32_t kRadioAccessAllowed = 1;
constexpr int32_t kPlaybackPlaying = 4;
constexpr uint32_t kNotificationKindToast = 1;
constexpr int32_t kNotifAccessUnspecified = 0;
constexpr int32_t kNotifAccessAllowed = 1;

bool await_async(void* async_op, int timeout_ms) {
  if (async_op == nullptr)
    return false;

  ComPtr info;
  HRESULT hr = vcall<HRESULT>(async_op, 0, &kIID_IAsyncInfo, info.put());
  if (FAILED(hr) || !info)
    return false;

  const DWORD start = GetTickCount();
  int32_t status = kAsyncStarted;
  for (;;) {
    hr = vcall<HRESULT>(info.ptr, 7, &status);  // IAsyncInfo::get_Status
    if (FAILED(hr))
      return false;
    if (status != kAsyncStarted)
      break;
    if (static_cast<int>(GetTickCount() - start) >= timeout_ms) {
      vcall<HRESULT>(info.ptr, 9);  // Cancel
      return false;
    }
    Sleep(8);
  }

  if (status != kAsyncCompleted)
    return false;

  HRESULT err_hr = S_OK;
  if (SUCCEEDED(vcall<HRESULT>(info.ptr, 8, &err_hr)) && FAILED(err_hr))
    return false;

  return true;
}

template <typename ResultT>
bool await_operation_result(void* async_op, ResultT* out, int timeout_ms = 2500) {
  if (!await_async(async_op, timeout_ms))
    return false;
  HRESULT hr = vcall<HRESULT>(async_op, 8, out);  // IAsyncOperation::GetResults
  return SUCCEEDED(hr);
}

ComPtr get_activation_factory(const wchar_t* runtime_class, REFIID iid) {
  const ComBaseApi& api = combase();
  if (!api.loaded)
    return {};
  HStringRef cls(runtime_class);
  if (cls.get() == nullptr)
    return {};
  ComPtr factory;
  if (FAILED(api.RoGetActivationFactory(cls.get(), iid, factory.put())))
    return {};
  return factory;
}

ComPtr qi(void* obj, REFIID iid) {
  void* out = nullptr;
  if (obj != nullptr &&
      SUCCEEDED(vcall<HRESULT>(obj, 0, &iid, &out)) &&
      out != nullptr) {
    return ComPtr(out);
  }
  return {};
}

int64_t winrt_filetime_to_unix(int64_t ft100ns) {
  // Windows epoch starts 1601-01-01; Unix epoch starts 1970-01-01.
  constexpr int64_t kEpochDiff100ns = 116444736000000000LL;
  if (ft100ns <= kEpochDiff100ns)
    return 0;
  return (ft100ns - kEpochDiff100ns) / 10000000LL;
}

void append_utf8_codepoint(uint32_t cp, std::string* out) {
  if (cp == 0 || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
    return;
  if (cp < 0x80u) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800u) {
    out->push_back(static_cast<char>(0xC0u | (cp >> 6)));
    out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp < 0x10000u) {
    out->push_back(static_cast<char>(0xE0u | (cp >> 12)));
    out->push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else {
    out->push_back(static_cast<char>(0xF0u | (cp >> 18)));
    out->push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  }
}

std::string unescape_xml_entities(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size();) {
    if (raw[i] == '&') {
      const size_t semi = raw.find(';', i + 1);
      if (semi != std::string::npos && semi - i <= 12) {
        const std::string ent = raw.substr(i + 1, semi - i - 1);
        if (ent == "amp") {
          out.push_back('&');
          i = semi + 1;
          continue;
        } else if (ent == "lt") {
          out.push_back('<');
          i = semi + 1;
          continue;
        } else if (ent == "gt") {
          out.push_back('>');
          i = semi + 1;
          continue;
        } else if (ent == "quot") {
          out.push_back('"');
          i = semi + 1;
          continue;
        } else if (ent == "apos") {
          out.push_back('\'');
          i = semi + 1;
          continue;
        } else if (ent.size() >= 2 && ent[0] == '#') {
          uint32_t cp = 0;
          bool valid = true;
          if (ent[1] == 'x' || ent[1] == 'X') {
            if (ent.size() < 3)
              valid = false;
            for (size_t k = 2; k < ent.size() && valid; ++k) {
              char c = ent[k];
              cp <<= 4;
              if (c >= '0' && c <= '9') cp |= static_cast<uint32_t>(c - '0');
              else if (c >= 'a' && c <= 'f') cp |= static_cast<uint32_t>(c - 'a' + 10);
              else if (c >= 'A' && c <= 'F') cp |= static_cast<uint32_t>(c - 'A' + 10);
              else valid = false;
            }
          } else {
            for (size_t k = 1; k < ent.size() && valid; ++k) {
              char c = ent[k];
              if (c >= '0' && c <= '9')
                cp = cp * 10u + static_cast<uint32_t>(c - '0');
              else
                valid = false;
            }
          }
          if (valid && cp > 0) {
            append_utf8_codepoint(cp, &out);
            i = semi + 1;
            continue;
          }
        }
      }
    }
    out.push_back(raw[i++]);
  }
  return out;
}

std::string trim_whitespace(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
    ++b;
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
    --e;
  return s.substr(b, e - b);
}

struct WpnDbFileStamp {
  bool exists = false;
  uint64_t size = 0;
  uint64_t mtime = 0;

  bool operator==(const WpnDbFileStamp& o) const {
    return exists == o.exists && size == o.size && mtime == o.mtime;
  }
};

WpnDbFileStamp stat_wide_file(const std::wstring& path) {
  WpnDbFileStamp st{};
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
    st.exists = true;
    st.size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    st.mtime = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
               fad.ftLastWriteTime.dwLowDateTime;
  }
  return st;
}

struct WpnDbCache {
  std::mutex mu;
  bool valid = false;
  WpnDbFileStamp db_stamp{};
  WpnDbFileStamp wal_stamp{};
  std::vector<WinomeSystemNotification> items;
};

WpnDbCache g_wpn_cache;

bool read_wpn_database_fallback(std::vector<WinomeSystemNotification>* out) {
  if (out == nullptr)
    return false;
  out->clear();

  wchar_t local_app[MAX_PATH] = {};
  DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app, MAX_PATH);
  if (len == 0 || len >= MAX_PATH)
    return false;

  const std::wstring base_dir = std::wstring(local_app);
  const std::wstring src_db =
      base_dir + L"\\Microsoft\\Windows\\Notifications\\wpndatabase.db";
  const std::wstring src_wal = src_db + L"-wal";
  const std::wstring src_shm = src_db + L"-shm";

  const WpnDbFileStamp db_st = stat_wide_file(src_db);
  if (!db_st.exists || db_st.size == 0)
    return false;
  const WpnDbFileStamp wal_st = stat_wide_file(src_wal);

  {
    std::lock_guard<std::mutex> lk(g_wpn_cache.mu);
    if (g_wpn_cache.valid && g_wpn_cache.db_stamp == db_st &&
        g_wpn_cache.wal_stamp == wal_st) {
      *out = g_wpn_cache.items;
      return true;
    }
  }

  const std::wstring winome_dir = base_dir + L"\\WINOME";
  CreateDirectoryW(winome_dir.c_str(), nullptr);
  const std::wstring cache_dir = winome_dir + L"\\wpn-cache";
  CreateDirectoryW(cache_dir.c_str(), nullptr);

  const std::wstring dst_db = cache_dir + L"\\wpn.db";
  const std::wstring dst_wal = dst_db + L"-wal";
  const std::wstring dst_shm = dst_db + L"-shm";

  if (!CopyFileW(src_db.c_str(), dst_db.c_str(), FALSE))
    return false;
  if (wal_st.exists)
    CopyFileW(src_wal.c_str(), dst_wal.c_str(), FALSE);
  else
    DeleteFileW(dst_wal.c_str());
  if (stat_wide_file(src_shm).exists)
    CopyFileW(src_shm.c_str(), dst_shm.c_str(), FALSE);
  else
    DeleteFileW(dst_shm.c_str());

  const std::string dst_utf8 =
      wide_to_utf8(dst_db.c_str(), static_cast<UINT32>(dst_db.size()));
  if (dst_utf8.empty())
    return false;

  sqlite3* db = nullptr;
  if (sqlite3_open_v2(dst_utf8.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) !=
          SQLITE_OK ||
      db == nullptr) {
    if (db != nullptr)
      sqlite3_close(db);
    return false;
  }

  FILETIME now_ft{};
  GetSystemTimeAsFileTime(&now_ft);
  const int64_t now_100ns =
      (static_cast<int64_t>(now_ft.dwHighDateTime) << 32) |
      static_cast<int64_t>(now_ft.dwLowDateTime);

  static const char* kSql =
      "SELECT n.Id, n.ArrivalTime, COALESCE(h.PrimaryId, ''), n.Payload "
      "FROM Notification n "
      "LEFT JOIN NotificationHandler h ON n.HandlerId = h.RecordId "
      "WHERE n.Type = 'toast' "
      "  AND (n.ExpiryTime = 0 OR n.ExpiryTime > ?1) "
      "ORDER BY n.ArrivalTime DESC "
      "LIMIT 50;";

  sqlite3_stmt* stmt = nullptr;
  std::vector<WinomeSystemNotification> parsed;
  bool query_ok = false;
  if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) == SQLITE_OK &&
      stmt != nullptr) {
    sqlite3_bind_int64(stmt, 1, now_100ns);
    query_ok = true;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const int64_t id_raw = sqlite3_column_int64(stmt, 0);
      const int64_t arrival_raw = sqlite3_column_int64(stmt, 1);
      const auto* primary_ptr =
          reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
      const auto* payload_ptr =
          static_cast<const char*>(sqlite3_column_blob(stmt, 3));
      const int payload_bytes = sqlite3_column_bytes(stmt, 3);
      if (payload_ptr == nullptr || payload_bytes <= 0)
        continue;

      std::string payload(payload_ptr, static_cast<size_t>(payload_bytes));
      WinomeSystemNotification item;
      if (!parse_wpn_toast_xml_full(payload, &item))
        continue;

      const std::string primary = primary_ptr != nullptr ? primary_ptr : "";
      item.id = static_cast<uint32_t>(id_raw);
      item.timestamp_unix = winrt_filetime_to_unix(arrival_raw);
      item.app_id = primary;
      item.app_name = friendly_app_name_from_wpn_id(primary);
      parsed.push_back(std::move(item));
    }
    sqlite3_finalize(stmt);
  }
  sqlite3_close(db);

  if (!query_ok)
    return false;

  {
    std::lock_guard<std::mutex> lk(g_wpn_cache.mu);
    g_wpn_cache.valid = true;
    g_wpn_cache.db_stamp = db_st;
    g_wpn_cache.wal_stamp = wal_st;
    g_wpn_cache.items = parsed;
  }
  *out = std::move(parsed);
  return true;
}

class WinRtWorker {
 public:
  static WinRtWorker& instance() {
    static WinRtWorker w;
    return w;
  }

  bool run(std::function<void()> fn, int wait_ms = 3000) {
    std::unique_lock<std::mutex> lock(mu_);
    if (stopped_)
      return false;
    ensure_started_locked();
    if (!started_)
      return false;

    auto state = std::make_shared<TaskState>();
    state->fn = std::move(fn);
    queue_.push(state);
    cv_.notify_one();

    return state->cv.wait_for(
        lock, std::chrono::milliseconds(wait_ms),
        [&] { return state->done || stopped_; });
  }

  void shutdown() {
    std::thread to_join;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (stopped_)
        return;
      stopped_ = true;
      cv_.notify_all();
      if (thread_.joinable())
        to_join = std::move(thread_);
    }
    if (to_join.joinable())
      to_join.join();
  }

  // Worker-thread cached objects (owned only by the worker MTA thread).
  ComPtr smtc_manager_;
  ComPtr notif_listener_;
  bool notif_access_requested_ = false;
  bool notif_denied_cached_ = false;
  std::chrono::steady_clock::time_point notif_denied_until_{};
  bool toast_allowed_last_ = false;

 private:
  struct TaskState {
    std::function<void()> fn;
    std::condition_variable cv;
    bool done = false;
  };

  WinRtWorker() = default;
  ~WinRtWorker() { shutdown(); }

  void ensure_started_locked() {
    if (started_ || !combase().loaded)
      return;
    started_ = true;
    thread_ = std::thread([this] { worker_loop(); });
  }

  void worker_loop() {
    HRESULT ro_hr = combase().RoInitialize(1 /* RO_INIT_MULTITHREADED */);
    for (;;) {
      std::shared_ptr<TaskState> task;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&] { return stopped_ || !queue_.empty(); });
        if (stopped_ && queue_.empty())
          break;
        task = std::move(queue_.front());
        queue_.pop();
      }
      if (task && task->fn)
        task->fn();
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (task) {
          task->done = true;
          task->cv.notify_all();
        }
      }
    }
    smtc_manager_.reset();
    notif_listener_.reset();
    if (SUCCEEDED(ro_hr))
      combase().RoUninitialize();
  }

  std::mutex mu_;
  std::condition_variable cv_;
  std::queue<std::shared_ptr<TaskState>> queue_;
  std::thread thread_;
  bool started_ = false;
  bool stopped_ = false;
};

ComPtr get_smtc_manager() {
  WinRtWorker& w = WinRtWorker::instance();
  if (w.smtc_manager_)
    return qi(w.smtc_manager_.ptr, kIID_IGSMTCManager);

  ComPtr statics = get_activation_factory(
      L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager",
      kIID_IGSMTCManagerStatics);
  if (!statics)
    return {};

  ComPtr op;
  if (FAILED(vcall<HRESULT>(statics.ptr, 6, op.put())) || !op)
    return {};

  ComPtr mgr_raw;
  if (!await_operation_result(op.ptr, mgr_raw.put(), 2000) || !mgr_raw)
    return {};

  w.smtc_manager_ = qi(mgr_raw.ptr, kIID_IGSMTCManager);
  return qi(w.smtc_manager_.ptr, kIID_IGSMTCManager);
}

ComPtr get_current_smtc_session() {
  ComPtr mgr = get_smtc_manager();
  if (!mgr)
    return {};

  ComPtr raw;
  vcall<HRESULT>(mgr.ptr, 6, raw.put());  // GetCurrentSession
  if (!raw) {
    ComPtr list;
    if (SUCCEEDED(vcall<HRESULT>(mgr.ptr, 7, list.put())) && list) {
      unsigned count = 0;
      if (SUCCEEDED(vcall<HRESULT>(list.ptr, 7, &count)) && count > 0)
        vcall<HRESULT>(list.ptr, 6, 0u, raw.put());  // GetAt(0)
    }
  }
  return qi(raw.ptr, kIID_IGSMTCSession);
}

ComPtr get_notification_listener(bool request_if_needed, bool* access_allowed) {
  if (access_allowed != nullptr)
    *access_allowed = false;

  WinRtWorker& w = WinRtWorker::instance();
  auto now = std::chrono::steady_clock::now();
  if (w.notif_denied_cached_ && now < w.notif_denied_until_) {
    return {};
  }

  if (!w.notif_listener_) {
    ComPtr statics = get_activation_factory(
        L"Windows.UI.Notifications.Management.UserNotificationListener",
        kIID_IUserNotificationListenerStatics);
    if (!statics) {
      capability_set(SubsystemId::WinRtNotifications, CapabilityState::Degraded,
                     "UserNotificationListener WinRT factory unavailable");
      w.notif_denied_cached_ = true;
      w.notif_denied_until_ = now + std::chrono::seconds(60);
      return {};
    }
    ComPtr raw;
    if (FAILED(vcall<HRESULT>(statics.ptr, 6, raw.put())) || !raw) {
      capability_set(SubsystemId::WinRtNotifications, CapabilityState::Degraded,
                     "UserNotificationListener::get_Current failed");
      w.notif_denied_cached_ = true;
      w.notif_denied_until_ = now + std::chrono::seconds(60);
      return {};
    }
    w.notif_listener_ = qi(raw.ptr, kIID_IUserNotificationListener);
    if (!w.notif_listener_) {
      capability_set(SubsystemId::WinRtNotifications, CapabilityState::Degraded,
                     "UserNotificationListener interface unavailable");
      w.notif_denied_cached_ = true;
      w.notif_denied_until_ = now + std::chrono::seconds(60);
      return {};
    }
  }

  int32_t access = kNotifAccessUnspecified;
  if (FAILED(vcall<HRESULT>(w.notif_listener_.ptr, 7, &access))) {
    capability_set(SubsystemId::WinRtNotifications, CapabilityState::Degraded,
                   "GetAccessStatus query failed");
    return {};
  }

  if (access == kNotifAccessUnspecified && request_if_needed &&
      !w.notif_access_requested_) {
    w.notif_access_requested_ = true;
    ComPtr op;
    if (SUCCEEDED(vcall<HRESULT>(w.notif_listener_.ptr, 6, op.put())) && op) {
      int32_t req_res = kNotifAccessUnspecified;
      if (await_operation_result(op.ptr, &req_res, 1500))
        access = req_res;
    }
  }

  if (access != kNotifAccessAllowed) {
    w.notif_denied_cached_ = true;
    w.notif_denied_until_ = now + std::chrono::seconds(60);
    capability_set(SubsystemId::WinRtNotifications, CapabilityState::Degraded,
                   "Toast listener access denied (privacy policy or unpackaged identity)");
    return {};
  }

  w.notif_denied_cached_ = false;
  capability_set(SubsystemId::WinRtNotifications, CapabilityState::Active);
  if (access_allowed != nullptr)
    *access_allowed = true;
  return qi(w.notif_listener_.ptr, kIID_IUserNotificationListener);
}

bool bluetooth_radio_op(bool do_set, bool target_enabled,
                        bool* out_present, bool* out_enabled) {
  ComPtr statics = get_activation_factory(L"Windows.Devices.Radios.Radio",
                                          kIID_IRadioStatics);
  if (!statics)
    return false;

  if (do_set) {
    ComPtr access_op;
    if (SUCCEEDED(vcall<HRESULT>(statics.ptr, 9, access_op.put())) && access_op) {
      int32_t access = 0;
      await_operation_result(access_op.ptr, &access, 1500);
    }
  }

  ComPtr list_op;
  if (FAILED(vcall<HRESULT>(statics.ptr, 6, list_op.put())) || !list_op)
    return false;

  ComPtr vec;
  if (!await_operation_result(list_op.ptr, vec.put(), 2000) || !vec)
    return false;

  unsigned count = 0;
  if (FAILED(vcall<HRESULT>(vec.ptr, 7, &count)))  // IVectorView::get_Size
    return false;

  bool found = false;
  bool any_on = false;
  bool set_ok = false;

  for (unsigned i = 0; i < count; ++i) {
    ComPtr raw;
    if (FAILED(vcall<HRESULT>(vec.ptr, 6, i, raw.put())) || !raw)
      continue;
    ComPtr radio = qi(raw.ptr, kIID_IRadio);
    if (!radio)
      continue;

    int32_t kind = 0;
    if (FAILED(vcall<HRESULT>(radio.ptr, 11, &kind)) ||
        kind != kRadioKindBluetooth) {
      continue;
    }

    found = true;
    int32_t state = 0;
    if (SUCCEEDED(vcall<HRESULT>(radio.ptr, 9, &state)) &&
        state == kRadioStateOn) {
      any_on = true;
    }

    if (do_set) {
      ComPtr set_op;
      int32_t desired = target_enabled ? kRadioStateOn : kRadioStateOff;
      if (SUCCEEDED(vcall<HRESULT>(radio.ptr, 6, desired, set_op.put())) &&
          set_op) {
        int32_t status = 0;
        if (await_operation_result(set_op.ptr, &status, 3000) &&
            status == kRadioAccessAllowed) {
          set_ok = true;
        }
      }
    }
  }

  if (out_present != nullptr)
    *out_present = found;
  if (out_enabled != nullptr)
    *out_enabled = any_on;
  return do_set ? set_ok : found;
}

}  // namespace

long winrt_acquire_com_getter_safe(void* owner,
                                   size_t vtable_slot,
                                   void** out,
                                   bool* out_compensated_addref) {
  if (out_compensated_addref != nullptr)
    *out_compensated_addref = false;
  if (out == nullptr)
    return E_POINTER;
  *out = nullptr;
  if (owner == nullptr)
    return E_POINTER;

  void* raw1 = nullptr;
  const HRESULT hr1 = vcall<HRESULT>(owner, vtable_slot, &raw1);
  if (FAILED(hr1) || raw1 == nullptr)
    return hr1;

  vcall<ULONG>(raw1, 1);
  const ULONG rc1 = vcall<ULONG>(raw1, 2);

  void* raw2 = nullptr;
  const HRESULT hr2 = vcall<HRESULT>(owner, vtable_slot, &raw2);
  if (SUCCEEDED(hr2) && raw2 != nullptr) {
    if (raw2 == raw1) {
      vcall<ULONG>(raw1, 1);
      const ULONG rc2 = vcall<ULONG>(raw1, 2);
      if (rc2 > rc1) {
        vcall<ULONG>(raw2, 2);
        *out = raw1;
        return S_OK;
      }
      // Missing AddRef() detected: compensate once so caller Release() leaves
      // the owner's internal reference count intact.
      vcall<ULONG>(raw1, 1);
      if (out_compensated_addref != nullptr)
        *out_compensated_addref = true;
      *out = raw1;
      return S_OK;
    }
    vcall<ULONG>(raw2, 2);
    *out = raw1;
    return S_OK;
  }

  if (rc1 > 1)
    vcall<ULONG>(raw1, 2);
  return SUCCEEDED(hr2) ? E_FAIL : hr2;
}

bool winrt_get_bluetooth_radio(bool* present, bool* enabled) {
  struct State {
    bool ok = false;
    bool present = false;
    bool enabled = false;
  };
  auto st = std::make_shared<State>();
  const bool completed = WinRtWorker::instance().run([st] {
    st->ok = bluetooth_radio_op(false, false, &st->present, &st->enabled);
  });
  if (!completed) {
    capability_record_failure(SubsystemId::WinRtBluetooth,
                              "Bluetooth radio WinRT query timed out");
    if (present != nullptr)
      *present = false;
    if (enabled != nullptr)
      *enabled = false;
    return false;
  }
  if (!combase().loaded) {
    capability_set(SubsystemId::WinRtBluetooth, CapabilityState::Degraded,
                   "combase.dll unavailable");
  } else {
    capability_set(SubsystemId::WinRtBluetooth,
                   st->present ? CapabilityState::Active
                               : CapabilityState::Degraded,
                   st->present ? std::string{} : "No Bluetooth radio adapter");
  }
  if (present != nullptr)
    *present = st->present;
  if (enabled != nullptr)
    *enabled = st->enabled;
  return st->ok;
}

bool winrt_set_bluetooth_radio(bool enabled) {
  struct State {
    bool ok = false;
  };
  auto st = std::make_shared<State>();
  const bool completed = WinRtWorker::instance().run([st, enabled] {
    st->ok = bluetooth_radio_op(true, enabled, nullptr, nullptr);
  }, 4000);
  if (!completed) {
    capability_record_failure(SubsystemId::WinRtBluetooth,
                              "Bluetooth radio toggle timed out");
    return false;
  }
  return st->ok;
}

bool winrt_get_media_info(WinomeMediaInfo* out) {
  if (out == nullptr)
    return false;
  *out = WinomeMediaInfo{};

  struct State {
    bool ok = false;
    bool mgr_ok = false;
    WinomeMediaInfo info;
  };
  auto st = std::make_shared<State>();
  const bool completed = WinRtWorker::instance().run([st] {
    ComPtr mgr = get_smtc_manager();
    st->mgr_ok = static_cast<bool>(mgr);
    if (!mgr)
      return;
    ComPtr session = get_current_smtc_session();
    if (!session)
      return;

    WINOME_HSTRING h_app = nullptr;
    if (SUCCEEDED(vcall<HRESULT>(session.ptr, 6, &h_app)))
      st->info.app_id = consume_hstring(h_app);

    ComPtr pb_raw;
    if (SUCCEEDED(vcall<HRESULT>(session.ptr, 9, pb_raw.put())) && pb_raw) {
      ComPtr pb = qi(pb_raw.ptr, kIID_IGSMTCPlaybackInfo);
      if (pb) {
        int32_t pb_status = 0;
        if (SUCCEEDED(vcall<HRESULT>(pb.ptr, 7, &pb_status)))
          st->info.playing = (pb_status == kPlaybackPlaying);
      }
    }

    ComPtr op;
    if (SUCCEEDED(vcall<HRESULT>(session.ptr, 7, op.put())) && op) {
      ComPtr props_raw;
      if (await_operation_result(op.ptr, props_raw.put(), 1200) && props_raw) {
        ComPtr props = qi(props_raw.ptr, kIID_IGSMTCMediaProps);
        if (props) {
          WINOME_HSTRING h_title = nullptr;
          WINOME_HSTRING h_artist = nullptr;
          WINOME_HSTRING h_album = nullptr;
          if (SUCCEEDED(vcall<HRESULT>(props.ptr, 6, &h_title)))
            st->info.title = consume_hstring(h_title);
          if (SUCCEEDED(vcall<HRESULT>(props.ptr, 9, &h_artist)))
            st->info.artist = consume_hstring(h_artist);
          if (SUCCEEDED(vcall<HRESULT>(props.ptr, 10, &h_album)))
            st->info.album = consume_hstring(h_album);
        }
      }
    }

    if (!st->info.title.empty() || !st->info.artist.empty() ||
        st->info.playing) {
      st->info.active = true;
      st->ok = true;
    }
  }, 2000);

  if (!completed) {
    capability_record_failure(SubsystemId::WinRtSmtc,
                              "WinRT SMTC query timed out");
    return false;
  }
  if (!combase().loaded || !st->mgr_ok) {
    capability_set(SubsystemId::WinRtSmtc, CapabilityState::Degraded,
                   "WinRT SMTC session manager unavailable");
  } else {
    capability_set(SubsystemId::WinRtSmtc, CapabilityState::Active);
  }
  *out = std::move(st->info);
  return st->ok;
}

bool winrt_media_control(WinomeMediaAction action) {
  struct State {
    bool ok = false;
  };
  auto st = std::make_shared<State>();
  const bool completed = WinRtWorker::instance().run([st, action] {
    ComPtr session = get_current_smtc_session();
    if (!session)
      return;

    size_t slot = 20;
    if (action == WinomeMediaAction::Next)
      slot = 16;
    else if (action == WinomeMediaAction::Previous)
      slot = 17;

    ComPtr op;
    if (SUCCEEDED(vcall<HRESULT>(session.ptr, slot, op.put())) && op) {
      uint8_t bool_res = 0;
      if (await_operation_result(op.ptr, &bool_res, 1500))
        st->ok = (bool_res != 0);
    }
  }, 2000);
  if (!completed) {
    capability_record_failure(SubsystemId::WinRtSmtc,
                              "WinRT SMTC control action timed out");
    return false;
  }
  return st->ok;
}

bool winrt_get_notifications(std::vector<WinomeSystemNotification>* out,
                             bool* access_allowed) {
  if (out == nullptr)
    return false;
  out->clear();

  struct State {
    bool ok = false;
    bool allowed = false;
    std::vector<WinomeSystemNotification> list;
  };
  auto st = std::make_shared<State>();

  const bool completed = WinRtWorker::instance().run([st] {
    ComPtr listener = get_notification_listener(true, &st->allowed);
    if (!listener)
      return;

    ComPtr op;
    if (FAILED(vcall<HRESULT>(listener.ptr, 10, kNotificationKindToast, op.put())) ||
        !op) {
      return;
    }

    ComPtr view;
    if (!await_operation_result(op.ptr, view.put(), 1500) || !view)
      return;

    unsigned count = 0;
    if (FAILED(vcall<HRESULT>(view.ptr, 7, &count)))
      return;

    for (unsigned i = 0; i < count; ++i) {
      ComPtr raw_un;
      if (FAILED(vcall<HRESULT>(view.ptr, 6, i, raw_un.put())) || !raw_un)
        continue;
      ComPtr un = qi(raw_un.ptr, kIID_IUserNotification);
      if (!un)
        continue;

      WinomeSystemNotification item;
      vcall<HRESULT>(un.ptr, 8, &item.id);
      int64_t created_100ns = 0;
      if (SUCCEEDED(vcall<HRESULT>(un.ptr, 9, &created_100ns)))
        item.timestamp_unix = winrt_filetime_to_unix(created_100ns);

      ComPtr app_raw;
      if (SUCCEEDED(vcall<HRESULT>(un.ptr, 7, app_raw.put())) && app_raw) {
        ComPtr app = qi(app_raw.ptr, kIID_IAppInfo);
        if (app) {
          WINOME_HSTRING h_aumid = nullptr;
          if (SUCCEEDED(vcall<HRESULT>(app.ptr, 7, &h_aumid)))
            item.app_id = consume_hstring(h_aumid);

          ComPtr disp_raw;
          if (SUCCEEDED(vcall<HRESULT>(app.ptr, 8, disp_raw.put())) && disp_raw) {
            ComPtr disp = qi(disp_raw.ptr, kIID_IAppDisplayInfo);
            if (disp) {
              WINOME_HSTRING h_name = nullptr;
              if (SUCCEEDED(vcall<HRESULT>(disp.ptr, 6, &h_name)))
                item.app_name = consume_hstring(h_name);
            }
          }
        }
      }

      ComPtr notif_raw;
      if (SUCCEEDED(vcall<HRESULT>(un.ptr, 6, notif_raw.put())) && notif_raw) {
        ComPtr notif = qi(notif_raw.ptr, kIID_INotification);
        ComPtr vis_raw;
        if (notif &&
            SUCCEEDED(vcall<HRESULT>(notif.ptr, 8, vis_raw.put())) && vis_raw) {
          ComPtr vis = qi(vis_raw.ptr, kIID_INotificationVisual);
          ComPtr bindings;
          // Windows 11 wpnapps.dll!NotificationVisual::get_Bindings (slot 8)
          // omits AddRef() on its internal m_bindings pointer; use the safe
          // getter wrapper so releasing `bindings` never frees vis's member.
          if (vis &&
              SUCCEEDED(winrt_acquire_com_getter_safe(
                  vis.ptr, 8, bindings.put())) &&
              bindings) {
            unsigned bcount = 0;
            if (SUCCEEDED(vcall<HRESULT>(bindings.ptr, 7, &bcount))) {
              for (unsigned bi = 0; bi < bcount; ++bi) {
                ComPtr b_raw;
                if (FAILED(vcall<HRESULT>(bindings.ptr, 6, bi, b_raw.put())) || !b_raw)
                  continue;
                ComPtr binding = qi(b_raw.ptr, kIID_INotificationBinding);
                ComPtr texts;
                if (!binding ||
                    FAILED(vcall<HRESULT>(binding.ptr, 11, texts.put())) || !texts)
                  continue;
                unsigned tcount = 0;
                if (FAILED(vcall<HRESULT>(texts.ptr, 7, &tcount)))
                  continue;
                for (unsigned ti = 0; ti < tcount; ++ti) {
                  ComPtr t_raw;
                  if (FAILED(vcall<HRESULT>(texts.ptr, 6, ti, t_raw.put())) || !t_raw)
                    continue;
                  ComPtr txt = qi(t_raw.ptr, kIID_IAdaptiveNotificationText);
                  if (!txt)
                    continue;
                  WINOME_HSTRING h_line = nullptr;
                  if (SUCCEEDED(vcall<HRESULT>(txt.ptr, 6, &h_line))) {
                    std::string line = consume_hstring(h_line);
                    if (line.empty())
                      continue;
                    if (item.title.empty()) {
                      item.title = std::move(line);
                    } else {
                      if (!item.body.empty())
                        item.body += "\n";
                      item.body += line;
                    }
                  }
                }
              }
            }
          }
        }
      }

      if (!item.title.empty() || !item.body.empty())
        st->list.push_back(std::move(item));
    }
    st->ok = true;
  }, 2000);

  if (!completed) {
    capability_record_failure(SubsystemId::WinRtNotifications,
                              "WinRT UserNotificationListener timed out");
    if (read_wpn_database_fallback(out)) {
      capability_set(SubsystemId::WinRtNotifications, CapabilityState::Active,
                     "WPN database read-only fallback");
      if (access_allowed != nullptr)
        *access_allowed = true;
      return true;
    }
    if (access_allowed != nullptr)
      *access_allowed = false;
    return false;
  }
  if (!st->ok || !st->allowed) {
    if (read_wpn_database_fallback(out)) {
      capability_set(SubsystemId::WinRtNotifications, CapabilityState::Active,
                     "WPN database read-only fallback");
      if (access_allowed != nullptr)
        *access_allowed = true;
      return true;
    }
  }
  if (access_allowed != nullptr)
    *access_allowed = st->allowed;

  // Enrich live WinRT notifications with WPN XML launch arguments, protocol
  // URIs, action buttons, and handler PrimaryId (UserNotificationListener does
  // not expose <toast launch="..."> or <action .../> via WinRT interfaces).
  if (!st->list.empty()) {
    std::vector<WinomeSystemNotification> wpn_items;
    if (read_wpn_database_fallback(&wpn_items) && !wpn_items.empty()) {
      std::unordered_map<uint32_t, const WinomeSystemNotification*> by_id;
      by_id.reserve(wpn_items.size());
      for (const auto& w : wpn_items) {
        if (w.id != 0)
          by_id[w.id] = &w;
      }
      for (auto& item : st->list) {
        auto it = by_id.find(item.id);
        if (it != by_id.end()) {
          const WinomeSystemNotification* w = it->second;
          if (item.app_id.empty())
            item.app_id = w->app_id;
          item.launch_args = w->launch_args;
          item.activation_type = w->activation_type;
          item.actions = w->actions;
          item.url = w->url;
        }
        if (item.url.empty()) {
          item.url = extract_notification_url(item.launch_args,
                                              item.activation_type,
                                              item.title, item.body);
        }
      }
    } else {
      for (auto& item : st->list) {
        item.url = extract_notification_url(item.launch_args,
                                            item.activation_type,
                                            item.title, item.body);
      }
    }
  }

  *out = std::move(st->list);
  return st->ok;
}

namespace {

std::string extract_xml_attr(const std::string& attrs, const char* name) {
  if (attrs.empty() || name == nullptr || *name == '\0')
    return {};
  const std::string lower_attrs = ascii_lower(attrs);
  const std::string lower_name = ascii_lower(name);
  size_t pos = 0;
  while (pos < lower_attrs.size()) {
    const size_t found = lower_attrs.find(lower_name, pos);
    if (found == std::string::npos)
      return {};
    const bool left_ok =
        found == 0 ||
        std::isspace(static_cast<unsigned char>(lower_attrs[found - 1]));
    size_t i = found + lower_name.size();
    while (i < lower_attrs.size() &&
           std::isspace(static_cast<unsigned char>(lower_attrs[i]))) {
      ++i;
    }
    if (!left_ok || i >= lower_attrs.size() || lower_attrs[i] != '=') {
      pos = found + lower_name.size();
      continue;
    }
    ++i;
    while (i < attrs.size() &&
           std::isspace(static_cast<unsigned char>(attrs[i]))) {
      ++i;
    }
    if (i >= attrs.size() || (attrs[i] != '"' && attrs[i] != '\'')) {
      pos = i;
      continue;
    }
    const char quote = attrs[i++];
    const size_t end = attrs.find(quote, i);
    if (end == std::string::npos)
      return {};
    return trim_whitespace(unescape_xml_entities(attrs.substr(i, end - i)));
  }
  return {};
}

std::string find_first_http_url(const std::string& text) {
  if (text.empty())
    return {};
  const std::string low = ascii_lower(text);
  const size_t p_https = low.find("https://");
  const size_t p_http = low.find("http://");
  size_t start = std::string::npos;
  if (p_https != std::string::npos && p_http != std::string::npos)
    start = std::min(p_https, p_http);
  else if (p_https != std::string::npos)
    start = p_https;
  else
    start = p_http;

  if (start == std::string::npos)
    return {};

  size_t end = start;
  while (end < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[end]);
    if (std::isspace(c) || c == '<' || c == '>' || c == '"' || c == '\'')
      break;
    ++end;
  }
  while (end > start) {
    const char tail = text[end - 1];
    if (tail == '.' || tail == ',' || tail == ';' || tail == ')' ||
        tail == ']' || tail == '}' || tail == '!' || tail == '?') {
      --end;
    } else {
      break;
    }
  }
  const std::string url = text.substr(start, end - start);
  return url.size() > 8 ? url : std::string{};
}

std::wstring resolve_win32_aumid_path(const std::string& app_id) {
  if (app_id.empty())
    return {};
  const std::wstring w = utf8_to_wide(app_id);
  if (w.empty())
    return {};

  // Windows WPN NotificationHandler.PrimaryId uses KnownFolder GUID prefixes
  // for desktop Win32 apps, e.g. {6D809377-6AF0-444B-8957-A3773F02200E}\App\app.exe
  if (w.size() >= 39 && w[0] == L'{') {
    const size_t close = w.find(L'}');
    if (close == 37) {
      const std::wstring guid_str = w.substr(0, 38);
      GUID kf_guid{};
      if (SUCCEEDED(CLSIDFromString(guid_str.c_str(), &kf_guid))) {
        PWSTR kf_path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(kf_guid, 0, nullptr, &kf_path)) &&
            kf_path != nullptr) {
          std::wstring full(kf_path);
          CoTaskMemFree(kf_path);
          std::wstring rest = w.substr(38);
          if (!rest.empty() && rest[0] != L'\\' && !full.empty() &&
              full.back() != L'\\') {
            full += L'\\';
          }
          full += rest;
          if (GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES)
            return full;
        }
      }
    }
  }

  if (w.find(L'\\') != std::wstring::npos &&
      GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES) {
    return w;
  }
  return {};
}

bool focus_window_by_exe_path(const std::wstring& exe_path) {
  if (exe_path.empty())
    return false;
  std::wstring target_base = exe_path;
  const size_t slash = target_base.find_last_of(L"\\/");
  if (slash != std::wstring::npos && slash + 1 < target_base.size())
    target_base = target_base.substr(slash + 1);

  struct MatchCtx {
    std::wstring full_path;
    std::wstring base_name;
    HWND found = nullptr;
  } ctx{exe_path, target_base, nullptr};

  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<MatchCtx*>(lp);
        if (!IsWindowVisible(hwnd))
          return TRUE;
        const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if ((ex & WS_EX_TOOLWINDOW) != 0)
          return TRUE;
        if (GetWindow(hwnd, GW_OWNER) != nullptr)
          return TRUE;

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == 0 || pid == GetCurrentProcessId())
          return TRUE;

        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (proc == nullptr)
          return TRUE;
        wchar_t buf[MAX_PATH * 2] = {};
        DWORD sz = MAX_PATH * 2;
        const bool got = QueryFullProcessImageNameW(proc, 0, buf, &sz) != FALSE;
        CloseHandle(proc);
        if (!got || buf[0] == L'\0')
          return TRUE;

        bool match = (_wcsicmp(buf, c->full_path.c_str()) == 0);
        if (!match && !c->base_name.empty()) {
          const wchar_t* last = wcsrchr(buf, L'\\');
          const wchar_t* b = last != nullptr ? last + 1 : buf;
          match = (_wcsicmp(b, c->base_name.c_str()) == 0);
        }
        if (match) {
          c->found = hwnd;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));

  if (ctx.found == nullptr)
    return false;
  if (IsIconic(ctx.found))
    ShowWindow(ctx.found, SW_RESTORE);
  DWORD pid = 0;
  GetWindowThreadProcessId(ctx.found, &pid);
  if (pid != 0)
    AllowSetForegroundWindow(pid);
  SetForegroundWindow(ctx.found);
  return true;
}

bool shell_open_wide(const std::wstring& target) {
  if (target.empty())
    return false;
  const HINSTANCE res = ShellExecuteW(nullptr, L"open", target.c_str(),
                                      nullptr, nullptr, SW_SHOWNORMAL);
  return reinterpret_cast<INT_PTR>(res) > 32;
}

}  // namespace

bool is_safe_protocol_uri(const std::string& value,
                          const std::string& activation_type) {
  const std::string s = trim_whitespace(value);
  if (s.size() < 4)
    return false;
  for (char c : s) {
    if (c == '\r' || c == '\n')
      return false;
  }

  const size_t colon = s.find(':');
  if (colon == std::string::npos || colon < 2 || colon > 32)
    return false;
  if (!std::isalpha(static_cast<unsigned char>(s[0])))
    return false;
  for (size_t i = 1; i < colon; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (!std::isalnum(c) && c != '+' && c != '-' && c != '.')
      return false;
  }

  const std::string scheme = ascii_lower(s.substr(0, colon));
  // Never treat local file/script/powershell schemes as click-through URIs.
  if (scheme == "file" || scheme == "javascript" || scheme == "vbscript" ||
      scheme == "cmd" || scheme == "powershell") {
    return false;
  }

  const std::string act = ascii_lower(trim_whitespace(activation_type));
  if (act == "protocol")
    return true;

  if (scheme == "http" || scheme == "https")
    return s.compare(colon, 3, "://") == 0 && s.size() > colon + 3;
  if (scheme == "mailto" || scheme == "ms-settings" ||
      scheme == "ms-windows-store" || scheme == "windowsdefender" ||
      scheme == "ms-clock" || scheme == "ms-actioncenter") {
    return s.size() > colon + 1;
  }
  // Custom app deep-link protocols (e.g. vscode://, tg://, discord://, steam://).
  if (s.compare(colon, 3, "://") == 0 && s.size() > colon + 3)
    return true;
  return false;
}

std::string extract_notification_url(const std::string& launch_args,
                                     const std::string& activation_type,
                                     const std::string& title,
                                     const std::string& body) {
  const std::string trimmed_launch = trim_whitespace(launch_args);
  if (is_safe_protocol_uri(trimmed_launch, activation_type))
    return trimmed_launch;
  std::string url = find_first_http_url(title);
  if (!url.empty())
    return url;
  return find_first_http_url(body);
}

bool parse_wpn_toast_xml_full(const std::string& xml,
                              WinomeSystemNotification* out) {
  if (out == nullptr || xml.empty())
    return false;
  out->title.clear();
  out->body.clear();
  out->launch_args.clear();
  out->activation_type.clear();
  out->url.clear();
  out->actions.clear();

  // 1. Parse <toast launch="..." activationType="...">
  const size_t toast_open = xml.find("<toast");
  if (toast_open != std::string::npos) {
    const size_t toast_end = xml.find('>', toast_open + 6);
    if (toast_end != std::string::npos) {
      const std::string attrs =
          xml.substr(toast_open + 6, toast_end - (toast_open + 6));
      out->launch_args = extract_xml_attr(attrs, "launch");
      out->activation_type = extract_xml_attr(attrs, "activationType");
    }
  }

  // 2. Parse visible <text> lines.
  if (!parse_wpn_toast_xml(xml, &out->title, &out->body))
    return false;

  // 3. Parse interactive <action .../> buttons (skip system & contextMenu).
  size_t pos = 0;
  while (pos < xml.size() && out->actions.size() < 3) {
    const size_t open = xml.find("<action", pos);
    if (open == std::string::npos)
      break;
    const size_t after = open + 7;
    if (after >= xml.size() ||
        (!std::isspace(static_cast<unsigned char>(xml[after])) &&
         xml[after] != '>' && xml[after] != '/')) {
      pos = after;
      continue;
    }
    const size_t end = xml.find('>', after);
    if (end == std::string::npos)
      break;
    pos = end + 1;

    const std::string attrs = xml.substr(after, end - after);
    const std::string placement = ascii_lower(extract_xml_attr(attrs, "placement"));
    if (placement == "contextmenu")
      continue;
    const std::string act_type =
        ascii_lower(extract_xml_attr(attrs, "activationType"));
    if (act_type == "system" || act_type == "background")
      continue;

    const std::string label = extract_xml_attr(attrs, "content");
    const std::string args = extract_xml_attr(attrs, "arguments");
    if (label.empty())
      continue;

    WinomeNotificationAction action;
    action.label = label;
    action.arguments = args;
    action.activation_type = act_type.empty() ? "foreground" : act_type;
    out->actions.push_back(std::move(action));
  }

  out->url = extract_notification_url(out->launch_args, out->activation_type,
                                      out->title, out->body);
  return true;
}

bool winrt_activate_notification(const std::string& app_id,
                                 const std::string& app_name,
                                 const std::string& launch_args,
                                 const std::string& activation_type,
                                 const std::string& fallback_url) {
  (void)app_name;

  // 1. Direct protocol / deep-link URI (e.g. https://, ms-settings:, vscode://).
  const std::string trimmed_launch = trim_whitespace(launch_args);
  if (is_safe_protocol_uri(trimmed_launch, activation_type)) {
    if (shell_open_wide(utf8_to_wide(trimmed_launch)))
      return true;
  }

  // 2. Standard Windows Action Center COM activation (passes launch arguments
  // directly to UWP / MSIX / AUMID-registered desktop apps).
  const std::wstring exe_path = resolve_win32_aumid_path(app_id);
  const bool looks_like_file_path =
      !exe_path.empty() || app_id.find('\\') != std::string::npos ||
      app_id.find('/') != std::string::npos;

  if (!app_id.empty() && !looks_like_file_path) {
    const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool need_uninit = SUCCEEDED(co_hr);
    IApplicationActivationManager* aam = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr,
                                  CLSCTX_LOCAL_SERVER,
                                  IID_IApplicationActivationManager,
                                  reinterpret_cast<void**>(&aam));
    bool activated = false;
    if (SUCCEEDED(hr) && aam != nullptr) {
      const std::wstring aumid_w = utf8_to_wide(app_id);
      const std::wstring args_w = utf8_to_wide(trimmed_launch);
      DWORD pid = 0;
      hr = aam->ActivateApplication(
          aumid_w.c_str(),
          args_w.empty() ? nullptr : args_w.c_str(),
          AO_NONE, &pid);
      aam->Release();
      if (SUCCEEDED(hr)) {
        if (pid != 0)
          AllowSetForegroundWindow(pid);
        activated = true;
      }
    }
    if (need_uninit)
      CoUninitialize();
    if (activated)
      return true;
  }

  // 3. Win32 executable path (KnownFolder-prefixed or full path): focus an
  // existing window if visible, otherwise launch the exe (wakes tray apps).
  if (!exe_path.empty()) {
    if (focus_window_by_exe_path(exe_path))
      return true;
    if (shell_open_wide(exe_path))
      return true;
  }

  // 4. Fallback to shell:AppsFolder\<AUMID> for registered apps.
  if (!app_id.empty() && !looks_like_file_path) {
    const std::wstring apps_uri = L"shell:AppsFolder\\" + utf8_to_wide(app_id);
    if (shell_open_wide(apps_uri))
      return true;
  }

  // 5. Fallback to an HTTP(S) / protocol link extracted from notification text.
  if (is_safe_protocol_uri(fallback_url, "")) {
    if (shell_open_wide(utf8_to_wide(fallback_url)))
      return true;
  }

  return false;
}

bool parse_wpn_toast_xml(const std::string& xml,
                         std::string* out_title,
                         std::string* out_body) {
  if (out_title != nullptr)
    out_title->clear();
  if (out_body != nullptr)
    out_body->clear();
  if (xml.empty())
    return false;

  std::string title;
  std::string body;
  size_t pos = 0;
  while (pos < xml.size()) {
    const size_t open = xml.find("<text", pos);
    if (open == std::string::npos)
      break;
    const size_t after_name = open + 5;
    if (after_name >= xml.size() ||
        (xml[after_name] != '>' && xml[after_name] != '/' &&
         !std::isspace(static_cast<unsigned char>(xml[after_name])))) {
      pos = after_name;
      continue;
    }
    const size_t tag_end = xml.find('>', after_name);
    if (tag_end == std::string::npos)
      break;
    if (xml[tag_end - 1] == '/') {
      pos = tag_end + 1;
      continue;
    }
    const std::string attrs = xml.substr(after_name, tag_end - after_name);
    const bool is_attribution =
        attrs.find("attribution") != std::string::npos;

    const size_t close = xml.find("</text>", tag_end + 1);
    if (close == std::string::npos)
      break;
    pos = close + 7;

    if (is_attribution)
      continue;

    const std::string content =
        trim_whitespace(unescape_xml_entities(xml.substr(tag_end + 1, close - (tag_end + 1))));
    if (content.empty())
      continue;
    if (title.empty()) {
      title = content;
    } else {
      if (!body.empty())
        body += "\n";
      body += content;
    }
  }

  if (title.empty() && body.empty())
    return false;
  if (out_title != nullptr)
    *out_title = std::move(title);
  if (out_body != nullptr)
    *out_body = std::move(body);
  return true;
}

std::string friendly_app_name_from_wpn_id(const std::string& primary_id) {
  if (primary_id.empty())
    return "Windows";

  std::string s = primary_id;
  // Strip UWP entry point suffix (!App) and publisher hash (_8wekyb3d8bbwe).
  const size_t bang = s.find('!');
  if (bang != std::string::npos)
    s = s.substr(0, bang);
  const size_t under = s.rfind('_');
  if (under != std::string::npos && s.size() - under >= 10) {
    bool alnum = true;
    for (size_t i = under + 1; i < s.size(); ++i) {
      if (!std::isalnum(static_cast<unsigned char>(s[i]))) {
        alnum = false;
        break;
      }
    }
    if (alnum)
      s = s.substr(0, under);
  }

  // Extract executable basename for Win32 path-based AUMIDs.
  const size_t slash = s.find_last_of("\\/");
  if (slash != std::string::npos && slash + 1 < s.size())
    s = s.substr(slash + 1);

  if (s.size() > 4) {
    std::string tail = s.substr(s.size() - 4);
    for (char& c : tail)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (tail == ".exe")
      s.resize(s.size() - 4);
  }

  // Strip common reverse-DNS / vendor prefixes (e.g. electron.app.X, Microsoft.X).
  if (s.rfind("electron.app.", 0) == 0 && s.size() > 13)
    s = s.substr(13);
  else if (s.rfind("Microsoft.Windows.", 0) == 0 && s.size() > 18)
    s = s.substr(18);
  else if (s.rfind("Microsoft.", 0) == 0 && s.size() > 10)
    s = s.substr(10);
  else if (s.rfind("Windows.SystemToast.", 0) == 0 && s.size() > 20)
    s = s.substr(20);

  if (s.size() > 10 && s.substr(s.size() - 10) == "ForWindows")
    s.resize(s.size() - 10);

  // If still dotted reverse-DNS (e.g. com.squirrel.Discord.Discord), take tail.
  const size_t dot = s.rfind('.');
  if (dot != std::string::npos && dot + 1 < s.size())
    s = s.substr(dot + 1);

  return s.empty() ? "Windows" : s;
}

bool winrt_remove_notification(uint32_t id) {
  struct State {
    bool ok = false;
    HRESULT hr = E_FAIL;
  };
  auto st = std::make_shared<State>();
  const bool completed = WinRtWorker::instance().run([st, id] {
    bool allowed = false;
    ComPtr listener = get_notification_listener(true, &allowed);
    if (listener && allowed) {
      st->hr = vcall<HRESULT>(listener.ptr, 13, id);
      st->ok = SUCCEEDED(st->hr);
    }
  }, 2000);
  {
    std::lock_guard<std::mutex> lk(g_wpn_cache.mu);
    g_wpn_cache.valid = false;
  }
  return completed && st->ok;
}

bool winrt_clear_notifications() {
  struct State {
    bool ok = false;
  };
  auto st = std::make_shared<State>();
  const bool completed = WinRtWorker::instance().run([st] {
    bool allowed = false;
    ComPtr listener = get_notification_listener(true, &allowed);
    if (!listener || !allowed)
      return;

    // On Windows 11, IUserNotificationListener::ClearNotifications (slot 12)
    // frequently no-ops for unpackaged listeners; enumerate active toast IDs
    // and remove each via RemoveNotification (slot 13) first, then call
    // ClearNotifications as belt-and-suspenders.
    bool removed_any = false;
    bool all_removed = true;
    ComPtr op;
    if (SUCCEEDED(vcall<HRESULT>(listener.ptr, 10, kNotificationKindToast,
                                 op.put())) &&
        op) {
      ComPtr view;
      if (await_operation_result(op.ptr, view.put(), 1500) && view) {
        unsigned count = 0;
        if (SUCCEEDED(vcall<HRESULT>(view.ptr, 7, &count))) {
          std::vector<uint32_t> ids;
          ids.reserve(count);
          for (unsigned i = 0; i < count; ++i) {
            ComPtr raw_un;
            if (FAILED(vcall<HRESULT>(view.ptr, 6, i, raw_un.put())) || !raw_un)
              continue;
            ComPtr un = qi(raw_un.ptr, kIID_IUserNotification);
            if (!un)
              continue;
            uint32_t nid = 0;
            if (SUCCEEDED(vcall<HRESULT>(un.ptr, 8, &nid)) && nid != 0)
              ids.push_back(nid);
          }
          for (uint32_t nid : ids) {
            if (SUCCEEDED(vcall<HRESULT>(listener.ptr, 13, nid)))
              removed_any = true;
            else
              all_removed = false;
          }
        }
      }
    }

    const HRESULT clear_hr = vcall<HRESULT>(listener.ptr, 12);
    st->ok = SUCCEEDED(clear_hr) || (removed_any && all_removed);
  }, 3000);
  {
    std::lock_guard<std::mutex> lk(g_wpn_cache.mu);
    g_wpn_cache.valid = false;
  }
  return completed && st->ok;
}

void winrt_shutdown() {
  WinRtWorker::instance().shutdown();
}

}  // namespace winome
