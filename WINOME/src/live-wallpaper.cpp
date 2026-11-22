// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors

#ifndef WINDOWS_FOUNDATION_UNIVERSALAPICONTRACT_VERSION
#define WINDOWS_FOUNDATION_UNIVERSALAPICONTRACT_VERSION 0x100000
#endif

#include "live-wallpaper.h"

#include <d3d11.h>
#include <dxgi.h>
#include <inspectable.h>
#include <windows.foundation.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <windows.graphics.directx.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace winome {

namespace {

using ABI::Windows::Foundation::IClosable;
using ABI::Windows::Foundation::ITypedEventHandler;
using ABI::Windows::Graphics::SizeInt32;
using ABI::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using ABI::Windows::Graphics::Capture::IDirect3D11CaptureFrame;
using ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool;
using ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics2;
using ABI::Windows::Graphics::Capture::IGraphicsCaptureItem;
using ABI::Windows::Graphics::Capture::IGraphicsCaptureSession;
using ABI::Windows::Graphics::Capture::IGraphicsCaptureSession2;
using ABI::Windows::Graphics::Capture::IGraphicsCaptureSession3;
using ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized;
using ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface;
using Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

// IID for ITypedEventHandler<Direct3D11CaptureFramePool*, IInspectable*>.
// Matches windows.graphics.capture.h:2632
// (__FITypedEventHandler_2_Windows__CGraphics__CCapture__CDirect3D11CaptureFramePool_IInspectable).
static const IID kIidFrameArrivedHandler = {
    0x51a947f7, 0x79cf, 0x5a3e,
    {0xa3, 0xa5, 0x12, 0x89, 0xcf, 0xa6, 0xdf, 0xe8}};
static const IID kIidIAgileObject = {
    0x94ea2b94, 0xe9cc, 0x49e0,
    {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
static const IID kIidIClosable = {
    0x30d5a829, 0x7fa4, 0x4026,
    {0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e}};
static const IID kIidGraphicsCaptureItemInterop = {
    0x3628e81b, 0x3cac, 0x4c60,
    {0xb7, 0xf4, 0x23, 0xce, 0x0e, 0x0c, 0x33, 0x56}};
static const IID kIidGraphicsCaptureItem = {
    0x79c3f95b, 0x31f7, 0x4ec2,
    {0xa4, 0x64, 0x63, 0x2e, 0xf5, 0xd3, 0x07, 0x60}};
static const IID kIidFramePoolStatics2 = {
    0x589b103f, 0x6bbc, 0x5df5,
    {0xa9, 0x91, 0x02, 0xe2, 0x8b, 0x3b, 0x66, 0xd5}};
static const IID kIidCaptureSession2 = {
    0x2c39ae40, 0x7d2e, 0x5044,
    {0x80, 0x4e, 0x8b, 0x67, 0x99, 0xd4, 0xcf, 0x9e}};
static const IID kIidCaptureSession3 = {
    0xf2cdd966, 0x22ae, 0x5ea1,
    {0x95, 0x96, 0x3a, 0x28, 0x93, 0x44, 0xc3, 0xbe}};
static const IID kIidDirect3DDevice = {
    0xa37624ab, 0x8d5f, 0x4650,
    {0x9d, 0x3e, 0x9e, 0xae, 0x3d, 0x9b, 0xc6, 0x70}};
static const IID kIidDxgiInterfaceAccess = {
    0xa9b3d012, 0x3df2, 0x4ee3,
    {0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1}};

struct DynamicWgcApi {
  HMODULE combase = nullptr;
  HMODULE d3d11 = nullptr;
  HRESULT (WINAPI* RoInitialize)(int) = nullptr;
  void (WINAPI* RoUninitialize)() = nullptr;
  HRESULT (WINAPI* RoGetActivationFactory)(HSTRING, REFIID, void**) = nullptr;
  HRESULT (WINAPI* WindowsCreateString)(const wchar_t*, UINT32, HSTRING*) = nullptr;
  HRESULT (WINAPI* WindowsDeleteString)(HSTRING) = nullptr;
  HRESULT (WINAPI* D3D11CreateDeviceFn)(
      IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
      const D3D_FEATURE_LEVEL*, UINT, UINT,
      ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**) = nullptr;
  HRESULT (WINAPI* CreateDirect3D11DeviceFromDXGIDeviceFn)(
      IDXGIDevice*, IInspectable**) = nullptr;
  bool ready = false;
  bool probed = false;
};

DynamicWgcApi& wgc_api() {
  static DynamicWgcApi api;
  if (!api.probed) {
    api.probed = true;
    api.combase = LoadLibraryW(L"combase.dll");
    api.d3d11 = LoadLibraryW(L"d3d11.dll");
    auto load = [](HMODULE mod, auto& fn, const char* name) {
      if (mod != nullptr) {
        fn = reinterpret_cast<std::decay_t<decltype(fn)>>(
            reinterpret_cast<void*>(GetProcAddress(mod, name)));
      }
    };
    load(api.combase, api.RoInitialize, "RoInitialize");
    load(api.combase, api.RoUninitialize, "RoUninitialize");
    load(api.combase, api.RoGetActivationFactory, "RoGetActivationFactory");
    load(api.combase, api.WindowsCreateString, "WindowsCreateString");
    load(api.combase, api.WindowsDeleteString, "WindowsDeleteString");
    load(api.d3d11, api.D3D11CreateDeviceFn, "D3D11CreateDevice");
    load(api.d3d11, api.CreateDirect3D11DeviceFromDXGIDeviceFn,
         "CreateDirect3D11DeviceFromDXGIDevice");
    api.ready = api.RoInitialize && api.RoUninitialize &&
                api.RoGetActivationFactory && api.WindowsCreateString &&
                api.WindowsDeleteString && api.D3D11CreateDeviceFn &&
                api.CreateDirect3D11DeviceFromDXGIDeviceFn;
  }
  return api;
}

class FrameArrivedHandler final
    : public ITypedEventHandler<Direct3D11CaptureFramePool*, IInspectable*> {
 public:
  explicit FrameArrivedHandler(HANDLE event_handle)
      : event_(event_handle), ref_count_(1) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr)
      return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, kIidFrameArrivedHandler) ||
        IsEqualIID(riid, kIidIAgileObject)) {
      *ppv = static_cast<
          ITypedEventHandler<Direct3D11CaptureFramePool*, IInspectable*>*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(ref_count_.fetch_add(1, std::memory_order_relaxed) + 1);
  }

