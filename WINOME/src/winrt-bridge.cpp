// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Dynamic combase.dll + vtable-slot WinRT bridge executed on a dedicated MTA
// worker thread so it never deadlocks or conflicts with GTK's STA thread.

#include "winrt-bridge.h"
#include "perf-metrics.h"

#include <windows.h>

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
    if (access_allowed != nullptr)
      *access_allowed = false;
    return false;
  }
  if (access_allowed != nullptr)
    *access_allowed = st->allowed;
  *out = std::move(st->list);
  return st->ok;
}

bool winrt_remove_notification(uint32_t id) {
  struct State {
    bool ok = false;
  };
  auto st = std::make_shared<State>();
  const bool completed = WinRtWorker::instance().run([st, id] {
    bool allowed = false;
    ComPtr listener = get_notification_listener(false, &allowed);
    if (listener && allowed)
      st->ok = SUCCEEDED(vcall<HRESULT>(listener.ptr, 13, id));
  }, 1500);
  return completed && st->ok;
}

bool winrt_clear_notifications() {
  struct State {
    bool ok = false;
  };
  auto st = std::make_shared<State>();
  const bool completed = WinRtWorker::instance().run([st] {
    bool allowed = false;
    ComPtr listener = get_notification_listener(false, &allowed);
    if (listener && allowed)
      st->ok = SUCCEEDED(vcall<HRESULT>(listener.ptr, 12));
  }, 1500);
  return completed && st->ok;
}

void winrt_shutdown() {
  WinRtWorker::instance().shutdown();
}

}  // namespace winome
