// SPDX-License-Identifier: MIT
// Origin: VirtualDesktopAccessor-rust
// <https://github.com/Ciantic/VirtualDesktopAccessor> (MIT),
// src/interfaces.rs + src/comobjects.rs, ported for WINOME 2026-08-19.
//
// The interface LAYOUT below mirrors the upstream Rust definitions 1:1
// (verified against Win11 22621 in-process: QueryService(53F5CA0B) resolves,
// GetDesktops is single-argument on patched 23H2, 3 desktops enumerated).
//
// CALLING CONVENTION NOTE: every COM method is invoked through a RAW VTABLE
// CALL (function pointer read from the object's vtable), never through C++
// virtual dispatch. GCC -O2 devirtualized the virtual calls on these
// all-pure-virtual interface structs (no derived class exists in the TU, so
// GCC substituted __cxa_pure_virtual), and the MinGW linker resolved that
// weak symbol to a garbage absolute address (0x100000000) — the process then
// jumped 256MB below its own image base and died at a module base
// (SIGSEGV, rip == some DLL's base). Raw calls cannot be devirtualized.
//
// THREAD MODEL (upstream comobjects.rs): "Virtual Desktop COM Objects don't
// like being called from different threads rapidly, something goes wrong."
// All calls are marshalled to a single dedicated worker thread; the caller
// waits with a hard timeout so an explorer-side deadlock can never freeze
// the GTK main loop.

#include "virtual-desktop.h"
#include "perf-metrics.h"

#include <glib.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace winome {
namespace vd {

bool
VdReconnectPolicy::can_attempt (uint64_t now_ms) const
{
  return now_ms >= backoff_until_ms;
}

void
VdReconnectPolicy::record_success ()
{
  ever_succeeded = true;
  consecutive_failures = 0;
  backoff_until_ms = 0;
}

uint64_t
VdReconnectPolicy::record_failure (uint64_t now_ms)
{
  static constexpr uint64_t kStepsMs[] = {2000, 5000, 15000, 30000};
  ++consecutive_failures;
  const size_t idx = std::min<size_t> (consecutive_failures - 1, 3);
  const uint64_t delay = kStepsMs[idx];
  backoff_until_ms = now_ms + delay;
  return delay;
}

void
VdReconnectPolicy::reset_backoff ()
{
  consecutive_failures = 0;
  backoff_until_ms = 0;
}

namespace {

std::atomic<bool> g_reconnect_reset_requested{false};

// --- GUIDs (upstream interfaces.rs) -----------------------------------------

constexpr GUID kCLSID_ImmersiveShell = {
    0xC2F03A33, 0x21F5, 0x47FA,
    {0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39}};
constexpr GUID kCLSID_VirtualDesktopManagerInternal = {
    0xC5E0CDCA, 0x7B6E, 0x41B2,
    {0x9F, 0xC4, 0xD9, 0x39, 0x75, 0xCC, 0x46, 0x7B}};
constexpr GUID kIID_IServiceProvider = {
    0x6D5140C1, 0x7436, 0x11CE,
    {0x80, 0x34, 0x00, 0xAA, 0x00, 0x60, 0x09, 0xFA}};
constexpr GUID kIID_IVirtualDesktopManager = {
    0xA5CD92FF, 0x29BE, 0x454C,
    {0x8D, 0x04, 0xD8, 0x28, 0x79, 0xFB, 0x3F, 0x1B}};
constexpr GUID kIID_IVirtualDesktopManagerInternal = {
    0x53F5CA0B, 0x158F, 0x4124,
    {0x90, 0x0C, 0x05, 0x71, 0x58, 0x06, 0x0B, 0x27}};
constexpr GUID kIID_IVirtualDesktop = {
    0x3F07F4BE, 0xB107, 0x441A,
    {0xAF, 0x0F, 0x39, 0xD8, 0x25, 0x29, 0x07, 0x2C}};
constexpr GUID kIID_IObjectArray = {
    0x92CA9DCD, 0x5622, 0x4BBA,
    {0xA8, 0x05, 0x5E, 0x9F, 0x54, 0x1B, 0xD8, 0xC9}};

// Pinning windows to all desktops (Win11): IVirtualDesktopPinnedApps off the
// ImmersiveShell provider (service CLSID_VirtualDesktopPinnedApps), fed an
// IApplicationView resolved through IApplicationViewCollection (queried with
// its own IID as the service id). Method order verified against
// MScholtes/VirtualDesktop11.cs and Ciantic/VirtualDesktopAccessor.
constexpr GUID kCLSID_VirtualDesktopPinnedApps = {
    0xB5A399E7, 0x1C87, 0x46B8,
    {0x88, 0xE9, 0xFC, 0x57, 0x47, 0xB1, 0x71, 0xBD}};
constexpr GUID kIID_IVirtualDesktopPinnedApps = {
    0x4CE81583, 0x1E4C, 0x4632,
    {0xA6, 0x21, 0x07, 0xA5, 0x35, 0x43, 0x14, 0x8F}};
constexpr GUID kIID_IApplicationViewCollection = {
    0x1841C6D7, 0x4F9D, 0x42C0,
    {0xAF, 0x41, 0x87, 0x47, 0x53, 0x8F, 0x10, 0xE5}};

// --- raw vtable calls ----------------------------------------------------------

// slot 3 = IUnknown::QueryInterface/AddRef/Release are 0/1/2 for every
// interface; the method slots below follow the upstream vtable order.
//
// LAYOUT NOTE (this is why "24H2+" matters): the interface IID is identical
// on 22621 and 24H2 (53F5CA0B), but 24H2 INSERTED
// `switch_desktop_and_move_foreground_view` at slot 10 — everything from
// slot 10 onward is shifted +1 on 24H2 vs 22621 (e.g. find_desktop is
// slot 13 on 22621, slot 14 on 24H2). Calling a shifted slot on the wrong
// build never returns (the explorer service hangs on the mismatched
// method). Only slots BEFORE the shift (get_desktops=7,
// get_current_desktop=6, switch_desktop=9) are layout-stable, so winome
// sticks to them: enumeration indexes desktops directly and never uses
// find_desktop.
constexpr int kSlotQueryService = 3;   // IServiceProvider
constexpr int kSlotIsOnCurrent = 3;    // IVirtualDesktopManager
constexpr int kSlotDesktopByWindow = 4;
constexpr int kSlotMoveViewToDesktop = 4; // IVirtualDesktopManagerInternal
constexpr int kSlotGetDesktops = 7;
constexpr int kSlotGetCurrentDesktop = 6;
constexpr int kSlotSwitchDesktop = 9;
constexpr int kSlotGetId = 4;          // IVirtualDesktop
constexpr int kSlotGetCount = 3;       // IObjectArray
constexpr int kSlotGetAt = 4;
constexpr int kSlotGetViewForHwnd = 6; // IApplicationViewCollection
constexpr int kSlotPinView = 7;        // IVirtualDesktopPinnedApps
constexpr int kSlotUnpinView = 8;      // IVirtualDesktopPinnedApps

// Call method @slot on COM interface @iface. The function pointer is read
// from the object's vtable at runtime — GCC cannot devirtualize this.
template <typename R, typename... Args>
inline R
vcall (void *iface, int slot, Args... args)
{
  void **vt = *reinterpret_cast<void ***> (iface);
  auto fn = reinterpret_cast<R (STDMETHODCALLTYPE *) (void *, Args...)> (
      vt[slot]);
  return fn (iface, args...);
}

inline ULONG
vrelease (void *iface)
{
  return vcall<ULONG> (iface, 2);
}

// --- the dedicated COM worker thread ------------------------------------------

struct Job {
  enum class Op {
    Desktops,
    CurrentIndex,
    Snapshot,
    WindowDesktopIndex,
    WindowOnCurrent,
    SwitchTo,
    PinWindow,
    UnpinWindow,
    MoveWindowToDesktop,
    MoveWindowsBatch,
    CreateDesktop,
    RemoveDesktop,
  } op;

