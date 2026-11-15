// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Multi-monitor enumeration, OS event watcher, and secondary panel host.

#include "monitor-manager.h"

#include "config.h"

#include <gdk/win32/gdkwin32.h>
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace winome {
namespace {

constexpr const wchar_t* kWatcherClass = L"WINOME_MonitorWatcher";
#ifndef WTS_SESSION_LOCK
constexpr WPARAM kWtsSessionLock = 0x7;
#else
constexpr WPARAM kWtsSessionLock = WTS_SESSION_LOCK;
#endif
#ifndef WTS_SESSION_UNLOCK
constexpr WPARAM kWtsSessionUnlock = 0x8;
#else
constexpr WPARAM kWtsSessionUnlock = WTS_SESSION_UNLOCK;
#endif
#ifndef NOTIFY_FOR_THIS_SESSION
constexpr DWORD kNotifyForThisSession = 0;
#else
constexpr DWORD kNotifyForThisSession = NOTIFY_FOR_THIS_SESSION;
#endif

struct SecondaryPanelSlot {
  HMONITOR hmonitor = nullptr;
  GtkWidget* window = nullptr;
  HWND hwnd = nullptr;
  RECT rect{};
};

struct MonitorManagerState {
  HWND primary_panel = nullptr;
  HWND watcher_hwnd = nullptr;
  MonitorRuntimeCallbacks cbs{};
  std::vector<SecondaryPanelSlot> secondary_panels;
  bool wts_registered = false;
  HMODULE wts_dll = nullptr;
  BOOL(WINAPI* wts_unreg)(HWND) = nullptr;
  guint display_debounce_id = 0;
  bool initialized = false;
};

MonitorManagerState g_mm;

using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);

void query_monitor_dpi(HMONITOR hmon, UINT* out_x, UINT* out_y) {
  static bool probed = false;
  static GetDpiForMonitorFn get_dpi_fn = nullptr;
  if (!probed) {
    probed = true;
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore != nullptr) {
      get_dpi_fn = reinterpret_cast<GetDpiForMonitorFn>(
          reinterpret_cast<void*>(GetProcAddress(shcore, "GetDpiForMonitor")));
    }
  }
  UINT dx = 96;
  UINT dy = 96;
  if (get_dpi_fn != nullptr && get_dpi_fn(hmon, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy) == S_OK) {
    *out_x = dx;
    *out_y = dy;
  } else {
    *out_x = 96;
    *out_y = 96;
  }
}

BOOL CALLBACK enum_monitors_proc(HMONITOR hmon,
                                 HDC G_GNUC_UNUSED hdc,
                                 LPRECT G_GNUC_UNUSED lprc,
                                 LPARAM lp) {
  auto* list = reinterpret_cast<std::vector<MonitorDescriptor>*>(lp);
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(hmon, &mi))
    return TRUE;

  MonitorDescriptor d;
  d.hmonitor = hmon;
  d.monitor_rect = mi.rcMonitor;
  d.work_rect = mi.rcWork;
  d.is_primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
  d.device_name = mi.szDevice;
  query_monitor_dpi(hmon, &d.dpi_x, &d.dpi_y);
  list->push_back(std::move(d));
  return TRUE;
}

int resolved_panel_height() {
  if (g_mm.cbs.panel_height != nullptr)
    return std::max(24, g_mm.cbs.panel_height());
  return 32;
}

void destroy_secondary_slot(SecondaryPanelSlot* slot) {
  if (slot == nullptr)
    return;
  if (slot->hwnd != nullptr && g_mm.cbs.unregister_panel != nullptr)
    g_mm.cbs.unregister_panel(slot->hwnd);
  if (slot->window != nullptr)
    gtk_window_destroy(GTK_WINDOW(slot->window));
  slot->window = nullptr;
  slot->hwnd = nullptr;
}

void destroy_all_secondary_panels() {
  for (auto& slot : g_mm.secondary_panels)
    destroy_secondary_slot(&slot);
  g_mm.secondary_panels.clear();
}

