// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Simultaneous Secondary Overview displays for multi-monitor setups,
// supporting both Unlinked (default: independent per-monitor open/close
// animations) and Linked (frame-locked with overview.cpp) modes.

#include "overview-secondary.h"

#include "config.h"
#include "easing.h"
#include "fonts.h"
#include "fullscreen-watcher.h"
#include "host-utils.h"
#include "monitor-manager.h"
#include "overview-layout.h"
#include "overview-trigger.h"
#include "overview.h"
#include "shell-panel.h"
#include "workspace-manager.h"

#include <dwmapi.h>
#include <gdk/win32/gdkwin32.h>
#include <gtk/gtk.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

namespace winome {
namespace {

constexpr const wchar_t* kSecOvOrigProcProp = L"WINOME_SecOvOrigProc";
constexpr int kWorkspaceScrollTimeoutMs = 150;
constexpr double kHoverScaleInc = 0.04;
constexpr guint kSecAnimMs = 250;

struct SecondarySlot {
  HWND target = nullptr;
  HTHUMBNAIL thumb = nullptr;
  RECT start{};        // live desktop rect (relative to monitor)
  RECT picker{};       // computed picker slot rect (relative to monitor)
  RECT current{};      // currently applied thumbnail rect
  bool minimized = false;
  std::string title;
};

struct SecondaryHost {
  HMONITOR monitor = nullptr;
  RECT mon_rect{};
  int mon_w = 0;
  int mon_h = 0;
  int panel_h = 0;
  int scale = 1;
  int ws_count = 1;
  int active_ws = 0;
  SecondaryOverviewGeometry geo{};

  GtkWidget* window = nullptr;
  GtkWidget* area = nullptr;
  HWND hwnd = nullptr;
  bool visible = false;
  bool target_open = false;
  bool linked_driven = false;
  double progress = 0.0;
  double anim_from = 0.0;
  double anim_to = 0.0;
  guint anim_source = 0;
  gint64 anim_start_us = 0;
  bool anim_started = false;

  int hover_slot = -1;
  int hover_pill = -1;
  int drop_highlight_pill = -1;
  int drop_highlight_ws = -1;
  bool drop_highlight_active_card = false;

  int drag_slot = -1;
  HWND drag_hwnd = nullptr;
  bool drag_active = false;
  bool drag_just_ended = false;
  RECT drag_orig_rect{};
  int drag_start_px = 0;
  int drag_start_py = 0;
  int drag_offset_x = 0;
  int drag_offset_y = 0;
  double drag_target_scale = 0.65;

  gint64 last_scroll_us = 0;
  guint rebuild_timer = 0;

  std::vector<SecondarySlot> slots;
};

std::vector<std::unique_ptr<SecondaryHost>> g_hosts;
GdkTexture* g_shared_wallpaper = nullptr;
bool g_ws_listener_installed = false;
bool g_sec_suppress_ws_rebuild = false;

RECT lerp_rect(const RECT& a, const RECT& b, double t) {
  auto mix = [t](LONG u, LONG v) -> LONG {
    return static_cast<LONG>(std::lround(u + (v - u) * t));
  };
  return RECT{mix(a.left, b.left), mix(a.top, b.top),
              mix(a.right, b.right), mix(a.bottom, b.bottom)};
}

RECT scale_rect_about_center(const RECT& r, double factor) {
  const double cx = (r.left + r.right) * 0.5;
  const double cy = (r.top + r.bottom) * 0.5;
  const double hw = (r.right - r.left) * 0.5 * factor;
  const double hh = (r.bottom - r.top) * 0.5 * factor;
  return RECT{static_cast<LONG>(std::lround(cx - hw)),
              static_cast<LONG>(std::lround(cy - hh)),
              static_cast<LONG>(std::lround(cx + hw)),
              static_cast<LONG>(std::lround(cy + hh))};
}

bool is_cloaked_flags(HWND hwnd, DWORD* out_flags = nullptr) {
  DWORD c = 0;
  DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &c, sizeof(c));
  if (out_flags != nullptr)
    *out_flags = c;
  return c != 0;
}

bool is_alt_tab_window(HWND hwnd) {
  if (hwnd == nullptr || !IsWindow(hwnd) || !IsWindowVisible(hwnd))
    return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == 0 || pid == GetCurrentProcessId())
    return false;
  LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  if ((ex & WS_EX_TOOLWINDOW) != 0)
    return false;
  if ((ex & WS_EX_APPWINDOW) == 0 && GetWindow(hwnd, GW_OWNER) != nullptr)
    return false;
  return true;
}

RECT get_window_rect_for_layout(HWND hwnd) {
  RECT r{};
  if (IsIconic(hwnd)) {
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (GetWindowPlacement(hwnd, &wp))
      return wp.rcNormalPosition;
  }
  if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r,
                                      sizeof(r)))) {
    return r;
  }
  GetWindowRect(hwnd, &r);
  return r;
}

std::string get_hwnd_title_utf8(HWND hwnd) {
  wchar_t buf[256] = {};
  int n = GetWindowTextW(hwnd, buf, 255);
  if (n <= 0)
    return {};
  int need = WideCharToMultiByte(CP_UTF8, 0, buf, n, nullptr, 0, nullptr, nullptr);
  if (need <= 0)
    return {};
  std::string out(static_cast<size_t>(need), '\0');
  WideCharToMultiByte(CP_UTF8, 0, buf, n, out.data(), need, nullptr, nullptr);
  return out;
}

void destroy_host_thumbnails(SecondaryHost* h) {
  if (h == nullptr)
    return;
  for (auto& s : h->slots) {
    if (s.thumb != nullptr) {
      DwmUnregisterThumbnail(s.thumb);
      s.thumb = nullptr;
    }
  }
  h->slots.clear();
  h->hover_slot = -1;
  h->drag_slot = -1;
  h->drag_hwnd = nullptr;
  h->drag_active = false;
}

void cancel_host_rebuild_timer(SecondaryHost* h) {
  if (h != nullptr && h->rebuild_timer != 0) {
    g_source_remove(h->rebuild_timer);
    h->rebuild_timer = 0;
  }
}

void cancel_host_anim(SecondaryHost* h) {
  if (h != nullptr && h->anim_source != 0) {
    if (h->window != nullptr)
      gtk_widget_remove_tick_callback(h->window, h->anim_source);
    h->anim_source = 0;
  }
}

LRESULT CALLBACK sec_ov_subclass_proc(HWND hwnd,
                                      UINT msg,
                                      WPARAM wParam,
                                      LPARAM lParam) {
  WNDPROC orig = reinterpret_cast<WNDPROC>(GetPropW(hwnd, kSecOvOrigProcProp));
  if (msg == WM_MOUSEACTIVATE)
    return MA_NOACTIVATE;
  if (msg == WM_NCACTIVATE && wParam != FALSE)
    return FALSE;
  if (msg == WM_STYLECHANGING && wParam == static_cast<WPARAM>(GWL_EXSTYLE) &&
      lParam != 0) {
    auto* ss = reinterpret_cast<STYLESTRUCT*>(lParam);
    ss->styleNew =
        (ss->styleNew | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST) &
        ~WS_EX_APPWINDOW;
  }
  if (msg == WM_NCDESTROY && orig != nullptr) {
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
    RemovePropW(hwnd, kSecOvOrigProcProp);
  }
  return orig != nullptr ? CallWindowProcW(orig, hwnd, msg, wParam, lParam)
                         : DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ensure_sec_ov_styles_and_subclass(HWND hwnd) {
  if (hwnd == nullptr || !IsWindow(hwnd))
    return;
  LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  LONG_PTR want =
      (ex | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST) &
      ~WS_EX_APPWINDOW;
  if (want != ex) {
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, want);
  }
  if (GetPropW(hwnd, kSecOvOrigProcProp) == nullptr) {
    WNDPROC orig = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(sec_ov_subclass_proc)));
    if (orig != nullptr)
      SetPropW(hwnd, kSecOvOrigProcProp, reinterpret_cast<HANDLE>(orig));
  }
}