  std::vector<DesktopInfo> out_desktops;
  std::vector<WindowMoveOp> in_moves;
  std::vector<bool> out_move_results;
  int out_index = -1;
  bool out_bool = false;
  HWND in_hwnd = nullptr;
  int in_index = 0;
  bool ok = false;
  bool done = false;
};

// COM state — only ever touched on the worker thread.
struct ComState {
  void *provider = nullptr;        // IServiceProvider
  void *internal = nullptr;        // IVirtualDesktopManagerInternal
  void *manager = nullptr;         // IVirtualDesktopManager
  void *pinned_apps = nullptr;     // IVirtualDesktopPinnedApps
  void *view_collection = nullptr; // IApplicationViewCollection
  VdReconnectPolicy reconnect;
  bool pin_disabled = false;       // pin services unreachable; pin only, not the
                                   // rest of the module

  void
  release (bool drop_provider = false)
  {
    if (internal != nullptr) {
      vrelease (internal);
      internal = nullptr;
    }
    if (manager != nullptr) {
      vrelease (manager);
      manager = nullptr;
    }
    if (view_collection != nullptr) {
      vrelease (view_collection);
      view_collection = nullptr;
    }
    if (pinned_apps != nullptr) {
      vrelease (pinned_apps);
      pinned_apps = nullptr;
    }
    // Only drop provider when actively reconnecting (never at process exit,
    // where releasing after combase TLS teardown can crash).
    if (drop_provider && provider != nullptr) {
      vrelease (provider);
      provider = nullptr;
    }
  }

  bool
  ensure ()
  {
    if (g_reconnect_reset_requested.exchange (false, std::memory_order_acq_rel)) {
      release (true);
      reconnect.reset_backoff ();
      pin_disabled = false;
    }

    if (internal != nullptr && manager != nullptr)
      return true;

    const uint64_t now_ms = GetTickCount64 ();
    if (!reconnect.can_attempt (now_ms))
      return false;

    auto fail_with_backoff = [&] (const char *detail) -> bool {
      release (true);
      const uint64_t wait_ms = reconnect.record_failure (now_ms);
      char msg[160] {};
      std::snprintf (msg, sizeof (msg), "%s (retry in %llus)", detail,
                     static_cast<unsigned long long> (wait_ms / 1000));
      capability_set (SubsystemId::VirtualDesktop, CapabilityState::Degraded,
                      msg);
      return false;
    };

    if (provider == nullptr) {
      void *obj = nullptr;
      HRESULT hr = CoCreateInstance (kCLSID_ImmersiveShell, nullptr,
                                     CLSCTX_LOCAL_SERVER,
                                     kIID_IServiceProvider, &obj);
      if (FAILED (hr) || obj == nullptr)
        return fail_with_backoff ("ImmersiveShell unavailable");
      provider = obj;
    }

    if (internal == nullptr) {
      void *obj = nullptr;
      HRESULT hr = vcall<HRESULT> (provider, kSlotQueryService,
                                   &kCLSID_VirtualDesktopManagerInternal,
                                   &kIID_IVirtualDesktopManagerInternal, &obj);
      if (FAILED (hr) || obj == nullptr)
        return fail_with_backoff ("IVirtualDesktopManagerInternal unavailable");
      internal = obj;
    }

    if (manager == nullptr) {
      void *obj = nullptr;
      HRESULT hr = vcall<HRESULT> (provider, kSlotQueryService,
                                   &kIID_IVirtualDesktopManager,
                                   &kIID_IVirtualDesktopManager, &obj);
      if (FAILED (hr) || obj == nullptr)
        return fail_with_backoff ("IVirtualDesktopManager unavailable");
      manager = obj;
    }

    reconnect.record_success ();
    const uint32_t build = windows_build_number ();
    const VtableProfile prof = vtable_slots_for_build (build);
    char detail_buf[128] {};
    std::snprintf (detail_buf, sizeof (detail_buf),
                   prof.can_manage_desktops
                       ? "build %u (full VD + create/remove)"
                       : "build %u (read-only switch/query)",
                   build);
    capability_set (SubsystemId::VirtualDesktop,
                    prof.can_manage_desktops ? CapabilityState::Active
                                             : CapabilityState::Degraded,
                    detail_buf);

    return true;
  }