  ULONG STDMETHODCALLTYPE Release() override {
    ULONG n = static_cast<ULONG>(ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1);
    if (n == 0)
      delete this;
    return n;
  }

  HRESULT STDMETHODCALLTYPE Invoke(IDirect3D11CaptureFramePool* /*sender*/,
                                   IInspectable* /*args*/) override {
    if (event_ != nullptr)
      SetEvent(event_);
    return S_OK;
  }

 private:
  HANDLE event_;
  std::atomic<long> ref_count_;
};

std::wstring to_lower_wide(const std::wstring& s) {
  std::wstring out = s;
  for (wchar_t& c : out)
    c = static_cast<wchar_t>(std::towlower(c));
  return out;
}

std::wstring query_hwnd_process_basename(HWND hwnd) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == 0)
    return {};
  HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (proc == nullptr)
    return {};
  wchar_t path[MAX_PATH] = {};
  DWORD len = MAX_PATH;
  BOOL ok = QueryFullProcessImageNameW(proc, 0, path, &len);
  CloseHandle(proc);
  if (!ok || len == 0)
    return {};
  const wchar_t* slash = std::wcsrchr(path, L'\\');
  return slash != nullptr ? std::wstring(slash + 1) : std::wstring(path);
}

// Fast BGRX (opaque 4-byte) downsample to <=320x180 for g_thumbs pills.
GBytes* downsample_bgrx_thumb(const uint8_t* src,
                              int src_w,
                              int src_h,
                              size_t src_stride,
                              int* out_w,
                              int* out_h,
                              size_t* out_stride) {
  if (src == nullptr || src_w <= 0 || src_h <= 0 || out_w == nullptr ||
      out_h == nullptr || out_stride == nullptr) {
    return nullptr;
  }
  const double s = std::min({320.0 / src_w, 180.0 / src_h, 1.0});
  const int tw = std::max(1, static_cast<int>(std::lround(src_w * s)));
  const int th = std::max(1, static_cast<int>(std::lround(src_h * s)));
  const size_t dst_stride = static_cast<size_t>(tw) * 4u;
  auto* dst = static_cast<uint8_t*>(
      g_try_malloc_n(static_cast<gsize>(th), static_cast<gsize>(dst_stride)));
  if (dst == nullptr)
    return nullptr;

  for (int y = 0; y < th; ++y) {
    const int sy = std::min(src_h - 1, (y * src_h) / th);
    const uint8_t* srow = src + static_cast<size_t>(sy) * src_stride;
    uint8_t* drow = dst + static_cast<size_t>(y) * dst_stride;
    for (int x = 0; x < tw; ++x) {
      const int sx = std::min(src_w - 1, (x * src_w) / tw);
      std::memcpy(drow + static_cast<size_t>(x) * 4u,
                  srow + static_cast<size_t>(sx) * 4u, 4u);
    }
  }
  *out_w = tw;
  *out_h = th;
  *out_stride = dst_stride;
  return g_bytes_new_take(dst, static_cast<gsize>(th) * dst_stride);
}

struct CapturedFrameBuffers {
  GBytes* full_bytes = nullptr;
  int full_w = 0;
  int full_h = 0;
  size_t full_stride = 0;
  GBytes* thumb_bytes = nullptr;
  int thumb_w = 0;
  int thumb_h = 0;
  size_t thumb_stride = 0;

  ~CapturedFrameBuffers() {
    if (full_bytes != nullptr)
      g_bytes_unref(full_bytes);
    if (thumb_bytes != nullptr)
      g_bytes_unref(thumb_bytes);
  }
};