void position_panel_hwnd(HWND hwnd, const RECT& mon_rect, int height, bool top_band) {
  if (hwnd == nullptr || !IsWindow(hwnd))
    return;
  const int w = mon_rect.right - mon_rect.left;
  if (w <= 0 || height <= 0)
    return;

  SetWindowPos(hwnd, top_band ? HWND_TOPMOST : HWND_NOTOPMOST,
               mon_rect.left, mon_rect.top, w, height,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);

  LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  LONG_PTR want = (ex & ~WS_EX_APPWINDOW) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
  if (want != ex)
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, want);
}

void on_secondary_realize(GtkWidget* widget, gpointer user_data) {
  auto* target_rect = static_cast<RECT*>(user_data);
  GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(widget));
  if (surface == nullptr)
    return;
  HWND hwnd = static_cast<HWND>(gdk_win32_surface_get_handle(surface));
  if (hwnd == nullptr)
    return;

  for (auto& slot : g_mm.secondary_panels) {
    if (slot.window == widget) {
      slot.hwnd = hwnd;
      break;
    }
  }

  position_panel_hwnd(hwnd, *target_rect, resolved_panel_height(), true);
  if (g_mm.cbs.pin_window != nullptr)
    g_mm.cbs.pin_window(hwnd);
  if (g_mm.cbs.register_panel != nullptr)
    g_mm.cbs.register_panel(hwnd);
}

gboolean idle_handle_wallpaper_change(gpointer G_GNUC_UNUSED data) {
  if (g_mm.cbs.on_wallpaper_changed != nullptr)
    g_mm.cbs.on_wallpaper_changed();
  return G_SOURCE_REMOVE;
}

gboolean idle_handle_theme_change(gpointer G_GNUC_UNUSED data) {
  if (g_mm.cbs.on_theme_changed != nullptr)
    g_mm.cbs.on_theme_changed();
  return G_SOURCE_REMOVE;
}

gboolean idle_handle_workarea_change(gpointer G_GNUC_UNUSED data) {
  if (g_mm.cbs.on_workarea_changed != nullptr)
    g_mm.cbs.on_workarea_changed();
  return G_SOURCE_REMOVE;
}

gboolean idle_handle_session_lock(gpointer data) {
  bool locked = reinterpret_cast<gintptr>(data) != 0;
  if (g_mm.cbs.on_session_lock_changed != nullptr)
    g_mm.cbs.on_session_lock_changed(locked);
  if (!locked)
    monitor_manager_sync();
  return G_SOURCE_REMOVE;
}

gboolean debounce_display_change(gpointer G_GNUC_UNUSED data) {
  g_mm.display_debounce_id = 0;
  monitor_manager_sync();
  if (g_mm.cbs.on_wallpaper_changed != nullptr)
    g_mm.cbs.on_wallpaper_changed();
  return G_SOURCE_REMOVE;
}

LRESULT CALLBACK watcher_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
      if (g_mm.display_debounce_id != 0)
        g_source_remove(g_mm.display_debounce_id);
      g_mm.display_debounce_id = g_timeout_add(120, debounce_display_change, nullptr);
      return 0;

    case WM_SETTINGCHANGE: {
      bool is_wallpaper = (wParam == SPI_SETDESKWALLPAPER);
      const wchar_t* area = reinterpret_cast<const wchar_t*>(lParam);
      if (is_wallpaper) {
        g_idle_add(idle_handle_wallpaper_change, nullptr);
      } else if (wParam == SPI_SETWORKAREA) {
        g_idle_add(idle_handle_workarea_change, nullptr);
      } else if (area != nullptr &&
                 (_wcsicmp(area, L"ImmersiveColorSet") == 0 ||
                  _wcsicmp(area, L"Environment") == 0)) {
        g_idle_add(idle_handle_theme_change, nullptr);
      }
      return 0;
    }

    case WM_WTSSESSION_CHANGE:
      if (wParam == kWtsSessionLock)
        g_idle_add(idle_handle_session_lock, reinterpret_cast<gpointer>(static_cast<gintptr>(1)));
      else if (wParam == kWtsSessionUnlock)
        g_idle_add(idle_handle_session_lock, reinterpret_cast<gpointer>(static_cast<gintptr>(0)));
      return 0;

    default:
      return DefWindowProcW(hwnd, msg, wParam, lParam);
  }
}