  static bool
  recoverable (HRESULT hr)
  {
    return hr == (HRESULT)0x800706BA /* RPC_S_SERVER_UNAVAILABLE */ ||
           hr == (HRESULT)0x800401FD /* CO_E_OBJNOTCONNECTED */ ||
           hr == (HRESULT)0x80040154 /* REGDB_E_CLASSNOTREG */;
  }

  std::vector<DesktopInfo>
  desktops ()
  {
    std::vector<DesktopInfo> out;
    if (!ensure ())
      return out;

    for (int attempt = 0; attempt < 2; ++attempt) {
      void *arr = nullptr;
      HRESULT hr = vcall<HRESULT> (internal, kSlotGetDesktops, &arr);
      if (FAILED (hr) || arr == nullptr) {
        if (recoverable (hr) && attempt == 0) {
          release (true);
          if (!ensure ())
            return out;
          continue;
        }
        return out;
      }

      UINT n = 0;
      vcall<HRESULT> (arr, kSlotGetCount, &n);
      out.reserve (n);
      for (UINT i = 0; i < n && i < 100; ++i) {
        void *d = nullptr;
        if (SUCCEEDED (vcall<HRESULT> (arr, kSlotGetAt, i,
                                       &kIID_IVirtualDesktop, &d)) &&
            d != nullptr) {
          GUID id{};
          if (SUCCEEDED (vcall<HRESULT> (d, kSlotGetId, &id)))
            out.push_back (DesktopInfo{(int)i, id});
          vrelease (d);
        }
      }
      vrelease (arr);
      break;
    }
    return out;
  }

  // Resolve a desktop GUID to its index.
  int
  index_of (const GUID &id)
  {
    for (const DesktopInfo &info : desktops ())
      if (IsEqualGUID (info.id, id))
        return info.index;
    return -1;
  }

  bool
  ensure_view_collection ()
  {
    if (view_collection != nullptr)
      return true;
    if (provider == nullptr)
      return false;

    void *obj = nullptr;
    HRESULT hr = vcall<HRESULT> (provider, kSlotQueryService,
                                 &kIID_IApplicationViewCollection,
                                 &kIID_IApplicationViewCollection, &obj);
    if (FAILED (hr) || obj == nullptr) {
      return false;
    }
    view_collection = obj;
    return true;
  }

  // Resolve the pin services lazily: a build without them keeps the rest of
  // the module working (only pinning fails).
  bool
  ensure_pin_services ()
  {
    if (pinned_apps != nullptr && ensure_view_collection ())
      return true;
    if (pin_disabled || provider == nullptr)
      return false;

    if (pinned_apps == nullptr) {
      void *obj = nullptr;
      HRESULT hr = vcall<HRESULT> (provider, kSlotQueryService,
                                   &kCLSID_VirtualDesktopPinnedApps,
                                   &kIID_IVirtualDesktopPinnedApps, &obj);
      if (FAILED (hr) || obj == nullptr) {
        pin_disabled = true;
        return false;
      }
      pinned_apps = obj;
    }

    if (!ensure_view_collection ()) {
      pin_disabled = true;
      return false;
    }
    return true;
  }

  void *
  desktop_at (void *arr, UINT index)
  {
    void *d = nullptr;
    if (arr != nullptr &&
        SUCCEEDED (vcall<HRESULT> (arr, kSlotGetAt, index,
                                   &kIID_IVirtualDesktop, &d))) {
      return d;
    }
    return nullptr;
  }

  bool
  set_window_pin_state (HWND hwnd, int slot)
  {
    if (!ensure () || !ensure_pin_services ())
      return false;

    for (int attempt = 0; attempt < 2; ++attempt) {
      void *view = nullptr;
      HRESULT hr = vcall<HRESULT> (view_collection, kSlotGetViewForHwnd, hwnd,
                                   &view);
      if (FAILED (hr) || view == nullptr) {
        if (recoverable (hr) && attempt == 0) {
          release (true);
          if (!ensure () || !ensure_pin_services ())
            return false;
          continue;
        }
        return false;
      }
      bool ok = SUCCEEDED (vcall<HRESULT> (pinned_apps, slot, view));
      vrelease (view);
      return ok;
    }
    return false;
  }

  bool
  move_window_to_desktop (HWND hwnd, int target_index)
  {
    if (!ensure () || !ensure_view_collection ())
      return false;

    for (int attempt = 0; attempt < 2; ++attempt) {
      void *view = nullptr;
      HRESULT hr = vcall<HRESULT> (view_collection, kSlotGetViewForHwnd, hwnd,
                                   &view);
      if (FAILED (hr) || view == nullptr) {
        if (recoverable (hr) && attempt == 0) {
          release (true);
          if (!ensure () || !ensure_view_collection ())
            return false;
          continue;
        }
        return false;
      }

      void *arr = nullptr;
      hr = vcall<HRESULT> (internal, kSlotGetDesktops, &arr);
      if (FAILED (hr) || arr == nullptr) {
        vrelease (view);
        if (recoverable (hr) && attempt == 0) {
          release (true);
          if (!ensure () || !ensure_view_collection ())
            return false;
          continue;
        }
        return false;
      }

      bool ok = false;
      UINT n = 0;
      vcall<HRESULT> (arr, kSlotGetCount, &n);
      if (target_index >= 0 && (UINT)target_index < n) {
        void *target_desktop = desktop_at (arr, (UINT)target_index);
        if (target_desktop != nullptr) {
          ok = SUCCEEDED (vcall<HRESULT> (internal, kSlotMoveViewToDesktop,
                                          view, target_desktop));
          vrelease (target_desktop);
        }
      }
      vrelease (arr);
      vrelease (view);
      return ok;
    }
    return false;
  }