// PrintWindow fallback for older Windows 10 builds where WGC cannot hide its
// yellow border (IGraphicsCaptureSession3 unavailable).
std::unique_ptr<CapturedFrameBuffers> capture_printwindow_fallback(
    HWND engine_child_hwnd,
    const RECT& monitor_rect) {
  if (engine_child_hwnd == nullptr || !IsWindow(engine_child_hwnd))
    return nullptr;
  RECT wr{};
  if (!GetWindowRect(engine_child_hwnd, &wr))
    return nullptr;
  const int ww = wr.right - wr.left;
  const int wh = wr.bottom - wr.top;
  if (ww <= 0 || wh <= 0)
    return nullptr;

  HDC screen_dc = GetDC(nullptr);
  if (screen_dc == nullptr)
    return nullptr;
  HDC mem_dc = CreateCompatibleDC(screen_dc);
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = ww;
  bmi.bmiHeader.biHeight = -wh;  // top-down BGRX
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(screen_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (mem_dc == nullptr || bmp == nullptr || bits == nullptr) {
    if (bmp != nullptr)
      DeleteObject(bmp);
    if (mem_dc != nullptr)
      DeleteDC(mem_dc);
    ReleaseDC(nullptr, screen_dc);
    return nullptr;
  }

  HGDIOBJ old_bmp = SelectObject(mem_dc, bmp);
  BOOL ok = PrintWindow(engine_child_hwnd, mem_dc, PW_RENDERFULLCONTENT);
  if (!ok)
    ok = PrintWindow(engine_child_hwnd, mem_dc, 0);

  std::unique_ptr<CapturedFrameBuffers> out;
  if (ok) {
    const RECT crop = compute_wgc_crop_rect(wr, monitor_rect, ww, wh);
    const int cw = crop.right - crop.left;
    const int ch = crop.bottom - crop.top;
    if (cw > 0 && ch > 0) {
      const size_t src_stride = static_cast<size_t>(ww) * 4u;
      const size_t dst_stride = static_cast<size_t>(cw) * 4u;
      auto* packed = static_cast<uint8_t*>(
          g_try_malloc_n(static_cast<gsize>(ch), static_cast<gsize>(dst_stride)));
      if (packed != nullptr) {
        const auto* src_base = static_cast<const uint8_t*>(bits);
        bool non_black = false;
        for (int y = 0; y < ch; ++y) {
          const uint8_t* srow =
              src_base + static_cast<size_t>(crop.top + y) * src_stride +
              static_cast<size_t>(crop.left) * 4u;
          uint8_t* drow = packed + static_cast<size_t>(y) * dst_stride;
          std::memcpy(drow, srow, dst_stride);
          if (!non_black && (y % 16 == 0)) {
            for (int x = 0; x < cw; x += 32) {
              if (drow[x * 4 + 0] != 0 || drow[x * 4 + 1] != 0 ||
                  drow[x * 4 + 2] != 0) {
                non_black = true;
                break;
              }
            }
          }
        }
        if (non_black) {
          out = std::make_unique<CapturedFrameBuffers>();
          out->full_w = cw;
          out->full_h = ch;
          out->full_stride = dst_stride;
          out->thumb_bytes = downsample_bgrx_thumb(
              packed, cw, ch, dst_stride, &out->thumb_w, &out->thumb_h,
              &out->thumb_stride);
          out->full_bytes =
              g_bytes_new_take(packed, static_cast<gsize>(ch) * dst_stride);
        } else {
          g_free(packed);
        }
      }
    }
  }

  SelectObject(mem_dc, old_bmp);
  DeleteObject(bmp);
  DeleteDC(mem_dc);
  ReleaseDC(nullptr, screen_dc);
  return out;
}

struct LiveSessionState {
  std::mutex mu;
  std::thread worker;
  HANDLE stop_event = nullptr;
  std::atomic<bool> running{false};
  std::atomic<bool> paused{false};
  std::atomic<uint64_t> session_gen{0};

  LiveWallpaperTarget target{};
  RECT monitor_rect{};
  bool live_stream = true;
  LiveWallpaperFrameCb on_frame = nullptr;
  void* user_data = nullptr;

  // Drop-oldest single-slot mailbox to the GTK main thread.
  std::mutex mailbox_mu;
  std::unique_ptr<CapturedFrameBuffers> latest_frame;
  uint64_t latest_frame_gen = 0;
  std::atomic<bool> idle_scheduled{false};
};

LiveSessionState g_session;

gboolean deliver_frame_idle(gpointer /*user_data*/) {
  g_session.idle_scheduled.store(false, std::memory_order_release);

  std::unique_ptr<CapturedFrameBuffers> frame;
  uint64_t frame_gen = 0;
  {
    std::lock_guard<std::mutex> lk(g_session.mailbox_mu);
    frame = std::move(g_session.latest_frame);
    frame_gen = g_session.latest_frame_gen;
  }
  if (!frame || frame->full_bytes == nullptr || frame->full_w <= 0 ||
      frame->full_h <= 0) {
    return G_SOURCE_REMOVE;
  }
  if (frame_gen != g_session.session_gen.load(std::memory_order_acquire))
    return G_SOURCE_REMOVE;

  LiveWallpaperFrameCb cb = nullptr;
  void* ud = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_session.mu);
    cb = g_session.on_frame;
    ud = g_session.user_data;
  }
  if (cb == nullptr)
    return G_SOURCE_REMOVE;

  GdkTexture* full_tex = gdk_memory_texture_new(
      frame->full_w, frame->full_h, GDK_MEMORY_B8G8R8X8, frame->full_bytes,
      frame->full_stride);
  GdkTexture* thumb_tex = nullptr;
  if (frame->thumb_bytes != nullptr && frame->thumb_w > 0 &&
      frame->thumb_h > 0) {
    thumb_tex = gdk_memory_texture_new(
        frame->thumb_w, frame->thumb_h, GDK_MEMORY_B8G8R8X8, frame->thumb_bytes,
        frame->thumb_stride);
  }
  if (full_tex != nullptr) {
    cb(full_tex, thumb_tex, ud);
    g_object_unref(full_tex);
  }
  if (thumb_tex != nullptr)
    g_object_unref(thumb_tex);
  return G_SOURCE_REMOVE;
}