void restack_host(SecondaryHost* h) {
  if (h == nullptr || !h->visible || h->hwnd == nullptr || !IsWindow(h->hwnd))
    return;
  HWND panel = monitor_manager_panel_for_monitor(h->monitor);
  HWND after = (panel != nullptr && IsWindow(panel) && IsWindowVisible(panel))
                   ? panel
                   : HWND_TOPMOST;
  if (after == HWND_TOPMOST) {
    SetWindowPos(h->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
  } else {
    SetWindowPos(panel, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    SetWindowPos(h->hwnd, panel, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
  }
}

void apply_host_thumbnails(SecondaryHost* h, double progress) {
  if (h == nullptr || !h->visible || h->hwnd == nullptr)
    return;
  const double p = std::clamp(progress, 0.0, 1.0);
  for (size_t i = 0; i < h->slots.size(); ++i) {
    SecondarySlot& s = h->slots[i];
    if (s.thumb == nullptr)
      continue;
    if (h->drag_active && static_cast<int>(i) == h->drag_slot)
      continue;
    RECT dst = lerp_rect(s.start, s.picker, p);
    if (static_cast<int>(i) == h->hover_slot && p > 0.85)
      dst = scale_rect_about_center(dst, 1.0 + kHoverScaleInc);
    s.current = dst;

    DWM_THUMBNAIL_PROPERTIES props{};
    props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE |
                    DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
    props.rcDestination = dst;
    props.fVisible = (dst.right > dst.left && dst.bottom > dst.top) ? TRUE : FALSE;
    props.opacity = s.minimized
                        ? static_cast<BYTE>(std::clamp(std::lround(255.0 * p), 0L, 255L))
                        : 255;
    props.fSourceClientAreaOnly = FALSE;
    DwmUpdateThumbnailProperties(s.thumb, &props);
  }
}

void finish_host_hide(SecondaryHost* h) {
  if (h == nullptr)
    return;
  cancel_host_anim(h);
  cancel_host_rebuild_timer(h);
  destroy_host_thumbnails(h);
  h->visible = false;
  h->target_open = false;
  h->linked_driven = false;
  h->progress = 0.0;
  h->hover_slot = -1;
  h->hover_pill = -1;
  h->drop_highlight_pill = -1;
  h->drop_highlight_ws = -1;
  h->drop_highlight_active_card = false;
  h->drag_just_ended = false;
  if (h->window != nullptr && gtk_widget_get_visible(h->window))
    gtk_widget_set_visible(h->window, FALSE);
  winome_shell_panel_set_overview_progress_for_monitor(h->monitor, 0.0);
  winome::evaluate_fullscreen_state();
  sync_overview_active_state(overview_any_open());
}

gboolean host_anim_tick(GtkWidget* widget G_GNUC_UNUSED,
                        GdkFrameClock* clock,
                        gpointer user_data) {
  auto* h = static_cast<SecondaryHost*>(user_data);
  if (h == nullptr || !h->visible)
    return G_SOURCE_REMOVE;

  const gint64 now = gdk_frame_clock_get_frame_time(clock);
  if (!h->anim_started) {
    h->anim_started = true;
    h->anim_start_us = now - 8000;
  }
  double t = (now - h->anim_start_us) / 1000.0 / static_cast<double>(kSecAnimMs);
  t = std::clamp(t, 0.0, 1.0);
  constexpr double kHalfPi = 1.57079632679489661923;
  const double e = (h->anim_to > h->anim_from)
                       ? std::sin(t * kHalfPi)
                       : (1.0 - (1.0 - t) * (1.0 - t));
  h->progress = std::clamp(h->anim_from + (h->anim_to - h->anim_from) * e,
                           0.0, 1.0);

  winome_shell_panel_set_overview_progress_for_monitor(h->monitor, h->progress);
  apply_host_thumbnails(h, h->progress);
  if (h->area != nullptr)
    gtk_widget_queue_draw(h->area);

  if (t >= 1.0) {
    h->anim_source = 0;
    if (!h->target_open)
      finish_host_hide(h);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

void start_host_anim(SecondaryHost* h, double to) {
  if (h == nullptr || h->window == nullptr)
    return;
  cancel_host_anim(h);
  h->linked_driven = false;
  h->anim_from = h->progress;
  h->anim_to = std::clamp(to, 0.0, 1.0);
  h->anim_started = false;
  h->anim_start_us = 0;
  h->anim_source =
      gtk_widget_add_tick_callback(h->window, host_anim_tick, h, nullptr);
}

struct EnumSecCtx {
  HMONITOR monitor = nullptr;
  RECT mon_rect{};
  int active_ws = 0;
  bool use_blacklist = true;
  std::vector<OvWindowInfo>* out = nullptr;
};

BOOL CALLBACK enum_sec_windows_proc(HWND hwnd, LPARAM lp) {
  auto* ctx = reinterpret_cast<EnumSecCtx*>(lp);
  if (!is_alt_tab_window(hwnd))
    return TRUE;
  if (is_overview_default_blacklisted_hwnd(hwnd, ctx->use_blacklist))
    return TRUE;

  DWORD cloaked = 0;
  is_cloaked_flags(hwnd, &cloaked);
  if ((cloaked & (0x1u | 0x4u)) != 0)
    return TRUE;

  RECT r = get_window_rect_for_layout(hwnd);
  if (r.right <= r.left || r.bottom <= r.top)
    return TRUE;
  if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != ctx->monitor)
    return TRUE;

  const int w_ws = ws::window_workspace(hwnd, ctx->monitor);
  if (ws::effective_mode() == ws::MultiMonitorWorkspaceMode::Independent) {
    if (w_ws != ctx->active_ws)
      return TRUE;
  } else if (ws::effective_mode() == ws::MultiMonitorWorkspaceMode::Span) {
    if (cloaked != 0 && w_ws != ctx->active_ws)
      return TRUE;
    if (!ws::window_on_active_workspace(hwnd, ctx->monitor))
      return TRUE;
  } else {
    if (cloaked != 0)
      return TRUE;
  }

  OffsetRect(&r, -ctx->mon_rect.left, -ctx->mon_rect.top);
  ctx->out->push_back(OvWindowInfo{hwnd, r});
  return TRUE;
}

void rebuild_host_layout(SecondaryHost* h) {
  if (h == nullptr || h->window == nullptr || h->monitor == nullptr)
    return;

  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(h->monitor, &mi))
    return;

  h->mon_rect = mi.rcMonitor;
  h->mon_w = mi.rcMonitor.right - mi.rcMonitor.left;
  h->mon_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
  if (h->mon_w <= 0 || h->mon_h <= 0)
    return;

  GdkSurface* surf = gtk_native_get_surface(GTK_NATIVE(h->window));
  if (surf == nullptr)
    return;
  h->hwnd = static_cast<HWND>(gdk_win32_surface_get_handle(surf));
  if (h->hwnd == nullptr || !IsWindow(h->hwnd))
    return;

  ensure_sec_ov_styles_and_subclass(h->hwnd);

  HWND panel = monitor_manager_panel_for_monitor(h->monitor);
  HWND after = (panel != nullptr && IsWindow(panel) && IsWindowVisible(panel))
                   ? panel
                   : HWND_TOPMOST;
  SetWindowPos(h->hwnd, after, mi.rcMonitor.left, mi.rcMonitor.top,
               h->mon_w, h->mon_h,
               SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);

  h->panel_h = 0;
  RECT pr{};
  if (panel != nullptr && IsWindowVisible(panel) && GetWindowRect(panel, &pr))
    h->panel_h = std::max<int>(0, pr.bottom - pr.top);

  h->scale = std::max(gtk_widget_get_scale_factor(h->window), 1);
  if (ws::is_independent_active())
    ws::sync_dynamic_workspaces(h->monitor);
  h->ws_count = std::max(ws::count_for_monitor(h->monitor), 1);
  h->active_ws = std::clamp(ws::active_for_monitor(h->monitor), 0, h->ws_count - 1);

  h->geo = compute_secondary_overview_geometry(
      h->mon_w, h->mon_h, h->panel_h, h->scale, h->ws_count);

  destroy_host_thumbnails(h);

  std::vector<OvWindowInfo> wins;
  EnumSecCtx ctx{h->monitor, h->mon_rect, h->active_ws,
                 get_config().overview_window_blacklist, &wins};
  EnumWindows(enum_sec_windows_proc, reinterpret_cast<LPARAM>(&ctx));

  if (!wins.empty()) {
    const RECT full_mon{0, 0, h->mon_w, h->mon_h};
    const RECT workarea{0, h->panel_h, h->mon_w, h->mon_h};
    const double spacing = 28.0 * h->scale;
    std::vector<OvSlot> computed;
    overview_compute_slots(wins, full_mon, workarea, h->geo.picker_box,
                           spacing, spacing, &computed);

    h->slots.reserve(computed.size());
    for (const OvSlot& sl : computed) {
      SecondarySlot entry{};
      entry.target = sl.window.hwnd;
      entry.minimized = IsIconic(sl.window.hwnd) != FALSE;
      entry.title = get_hwnd_title_utf8(sl.window.hwnd);
      const LONG pw = std::max<LONG>(
          1, static_cast<LONG>(std::lround((sl.window.rect.right - sl.window.rect.left) * sl.scale)));
      const LONG ph = std::max<LONG>(
          1, static_cast<LONG>(std::lround((sl.window.rect.bottom - sl.window.rect.top) * sl.scale)));
      entry.picker = RECT{
          static_cast<LONG>(std::lround(sl.x)),
          static_cast<LONG>(std::lround(sl.y)),
          static_cast<LONG>(std::lround(sl.x)) + pw,
          static_cast<LONG>(std::lround(sl.y)) + ph,
      };
      if (entry.minimized) {
        entry.start = RECT{h->geo.picker_box.left, h->geo.picker_box.top,
                           h->geo.picker_box.left + 1, h->geo.picker_box.top + 1};
      } else {
        entry.start = sl.window.rect;
      }

      HTHUMBNAIL thumb = nullptr;
      if (SUCCEEDED(DwmRegisterThumbnail(h->hwnd, entry.target, &thumb)) &&
          thumb != nullptr) {
        entry.thumb = thumb;
      }
      h->slots.push_back(std::move(entry));
    }
  }

  apply_host_thumbnails(h, h->progress);
  if (h->area != nullptr)
    gtk_widget_queue_draw(h->area);
}

void draw_rounded_rect_path(cairo_t* cr,
                            double x,
                            double y,
                            double w,
                            double h,
                            double r) {
  r = std::min({r, w * 0.5, h * 0.5});
  if (r <= 0.1) {
    cairo_rectangle(cr, x, y, w, h);
    return;
  }
  constexpr double kPi = 3.14159265358979323846;
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -kPi * 0.5, 0.0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0.0, kPi * 0.5);
  cairo_arc(cr, x + r, y + h - r, r, kPi * 0.5, kPi);
  cairo_arc(cr, x + r, y + r, r, kPi, kPi * 1.5);
  cairo_close_path(cr);
}

void paint_wallpaper_card(cairo_t* cr,
                          const RECT& rect_px,
                          int scale,
                          double radius,
                          double panel_top_ratio,
                          double dim_alpha) {
  const double x = static_cast<double>(rect_px.left) / scale;
  const double y = static_cast<double>(rect_px.top) / scale;
  const double w = static_cast<double>(rect_px.right - rect_px.left) / scale;
  const double h = static_cast<double>(rect_px.bottom - rect_px.top) / scale;
  if (w <= 1.0 || h <= 1.0)
    return;

  cairo_save(cr);
  draw_rounded_rect_path(cr, x, y, w, h, radius);
  cairo_clip(cr);

  if (g_shared_wallpaper != nullptr) {
    const int tw = gdk_texture_get_width(g_shared_wallpaper);
    const int th = gdk_texture_get_height(g_shared_wallpaper);
    if (tw > 0 && th > 0) {
      static GdkTexture* cached_tex = nullptr;
      static cairo_surface_t* cached_surf = nullptr;
      if (cached_tex != g_shared_wallpaper) {
        if (cached_surf != nullptr) {
          cairo_surface_destroy(cached_surf);
          cached_surf = nullptr;
        }
        cached_tex = g_shared_wallpaper;
        cached_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw, th);
        if (cached_surf != nullptr &&
            cairo_surface_status(cached_surf) == CAIRO_STATUS_SUCCESS) {
          gdk_texture_download(
              g_shared_wallpaper, cairo_image_surface_get_data(cached_surf),
              static_cast<gsize>(cairo_image_surface_get_stride(cached_surf)));
          cairo_surface_mark_dirty(cached_surf);
        }
      }
      if (cached_surf != nullptr) {
        const WallpaperDrawRect dr = compute_workarea_wallpaper_draw_rect(
            x, y, w, h, static_cast<double>(tw), static_cast<double>(th),
            panel_top_ratio);
        cairo_save(cr);
        cairo_translate(cr, dr.x, dr.y);
        cairo_scale(cr, dr.w / tw, dr.h / th);
        cairo_set_source_surface(cr, cached_surf, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint(cr);
        cairo_restore(cr);
      }
    }
  } else {
    cairo_set_source_rgb(cr, 0.16, 0.17, 0.20);
    cairo_paint(cr);
  }

  if (dim_alpha > 0.001) {
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, dim_alpha);
    cairo_paint(cr);
  }
  cairo_restore(cr);
}

void draw_secondary_overview(GtkDrawingArea* area G_GNUC_UNUSED,
                             cairo_t* cr,
                             int width,
                             int height,
                             gpointer user_data) {
  auto* h = static_cast<SecondaryHost*>(user_data);
  if (h == nullptr || width <= 0 || height <= 0)
    return;

  const int scale = std::max(h->scale, 1);
  const double p = std::clamp(h->progress, 0.0, 1.0);
  const int wa_h = std::max(h->mon_h - h->panel_h, 1);
  const double panel_ratio =
      (h->panel_h > 0) ? (static_cast<double>(h->panel_h) / wa_h) : 0.0;

  // GNOME overviewGroup dark backdrop (#222226).
  cairo_set_source_rgb(cr, 34.0 / 255.0, 34.0 / 255.0, 38.0 / 255.0);
  cairo_paint(cr);

  const RECT active_rect = lerp_rect(h->geo.full_box, h->geo.picker_box, p);
  const double radius = 30.0 * p;

  if (h->ws_count > 1 && p > 0.05) {
    for (int d = 0; d < h->ws_count; ++d) {
      if (d == h->active_ws || std::abs(d - h->active_ws) > 1)
        continue;
      RECT target_nb = compute_secondary_workspace_box(h->geo, d, h->active_ws);
      RECT start_nb = h->geo.full_box;
      OffsetRect(&start_nb, (d - h->active_ws) * (h->mon_w + h->geo.ws_spacing), 0);
      RECT cur_nb = lerp_rect(start_nb, target_nb, p);
      if (cur_nb.right <= 0 || cur_nb.left >= h->mon_w)
        continue;
      paint_wallpaper_card(cr, cur_nb, scale, radius, panel_ratio, 0.25);
      if (h->drop_highlight_ws == d) {
        const double nx = static_cast<double>(cur_nb.left) / scale;
        const double ny = static_cast<double>(cur_nb.top) / scale;
        const double nw = static_cast<double>(cur_nb.right - cur_nb.left) / scale;
        const double nh = static_cast<double>(cur_nb.bottom - cur_nb.top) / scale;
        draw_rounded_rect_path(cr, nx, ny, nw, nh, radius);
        cairo_set_source_rgba(cr, 0.208, 0.518, 0.894, 0.95);
        cairo_set_line_width(cr, 3.0);
        cairo_stroke(cr);
      }
    }
  }

  paint_wallpaper_card(cr, active_rect, scale, radius, panel_ratio, 0.0);
  if (h->drop_highlight_active_card && p > 0.05) {
    const double ax = static_cast<double>(active_rect.left) / scale;
    const double ay = static_cast<double>(active_rect.top) / scale;
    const double aw = static_cast<double>(active_rect.right - active_rect.left) / scale;
    const double ah = static_cast<double>(active_rect.bottom - active_rect.top) / scale;
    draw_rounded_rect_path(cr, ax, ay, aw, ah, radius);
    cairo_set_source_rgba(cr, 0.208, 0.518, 0.894, 0.95);
    cairo_set_line_width(cr, 3.0);
    cairo_stroke(cr);
  }

  if (h->ws_count > 1 && !h->geo.pill_rects.empty() && p > 0.05) {
    cairo_save(cr);
    for (size_t i = 0; i < h->geo.pill_rects.size(); ++i) {
      const RECT& pr = h->geo.pill_rects[i];
      const bool is_active = (static_cast<int>(i) == h->active_ws);
      const bool is_hover = (static_cast<int>(i) == h->hover_pill);
      const bool is_drop = (static_cast<int>(i) == h->drop_highlight_pill);
      paint_wallpaper_card(cr, pr, scale, 6.0, panel_ratio,
                           (is_active || is_drop) ? 0.0 : (is_hover ? 0.12 : 0.28));
      const double px = static_cast<double>(pr.left) / scale;
      const double py = static_cast<double>(pr.top) / scale;
      const double pw = static_cast<double>(pr.right - pr.left) / scale;
      const double ph = static_cast<double>(pr.bottom - pr.top) / scale;
      draw_rounded_rect_path(cr, px, py, pw, ph, 6.0);
      if (is_drop) {
        cairo_set_source_rgba(cr, 0.208, 0.518, 0.894, 0.96);
        cairo_set_line_width(cr, 3.0);
        cairo_stroke(cr);
      } else if (is_active) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.92 * p);
        cairo_set_line_width(cr, 2.0);
        cairo_stroke(cr);
      } else if (is_hover) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.45 * p);
        cairo_set_line_width(cr, 1.5);
        cairo_stroke(cr);
      }
    }
    cairo_restore(cr);
  }

  if (p > 0.85 && h->hover_slot >= 0 &&
      h->hover_slot < static_cast<int>(h->slots.size())) {
    const SecondarySlot& s = h->slots[static_cast<size_t>(h->hover_slot)];
    if (!s.title.empty()) {
      PangoLayout* layout = pango_cairo_create_layout(cr);
      PangoFontDescription* desc = pango_font_description_from_string("Cantarell Bold 10");
      pango_layout_set_font_description(layout, desc);
      pango_font_description_free(desc);
      pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
      const double max_w = std::min(static_cast<double>(width) - 48.0, 320.0);
      pango_layout_set_width(layout, static_cast<int>(max_w * PANGO_SCALE));
      pango_layout_set_text(layout, s.title.c_str(), -1);

      int tw = 0, th = 0;
      pango_layout_get_pixel_size(layout, &tw, &th);
      const double pad_x = 12.0;
      const double pad_y = 5.0;
      const double box_w = tw + 2.0 * pad_x;
      const double box_h = th + 2.0 * pad_y;
      const double cx = (s.current.left + s.current.right) * 0.5 / scale;
      const double bx = std::clamp(cx - box_w * 0.5, 12.0,
                                   std::max(12.0, width - box_w - 12.0));
      const double by = std::min(static_cast<double>(s.current.bottom) / scale + 8.0,
                                 static_cast<double>(height) - box_h - 12.0);

      draw_rounded_rect_path(cr, bx, by, box_w, box_h, box_h * 0.5);
      cairo_set_source_rgba(cr, 0.10, 0.10, 0.12, 0.90);
      cairo_fill(cr);

      cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.96);
      cairo_move_to(cr, bx + pad_x, by + pad_y);
      pango_cairo_show_layout(cr, layout);
      g_object_unref(layout);
    }
  }
}