  std::vector<bool>
  move_windows_batch (const std::vector<WindowMoveOp> &moves)
  {
    std::vector<bool> results (moves.size (), false);
    if (moves.empty ())
      return results;
    if (!ensure () || !ensure_view_collection ())
      return results;

    void *arr = nullptr;
    HRESULT hr = vcall<HRESULT> (internal, kSlotGetDesktops, &arr);
    if (FAILED (hr) || arr == nullptr) {
      if (recoverable (hr)) {
        release (true);
        if (!ensure () || !ensure_view_collection ())
          return results;
        hr = vcall<HRESULT> (internal, kSlotGetDesktops, &arr);
      }
      if (FAILED (hr) || arr == nullptr)
        return results;
    }

    UINT n = 0;
    vcall<HRESULT> (arr, kSlotGetCount, &n);
    std::vector<void *> desktop_ptrs (n, nullptr);
    for (UINT i = 0; i < n; ++i)
      desktop_ptrs[i] = desktop_at (arr, i);

    for (size_t i = 0; i < moves.size (); ++i) {
      const WindowMoveOp &m = moves[i];
      if (m.hwnd == nullptr || m.target_index < 0 ||
          static_cast<UINT> (m.target_index) >= n) {
        continue;
      }
      void *target_desktop = desktop_ptrs[static_cast<size_t> (m.target_index)];
      if (target_desktop == nullptr)
        continue;
      void *view = nullptr;
      HRESULT vhr = vcall<HRESULT> (view_collection, kSlotGetViewForHwnd,
                                    m.hwnd, &view);
      if (SUCCEEDED (vhr) && view != nullptr) {
        results[i] = SUCCEEDED (vcall<HRESULT> (
            internal, kSlotMoveViewToDesktop, view, target_desktop));
        vrelease (view);
      }
    }

    for (void *d : desktop_ptrs) {
      if (d != nullptr)
        vrelease (d);
    }
    vrelease (arr);
    return results;
  }

  bool
  create_desktop ()
  {
    VtableProfile slots = vtable_slots_for_build (windows_build_number ());
    if (!slots.can_manage_desktops || slots.create_desktop < 0 || !ensure ())
      return false;

    void *created = nullptr;
    HRESULT hr = vcall<HRESULT> (internal, slots.create_desktop, &created);
    if (created != nullptr)
      vrelease (created);
    return SUCCEEDED (hr);
  }