void publish_frame(std::unique_ptr<CapturedFrameBuffers> frame, uint64_t gen) {
  if (!frame || gen != g_session.session_gen.load(std::memory_order_acquire))
    return;
  {
    std::lock_guard<std::mutex> lk(g_session.mailbox_mu);
    g_session.latest_frame = std::move(frame);
    g_session.latest_frame_gen = gen;
  }
  bool expected = false;
  if (g_session.idle_scheduled.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) {
    g_idle_add_full(G_PRIORITY_LOW, deliver_frame_idle, nullptr, nullptr);
  }
}

void close_and_release_inspectable(IUnknown* obj) {
  if (obj == nullptr)
    return;
  IClosable* closable = nullptr;
  if (SUCCEEDED(obj->QueryInterface(kIidIClosable,
                                    reinterpret_cast<void**>(&closable))) &&
      closable != nullptr) {
    closable->Close();
    closable->Release();
  }
  obj->Release();
}

void worker_main(LiveWallpaperTarget target,
                 RECT monitor_rect,
                 bool live_stream,
                 uint64_t gen,
                 HANDLE stop_event) {
  DynamicWgcApi& api = wgc_api();
  bool wgc_started = false;

  if (api.ready && target.capture_hwnd != nullptr &&
      IsWindow(target.capture_hwnd)) {
    const HRESULT ro_hr = api.RoInitialize(1 /* RO_INIT_MULTITHREADED */);
    const bool did_ro = SUCCEEDED(ro_hr);

    ID3D11Device* d3d_dev = nullptr;
    ID3D11DeviceContext* d3d_ctx = nullptr;
    IDXGIDevice* dxgi_dev = nullptr;
    IInspectable* winrt_dev_insp = nullptr;
    IDirect3DDevice* winrt_dev = nullptr;
    IGraphicsCaptureItemInterop* interop = nullptr;
    IGraphicsCaptureItem* item = nullptr;
    IDirect3D11CaptureFramePoolStatics2* pool_statics = nullptr;
    IDirect3D11CaptureFramePool* frame_pool = nullptr;
    IGraphicsCaptureSession* session = nullptr;
    FrameArrivedHandler* handler = nullptr;
    EventRegistrationToken token{};
    bool has_token = false;
    ID3D11Texture2D* staging = nullptr;
    int staging_w = 0;
    int staging_h = 0;
    HANDLE frame_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    auto cleanup_wgc = [&]() {
      if (staging != nullptr) {
        staging->Release();
        staging = nullptr;
      }
      if (frame_pool != nullptr && has_token) {
        frame_pool->remove_FrameArrived(token);
        has_token = false;
      }
      if (handler != nullptr) {
        handler->Release();
        handler = nullptr;
      }
      close_and_release_inspectable(session);
      session = nullptr;
      close_and_release_inspectable(frame_pool);
      frame_pool = nullptr;
      if (pool_statics != nullptr) {
        pool_statics->Release();
        pool_statics = nullptr;
      }
      if (item != nullptr) {
        item->Release();
        item = nullptr;
      }
      if (interop != nullptr) {
        interop->Release();
        interop = nullptr;
      }
      if (winrt_dev != nullptr) {
        winrt_dev->Release();
        winrt_dev = nullptr;
      }
      if (winrt_dev_insp != nullptr) {
        winrt_dev_insp->Release();
        winrt_dev_insp = nullptr;
      }
      if (dxgi_dev != nullptr) {
        dxgi_dev->Release();
        dxgi_dev = nullptr;
      }
      if (d3d_ctx != nullptr) {
        d3d_ctx->Release();
        d3d_ctx = nullptr;
      }
      if (d3d_dev != nullptr) {
        d3d_dev->Release();
        d3d_dev = nullptr;
      }
      if (frame_event != nullptr) {
        CloseHandle(frame_event);
        frame_event = nullptr;
      }
      if (did_ro)
        api.RoUninitialize();
    };

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    HRESULT hr = api.D3D11CreateDeviceFn(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 3, D3D11_SDK_VERSION,
        &d3d_dev, nullptr, &d3d_ctx);

    if (SUCCEEDED(hr) && d3d_dev != nullptr) {
      hr = d3d_dev->QueryInterface(__uuidof(IDXGIDevice),
                                   reinterpret_cast<void**>(&dxgi_dev));
    }
    if (SUCCEEDED(hr) && dxgi_dev != nullptr) {
      hr = api.CreateDirect3D11DeviceFromDXGIDeviceFn(dxgi_dev, &winrt_dev_insp);
    }
    if (SUCCEEDED(hr) && winrt_dev_insp != nullptr) {
      hr = winrt_dev_insp->QueryInterface(kIidDirect3DDevice,
                                          reinterpret_cast<void**>(&winrt_dev));
    }
    if (SUCCEEDED(hr) && winrt_dev != nullptr) {
      const wchar_t* cls_item =
          L"Windows.Graphics.Capture.GraphicsCaptureItem";
      HSTRING hs = nullptr;
      if (SUCCEEDED(api.WindowsCreateString(
              cls_item, static_cast<UINT32>(std::wcslen(cls_item)), &hs))) {
        hr = api.RoGetActivationFactory(hs, kIidGraphicsCaptureItemInterop,
                                        reinterpret_cast<void**>(&interop));
        api.WindowsDeleteString(hs);
      } else {
        hr = E_FAIL;
      }
    }
    if (SUCCEEDED(hr) && interop != nullptr) {
      hr = interop->CreateForWindow(target.capture_hwnd, kIidGraphicsCaptureItem,
                                    reinterpret_cast<void**>(&item));
    }
    SizeInt32 item_size{0, 0};
    if (SUCCEEDED(hr) && item != nullptr) {
      hr = item->get_Size(&item_size);
      if (item_size.Width <= 0 || item_size.Height <= 0)
        hr = E_FAIL;
    }
    if (SUCCEEDED(hr)) {
      const wchar_t* cls_pool =
          L"Windows.Graphics.Capture.Direct3D11CaptureFramePool";
      HSTRING hs = nullptr;
      if (SUCCEEDED(api.WindowsCreateString(
              cls_pool, static_cast<UINT32>(std::wcslen(cls_pool)), &hs))) {
        hr = api.RoGetActivationFactory(hs, kIidFramePoolStatics2,
                                        reinterpret_cast<void**>(&pool_statics));
        api.WindowsDeleteString(hs);
      } else {
        hr = E_FAIL;
      }
    }
    if (SUCCEEDED(hr) && pool_statics != nullptr) {
      hr = pool_statics->CreateFreeThreaded(
          winrt_dev, DirectXPixelFormat_B8G8R8A8UIntNormalized, 2, item_size,
          &frame_pool);
    }
    if (SUCCEEDED(hr) && frame_pool != nullptr && frame_event != nullptr) {
      handler = new FrameArrivedHandler(frame_event);
      hr = frame_pool->add_FrameArrived(handler, &token);
      if (SUCCEEDED(hr))
        has_token = true;
    }
    if (SUCCEEDED(hr) && frame_pool != nullptr) {
      hr = frame_pool->CreateCaptureSession(item, &session);
    }

    // Require borderless capture (Win11 21H2+ IGraphicsCaptureSession3) so we
    // never flash a yellow system capture border around the user's desktop.
    bool borderless_ok = false;
    if (SUCCEEDED(hr) && session != nullptr) {
      IGraphicsCaptureSession2* s2 = nullptr;
      if (SUCCEEDED(session->QueryInterface(kIidCaptureSession2,
                                            reinterpret_cast<void**>(&s2))) &&
          s2 != nullptr) {
        s2->put_IsCursorCaptureEnabled(FALSE);
        s2->Release();
      }
      IGraphicsCaptureSession3* s3 = nullptr;
      if (SUCCEEDED(session->QueryInterface(kIidCaptureSession3,
                                            reinterpret_cast<void**>(&s3))) &&
          s3 != nullptr) {
        if (SUCCEEDED(s3->put_IsBorderRequired(FALSE)))
          borderless_ok = true;
        s3->Release();
      }
    }

    if (SUCCEEDED(hr) && borderless_ok && session != nullptr) {
      hr = session->StartCapture();
      wgc_started = SUCCEEDED(hr);
    }

    if (wgc_started) {
      g_print("[live-wp] WGC started hwnd=%p engine=%ls mode=%s\n",
              static_cast<void*>(target.capture_hwnd),
              target.process_name.c_str(),
              live_stream ? "live" : "snapshot");

      HANDLE waits[2] = {stop_event, frame_event};
      uint64_t delivered_count = 0;
      auto last_deliver = std::chrono::steady_clock::time_point{};
      SizeInt32 pool_size = item_size;

      for (;;) {
        const DWORD timeout_ms = (delivered_count == 0) ? 1500u : 250u;
        const DWORD rc = WaitForMultipleObjects(2, waits, FALSE, timeout_ms);
        if (rc == WAIT_OBJECT_0)
          break;
        if (gen != g_session.session_gen.load(std::memory_order_acquire))
          break;
        if (rc != WAIT_OBJECT_0 + 1) {
          if (delivered_count == 0 && !live_stream)
            break;
          continue;
        }

        // Drain the frame pool, retaining only the freshest frame.
        IDirect3D11CaptureFrame* latest = nullptr;
        for (;;) {
          IDirect3D11CaptureFrame* next = nullptr;
          HRESULT fhr = frame_pool->TryGetNextFrame(&next);
          if (FAILED(fhr) || next == nullptr)
            break;
          if (latest != nullptr)
            close_and_release_inspectable(latest);
          latest = next;
        }
        if (latest == nullptr)
          continue;

        // Always allow frame 0 through so opening Overview gets fresh imagery
        // immediately; throttle subsequent live frames to ~30 FPS and pause
        // during 250ms Overview animations to preserve Intel iGPU headroom.
        const auto now = std::chrono::steady_clock::now();
        const bool skip_frame =
            (delivered_count > 0) &&
            (g_session.paused.load(std::memory_order_acquire) ||
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 now - last_deliver)
                     .count() < 33);

        SizeInt32 content_size{0, 0};
        latest->get_ContentSize(&content_size);
        if (content_size.Width > 0 && content_size.Height > 0 &&
            (content_size.Width != pool_size.Width ||
             content_size.Height != pool_size.Height)) {
          pool_size = content_size;
          frame_pool->Recreate(winrt_dev,
                               DirectXPixelFormat_B8G8R8A8UIntNormalized, 2,
                               pool_size);
        }

        if (!skip_frame) {
          IDirect3DSurface* surf = nullptr;
          if (SUCCEEDED(latest->get_Surface(&surf)) && surf != nullptr) {
            IDirect3DDxgiInterfaceAccess* access = nullptr;
            if (SUCCEEDED(surf->QueryInterface(
                    kIidDxgiInterfaceAccess,
                    reinterpret_cast<void**>(&access))) &&
                access != nullptr) {
              ID3D11Texture2D* gpu_tex = nullptr;
              if (SUCCEEDED(access->GetInterface(
                      __uuidof(ID3D11Texture2D),
                      reinterpret_cast<void**>(&gpu_tex))) &&
                  gpu_tex != nullptr) {
                D3D11_TEXTURE2D_DESC desc{};
                gpu_tex->GetDesc(&desc);
                RECT host_rc = target.host_rect;
                GetWindowRect(target.capture_hwnd, &host_rc);
                const RECT crop = compute_wgc_crop_rect(
                    host_rc, monitor_rect, static_cast<int>(desc.Width),
                    static_cast<int>(desc.Height));
                const int cw = crop.right - crop.left;
                const int ch = crop.bottom - crop.top;
                if (cw > 0 && ch > 0) {
                  if (staging == nullptr || staging_w != cw || staging_h != ch) {
                    if (staging != nullptr) {
                      staging->Release();
                      staging = nullptr;
                    }
                    D3D11_TEXTURE2D_DESC sdesc{};
                    sdesc.Width = static_cast<UINT>(cw);
                    sdesc.Height = static_cast<UINT>(ch);
                    sdesc.MipLevels = 1;
                    sdesc.ArraySize = 1;
                    sdesc.Format = desc.Format;
                    sdesc.SampleDesc.Count = 1;
                    sdesc.Usage = D3D11_USAGE_STAGING;
                    sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    if (SUCCEEDED(d3d_dev->CreateTexture2D(&sdesc, nullptr,
                                                           &staging))) {
                      staging_w = cw;
                      staging_h = ch;
                    }
                  }
                  if (staging != nullptr) {
                    D3D11_BOX box{};
                    box.left = static_cast<UINT>(crop.left);
                    box.top = static_cast<UINT>(crop.top);
                    box.right = static_cast<UINT>(crop.right);
                    box.bottom = static_cast<UINT>(crop.bottom);
                    box.front = 0;
                    box.back = 1;
                    d3d_ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, gpu_tex,
                                                   0, &box);
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    if (SUCCEEDED(d3d_ctx->Map(staging, 0, D3D11_MAP_READ, 0,
                                               &mapped)) &&
                        mapped.pData != nullptr) {
                      const size_t dst_stride = static_cast<size_t>(cw) * 4u;
                      auto* packed = static_cast<uint8_t*>(g_try_malloc_n(
                          static_cast<gsize>(ch),
                          static_cast<gsize>(dst_stride)));
                      if (packed != nullptr) {
                        const auto* sbase =
                            static_cast<const uint8_t*>(mapped.pData);
                        for (int y = 0; y < ch; ++y) {
                          std::memcpy(
                              packed + static_cast<size_t>(y) * dst_stride,
                              sbase + static_cast<size_t>(y) * mapped.RowPitch,
                              dst_stride);
                        }
                        auto frame = std::make_unique<CapturedFrameBuffers>();
                        frame->full_w = cw;
                        frame->full_h = ch;
                        frame->full_stride = dst_stride;
                        if (delivered_count == 0 || (delivered_count % 4 == 0)) {
                          frame->thumb_bytes = downsample_bgrx_thumb(
                              packed, cw, ch, dst_stride, &frame->thumb_w,
                              &frame->thumb_h, &frame->thumb_stride);
                        }
                        frame->full_bytes = g_bytes_new_take(
                            packed, static_cast<gsize>(ch) * dst_stride);
                        publish_frame(std::move(frame), gen);
                        ++delivered_count;
                        last_deliver = now;
                      }
                      d3d_ctx->Unmap(staging, 0);
                    }
                  }
                }
                gpu_tex->Release();
              }
              access->Release();
            }
            surf->Release();
          }
        }
        close_and_release_inspectable(latest);

        if (!live_stream && delivered_count >= 1)
          break;
      }
    }

    cleanup_wgc();
  }

  // Graceful borderless fallback when WGC is unavailable or cannot hide its
  // yellow border (e.g. older Windows 10).
  if (!wgc_started &&
      gen == g_session.session_gen.load(std::memory_order_acquire)) {
    auto fb = capture_printwindow_fallback(target.engine_child_hwnd, monitor_rect);
    if (fb) {
      g_print("[live-wp] PrintWindow fallback snapshot engine=%ls\n",
              target.process_name.c_str());
      publish_frame(std::move(fb), gen);
    }
  }

  g_session.running.store(false, std::memory_order_release);
}

}  // namespace