int hit_test_slot(const SecondaryHost* h, int px, int py) {
  if (h == nullptr)
    return -1;
  for (size_t i = 0; i < h->slots.size(); ++i) {
    const RECT& r = h->slots[i].current;
    if (px >= r.left && px < r.right && py >= r.top && py < r.bottom)
      return static_cast<int>(i);
  }
  return -1;
}

int hit_test_pill(const SecondaryHost* h, int px, int py) {
  if (h == nullptr || h->ws_count <= 1)
    return -1;
  for (size_t i = 0; i < h->geo.pill_rects.size(); ++i) {
    const RECT& r = h->geo.pill_rects[i];
    if (px >= r.left && px < r.right && py >= r.top && py < r.bottom)
      return static_cast<int>(i);
  }
  return -1;
}

gboolean delayed_host_rebuild_cb(gpointer data) {
  auto* h = static_cast<SecondaryHost*>(data);
  if (h != nullptr) {
    h->rebuild_timer = 0;
    if (h->visible && h->target_open)
      rebuild_host_layout(h);
  }
  return G_SOURCE_REMOVE;
}

void dismiss_host_or_all(SecondaryHost* h, const char* reason) {
  if (h == nullptr)
    return;
  if (get_config().multi_monitor_overview_linked) {
    close_overview(reason);
  } else {
    overview_secondary_close_monitor(h->monitor);
  }
}