void on_monitor_config_changed(void* data G_GNUC_UNUSED) {
  monitor_manager_sync();
}

}  // namespace

std::vector<MonitorDescriptor> enumerate_monitors() {
  std::vector<MonitorDescriptor> monitors;
  EnumDisplayMonitors(nullptr, nullptr, enum_monitors_proc,
                      reinterpret_cast<LPARAM>(&monitors));
  std::stable_sort(monitors.begin(), monitors.end(),
                   [](const MonitorDescriptor& a, const MonitorDescriptor& b) {
                     return a.is_primary && !b.is_primary;
                   });
  return monitors;
}

RECT compute_panel_work_area(const RECT& monitor_rect, int panel_height) {
  RECT work = monitor_rect;
  if (panel_height > 0 && monitor_rect.bottom > monitor_rect.top) {
    LONG clamped = std::min<LONG>(panel_height, monitor_rect.bottom - monitor_rect.top);
    work.top += clamped;
  }
  return work;
}

bool same_monitor_topology(const std::vector<MonitorDescriptor>& a,
                           const std::vector<MonitorDescriptor>& b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].is_primary != b[i].is_primary ||
        a[i].dpi_x != b[i].dpi_x || a[i].dpi_y != b[i].dpi_y ||
        std::memcmp(&a[i].monitor_rect, &b[i].monitor_rect, sizeof(RECT)) != 0) {
      return false;
    }
  }
  return true;
}

void monitor_manager_sync() {
  if (!g_mm.initialized)
    return;

  std::vector<MonitorDescriptor> monitors = enumerate_monitors();
  if (monitors.empty())
    return;

  const int panel_h = resolved_panel_height();

  // Re-position primary panel and primary monitor work area.
  const MonitorDescriptor& primary = monitors.front();
  if (g_mm.primary_panel != nullptr && IsWindow(g_mm.primary_panel)) {
    position_panel_hwnd(g_mm.primary_panel, primary.monitor_rect, panel_h, true);
    RECT work = compute_panel_work_area(primary.monitor_rect, panel_h);
    SystemParametersInfoW(SPI_SETWORKAREA, 0, &work, 0);
  }
  if (g_mm.cbs.on_display_changed != nullptr)
    g_mm.cbs.on_display_changed(primary.monitor_rect);

  if (!get_config().multi_monitor_panels ||
      g_mm.cbs.create_secondary_panel == nullptr) {
    destroy_all_secondary_panels();
    return;
  }

  // Collect desired secondary monitors.
  std::vector<MonitorDescriptor> secondaries;
  for (size_t i = 1; i < monitors.size(); ++i)
    secondaries.push_back(monitors[i]);

  // Remove panels whose monitor vanished.
  for (size_t i = 0; i < g_mm.secondary_panels.size();) {
    bool still_present = false;
    for (const auto& m : secondaries) {
      if (m.hmonitor == g_mm.secondary_panels[i].hmonitor) {
        still_present = true;
        break;
      }
    }
    if (!still_present) {
      destroy_secondary_slot(&g_mm.secondary_panels[i]);
      g_mm.secondary_panels.erase(g_mm.secondary_panels.begin() +
                                  static_cast<ptrdiff_t>(i));
    } else {
      ++i;
    }
  }

  // Update existing secondary panels and create missing ones.
  for (const auto& mon : secondaries) {
    bool found = false;
    for (auto& slot : g_mm.secondary_panels) {
      if (slot.hmonitor == mon.hmonitor) {
        slot.rect = mon.monitor_rect;
        if (slot.hwnd != nullptr)
          position_panel_hwnd(slot.hwnd, mon.monitor_rect, panel_h, true);
        found = true;
        break;
      }
    }
    if (!found) {
      GtkWidget* win = g_mm.cbs.create_secondary_panel();
      if (win != nullptr) {
        SecondaryPanelSlot slot;
        slot.hmonitor = mon.hmonitor;
        slot.window = win;
        slot.rect = mon.monitor_rect;
        g_mm.secondary_panels.push_back(slot);

        RECT* rect_copy = g_new(RECT, 1);
        *rect_copy = mon.monitor_rect;
        g_signal_connect_data(
            win, "realize", G_CALLBACK(on_secondary_realize),
            rect_copy, [](gpointer p, GClosure* c G_GNUC_UNUSED) { g_free(p); },
            GConnectFlags(0));
        gtk_window_set_default_size(
            GTK_WINDOW(win),
            mon.monitor_rect.right - mon.monitor_rect.left, panel_h);
        gtk_widget_set_visible(win, TRUE);
        GdkSurface* surf = gtk_native_get_surface(GTK_NATIVE(win));
        if (surf != nullptr) {
          HWND sec_hwnd = static_cast<HWND>(gdk_win32_surface_get_handle(surf));
          if (sec_hwnd != nullptr)
            position_panel_hwnd(sec_hwnd, mon.monitor_rect, panel_h, true);
        }
      }
    }
  }
}