std::string sanitize_live_wallpaper_mode(const std::string& mode) {
  if (mode == "live" || mode == "snapshot" || mode == "off")
    return mode;
  return "live";
}

bool is_live_wallpaper_candidate(const std::wstring& class_name,
                                 const std::wstring& process_basename,
                                 const RECT& child_rect,
                                 const RECT& monitor_rect,
                                 bool visible) {
  if (!visible)
    return false;
  const LONG mon_w = monitor_rect.right - monitor_rect.left;
  const LONG mon_h = monitor_rect.bottom - monitor_rect.top;
  if (mon_w <= 0 || mon_h <= 0)
    return false;
  if (child_rect.right <= child_rect.left ||
      child_rect.bottom <= child_rect.top) {
    return false;
  }

  const std::wstring cls = to_lower_wide(class_name);
  if (cls == L"shelldll_defview" || cls == L"syslistview32" ||
      cls == L"workerw" || cls == L"progman" || cls == L"folderview" ||
      cls == L"directuihwnd" || cls == L"scrollbar" ||
      cls == L"toolbarwindow32" || cls == L"rebarwindow32" ||
      cls == L"button" || cls == L"static") {
    return false;
  }

  const std::wstring proc = to_lower_wide(process_basename);
  if (proc.empty() || proc == L"explorer.exe" || proc == L"winome.exe" ||
      proc == L"shellexperiencehost.exe" || proc == L"searchhost.exe" ||
      proc == L"startmenuexperiencehost.exe") {
    return false;
  }

  const LONG ix1 = std::max(child_rect.left, monitor_rect.left);
  const LONG iy1 = std::max(child_rect.top, monitor_rect.top);
  const LONG ix2 = std::min(child_rect.right, monitor_rect.right);
  const LONG iy2 = std::min(child_rect.bottom, monitor_rect.bottom);
  if (ix2 <= ix1 || iy2 <= iy1)
    return false;

  const double cov_w = static_cast<double>(ix2 - ix1) / static_cast<double>(mon_w);
  const double cov_h = static_cast<double>(iy2 - iy1) / static_cast<double>(mon_h);
  return cov_w >= 0.5 && cov_h >= 0.5;
}