void on_sec_motion(GtkEventControllerMotion* ctrl G_GNUC_UNUSED,
                   double x,
                   double y,
                   gpointer user_data) {
  auto* h = static_cast<SecondaryHost*>(user_data);
  if (h == nullptr || !h->visible || !h->target_open)
    return;
  const int px = static_cast<int>(std::lround(x * h->scale));
  const int py = static_cast<int>(std::lround(y * h->scale));
  const int slot = hit_test_slot(h, px, py);
  const int pill = hit_test_pill(h, px, py);
  if (slot != h->hover_slot || pill != h->hover_pill) {
    h->hover_slot = slot;
    h->hover_pill = pill;
    apply_host_thumbnails(h, h->progress);
    if (h->area != nullptr)
      gtk_widget_queue_draw(h->area);
  }
}

void on_sec_leave(GtkEventControllerMotion* ctrl G_GNUC_UNUSED,
                  gpointer user_data) {
  auto* h = static_cast<SecondaryHost*>(user_data);
  if (h == nullptr)
    return;
  if (h->hover_slot != -1 || h->hover_pill != -1) {
    h->hover_slot = -1;
    h->hover_pill = -1;
    apply_host_thumbnails(h, h->progress);
    if (h->area != nullptr)
      gtk_widget_queue_draw(h->area);
  }
}