void monitor_manager_init(HWND primary_panel_hwnd,
                          const MonitorRuntimeCallbacks& callbacks) {
  if (g_mm.initialized)
    return;

  g_mm.primary_panel = primary_panel_hwnd;
  g_mm.cbs = callbacks;
  g_mm.initialized = true;

  HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = watcher_wnd_proc;
  wc.hInstance = inst;
  wc.lpszClassName = kWatcherClass;
  RegisterClassExW(&wc);

  // Use a zero-size hidden top-level popup window (not HWND_MESSAGE) so OS
  // broadcast messages (WM_DISPLAYCHANGE / WM_SETTINGCHANGE / WTS) arrive.
  g_mm.watcher_hwnd = CreateWindowExW(
      WS_EX_TOOLWINDOW, kWatcherClass, L"WINOME Monitor Watcher",
      WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);

  if (g_mm.watcher_hwnd != nullptr) {
    g_mm.wts_dll = LoadLibraryW(L"wtsapi32.dll");
    if (g_mm.wts_dll != nullptr) {
      using WtsRegFn = BOOL(WINAPI*)(HWND, DWORD);
      auto reg_fn = reinterpret_cast<WtsRegFn>(
          reinterpret_cast<void*>(
              GetProcAddress(g_mm.wts_dll, "WTSRegisterSessionNotification")));
      g_mm.wts_unreg = reinterpret_cast<BOOL(WINAPI*)(HWND)>(
          reinterpret_cast<void*>(
              GetProcAddress(g_mm.wts_dll, "WTSUnRegisterSessionNotification")));
      if (reg_fn != nullptr && reg_fn(g_mm.watcher_hwnd, kNotifyForThisSession))
        g_mm.wts_registered = true;
    }
  }

  add_config_listener(on_monitor_config_changed, nullptr);
  monitor_manager_sync();
}

void monitor_manager_shutdown() {
  if (!g_mm.initialized)
    return;
  g_mm.initialized = false;

  remove_config_listener(on_monitor_config_changed, nullptr);
  if (g_mm.display_debounce_id != 0) {
    g_source_remove(g_mm.display_debounce_id);
    g_mm.display_debounce_id = 0;
  }

  destroy_all_secondary_panels();

  if (g_mm.watcher_hwnd != nullptr) {
    if (g_mm.wts_registered && g_mm.wts_unreg != nullptr)
      g_mm.wts_unreg(g_mm.watcher_hwnd);
    DestroyWindow(g_mm.watcher_hwnd);
    g_mm.watcher_hwnd = nullptr;
  }
  g_mm.wts_registered = false;
  g_mm.wts_unreg = nullptr;
  if (g_mm.wts_dll != nullptr) {
    FreeLibrary(g_mm.wts_dll);
    g_mm.wts_dll = nullptr;
  }
  g_mm.primary_panel = nullptr;
}

}  // namespace winome