  bool
  remove_desktop (int index)
  {
    VtableProfile slots = vtable_slots_for_build (windows_build_number ());
    if (!slots.can_manage_desktops || slots.remove_desktop < 0 || !ensure ())
      return false;

    void *arr = nullptr;
    if (FAILED (vcall<HRESULT> (internal, kSlotGetDesktops, &arr)) ||
        arr == nullptr) {
      return false;
    }

    bool ok = false;
    UINT n = 0;
    vcall<HRESULT> (arr, kSlotGetCount, &n);
    if (n > 1 && index >= 0 && (UINT)index < n) {
      UINT fallback_index = (index > 0) ? (UINT)(index - 1) : 1u;
      void *destroy_desktop = desktop_at (arr, (UINT)index);
      void *fallback_desktop = desktop_at (arr, fallback_index);
      if (destroy_desktop != nullptr && fallback_desktop != nullptr) {
        ok = SUCCEEDED (vcall<HRESULT> (internal, slots.remove_desktop,
                                        destroy_desktop, fallback_desktop));
      }
      if (destroy_desktop != nullptr)
        vrelease (destroy_desktop);
      if (fallback_desktop != nullptr)
        vrelease (fallback_desktop);
    }
    vrelease (arr);
    return ok;
  }
};

ComState g_com;  // worker-thread-only

std::mutex g_queue_mu;
std::condition_variable g_cv;
std::deque<std::shared_ptr<Job>> g_queue;  // FIFO job queue; never drop concurrent callers
bool g_worker_quit = false;
bool g_worker_stuck = false;  // true while a COM call has exceeded timeout

// Forward-declared for mutation paths below; implemented in observer section.
void obs_schedule_hint_async (void);

void
run_on_worker (const std::shared_ptr<Job> &job, bool wait_for_result = true)
{
  static std::once_flag worker_once;
  static std::thread worker;

  std::call_once (worker_once, [] () {
    worker = std::thread ([] () {
      // Dedicated MTA for the lifetime of the process (upstream pins the
      // COM objects in a thread_local; a dedicated thread is equivalent).
      CO_MTA_USAGE_COOKIE cookie = nullptr;
      CoIncrementMTAUsage (&cookie);

      std::unique_lock<std::mutex> lock (g_queue_mu);
      for (;;) {
        g_cv.wait (lock, [] { return !g_queue.empty () || g_worker_quit; });
        if (g_worker_quit)
          break;

        std::shared_ptr<Job> cur = std::move (g_queue.front ());
        g_queue.pop_front ();
        lock.unlock ();

        switch (cur->op) {
        case Job::Op::Desktops: {
          cur->out_desktops = g_com.desktops ();
          cur->ok = !cur->out_desktops.empty ();
          break;
        }
        case Job::Op::CurrentIndex: {
          // Enumerate FIRST, then query the current desktop: on 22621 the
          // explorer VD service hangs when GetCurrentDesktop is the first
          // call after Connect (verified empirically; the enumerate-then-
          // query order is reliable).
          auto ds = g_com.desktops ();
          if (!g_com.ensure () || ds.empty ())
            break;
          void *d = nullptr;
          HRESULT hr = vcall<HRESULT> (g_com.internal, kSlotGetCurrentDesktop,
                                       &d);
          if (SUCCEEDED (hr) && d != nullptr) {
            GUID id{};
            bool got = SUCCEEDED (vcall<HRESULT> (d, kSlotGetId, &id));
            vrelease (d);
            if (got) {
              for (const DesktopInfo &info : ds)
                if (IsEqualGUID (info.id, id)) {
                  cur->out_index = info.index;
                  cur->ok = true;
                  break;
                }
            }
          }
          break;
        }
        case Job::Op::Snapshot: {
          auto ds = g_com.desktops ();
          if (!g_com.ensure () || ds.empty ())
            break;
          int active_idx = -1;
          void *d = nullptr;
          HRESULT hr = vcall<HRESULT> (g_com.internal, kSlotGetCurrentDesktop,
                                       &d);
          if (SUCCEEDED (hr) && d != nullptr) {
            GUID id{};
            bool got = SUCCEEDED (vcall<HRESULT> (d, kSlotGetId, &id));
            vrelease (d);
            if (got) {
              for (const DesktopInfo &info : ds)
                if (IsEqualGUID (info.id, id)) {
                  active_idx = info.index;
                  break;
                }
            }
          }
          cur->out_desktops = std::move (ds);
          cur->out_index = active_idx >= 0 ? active_idx : 0;
          cur->ok = true;
          break;
        }
        case Job::Op::WindowDesktopIndex: {
          if (!g_com.ensure ())
            break;
          GUID id{};
          if (SUCCEEDED (vcall<HRESULT> (g_com.manager, kSlotDesktopByWindow,
                                         cur->in_hwnd, &id))) {
            GUID zero{};
            if (!IsEqualGUID (id, zero)) {
              int idx = g_com.index_of (id);
              if (idx >= 0) {
                cur->out_index = idx;
                cur->ok = true;
              }
            }
          }
          break;
        }
        case Job::Op::WindowOnCurrent: {
          if (!g_com.ensure ())
            break;
          BOOL on = FALSE;
          if (SUCCEEDED (vcall<HRESULT> (g_com.manager, kSlotIsOnCurrent,
                                         cur->in_hwnd, &on))) {
            cur->out_bool = on != FALSE;
            cur->ok = true;
          }
          break;
        }
        case Job::Op::SwitchTo: {
          if (!g_com.ensure ())
            break;
          // Resolve by INDEX through the array (GetDesktops -> GetAt), never
          // find_desktop: its vtable slot differs between 22621 and 24H2.
          void *arr = nullptr;
          if (FAILED (vcall<HRESULT> (g_com.internal, kSlotGetDesktops,
                                      &arr)) ||
              arr == nullptr)
            break;
          UINT n = 0;
          vcall<HRESULT> (arr, kSlotGetCount, &n);
          if (cur->in_index >= 0 && (UINT)cur->in_index < n) {
            void *d = nullptr;
            if (SUCCEEDED (vcall<HRESULT> (arr, kSlotGetAt,
                                           (UINT)cur->in_index,
                                           &kIID_IVirtualDesktop, &d)) &&
                d != nullptr) {
              cur->ok = SUCCEEDED (
                  vcall<HRESULT> (g_com.internal, kSlotSwitchDesktop, d));
              vrelease (d);
            }
          }
          vrelease (arr);
          break;
        }
        case Job::Op::PinWindow: {
          cur->ok = g_com.set_window_pin_state (cur->in_hwnd, kSlotPinView);
          break;
        }
        case Job::Op::UnpinWindow: {
          cur->ok = g_com.set_window_pin_state (cur->in_hwnd, kSlotUnpinView);
          break;
        }
        case Job::Op::MoveWindowToDesktop: {
          cur->ok = g_com.move_window_to_desktop (cur->in_hwnd, cur->in_index);
          break;
        }
        case Job::Op::MoveWindowsBatch: {
          cur->out_move_results = g_com.move_windows_batch (cur->in_moves);
          cur->ok = !cur->out_move_results.empty ();
          break;
        }
        case Job::Op::CreateDesktop: {
          cur->ok = g_com.create_desktop ();
          break;
        }
        case Job::Op::RemoveDesktop: {
          cur->ok = g_com.remove_desktop (cur->in_index);
          break;
        }
        }

        lock.lock ();
        cur->done = true;
        if (g_worker_stuck) {
          g_worker_stuck = false;
          winome::capability_set (winome::SubsystemId::VirtualDesktop,
                                  winome::CapabilityState::Active,
                                  "Explorer VD COM recovered");
        }
        g_cv.notify_all ();
      }
    });
    worker.detach ();  // never join: Release after combase teardown crashes
  });

  std::unique_lock<std::mutex> lock (g_queue_mu);
  if (g_worker_stuck || g_worker_quit) {
    job->ok = false;
    return;
  }
  if (!wait_for_result && g_queue.size () >= 64) {
    job->ok = false;
    return;
  }
  g_queue.push_back (job);
  g_cv.notify_all ();
  if (!wait_for_result)
    return;

  // 8s is far above a healthy round trip (single-digit ms) and below any
  // permanent UI hang; FIFO queue prevents concurrent callers from dropping jobs.
  bool finished = g_cv.wait_for (lock, std::chrono::seconds (8), [&job] {
    return job->done || g_worker_quit;
  });
  if (!finished && !job->done) {
    g_worker_stuck = true;
    winome::capability_set (winome::SubsystemId::VirtualDesktop,
                            winome::CapabilityState::Tripped,
                            "Explorer VD COM call timed out (>8s)");
  }
}

// --- Centralized event-driven virtual desktop observer -----------------------

std::atomic<uint64_t> g_mutation_seq{0};

struct ListenerEntry {
  DesktopChangedCallback cb = nullptr;
  void *user_data = nullptr;
};

struct ObserverState {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<ListenerEntry> listeners;
  DesktopSnapshot snapshot{1, 0, false};
  uint64_t snapshot_seq = 0;
  std::thread worker;
  bool worker_started = false;
  bool worker_stop = false;
  bool probe_requested = false;
  bool main_notify_queued = false;
  HWINEVENTHOOK desktop_hook = nullptr;
  guint init_delay_id = 0;
  guint fallback_timer_id = 0;
  guint debounce_id = 0;