void on_sec_click_released(GtkGestureClick* gesture,
                           int n_press G_GNUC_UNUSED,
                           double x,
                           double y,
                           gpointer user_data) {
  auto* h = static_cast<SecondaryHost*>(user_data);
  if (h == nullptr || !h->visible || !h->target_open)
    return;
  if (h->drag_just_ended) {
    h->drag_just_ended = false;
    return;
  }
  const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
  const int px = static_cast<int>(std::lround(x * h->scale));
  const int py = static_cast<int>(std::lround(y * h->scale));

  if (py < h->panel_h)
    return;

  // 1. Top workspace pill switches THIS secondary monitor's workspace.
  const int pill = hit_test_pill(h, px, py);
  if (pill >= 0) {
    if (button == GDK_BUTTON_PRIMARY && pill != h->active_ws) {
      if (ws::switch_to(h->monitor, pill))
        rebuild_host_layout(h);
    }
    return;
  }

  // 2. Peeking neighbor workspace switches THIS secondary monitor.
  if (h->ws_count > 1) {
    for (int d = 0; d < h->ws_count; ++d) {
      if (d == h->active_ws || std::abs(d - h->active_ws) > 1)
        continue;
      RECT nb = compute_secondary_workspace_box(h->geo, d, h->active_ws);
      if (px >= nb.left && px < nb.right && py >= nb.top && py < nb.bottom) {
        if (button == GDK_BUTTON_PRIMARY && ws::switch_to(h->monitor, d))
          rebuild_host_layout(h);
        return;
      }
    }
  }

  // 3. Clicking a window preview activates it (LMB) or closes it (MMB).
  const int slot = hit_test_slot(h, px, py);
  if (slot >= 0 && slot < static_cast<int>(h->slots.size())) {
    HWND target = h->slots[static_cast<size_t>(slot)].target;
    if (button == GDK_BUTTON_MIDDLE) {
      if (IsWindow(target)) {
        PostMessageW(target, WM_CLOSE, 0, 0);
        cancel_host_rebuild_timer(h);
        h->rebuild_timer = g_timeout_add(160, delayed_host_rebuild_cb, h);
      }
      return;
    }
    if (button == GDK_BUTTON_PRIMARY) {
      overview_discard_restore_focus();
      if (IsWindow(target)) {
        if (IsIconic(target))
          ShowWindow(target, SW_RESTORE);
        AllowSetForegroundWindow(ASFW_ANY);
        SetForegroundWindow(target);
      }
      dismiss_host_or_all(h, "sec-ov-click-slot");
      return;
    }
  }

  // 4. Clicking empty backdrop closes this monitor's overview (or all if linked).
  if (button == GDK_BUTTON_PRIMARY)
    dismiss_host_or_all(h, "sec-ov-click-bg");
}

int resolve_sec_drop_target_local(const SecondaryHost* h,
                                  int px,
                                  int py,
                                  bool allow_active_card,
                                  int* out_pill,
                                  int* out_peek,
                                  bool* out_active_card) {
  if (out_pill != nullptr)
    *out_pill = -1;
  if (out_peek != nullptr)
    *out_peek = -1;
  if (out_active_card != nullptr)
    *out_active_card = false;
  if (h == nullptr || py < h->panel_h)
    return -1;

  // 1. Top workspace pills (with a generous 8px*scale drop pad).
  if (h->ws_count > 1 && !h->geo.pill_rects.empty()) {
    const int pad = 8 * std::max(h->scale, 1);
    for (size_t i = 0; i < h->geo.pill_rects.size(); ++i) {
      const RECT& r = h->geo.pill_rects[i];
      if (px >= r.left - pad && px <= r.right + pad &&
          py >= r.top - pad && py <= r.bottom + pad) {
        if (out_pill != nullptr)
          *out_pill = static_cast<int>(i);
        return static_cast<int>(i);
      }
    }
  }

  // 2. Peeking adjacent workspaces on the left/right edges.
  if (h->ws_count > 1) {
    for (int d = 0; d < h->ws_count; ++d) {
      if (d == h->active_ws || std::abs(d - h->active_ws) > 1)
        continue;
      RECT nb = compute_secondary_workspace_box(h->geo, d, h->active_ws);
      bool hit = (px >= nb.left && px < nb.right && py >= nb.top && py < nb.bottom);
      if (!hit && py >= h->geo.picker_box.top && py < h->geo.picker_box.bottom) {
        if (d == h->active_ws - 1 && px < h->geo.picker_box.left)
          hit = true;
        else if (d == h->active_ws + 1 && px >= h->geo.picker_box.right)
          hit = true;
      }
      if (hit) {
        if (out_peek != nullptr)
          *out_peek = d;
        if (out_pill != nullptr)
          *out_pill = d;
        return d;
      }
    }
  }

  // 3. Anywhere else inside this open Secondary Overview targets active_ws.
  if (allow_active_card) {
    if (out_active_card != nullptr)
      *out_active_card = true;
    if (out_pill != nullptr && h->ws_count > 1)
      *out_pill = h->active_ws;
    return h->active_ws;
  }
  return -1;
}

void set_host_drop_highlight(SecondaryHost* h,
                             int pill,
                             int peek,
                             bool active_card) {
  if (h == nullptr)
    return;
  if (h->drop_highlight_pill != pill || h->drop_highlight_ws != peek ||
      h->drop_highlight_active_card != active_card) {
    h->drop_highlight_pill = pill;
    h->drop_highlight_ws = peek;
    h->drop_highlight_active_card = active_card;
    if (h->area != nullptr)
      gtk_widget_queue_draw(h->area);
  }
}