RECT compute_wgc_crop_rect(const RECT& host_rect,
                           const RECT& monitor_rect,
                           int tex_w,
                           int tex_h) {
  RECT out{0, 0, 0, 0};
  if (tex_w <= 0 || tex_h <= 0)
    return out;

  const int host_w = static_cast<int>(host_rect.right - host_rect.left);
  const int host_h = static_cast<int>(host_rect.bottom - host_rect.top);
  if (host_w <= 0 || host_h <= 0) {
    out.right = tex_w;
    out.bottom = tex_h;
    return out;
  }

  const double sx = static_cast<double>(tex_w) / static_cast<double>(host_w);
  const double sy = static_cast<double>(tex_h) / static_cast<double>(host_h);

  const int l = std::clamp(
      static_cast<int>(std::lround((monitor_rect.left - host_rect.left) * sx)),
      0, tex_w - 1);
  const int t = std::clamp(
      static_cast<int>(std::lround((monitor_rect.top - host_rect.top) * sy)),
      0, tex_h - 1);
  const int r = std::clamp(
      static_cast<int>(std::lround((monitor_rect.right - host_rect.left) * sx)),
      l + 1, tex_w);
  const int b = std::clamp(
      static_cast<int>(std::lround((monitor_rect.bottom - host_rect.top) * sy)),
      t + 1, tex_h);

  out.left = l;
  out.top = t;
  out.right = r;
  out.bottom = b;
  return out;
}

