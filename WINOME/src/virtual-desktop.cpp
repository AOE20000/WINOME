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

#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace winome {
namespace vd {

namespace {

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
constexpr int kSlotGetDesktops = 7;    // IVirtualDesktopManagerInternal
constexpr int kSlotGetCurrentDesktop = 6;
constexpr int kSlotSwitchDesktop = 9;
constexpr int kSlotGetId = 4;          // IVirtualDesktop
constexpr int kSlotGetCount = 3;       // IObjectArray
constexpr int kSlotGetAt = 4;
constexpr int kSlotGetViewForHwnd = 6; // IApplicationViewCollection
constexpr int kSlotPinView = 7;        // IVirtualDesktopPinnedApps

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
    WindowDesktopIndex,
    WindowOnCurrent,
    SwitchTo,
    PinWindow,
  } op;

  std::vector<DesktopInfo> out_desktops;
  int out_index = -1;
  bool out_bool = false;
  HWND in_hwnd = nullptr;
  int in_index = 0;
  bool ok = false;
};

// COM state — only ever touched on the worker thread.
struct ComState {
  void *provider = nullptr;        // IServiceProvider
  void *internal = nullptr;        // IVirtualDesktopManagerInternal
  void *manager = nullptr;         // IVirtualDesktopManager
  void *pinned_apps = nullptr;     // IVirtualDesktopPinnedApps
  void *view_collection = nullptr; // IApplicationViewCollection
  bool disabled = false;
  bool pin_disabled = false;  // pin services unreachable; pin only, not the
                              // rest of the module

  void
  release ()
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
    // provider deliberately leaked at process exit: releasing it after
    // combase's TLS teardown crashes (upstream hit this too).
  }

  bool
  ensure ()
  {
    if (internal != nullptr && manager != nullptr)
      return true;
    if (disabled)
      return false;

    if (provider == nullptr) {
      void *obj = nullptr;
      HRESULT hr = CoCreateInstance (kCLSID_ImmersiveShell, nullptr,
                                     CLSCTX_LOCAL_SERVER,
                                     kIID_IServiceProvider, &obj);
      if (FAILED (hr) || obj == nullptr) {
        disabled = true;
        return false;
      }
      provider = obj;
    }

    if (internal == nullptr) {
      void *obj = nullptr;
      HRESULT hr = vcall<HRESULT> (provider, kSlotQueryService,
                                   &kCLSID_VirtualDesktopManagerInternal,
                                   &kIID_IVirtualDesktopManagerInternal, &obj);
      if (FAILED (hr) || obj == nullptr) {
        disabled = true;  // build without this interface; never retry
        return false;
      }
      internal = obj;
    }

    if (manager == nullptr) {
      void *obj = nullptr;
      HRESULT hr = vcall<HRESULT> (provider, kSlotQueryService,
                                   &kIID_IVirtualDesktopManager,
                                   &kIID_IVirtualDesktopManager, &obj);
      if (FAILED (hr) || obj == nullptr) {
        disabled = true;
        return false;
      }
      manager = obj;
    }

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
          release ();
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

  // Resolve the pin services lazily: a build without them keeps the rest of
  // the module working (only pinning fails).
  bool
  ensure_pin_services ()
  {
    if (pinned_apps != nullptr && view_collection != nullptr)
      return true;
    if (pin_disabled || disabled || provider == nullptr)
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

    if (view_collection == nullptr) {
      // Queried with its own IID as the service id (upstream does this).
      void *obj = nullptr;
      HRESULT hr = vcall<HRESULT> (provider, kSlotQueryService,
                                   &kIID_IApplicationViewCollection,
                                   &kIID_IApplicationViewCollection, &obj);
      if (FAILED (hr) || obj == nullptr) {
        pin_disabled = true;
        return false;
      }
      view_collection = obj;
    }
    return true;
  }

  // Pin a window to ALL virtual desktops (the taskbar's "show this window on
  // every desktop"): it then stays put during desktop switches instead of
  // sliding away with its owning desktop and being pulled back by our
  // z-order re-assertions.
  bool
  pin_window (HWND hwnd)
  {
    if (!ensure () || !ensure_pin_services ())
      return false;

    for (int attempt = 0; attempt < 2; ++attempt) {
      void *view = nullptr;
      HRESULT hr = vcall<HRESULT> (view_collection, kSlotGetViewForHwnd, hwnd,
                                   &view);
      if (FAILED (hr) || view == nullptr) {
        if (recoverable (hr) && attempt == 0) {
          release ();
          if (!ensure () || !ensure_pin_services ())
            return false;
          continue;
        }
        return false;
      }
      bool ok = SUCCEEDED (
          vcall<HRESULT> (pinned_apps, kSlotPinView, view));
      vrelease (view);
      return ok;
    }
    return false;
  }
};

ComState g_com;  // worker-thread-only

std::mutex g_queue_mu;
std::condition_variable g_cv;
Job *g_pending = nullptr;       // queued job (single slot)
std::vector<Job *> g_done;      // completed jobs awaiting pickup
bool g_worker_quit = false;
bool g_worker_stuck = false;    // a call never returned; abandon the module