void on_sec_drag_begin(GtkGestureDrag* drag,
                       double x,
                       double y,
                       gpointer user_data) {
  auto* h = static_cast<SecondaryHost*>(user_data);
  if (h == nullptr || !h->visible || !h->target_open || h->progress < 0.85) {
    gtk_gesture_set_state(GTK_GESTURE(drag), GTK_EVENT_SEQUENCE_DENIED);
    return;
  }
  const int px = static_cast<int>(std::lround(x * h->scale));
  const int py = static_cast<int>(std::lround(y * h->scale));
  if (py < h->panel_h || hit_test_pill(h, px, py) >= 0) {
    gtk_gesture_set_state(GTK_GESTURE(drag), GTK_EVENT_SEQUENCE_DENIED);
    return;
  }
  const int slot = hit_test_slot(h, px, py);
  if (slot < 0 || slot >= static_cast<int>(h->slots.size())) {
    gtk_gesture_set_state(GTK_GESTURE(drag), GTK_EVENT_SEQUENCE_DENIED);
    return;
  }

  h->drag_slot = slot;
  h->drag_hwnd = h->slots[static_cast<size_t>(slot)].target;
  h->drag_active = false;
  h->drag_just_ended = false;
  h->drag_orig_rect = h->slots[static_cast<size_t>(slot)].picker;
  h->drag_start_px = px;
  h->drag_start_py = py;
  h->drag_offset_x = h->drag_orig_rect.left - px;
  h->drag_offset_y = h->drag_orig_rect.top - py;
  const int ow = h->drag_orig_rect.right - h->drag_orig_rect.left;
  const int oh = h->drag_orig_rect.bottom - h->drag_orig_rect.top;
  const int max_dim = std::max(ow, oh);
  h->drag_target_scale =
      max_dim > 0
          ? std::clamp(static_cast<double>(256 * h->scale) / max_dim, 0.38, 0.82)
          : 0.65;
}

void on_sec_drag_update(GtkGestureDrag* drag,
                        double dx,
                        double dy,
                        gpointer user_data) {
  auto* h = static_cast<SecondaryHost*>(user_data);
  if (h == nullptr || h->drag_slot < 0 ||
      h->drag_slot >= static_cast<int>(h->slots.size())) {
    return;
  }
  if (!h->drag_active) {
    if (dx * dx + dy * dy < 36.0)
      return;
    h->drag_active = true;
    gtk_gesture_set_state(GTK_GESTURE(drag), GTK_EVENT_SEQUENCE_CLAIMED);
    h->hover_slot = -1;
    if (h->area != nullptr)
      gtk_widget_queue_draw(h->area);
  }

  const int cur_px = h->drag_start_px + static_cast<int>(std::lround(dx * h->scale));
  const int cur_py = h->drag_start_py + static_cast<int>(std::lround(dy * h->scale));

  SecondarySlot& s = h->slots[static_cast<size_t>(h->drag_slot)];
  if (s.thumb != nullptr) {
    const int ow = h->drag_orig_rect.right - h->drag_orig_rect.left;
    const int oh = h->drag_orig_rect.bottom - h->drag_orig_rect.top;
    const int nw = std::max(24, static_cast<int>(std::lround(ow * h->drag_target_scale)));
    const int nh = std::max(24, static_cast<int>(std::lround(oh * h->drag_target_scale)));
    const int nx = cur_px + static_cast<int>(std::lround(h->drag_offset_x * h->drag_target_scale));
    const int ny = cur_py + static_cast<int>(std::lround(h->drag_offset_y * h->drag_target_scale));
    RECT dst{nx, ny, nx + nw, ny + nh};
    s.current = dst;

    const bool on_this_mon =
        (cur_px >= 0 && cur_px < h->mon_w && cur_py >= 0 && cur_py < h->mon_h);
    DWM_THUMBNAIL_PROPERTIES props{};
    props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE |
                    DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
    props.rcDestination = dst;
    props.fVisible = on_this_mon ? TRUE : FALSE;
    props.opacity = 225;
    props.fSourceClientAreaOnly = FALSE;
    DwmUpdateThumbnailProperties(s.thumb, &props);
  }

  POINT screen_pt{};
  if (GetCursorPos(&screen_pt)) {
    HMONITOR cur_mon = MonitorFromPoint(screen_pt, MONITOR_DEFAULTTONULL);
    if (cur_mon == h->monitor) {
      overview_main_clear_external_drop_highlight();
      for (auto& other : g_hosts) {
        if (other.get() != h)
          set_host_drop_highlight(other.get(), -1, -1, false);
      }
      int pill = -1;
      int peek = -1;
      bool active_card = false;
      const int tgt = resolve_sec_drop_target_local(
          h, cur_px, cur_py, false, &pill, &peek, &active_card);
      if (tgt >= 0 && tgt != h->active_ws)
        set_host_drop_highlight(h, pill, peek, false);
      else
        set_host_drop_highlight(h, -1, -1, false);
    } else {
      set_host_drop_highlight(h, -1, -1, false);
      overview_main_set_external_drop_highlight(screen_pt);
      overview_secondary_set_drop_highlight(screen_pt);
    }
  }
}

void on_sec_drag_end(GtkGestureDrag* drag G_GNUC_UNUSED,
                     double dx G_GNUC_UNUSED,
                     double dy G_GNUC_UNUSED,
                     gpointer user_data) {
  auto* h = static_cast<SecondaryHost*>(user_data);
  if (h == nullptr || h->drag_slot < 0)
    return;

  const bool was_drag = h->drag_active;
  const HWND target_hwnd = h->drag_hwnd;
  h->drag_slot = -1;
  h->drag_hwnd = nullptr;
  h->drag_active = false;

  overview_main_clear_external_drop_highlight();
  overview_secondary_clear_drop_highlight();

  if (!was_drag)
    return;

  h->drag_just_ended = true;

  bool moved = false;
  POINT screen_pt{};
  if (target_hwnd != nullptr && IsWindow(target_hwnd) && GetCursorPos(&screen_pt)) {
    g_sec_suppress_ws_rebuild = true;
    HMONITOR main_mon = nullptr;
    int main_ws = -1;
    bool main_create = false;
    CrossMonitorDropTarget dt{};
    if (overview_main_hit_test_screen_point(screen_pt, &main_mon, &main_ws,
                                            &main_create)) {
      if (main_create && ws::create_workspace(main_mon)) {
        const int n = ws::count_for_monitor(main_mon);
        if (n > 0)
          main_ws = n - 1;
      }
      moved = ws::transfer_window_to_monitor_workspace(
          target_hwnd, main_mon, main_ws, screen_pt, false);
    } else if (overview_secondary_hit_test_screen_point(screen_pt, &dt)) {
      if (dt.monitor == h->monitor) {
        if (dt.workspace >= 0 && dt.workspace != h->active_ws)
          moved = ws::move_window_to_workspace(target_hwnd, h->monitor, dt.workspace);
      } else {
        moved = ws::transfer_window_to_monitor_workspace(
            target_hwnd, dt.monitor, dt.workspace, screen_pt, false);
      }
    } else {
      HMONITOR drop_mon = MonitorFromPoint(screen_pt, MONITOR_DEFAULTTONULL);
      if (drop_mon != nullptr && drop_mon != h->monitor) {
        moved = ws::transfer_window_to_monitor_workspace(
            target_hwnd, drop_mon, ws::active_for_monitor(drop_mon),
            screen_pt, true);
      }
    }
    g_sec_suppress_ws_rebuild = false;
  }

  if (moved) {
    overview_refresh_all_open();
    return;
  }

  apply_host_thumbnails(h, h->progress);
  if (h->area != nullptr)
    gtk_widget_queue_draw(h->area);
}