LiveWallpaperTarget live_wallpaper_detect_target(const RECT& monitor_rect) {
  LiveWallpaperTarget best;
  long long best_area = 0;

  auto inspect_children = [&](HWND container, auto&& self_ref, int depth) -> void {
    if (container == nullptr || depth > 2)
      return;
    for (HWND child = GetWindow(container, GW_CHILD); child != nullptr;
         child = GetWindow(child, GW_HWNDNEXT)) {
      wchar_t cls[128] = {};
      GetClassNameW(child, cls, 128);
      // Win11 24H2 nests a background WorkerW directly under Progman; descend
      // into nested WorkerW containers while never matching WorkerW itself.
      if (_wcsicmp(cls, L"WorkerW") == 0) {
        self_ref(child, self_ref, depth + 1);
        continue;
      }
      if (!IsWindowVisible(child))
        continue;
      RECT rc{};
      if (!GetWindowRect(child, &rc))
        continue;
      const std::wstring proc = query_hwnd_process_basename(child);
      if (!is_live_wallpaper_candidate(cls, proc, rc, monitor_rect, true))
        continue;

      const LONG ix1 = std::max(rc.left, monitor_rect.left);
      const LONG iy1 = std::max(rc.top, monitor_rect.top);
      const LONG ix2 = std::min(rc.right, monitor_rect.right);
      const LONG iy2 = std::min(rc.bottom, monitor_rect.bottom);
      const long long area =
          static_cast<long long>(ix2 - ix1) * static_cast<long long>(iy2 - iy1);
      if (area > best_area) {
        HWND root = GetAncestor(child, GA_ROOT);
        if (root == nullptr)
          root = container;
        RECT host_rc{};
        if (!GetWindowRect(root, &host_rc))
          host_rc = monitor_rect;
        best_area = area;
        best.active = true;
        best.engine_child_hwnd = child;
        best.capture_hwnd = root;
        best.host_rect = host_rc;
        best.child_rect = rc;
        best.process_name = proc;
      }
    }
  };

  if (HWND progman = FindWindowW(L"Progman", nullptr))
    inspect_children(progman, inspect_children, 0);

  for (HWND ww = FindWindowExW(nullptr, nullptr, L"WorkerW", nullptr);
       ww != nullptr;
       ww = FindWindowExW(nullptr, ww, L"WorkerW", nullptr)) {
    inspect_children(ww, inspect_children, 0);
  }

  return best;
}