// Execute @job on the worker thread. Blocks until COMPLETION (not pickup —
// waking on pickup raced with the worker still writing the results), with a
// hard timeout: the explorer-side VD RPC service can deadlock (e.g. for
// callers at a lower integrity level than the shell); rather than freezing
// the GTK main thread, give up after the timeout and disable the module.
void
run_on_worker (Job *job)
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
        g_cv.wait (lock, [] { return g_pending != nullptr || g_worker_quit; });
        if (g_worker_quit)
          break;

        Job *job = g_pending;
        g_pending = nullptr;
        lock.unlock ();

        switch (job->op) {
        case Job::Op::Desktops: {
          job->out_desktops = g_com.desktops ();
          job->ok = !job->out_desktops.empty ();
          break;
        }
        case Job::Op::CurrentIndex: {
          // Enumerate FIRST, then query the current desktop: on 22621 the
          // explorer VD service hangs when GetCurrentDesktop is the first
          // call after Connect (verified empirically; the enumerate-then-
          // query order is reliable).
          auto desktops = g_com.desktops ();
          if (!g_com.ensure () || desktops.empty ())
            break;
          void *d = nullptr;
          HRESULT hr = vcall<HRESULT> (g_com.internal, kSlotGetCurrentDesktop,
                                       &d);
          if (SUCCEEDED (hr) && d != nullptr) {
            GUID id{};
            bool got = SUCCEEDED (vcall<HRESULT> (d, kSlotGetId, &id));
            vrelease (d);
            if (got) {
              for (const DesktopInfo &info : desktops)
                if (IsEqualGUID (info.id, id)) {
                  job->out_index = info.index;
                  job->ok = true;
                  break;
                }
            }
          }
          break;
        }
        case Job::Op::WindowDesktopIndex: {
          if (!g_com.ensure ())
            break;
          GUID id{};
          if (SUCCEEDED (vcall<HRESULT> (g_com.manager, kSlotDesktopByWindow,
                                         job->in_hwnd, &id))) {
            GUID zero{};
            if (!IsEqualGUID (id, zero)) {
              int idx = g_com.index_of (id);
              if (idx >= 0) {
                job->out_index = idx;
                job->ok = true;
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
                                         job->in_hwnd, &on))) {
            job->out_bool = on != FALSE;
            job->ok = true;
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
          if (job->in_index >= 0 && (UINT)job->in_index < n) {
            void *d = nullptr;
            if (SUCCEEDED (vcall<HRESULT> (arr, kSlotGetAt,
                                           (UINT)job->in_index,
                                           &kIID_IVirtualDesktop, &d)) &&
                d != nullptr) {
              job->ok = SUCCEEDED (
                  vcall<HRESULT> (g_com.internal, kSlotSwitchDesktop, d));
              vrelease (d);
            }
          }
          vrelease (arr);
          break;
        }
        case Job::Op::PinWindow: {
          job->ok = g_com.pin_window (job->in_hwnd);
          break;
        }
        }

        lock.lock ();
        g_done.push_back (job);
        g_cv.notify_all ();
      }
    });
    worker.detach ();  // never join: Release after combase teardown crashes
  });

  std::unique_lock<std::mutex> lock (g_queue_mu);
  if (g_worker_stuck) {
    delete job;  // worker parked inside explorer; caller sees failure
    return;
  }
  g_pending = job;
  g_cv.notify_all ();
  // 8s is far above a healthy round trip (single-digit ms) and below any
  // user-tolerable UI freeze.
  bool done = g_cv.wait_for (lock, std::chrono::seconds (8), [job] {
    return std::find (g_done.begin (), g_done.end (), job) != g_done.end ();
  });
  if (done) {
    g_done.erase (std::find (g_done.begin (), g_done.end (), job));
    // Results read by the caller; ownership stays with the caller.
  } else {
    // Explorer never answered: disable the module. The worker may still
    // complete this job later — it owns the memory now, and pushes it to
    // g_done where nobody picks it up (one small leak, once).
    g_worker_stuck = true;
  }
}

}  // namespace

std::vector<DesktopInfo>
desktops (void)
{
  Job *job = new Job{Job::Op::Desktops};
  run_on_worker (job);
  std::vector<DesktopInfo> out = std::move (job->out_desktops);
  delete job;
  return out;
}

int
count (void)
{
  return (int)desktops ().size ();
}

int
current_index (void)
{
  Job *job = new Job{Job::Op::CurrentIndex};
  run_on_worker (job);
  int out = job->ok ? job->out_index : -1;
  delete job;
  return out;
}

int
window_desktop_index (HWND hwnd)
{
  Job *job = new Job{Job::Op::WindowDesktopIndex};
  job->in_hwnd = hwnd;
  run_on_worker (job);
  int out = job->ok ? job->out_index : -1;
  delete job;
  return out;
}

bool
window_on_current (HWND hwnd)
{
  Job *job = new Job{Job::Op::WindowOnCurrent};
  job->in_hwnd = hwnd;
  run_on_worker (job);
  bool out = job->ok ? job->out_bool : false;
  delete job;
  return out;
}

bool
switch_to (int index)
{
  Job *job = new Job{Job::Op::SwitchTo};
  job->in_index = index;
  run_on_worker (job);
  bool out = job->ok;
  delete job;
  return out;
}

bool
pin_window (HWND hwnd)
{
  if (hwnd == nullptr)
    return false;
  Job *job = new Job{Job::Op::PinWindow};
  job->in_hwnd = hwnd;
  run_on_worker (job);
  bool out = job->ok;
  delete job;
  return out;
}

bool
available (void)
{
  return !desktops ().empty ();
}

}  // namespace vd
}  // namespace winome