gboolean on_sec_scroll(GtkEventControllerScroll* ctrl G_GNUC_UNUSED,
                       double dx,
                       double dy,
                       gpointer user_data) {
  auto* h = static_cast<SecondaryHost*>(user_data);
  if (h == nullptr || !h->visible || !h->target_open || h->ws_count <= 1)
    return GDK_EVENT_PROPAGATE;
  const double delta = (std::fabs(dy) >= std::fabs(dx)) ? dy : dx;
  if (std::fabs(delta) < 1e-4)
    return GDK_EVENT_PROPAGATE;
  const gint64 now_us = g_get_monotonic_time();
  if (now_us - h->last_scroll_us <
      static_cast<gint64>(kWorkspaceScrollTimeoutMs) * 1000) {
    return GDK_EVENT_STOP;
  }
  const int target = h->active_ws + (delta > 0.0 ? 1 : -1);
  if (target < 0 || target >= h->ws_count)
    return GDK_EVENT_STOP;
  h->last_scroll_us = now_us;
  if (ws::switch_to(h->monitor, target))
    rebuild_host_layout(h);
  return GDK_EVENT_STOP;
}

void on_sec_map(GtkWidget* widget G_GNUC_UNUSED, gpointer user_data) {
  auto* h = static_cast<SecondaryHost*>(user_data);
  if (h != nullptr && h->visible) {
    g_idle_add_full(
        G_PRIORITY_HIGH_IDLE,
        +[](gpointer d) -> gboolean {
          auto* host = static_cast<SecondaryHost*>(d);
          for (const auto& entry : g_hosts) {
            if (entry.get() == host && host->visible) {
              rebuild_host_layout(host);
              restack_host(host);
              break;
            }
          }
          return G_SOURCE_REMOVE;
        },
        h, nullptr);
  }
}

void on_sec_ws_changed(HMONITOR monitor,
                       int count G_GNUC_UNUSED,
                       int active G_GNUC_UNUSED,
                       void* user_data G_GNUC_UNUSED) {
  if (g_sec_suppress_ws_rebuild)
    return;
  for (auto& host : g_hosts) {
    if (!host->visible || !host->target_open)
      continue;
    if (monitor == nullptr || monitor == host->monitor)
      rebuild_host_layout(host.get());
  }
}

std::unique_ptr<SecondaryHost> create_host(HMONITOR mon) {
  auto h = std::make_unique<SecondaryHost>();
  h->monitor = mon;

  h->window = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(h->window), "WINOME Secondary Overview");
  gtk_window_set_decorated(GTK_WINDOW(h->window), FALSE);
  gtk_window_set_resizable(GTK_WINDOW(h->window), FALSE);
  gtk_widget_set_name(h->window, "overviewGroup");
  install_bundled_fonts(h->window);

  h->area = gtk_drawing_area_new();
  gtk_widget_set_hexpand(h->area, TRUE);
  gtk_widget_set_vexpand(h->area, TRUE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(h->area),
                                 draw_secondary_overview, h.get(), nullptr);
  gtk_window_set_child(GTK_WINDOW(h->window), h->area);

  GtkGesture* click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
  g_signal_connect(click, "released", G_CALLBACK(on_sec_click_released), h.get());
  gtk_widget_add_controller(h->area, GTK_EVENT_CONTROLLER(click));

  GtkGesture* drag = gtk_gesture_drag_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
  g_signal_connect(drag, "drag-begin", G_CALLBACK(on_sec_drag_begin), h.get());
  g_signal_connect(drag, "drag-update", G_CALLBACK(on_sec_drag_update), h.get());
  g_signal_connect(drag, "drag-end", G_CALLBACK(on_sec_drag_end), h.get());
  gtk_widget_add_controller(h->area, GTK_EVENT_CONTROLLER(drag));

  GtkEventController* motion = gtk_event_controller_motion_new();
  g_signal_connect(motion, "motion", G_CALLBACK(on_sec_motion), h.get());
  g_signal_connect(motion, "leave", G_CALLBACK(on_sec_leave), h.get());
  gtk_widget_add_controller(h->area, motion);

  GtkEventController* scroll = gtk_event_controller_scroll_new(
      GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  g_signal_connect(scroll, "scroll", G_CALLBACK(on_sec_scroll), h.get());
  gtk_widget_add_controller(h->area, scroll);

  g_signal_connect(h->window, "map", G_CALLBACK(on_sec_map), h.get());
  return h;
}

void destroy_host(SecondaryHost* h) {
  if (h == nullptr)
    return;
  cancel_host_anim(h);
  cancel_host_rebuild_timer(h);
  destroy_host_thumbnails(h);
  h->visible = false;
  h->target_open = false;
  h->hwnd = nullptr;
  if (h->window != nullptr) {
    gtk_window_destroy(GTK_WINDOW(h->window));
    h->window = nullptr;
    h->area = nullptr;
  }
}

void ensure_shared_resources(GdkTexture* wallpaper_texture) {
  if (wallpaper_texture != nullptr && wallpaper_texture != g_shared_wallpaper) {
    g_clear_object(&g_shared_wallpaper);
    g_shared_wallpaper = GDK_TEXTURE(g_object_ref(wallpaper_texture));
  }
  if (!g_ws_listener_installed) {
    g_ws_listener_installed = true;
    ws::add_change_listener(on_sec_ws_changed, nullptr);
  }
}

SecondaryHost* find_or_create_host(HMONITOR mon) {
  for (auto& existing : g_hosts) {
    if (existing->monitor == mon)
      return existing.get();
  }
  auto created = create_host(mon);
  SecondaryHost* ptr = created.get();
  g_hosts.push_back(std::move(created));
  return ptr;
}

}  // namespace

void overview_secondary_sync(HMONITOR primary_overview_monitor,
                             GdkTexture* wallpaper_texture,
                             double progress) {
  ensure_shared_resources(wallpaper_texture);
  const double p = std::clamp(progress, 0.0, 1.0);

  const auto monitors = enumerate_monitors();
  std::vector<HMONITOR> wanted;
  for (const auto& m : monitors) {
    if (m.hmonitor != nullptr && m.hmonitor != primary_overview_monitor)
      wanted.push_back(m.hmonitor);
  }

  for (size_t i = 0; i < g_hosts.size();) {
    const HMONITOR hm = g_hosts[i]->monitor;
    if (std::find(wanted.begin(), wanted.end(), hm) == wanted.end()) {
      destroy_host(g_hosts[i].get());
      g_hosts.erase(g_hosts.begin() + static_cast<ptrdiff_t>(i));
    } else {
      ++i;
    }
  }

  for (HMONITOR mon : wanted) {
    SecondaryHost* host = find_or_create_host(mon);
    cancel_host_anim(host);
    host->linked_driven = true;
    host->target_open = true;
    host->visible = true;
    host->progress = p;
    if (!gtk_widget_get_visible(host->window))
      gtk_widget_set_visible(host->window, TRUE);
    rebuild_host_layout(host);
    restack_host(host);
  }
}