bool live_wallpaper_start(const LiveWallpaperTarget& target,
                          const RECT& monitor_rect,
                          bool live_stream,
                          LiveWallpaperFrameCb on_frame,
                          void* user_data) {
  if (!target.active || target.capture_hwnd == nullptr || on_frame == nullptr) {
    live_wallpaper_stop();
    return false;
  }

  {
    std::lock_guard<std::mutex> lk(g_session.mu);
    if (g_session.running.load(std::memory_order_acquire) &&
        g_session.target.capture_hwnd == target.capture_hwnd &&
        g_session.target.engine_child_hwnd == target.engine_child_hwnd &&
        EqualRect(&g_session.monitor_rect, &monitor_rect) &&
        g_session.live_stream == live_stream) {
      g_session.on_frame = on_frame;
      g_session.user_data = user_data;
      return true;
    }
  }

  live_wallpaper_stop();

  std::lock_guard<std::mutex> lk(g_session.mu);
  g_session.stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (g_session.stop_event == nullptr)
    return false;

  g_session.target = target;
  g_session.monitor_rect = monitor_rect;
  g_session.live_stream = live_stream;
  g_session.on_frame = on_frame;
  g_session.user_data = user_data;
  const uint64_t gen =
      g_session.session_gen.fetch_add(1, std::memory_order_acq_rel) + 1;
  g_session.running.store(true, std::memory_order_release);

  HANDLE ev = g_session.stop_event;
  g_session.worker = std::thread(worker_main, target, monitor_rect, live_stream,
                                 gen, ev);
  return true;
}

void live_wallpaper_set_paused(bool paused) {
  g_session.paused.store(paused, std::memory_order_release);
}

void live_wallpaper_stop() {
  std::thread to_join;
  HANDLE ev = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_session.mu);
    g_session.session_gen.fetch_add(1, std::memory_order_acq_rel);
    g_session.running.store(false, std::memory_order_release);
    ev = g_session.stop_event;
    g_session.stop_event = nullptr;
    if (ev != nullptr)
      SetEvent(ev);
    to_join = std::move(g_session.worker);
    g_session.on_frame = nullptr;
    g_session.user_data = nullptr;
  }
  if (to_join.joinable())
    to_join.join();
  if (ev != nullptr)
    CloseHandle(ev);
  {
    std::lock_guard<std::mutex> lk(g_session.mailbox_mu);
    g_session.latest_frame.reset();
  }
}

bool live_wallpaper_is_running() {
  return g_session.running.load(std::memory_order_acquire);
}

}  // namespace winome