  ~ObserverState () {
    std::thread to_join;
    {
      std::lock_guard<std::mutex> lock (mu);
      worker_stop = true;
      worker_started = false;
      to_join = std::move (worker);
    }
    cv.notify_all ();
    if (to_join.joinable ())
      to_join.join ();
  }
};

ObserverState &
obs_state (void)
{
  static ObserverState s;
  return s;
}

gboolean
obs_deliver_main (gpointer user_data)
{
  (void)user_data;
  std::vector<ListenerEntry> copy;
  DesktopSnapshot snap;
  {
    ObserverState &s = obs_state ();
    std::lock_guard<std::mutex> lock (s.mu);
    s.main_notify_queued = false;
    if (s.listeners.empty () || !s.snapshot.valid)
      return G_SOURCE_REMOVE;
    if (s.snapshot_seq != g_mutation_seq.load (std::memory_order_acquire))
      return G_SOURCE_REMOVE;
    copy = s.listeners;
    snap = s.snapshot;
  }
  for (const ListenerEntry &e : copy) {
    if (e.cb != nullptr)
      e.cb (snap.count, snap.active_index, e.user_data);
  }
  return G_SOURCE_REMOVE;
}

void
obs_worker_main (void)
{
  ObserverState &s = obs_state ();
  for (;;) {
    {
      std::unique_lock<std::mutex> lock (s.mu);
      s.cv.wait (lock, [&] { return s.worker_stop || s.probe_requested; });
      if (s.worker_stop)
        break;
      s.probe_requested = false;
      if (s.listeners.empty ())
        continue;
    }

    const uint64_t seq_before =
        g_mutation_seq.load (std::memory_order_acquire);

    auto job = std::make_shared<Job> ();
    job->op = Job::Op::Snapshot;
    run_on_worker (job);

    if (g_mutation_seq.load (std::memory_order_acquire) != seq_before) {
      // A newer desktop switch/create/remove occurred while this snapshot was
      // queued or executing; discard stale intermediate state and re-probe.
      std::lock_guard<std::mutex> lock (s.mu);
      if (!s.worker_stop)
        s.probe_requested = true;
      continue;
    }

    DesktopSnapshot fresh;
    if (job->ok && !job->out_desktops.empty ()) {
      fresh.count = static_cast<int> (job->out_desktops.size ());
      fresh.active_index = (job->out_index >= 0) ? job->out_index : 0;
      fresh.valid = true;
    }

    bool should_schedule = false;
    {
      std::lock_guard<std::mutex> lock (s.mu);
      if (!s.worker_stop && fresh.valid &&
          g_mutation_seq.load (std::memory_order_acquire) == seq_before) {
        bool changed = !s.snapshot.valid ||
                       s.snapshot.count != fresh.count ||
                       s.snapshot.active_index != fresh.active_index;
        s.snapshot = fresh;
        s.snapshot_seq = seq_before;
        if (changed && !s.listeners.empty () && !s.main_notify_queued) {
          s.main_notify_queued = true;
          should_schedule = true;
        }
      }
    }
    if (should_schedule)
      g_idle_add (obs_deliver_main, nullptr);
  }
}

void
obs_request_probe_now (void)
{
  ObserverState &s = obs_state ();
  {
    std::lock_guard<std::mutex> lock (s.mu);
    if (s.listeners.empty () || !s.worker_started || s.worker_stop)
      return;
    s.probe_requested = true;
  }
  s.cv.notify_one ();
}

gboolean
obs_debounce_cb (gpointer user_data)
{
  (void)user_data;
  {
    ObserverState &s = obs_state ();
    std::lock_guard<std::mutex> lock (s.mu);
    s.debounce_id = 0;
  }
  obs_request_probe_now ();
  return G_SOURCE_REMOVE;
}

void
obs_schedule_debounced (guint delay_ms)
{
  ObserverState &s = obs_state ();
  std::lock_guard<std::mutex> lock (s.mu);
  if (s.listeners.empty () || s.worker_stop)
    return;
  if (s.debounce_id != 0)
    return;
  s.debounce_id = g_timeout_add (delay_ms, obs_debounce_cb, nullptr);
}

gboolean
obs_init_delay_cb (gpointer user_data)
{
  (void)user_data;
  {
    ObserverState &s = obs_state ();
    std::lock_guard<std::mutex> lock (s.mu);
    s.init_delay_id = 0;
  }
  obs_request_probe_now ();
  return G_SOURCE_REMOVE;
}

gboolean
obs_fallback_tick_cb (gpointer user_data)
{
  (void)user_data;
  {
    ObserverState &s = obs_state ();
    std::lock_guard<std::mutex> lock (s.mu);
    if (s.listeners.empty () || s.worker_stop) {
      s.fallback_timer_id = 0;
      return G_SOURCE_REMOVE;
    }
  }
  obs_request_probe_now ();
  return G_SOURCE_CONTINUE;
}

void CALLBACK
on_desktop_winevent (HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                     LONG id_object, LONG id_child, DWORD thread, DWORD ms)
{
  (void)hook;
  (void)event;
  (void)hwnd;
  (void)id_object;
  (void)id_child;
  (void)thread;
  (void)ms;
  // Explorer updates GetCurrentDesktop synchronously or within tens of ms
  // after EVENT_SYSTEM_DESKTOPSWITCH fires.
  obs_schedule_debounced (40);
}

void
obs_schedule_hint_async (void)
{
  {
    ObserverState &s = obs_state ();
    std::lock_guard<std::mutex> lock (s.mu);
    if (s.listeners.empty () || s.worker_stop)
      return;
  }
  g_idle_add (
      +[] (gpointer) -> gboolean {
        obs_schedule_debounced (60);
        return G_SOURCE_REMOVE;
      },
      nullptr);
}

}  // namespace

std::vector<DesktopInfo>
desktops (void)
{
  auto job = std::make_shared<Job> ();
  job->op = Job::Op::Desktops;
  run_on_worker (job);
  return std::move (job->out_desktops);
}

int
count (void)
{
  return (int)desktops ().size ();
}

int
current_index (void)
{
  auto job = std::make_shared<Job> ();
  job->op = Job::Op::CurrentIndex;
  run_on_worker (job);
  return job->ok ? job->out_index : -1;
}

int
window_desktop_index (HWND hwnd)
{
  auto job = std::make_shared<Job> ();
  job->op = Job::Op::WindowDesktopIndex;
  job->in_hwnd = hwnd;
  run_on_worker (job);
  return job->ok ? job->out_index : -1;
}

bool
window_on_current (HWND hwnd)
{
  auto job = std::make_shared<Job> ();
  job->op = Job::Op::WindowOnCurrent;
  job->in_hwnd = hwnd;
  run_on_worker (job);
  return job->ok ? job->out_bool : false;
}

bool
switch_to (int index)
{
  g_mutation_seq.fetch_add (1, std::memory_order_acq_rel);
  auto job = std::make_shared<Job> ();
  job->op = Job::Op::SwitchTo;
  job->in_index = index;
  run_on_worker (job);
  const uint64_t seq_after =
      g_mutation_seq.fetch_add (1, std::memory_order_acq_rel) + 1;
  if (job->ok) {
    bool should_schedule = false;
    {
      ObserverState &s = obs_state ();
      std::lock_guard<std::mutex> lock (s.mu);
      if (s.snapshot.valid && index >= 0 && index < s.snapshot.count) {
        const bool changed = (s.snapshot.active_index != index);
        s.snapshot.active_index = index;
        s.snapshot_seq = seq_after;
        if (changed && !s.listeners.empty () && !s.main_notify_queued) {
          s.main_notify_queued = true;
          should_schedule = true;
        }
      }
    }
    if (should_schedule)
      g_idle_add (obs_deliver_main, nullptr);
    obs_schedule_hint_async ();
  }
  return job->ok;
}

bool
pin_window (HWND hwnd)
{
  if (hwnd == nullptr)
    return false;
  auto job = std::make_shared<Job> ();
  job->op = Job::Op::PinWindow;
  job->in_hwnd = hwnd;
  run_on_worker (job, true);
  return job->ok;
}

void
pin_window_async (HWND hwnd)
{
  if (hwnd == nullptr)
    return;
  auto job = std::make_shared<Job> ();
  job->op = Job::Op::PinWindow;
  job->in_hwnd = hwnd;
  run_on_worker (job, false);
}

void
reset_circuit_breaker (void)
{
  {
    std::lock_guard<std::mutex> lock (g_queue_mu);
    g_worker_stuck = false;
    g_queue.clear ();
  }
  g_reconnect_reset_requested.store (true, std::memory_order_release);
  winome::capability_set (winome::SubsystemId::VirtualDesktop,
                          winome::CapabilityState::Active,
                          "manual recovery");
  obs_schedule_hint_async ();
}

VtableProfile
vtable_slots_for_build (uint32_t build_number)
{
  VtableProfile p;
  if (build_number >= 26100) {
    // Win11 24H2+ inserts SwitchDesktopAndMoveForegroundView at slot 10.
    p.move_view_to_desktop = 4;
    p.create_desktop = 11;
    p.move_desktop = 12;
    p.remove_desktop = 13;
    p.can_manage_desktops = true;
  } else if (build_number >= 22621) {
    // Win11 22H2 / 23H2 (builds 22621..25999).
    p.move_view_to_desktop = 4;
    p.create_desktop = 10;
    p.move_desktop = 11;
    p.remove_desktop = 12;
    p.can_manage_desktops = true;
  }
  return p;
}

uint32_t
windows_build_number (void)
{
  static uint32_t cached = [] () -> uint32_t {
    HMODULE ntdll = GetModuleHandleW (L"ntdll.dll");
    if (ntdll == nullptr)
      return 0;
    using RtlGetVersionFn = LONG (WINAPI *) (OSVERSIONINFOW *);
    auto fn = reinterpret_cast<RtlGetVersionFn> (
        reinterpret_cast<void *> (GetProcAddress (ntdll, "RtlGetVersion")));
    if (fn == nullptr)
      return 0;
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof (info);
    if (fn (&info) != 0)
      return 0;
    return static_cast<uint32_t> (info.dwBuildNumber);
  }();
  return cached;
}

bool
unpin_window (HWND hwnd)
{
  if (hwnd == nullptr)
    return false;
  auto job = std::make_shared<Job> ();
  job->op = Job::Op::UnpinWindow;
  job->in_hwnd = hwnd;
  run_on_worker (job);
  return job->ok;
}

bool
move_window_to_desktop (HWND hwnd, int target_index)
{
  if (hwnd == nullptr || target_index < 0)
    return false;
  auto job = std::make_shared<Job> ();
  job->op = Job::Op::MoveWindowToDesktop;
  job->in_hwnd = hwnd;
  job->in_index = target_index;
  run_on_worker (job);
  return job->ok;
}

std::vector<bool>
move_windows_batch (const std::vector<WindowMoveOp> &moves)
{
  if (moves.empty ())
    return {};
  auto job = std::make_shared<Job> ();
  job->op = Job::Op::MoveWindowsBatch;
  job->in_moves = moves;
  run_on_worker (job);
  if (job->out_move_results.size () != moves.size ())
    return std::vector<bool> (moves.size (), false);
  return job->out_move_results;
}

bool
create_desktop (void)
{
  g_mutation_seq.fetch_add (1, std::memory_order_acq_rel);
  auto job = std::make_shared<Job> ();
  job->op = Job::Op::CreateDesktop;
  run_on_worker (job);
  g_mutation_seq.fetch_add (1, std::memory_order_acq_rel);
  if (job->ok)
    obs_schedule_hint_async ();
  return job->ok;
}

bool
remove_desktop (int index)
{
  if (index < 0)
    return false;
  g_mutation_seq.fetch_add (1, std::memory_order_acq_rel);
  auto job = std::make_shared<Job> ();
  job->op = Job::Op::RemoveDesktop;
  job->in_index = index;
  run_on_worker (job);
  g_mutation_seq.fetch_add (1, std::memory_order_acq_rel);
  if (job->ok)
    obs_schedule_hint_async ();
  return job->ok;
}

DesktopSnapshot
cached_snapshot (void)
{
  ObserverState &s = obs_state ();
  std::lock_guard<std::mutex> lock (s.mu);
  return s.snapshot;
}

void
add_change_listener (DesktopChangedCallback cb, void *user_data)
{
  if (cb == nullptr)
    return;

  DesktopSnapshot immediate;
  bool deliver_immediate = false;
  bool first = false;

  ObserverState &s = obs_state ();
  {
    std::lock_guard<std::mutex> lock (s.mu);
    for (const ListenerEntry &e : s.listeners) {
      if (e.cb == cb && e.user_data == user_data)
        return;
    }
    first = s.listeners.empty ();
    s.listeners.push_back ({cb, user_data});
    if (s.snapshot.valid) {
      immediate = s.snapshot;
      deliver_immediate = true;
    }
    if (first) {
      if (!s.worker_started) {
        s.worker_stop = false;
        s.worker = std::thread (obs_worker_main);
        s.worker_started = true;
      }
      if (s.desktop_hook == nullptr) {
        s.desktop_hook = SetWinEventHook (
            EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH, nullptr,
            on_desktop_winevent, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
      }
      // Defer initial COM connect slightly so startup avoids contending with
      // early Explorer initialization.
      if (s.init_delay_id == 0)
        s.init_delay_id = g_timeout_add (1500, obs_init_delay_cb, nullptr);
      // Low-frequency 15s backstop for Win+Tab Task View mutations; normal
      // switches arrive immediately via EVENT_SYSTEM_DESKTOPSWITCH.
      if (s.fallback_timer_id == 0)
        s.fallback_timer_id = g_timeout_add (15000, obs_fallback_tick_cb, nullptr);
    }
  }

  if (deliver_immediate)
    cb (immediate.count, immediate.active_index, user_data);
}

void
remove_change_listener (DesktopChangedCallback cb, void *user_data)
{
  std::thread to_join;
  HWINEVENTHOOK hook_to_remove = nullptr;
  guint rm_init = 0, rm_fallback = 0, rm_debounce = 0;

  ObserverState &s = obs_state ();
  {
    std::lock_guard<std::mutex> lock (s.mu);
    s.listeners.erase (
        std::remove_if (s.listeners.begin (), s.listeners.end (),
                        [cb, user_data] (const ListenerEntry &e) {
                          return e.cb == cb && e.user_data == user_data;
                        }),
        s.listeners.end ());
    if (s.listeners.empty () && s.worker_started) {
      s.worker_stop = true;
      s.worker_started = false;
      to_join = std::move (s.worker);
      hook_to_remove = s.desktop_hook;
      s.desktop_hook = nullptr;
      rm_init = s.init_delay_id;
      s.init_delay_id = 0;
      rm_fallback = s.fallback_timer_id;
      s.fallback_timer_id = 0;
      rm_debounce = s.debounce_id;
      s.debounce_id = 0;
    }
  }

  if (rm_init != 0)
    g_source_remove (rm_init);
  if (rm_fallback != 0)
    g_source_remove (rm_fallback);
  if (rm_debounce != 0)
    g_source_remove (rm_debounce);
  if (hook_to_remove != nullptr)
    UnhookWinEvent (hook_to_remove);
  if (to_join.joinable ()) {
    s.cv.notify_all ();
    to_join.join ();
  }
}

void
notify_change_hint_async (void)
{
  obs_schedule_hint_async ();
}

void
probe_now_async (void)
{
  obs_request_probe_now ();
}

bool
available (void)
{
  return !desktops ().empty ();
}

void
shutdown (void)
{
  std::thread to_join;
  HWINEVENTHOOK hook_to_remove = nullptr;
  guint rm_init = 0, rm_fallback = 0, rm_debounce = 0;
  ObserverState &s = obs_state ();
  {
    std::lock_guard<std::mutex> lock (s.mu);
    s.listeners.clear ();
    if (s.worker_started) {
      s.worker_stop = true;
      s.worker_started = false;
      to_join = std::move (s.worker);
    }
    hook_to_remove = s.desktop_hook;
    s.desktop_hook = nullptr;
    rm_init = s.init_delay_id;
    s.init_delay_id = 0;
    rm_fallback = s.fallback_timer_id;
    s.fallback_timer_id = 0;
    rm_debounce = s.debounce_id;
    s.debounce_id = 0;
  }
  if (rm_init != 0) g_source_remove (rm_init);
  if (rm_fallback != 0) g_source_remove (rm_fallback);
  if (rm_debounce != 0) g_source_remove (rm_debounce);
  if (hook_to_remove != nullptr) UnhookWinEvent (hook_to_remove);
  if (to_join.joinable ()) {
    s.cv.notify_all ();
    to_join.join ();
  }
}

}  // namespace vd
}  // namespace winome