void overview_secondary_set_progress(double progress) {
  const double p = std::clamp(progress, 0.0, 1.0);
  for (auto& h : g_hosts) {
    if (!h->visible || !h->linked_driven)
      continue;
    h->progress = p;
    apply_host_thumbnails(h.get(), p);
    if (h->area != nullptr)
      gtk_widget_queue_draw(h->area);
  }
}

void overview_secondary_open_monitor(HMONITOR monitor,
                                     GdkTexture* wallpaper_texture) {
  if (monitor == nullptr)
    return;
  ensure_shared_resources(wallpaper_texture);

  SecondaryHost* host = find_or_create_host(monitor);
  const bool was_visible = host->visible;
  host->linked_driven = false;
  host->target_open = true;
  host->visible = true;
  if (!was_visible)
    host->progress = 0.0;
  if (!gtk_widget_get_visible(host->window))
    gtk_widget_set_visible(host->window, TRUE);
  rebuild_host_layout(host);
  restack_host(host);
  start_host_anim(host, 1.0);
  winome::evaluate_fullscreen_state();
  sync_overview_active_state(true);
}

void overview_secondary_close_monitor(HMONITOR monitor) {
  if (monitor == nullptr)
    return;
  for (auto& h : g_hosts) {
    if (h->monitor != monitor)
      continue;
    if (!h->visible)
      return;
    h->target_open = false;
    h->linked_driven = false;
    start_host_anim(h.get(), 0.0);
    return;
  }
}

void overview_secondary_close_all_animated() {
  for (auto& h : g_hosts) {
    if (!h->visible)
      continue;
    h->target_open = false;
    h->linked_driven = false;
    start_host_anim(h.get(), 0.0);
  }
}

bool overview_secondary_is_open_for_monitor(HMONITOR monitor) {
  if (monitor == nullptr)
    return false;
  for (const auto& h : g_hosts) {
    if (h->monitor == monitor && h->visible && h->target_open)
      return true;
  }
  return false;
}

bool overview_secondary_any_open() {
  for (const auto& h : g_hosts) {
    if (h->visible && h->target_open)
      return true;
  }
  return false;
}

void overview_secondary_set_wallpaper(GdkTexture* wallpaper_texture) {
  if (wallpaper_texture == g_shared_wallpaper)
    return;
  g_clear_object(&g_shared_wallpaper);
  if (wallpaper_texture != nullptr)
    g_shared_wallpaper = GDK_TEXTURE(g_object_ref(wallpaper_texture));
  for (auto& h : g_hosts) {
    if (h->visible && h->area != nullptr)
      gtk_widget_queue_draw(h->area);
  }
}

void overview_secondary_hide_all() {
  for (auto& h : g_hosts) {
    if (!h->visible && h->slots.empty())
      continue;
    finish_host_hide(h.get());
  }
}

void overview_secondary_restack() {
  for (auto& h : g_hosts) {
    if (h->visible)
      restack_host(h.get());
  }
}

void overview_secondary_shutdown() {
  if (g_ws_listener_installed) {
    ws::remove_change_listener(on_sec_ws_changed, nullptr);
    g_ws_listener_installed = false;
  }
  for (auto& h : g_hosts)
    destroy_host(h.get());
  g_hosts.clear();
  g_clear_object(&g_shared_wallpaper);
}

bool overview_secondary_is_hwnd(HWND hwnd) {
  if (hwnd == nullptr)
    return false;
  for (const auto& h : g_hosts) {
    if (h->hwnd == hwnd)
      return true;
  }
  return false;
}

bool overview_secondary_hit_test_screen_point(POINT screen_pt,
                                              CrossMonitorDropTarget* out) {
  for (const auto& h : g_hosts) {
    if (!h->visible || !h->target_open)
      continue;
    if (screen_pt.x < h->mon_rect.left || screen_pt.x >= h->mon_rect.right ||
        screen_pt.y < h->mon_rect.top || screen_pt.y >= h->mon_rect.bottom) {
      continue;
    }
    const int px = screen_pt.x - h->mon_rect.left;
    const int py = screen_pt.y - h->mon_rect.top;
    const int ws_idx = resolve_sec_drop_target_local(
        h.get(), px, py, true, nullptr, nullptr, nullptr);
    if (ws_idx < 0)
      return false;
    if (out != nullptr) {
      out->monitor = h->monitor;
      out->workspace = ws_idx;
      out->create_new = false;
      out->is_overview = true;
    }
    return true;
  }
  return false;
}

void overview_secondary_set_drop_highlight(POINT screen_pt) {
  for (auto& h : g_hosts) {
    if (!h->visible || !h->target_open) {
      set_host_drop_highlight(h.get(), -1, -1, false);
      continue;
    }
    if (screen_pt.x >= h->mon_rect.left && screen_pt.x < h->mon_rect.right &&
        screen_pt.y >= h->mon_rect.top && screen_pt.y < h->mon_rect.bottom) {
      const int px = screen_pt.x - h->mon_rect.left;
      const int py = screen_pt.y - h->mon_rect.top;
      int pill = -1;
      int peek = -1;
      bool active_card = false;
      const int tgt = resolve_sec_drop_target_local(
          h.get(), px, py, true, &pill, &peek, &active_card);
      if (tgt >= 0)
        set_host_drop_highlight(h.get(), pill, peek, active_card);
      else
        set_host_drop_highlight(h.get(), -1, -1, false);
    } else {
      set_host_drop_highlight(h.get(), -1, -1, false);
    }
  }
}

void overview_secondary_clear_drop_highlight() {
  for (auto& h : g_hosts)
    set_host_drop_highlight(h.get(), -1, -1, false);
}

void overview_secondary_refresh_monitor(HMONITOR monitor) {
  for (auto& h : g_hosts) {
    if (!h->visible || !h->target_open)
      continue;
    if (monitor == nullptr || h->monitor == monitor)
      rebuild_host_layout(h.get());
  }
}

void overview_secondary_reposition_all() {
  if (g_hosts.empty())
    return;
  const auto monitors = enumerate_monitors();
  bool removed_any = false;
  for (size_t i = 0; i < g_hosts.size();) {
    const HMONITOR hm = g_hosts[i]->monitor;
    bool still_present = false;
    for (const auto& m : monitors) {
      if (m.hmonitor == hm) {
        still_present = true;
        break;
      }
    }
    if (!still_present) {
      if (g_hosts[i]->visible)
        removed_any = true;
      destroy_host(g_hosts[i].get());
      g_hosts.erase(g_hosts.begin() + static_cast<ptrdiff_t>(i));
    } else {
      if (g_hosts[i]->visible && g_hosts[i]->target_open) {
        rebuild_host_layout(g_hosts[i].get());
        restack_host(g_hosts[i].get());
      } else if (g_hosts[i]->visible && !g_hosts[i]->target_open) {
        finish_host_hide(g_hosts[i].get());
      }
      ++i;
    }
  }
  if (removed_any) {
    winome::evaluate_fullscreen_state();
    sync_overview_active_state(overview_any_open());
  }
}

}  // namespace winome
