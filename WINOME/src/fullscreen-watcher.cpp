// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Event-driven per-monitor fullscreen detection & lock-screen panel hiding.

#include "fullscreen-watcher.h"
#include "host-utils.h"
#include "hot-corner.h"
#include "overview-trigger.h"
#include "overview.h"
#include "shell-panel.h"

#include <windows.h>
#include <dwmapi.h>
#include <glib.h>

#include <algorithm>
#include <string>
#include <vector>

namespace winome {
namespace {

struct PanelTrackEntry {
  HWND hwnd = nullptr;
  bool is_primary = false;
  bool hidden = false;
  HWND tracked_fs_hwnd = nullptr;
  FullscreenHysteresisTracker hysteresis{80};
};

std::vector<PanelTrackEntry> g_panels;
bool g_session_locked = false;
guint g_tick_source = 0;
HWINEVENTHOOK g_fg_hook = nullptr;
HWINEVENTHOOK g_loc_hook = nullptr;
HWINEVENTHOOK g_state_hook = nullptr;
HWINEVENTHOOK g_movesize_hook = nullptr;

bool belongs_to_process(HWND hwnd, const wchar_t* name) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == 0)
    return false;

  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (process == nullptr)
    return false;

  wchar_t path[MAX_PATH] = {0};
  DWORD len = MAX_PATH;
  bool ok = QueryFullProcessImageNameW(process, 0, path, &len) != 0;
  CloseHandle(process);
  if (!ok)
    return false;

  std::wstring full(path);
  size_t sep = full.find_last_of(L'\\');
  std::wstring base = (sep != std::wstring::npos) ? full.substr(sep + 1) : full;
  return _wcsicmp(base.c_str(), name) == 0;
}

volatile LONG g_expected_panel_height = 32;
WorkareaAssertGuard g_workarea_guard;
guint g_workarea_retry_id = 0;
guint g_workarea_settle_eval_id = 0;
guint g_unhide_debounce_id = 0;
guint g_hide_debounce_id = 0;
guint g_drag_trailing_eval_id = 0;

int64_t monotonic_ms() {
  return g_get_monotonic_time() / 1000;
}

bool get_window_frame_bounds(HWND hwnd, RECT* out) {
  if (hwnd == nullptr || out == nullptr)
    return false;
  RECT dwm_rc{};
  if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                      &dwm_rc, sizeof(dwm_rc))) &&
      dwm_rc.right > dwm_rc.left && dwm_rc.bottom > dwm_rc.top) {
    *out = dwm_rc;
    return true;
  }
  return GetWindowRect(hwnd, out) != FALSE;
}

bool is_in_move_size() {
  GUITHREADINFO gti{};
  gti.cbSize = sizeof(gti);
  return GetGUIThreadInfo(0, &gti) != FALSE && (gti.flags & GUI_INMOVESIZE) != 0;
}

bool is_window_cloaked(HWND hwnd) {
  BOOL cloaked = FALSE;
  return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED,
                                         &cloaked, sizeof(cloaked))) &&
         cloaked != FALSE;
}

bool is_desktop_shell_class(HWND hwnd) {
  if (hwnd == nullptr)
    return false;
  wchar_t cls[64] = {0};
  if (GetClassNameW(hwnd, cls, 64) <= 0)
    return false;
  return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0;
}

bool is_transient_overlay_window(HWND hwnd) {
  if (hwnd == nullptr)
    return true;
  if (is_desktop_shell_class(hwnd))
    return false;
  const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  if ((ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0)
    return true;
  wchar_t cls[64] = {0};
  if (GetClassNameW(hwnd, cls, 64) > 0) {
    if (wcscmp(cls, L"IME") == 0 || wcscmp(cls, L"MSCTFIME UI") == 0 ||
        wcscmp(cls, L"tooltips_class32") == 0) {
      return true;
    }
  }
  return false;
}

bool is_same_process_popup_overlay(HWND hwnd, HWND root_target) {
  if (hwnd == nullptr || root_target == nullptr || !IsWindow(root_target))
    return false;
  DWORD pid_h = 0, pid_r = 0;
  GetWindowThreadProcessId(hwnd, &pid_h);
  GetWindowThreadProcessId(root_target, &pid_r);
  if (pid_h == 0 || pid_h != pid_r)
    return false;
  const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  return (style & WS_POPUP) != 0 && (style & WS_THICKFRAME) == 0;
}

bool is_owned_by_window(HWND hwnd, HWND root_target) {
  if (hwnd == nullptr || root_target == nullptr || !IsWindow(root_target))
    return false;
  if (hwnd == root_target)
    return true;
  if (GetAncestor(hwnd, GA_ROOTOWNER) == root_target)
    return true;
  HWND cur = hwnd;
  for (int i = 0; i < 8 && cur != nullptr; ++i) {
    cur = GetWindow(cur, GW_OWNER);
    if (cur == root_target)
      return true;
  }
  return is_same_process_popup_overlay(hwnd, root_target);
}

bool covers_monitor(HWND hwnd, HMONITOR* out_mon) {
  RECT r{};
  if (!get_window_frame_bounds(hwnd, &r))
    return false;

  HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (mon == nullptr || !GetMonitorInfoW(mon, &mi))
    return false;

  const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  const bool has_frame = has_standard_maximized_frame_style(style);
  const bool suppress_zoomed = g_workarea_guard.in_settle_grace(monotonic_ms());
  const bool full = is_fullscreen_bounds_for_monitor(
      r, mi.rcMonitor, mi.rcWork, IsZoomed(hwnd) != FALSE, 2, has_frame,
      suppress_zoomed);
  if (full && out_mon != nullptr)
    *out_mon = mon;
  return full;
}

bool is_tracked_fullscreen_still_active(HWND tracked, HMONITOR mon) {
  if (tracked == nullptr || mon == nullptr || !IsWindow(tracked))
    return false;
  if (!IsWindowVisible(tracked) || IsIconic(tracked) || is_window_cloaked(tracked))
    return false;
  HMONITOR cur_mon = nullptr;
  if (!covers_monitor(tracked, &cur_mon) || cur_mon != mon)
    return false;

  // Verify no unrelated non-transient app window sits above @tracked on @mon.
  const DWORD self_pid = GetCurrentProcessId();
  for (HWND prev = GetWindow(tracked, GW_HWNDPREV); prev != nullptr;
       prev = GetWindow(prev, GW_HWNDPREV)) {
    if (!IsWindowVisible(prev) || IsIconic(prev) || is_window_cloaked(prev))
      continue;
    DWORD pid = 0;
    GetWindowThreadProcessId(prev, &pid);
    if (pid == self_pid || is_owned_by_window(prev, tracked) ||
        is_transient_overlay_window(prev) || is_desktop_shell_class(prev)) {
      continue;
    }
    RECT pr{};
    if (!GetWindowRect(prev, &pr) || (pr.right - pr.left) < 64 ||
        (pr.bottom - pr.top) < 64) {
      continue;
    }
    if (MonitorFromWindow(prev, MONITOR_DEFAULTTONULL) == mon)
      return false;
  }
  return true;
}

void safe_remove_source(guint* source_id) {
  if (source_id == nullptr || *source_id == 0)
    return;
  const guint id = *source_id;
  *source_id = 0;
  if (g_main_context_find_source_by_id(nullptr, id) != nullptr)
    g_source_remove(id);
}

void cancel_workarea_retry() {
  safe_remove_source(&g_workarea_retry_id);
}

void cancel_workarea_settle_eval() {
  safe_remove_source(&g_workarea_settle_eval_id);
}

void cancel_unhide_debounce() {
  safe_remove_source(&g_unhide_debounce_id);
}

void cancel_hide_debounce() {
  safe_remove_source(&g_hide_debounce_id);
}

void cancel_drag_trailing_eval() {
  safe_remove_source(&g_drag_trailing_eval_id);
}

void reassert_primary_workarea_and_styles();

gboolean workarea_retry_cb(gpointer G_GNUC_UNUSED data) {
  g_workarea_retry_id = 0;
  if (!g_panels.empty())
    reassert_primary_workarea_and_styles();
  return G_SOURCE_REMOVE;
}

void schedule_workarea_retry(int delay_ms) {
  cancel_workarea_retry();
  g_workarea_retry_id = g_timeout_add(
      static_cast<guint>(std::max(50, delay_ms)), workarea_retry_cb, nullptr);
}

gboolean workarea_settle_eval_cb(gpointer G_GNUC_UNUSED data) {
  g_workarea_settle_eval_id = 0;
  evaluate_fullscreen_state();
  return G_SOURCE_REMOVE;
}

void schedule_workarea_settle_eval(int delay_ms) {
  cancel_workarea_settle_eval();
  g_workarea_settle_eval_id = g_timeout_add(
      static_cast<guint>(std::max(50, delay_ms)), workarea_settle_eval_cb,
      nullptr);
}

void schedule_unhide_debounce(int delay_ms);
void schedule_hide_debounce(int delay_ms);

gboolean unhide_debounce_cb(gpointer G_GNUC_UNUSED data) {
  g_unhide_debounce_id = 0;
  evaluate_fullscreen_state();
  return G_SOURCE_REMOVE;
}

void schedule_unhide_debounce(int delay_ms) {
  cancel_unhide_debounce();
  g_unhide_debounce_id = g_timeout_add(
      static_cast<guint>(std::max(20, delay_ms)), unhide_debounce_cb, nullptr);
}

gboolean hide_debounce_cb(gpointer G_GNUC_UNUSED data) {
  g_hide_debounce_id = 0;
  evaluate_fullscreen_state();
  return G_SOURCE_REMOVE;
}

void schedule_hide_debounce(int delay_ms) {
  cancel_hide_debounce();
  g_hide_debounce_id = g_timeout_add(
      static_cast<guint>(std::max(20, delay_ms)), hide_debounce_cb, nullptr);
}

gboolean drag_trailing_eval_cb(gpointer G_GNUC_UNUSED data) {
  g_drag_trailing_eval_id = 0;
  evaluate_fullscreen_state();
  return G_SOURCE_REMOVE;
}

void schedule_drag_trailing_eval() {
  cancel_drag_trailing_eval();
  g_drag_trailing_eval_id = g_timeout_add(120, drag_trailing_eval_cb, nullptr);
}

bool strip_panel_resize_styles(HWND hwnd) {
  const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  const LONG_PTR stripped =
      style & ~static_cast<LONG_PTR>(WS_THICKFRAME | WS_MAXIMIZEBOX |
                                      WS_MINIMIZEBOX);
  if (stripped != style) {
    SetWindowLongPtrW(hwnd, GWL_STYLE, stripped);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
    return true;
  }
  return false;
}

void apply_primary_panel_geometry_and_workarea(HWND primary_hwnd,
                                               bool panel_hidden) {
  if (primary_hwnd == nullptr || !IsWindow(primary_hwnd))
    return;

  // Never move/resize a hidden panel or issue SPI_SETWORKAREA while a
  // fullscreen application (possibly at custom resolution), lock screen, or
  // Overview animation/session is active. Issuing SPI_SETWORKAREA while the
  // fullscreen Overview window is open causes Explorer/DWM workarea ping-pong
  // storms right in the middle of 250ms animations.
  if (panel_hidden || winome::overview_active() ||
      winome::overview_is_open() != 0) {
    g_workarea_guard.pending_after_defer = true;
    cancel_workarea_retry();
    return;
  }

  strip_panel_resize_styles(primary_hwnd);

  HMONITOR primary_mon =
      MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  const bool primary_ok =
      primary_mon != nullptr && GetMonitorInfoW(primary_mon, &mi) != FALSE &&
      (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
  if (!primary_ok) {
    g_print("[workarea] skip: invalid primary monitor info\n");
    return;
  }

  const int raw_expected =
      static_cast<int>(InterlockedCompareExchange(&g_expected_panel_height, 0, 0));
  const int canonical_h =
      clamp_panel_height_for_monitor(mi.rcMonitor, raw_expected, 32);
  if (canonical_h <= 0)
    return;

  const RECT expected_panel =
      compute_primary_panel_rect(mi.rcMonitor, canonical_h);
  RECT panel_rect{};
  if (GetWindowRect(primary_hwnd, &panel_rect) &&
      !WorkareaAssertGuard::rects_equal(panel_rect, expected_panel)) {
    g_print("[panel] repair primary rect (%ld,%ld %ldx%ld) -> (%ld,%ld %ldx%ld)\n",
            panel_rect.left, panel_rect.top,
            panel_rect.right - panel_rect.left,
            panel_rect.bottom - panel_rect.top,
            expected_panel.left, expected_panel.top,
            expected_panel.right - expected_panel.left,
            expected_panel.bottom - expected_panel.top);
    SetWindowPos(primary_hwnd, nullptr,
                 expected_panel.left, expected_panel.top,
                 expected_panel.right - expected_panel.left,
                 expected_panel.bottom - expected_panel.top,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
  }

  RECT current{};
  if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &current, 0))
    return;

  RECT want_work = mi.rcMonitor;
  want_work.top += canonical_h;
  const int64_t now_ms = monotonic_ms();
  const WorkareaAssertAction action = g_workarea_guard.evaluate(
      now_ms, want_work, current, primary_ok, panel_hidden, is_in_move_size());

  switch (action) {
    case WorkareaAssertAction::Apply:
      cancel_workarea_retry();
      if (SystemParametersInfoW(SPI_SETWORKAREA, 0, &want_work, 0)) {
        g_print("[workarea] applied (%ld,%ld,%ld,%ld) prev=(%ld,%ld,%ld,%ld) h=%d\n",
                want_work.left, want_work.top, want_work.right, want_work.bottom,
                current.left, current.top, current.right, current.bottom,
                canonical_h);
        // Perform one trailing verify in case Explorer overwrites the workarea
        // shortly after Shell_TrayWnd / popup deactivation without rebroadcasting.
        schedule_workarea_retry(220);
        if (g_workarea_guard.in_settle_grace(now_ms)) {
          schedule_workarea_settle_eval(
              static_cast<int>(WorkareaAssertGuard::kWorkareaSettleGraceMs) + 40);
        }
      } else {
        g_print("[workarea] SPI_SETWORKAREA failed gle=%lu\n", GetLastError());
      }
      break;
    case WorkareaAssertAction::SkipInMoveSize:
    case WorkareaAssertAction::DeferCooldown:
    case WorkareaAssertAction::DeferBackoff:
      if (action == WorkareaAssertAction::DeferBackoff) {
        g_print("[workarea] backoff active current=(%ld,%ld,%ld,%ld) want=(%ld,%ld,%ld,%ld)\n",
                current.left, current.top, current.right, current.bottom,
                want_work.left, want_work.top, want_work.right, want_work.bottom);
      }
      schedule_workarea_retry(g_workarea_guard.remaining_defer_ms(now_ms));
      break;
    case WorkareaAssertAction::SkipUnchanged:
      cancel_workarea_retry();
      break;
    case WorkareaAssertAction::SkipHidden:
    case WorkareaAssertAction::SkipInvalidMonitor:
      break;
  }
}

bool is_desktop_window(HWND hwnd) {
  wchar_t cls[256] = {0};
  if (GetClassNameW(hwnd, cls, 256) == 0)
    return false;
  return wcscmp(cls, L"Progman") == 0 ||
         wcscmp(cls, L"WorkerW") == 0 ||
         wcscmp(cls, L"SHELLDLL_DefView") == 0 ||
         wcscmp(cls, L"SysListView32") == 0 ||
         wcscmp(cls, L"Shell_TrayWnd") == 0 ||
         wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
         wcscmp(cls, L"XamlExplorerHostIslandWindow") == 0 ||
         wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0 ||
         wcscmp(cls, L"EdgeUiInputTopWndClass") == 0 ||
         wcscmp(cls, L"EdgeUiInputWndClass") == 0 ||
         wcscmp(cls, L"DummyDWMListenerWindow") == 0 ||
         wcscmp(cls, L"ForegroundStaging") == 0;
}

// Compute which monitor (if any) is currently covered by a fullscreen foreground
// window. When @all_locked is true every panel must hide regardless of monitor.
void detect_fullscreen_state(bool* all_locked, HMONITOR* fullscreen_mon) {
  *all_locked = g_session_locked;
  *fullscreen_mon = nullptr;

  if (g_session_locked)
    return;

  HWND fg = GetForegroundWindow();
  if (fg == nullptr)
    return;

  DWORD fg_pid = 0;
  GetWindowThreadProcessId(fg, &fg_pid);
  if (fg_pid == GetCurrentProcessId())
    return;

  if (belongs_to_process(fg, L"LogonUI.exe")) {
    *all_locked = true;
    return;
  }

  if (!IsWindowVisible(fg) || IsIconic(fg))
    return;

  BOOL cloaked = FALSE;
  if (SUCCEEDED(DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloaked,
                                      sizeof(cloaked))) &&
      cloaked != FALSE) {
    return;
  }

  if (is_desktop_window(fg))
    return;

  if (belongs_to_process(fg, L"winome.exe"))
    return;

  LONG_PTR style = GetWindowLongPtrW(fg, GWL_STYLE);
  LONG_PTR ex_style = GetWindowLongPtrW(fg, GWL_EXSTYLE);
  if ((style & WS_CHILD) != 0 ||
      (ex_style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)) != 0) {
    return;
  }

  if (winome::is_overview_default_blacklisted_hwnd(fg, true))
    return;

  HMONITOR covered = nullptr;
  if (covers_monitor(fg, &covered))
    *fullscreen_mon = covered;
}

void reassert_primary_workarea_and_styles() {
  for (const auto& entry : g_panels) {
    if (!IsWindow(entry.hwnd))
      continue;

    LONG_PTR ex = GetWindowLongPtrW(entry.hwnd, GWL_EXSTYLE);
    LONG_PTR want = (ex & ~WS_EX_APPWINDOW) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (ex != want)
      SetWindowLongPtrW(entry.hwnd, GWL_EXSTYLE, want);

    if (entry.is_primary) {
      apply_primary_panel_geometry_and_workarea(entry.hwnd, entry.hidden);
    } else {
      strip_panel_resize_styles(entry.hwnd);
    }
  }
  bool has_visible_panel = false;
  for (const auto& entry : g_panels) {
    if (entry.is_primary) {
      has_visible_panel = !entry.hidden && IsWindow(entry.hwnd) &&
                          IsWindowVisible(entry.hwnd);
      break;
    }
  }
  if (has_visible_panel && !winome::overview_active() &&
      winome::overview_is_open() == 0) {
    winome::overview_restack();
  }
}

void CALLBACK fg_win_event_proc(HWINEVENTHOOK hook G_GNUC_UNUSED,
                                  DWORD event,
                                  HWND hwnd,
                                  LONG id_object,
                                  LONG id_child,
                                  DWORD thread G_GNUC_UNUSED,
                                  DWORD time G_GNUC_UNUSED) {
  if (id_object != OBJID_WINDOW || id_child != CHILDID_SELF)
    return;
  if (event == EVENT_SYSTEM_MOVESIZESTART) {
    overview_on_external_window_drag_start(hwnd);
    return;
  }
  if (event == EVENT_SYSTEM_MOVESIZEEND) {
    overview_on_external_window_drag_end(hwnd);
    reassert_primary_workarea_and_styles();
    evaluate_fullscreen_state();
    return;
  }
  if (event == EVENT_OBJECT_LOCATIONCHANGE) {
    if (hwnd != GetForegroundWindow())
      return;
    if (is_in_move_size()) {
      overview_on_external_window_drag_move(hwnd);
      schedule_drag_trailing_eval();
      return;
    }
  } else {
    // Repair workarea before evaluating maximized/fullscreen bounds on focus
    // changes (e.g. closing an Explorer tray popup or restoring a window).
    reassert_primary_workarea_and_styles();
  }
  evaluate_fullscreen_state();
}

gboolean maintenance_tick(gpointer G_GNUC_UNUSED user_data) {
  reassert_primary_workarea_and_styles();
  evaluate_fullscreen_state();
  return G_SOURCE_CONTINUE;
}

}  // namespace

void set_expected_panel_height(int height) {
  if (height > 0)
    InterlockedExchange(&g_expected_panel_height, static_cast<LONG>(height));
}

void evaluate_fullscreen_state(void) {
  if (g_panels.empty()) {
    cancel_unhide_debounce();
    cancel_hide_debounce();
    cancel_workarea_retry();
    cancel_workarea_settle_eval();
    cancel_drag_trailing_eval();
    return;
  }

  bool all_locked = false;
  HMONITOR raw_fs_mon = nullptr;
  detect_fullscreen_state(&all_locked, &raw_fs_mon);

  HWND fg = GetForegroundWindow();
  DWORD fg_pid = 0;
  if (fg != nullptr)
    GetWindowThreadProcessId(fg, &fg_pid);
  const bool fg_is_self = (fg != nullptr && fg_pid == GetCurrentProcessId());
  const bool fg_transient_or_null =
      !fg_is_self && is_transient_overlay_window(fg);

  const int64_t now_ms = monotonic_ms();

  bool any_shown = false;
  bool all_panels_hidden = true;
  bool any_valid_panel = false;
  int64_t earliest_unhide_ms = -1;
  int64_t earliest_hide_ms = -1;

  for (auto& entry : g_panels) {
    if (!IsWindow(entry.hwnd))
      continue;
    any_valid_panel = true;
    HMONITOR pmon = MonitorFromWindow(entry.hwnd, MONITOR_DEFAULTTONEAREST);
    const bool ov_open = overview_is_open_for_monitor(pmon);
    const bool pop_open =
        winome_shell_panel_top_popover_hwnd_for_monitor(pmon) != nullptr;
    const HMONITOR raw_for_panel =
        (!all_locked && !ov_open && !pop_open && raw_fs_mon != nullptr &&
         pmon == raw_fs_mon)
            ? pmon
            : nullptr;

    if (raw_for_panel != nullptr && fg != nullptr)
      entry.tracked_fs_hwnd = fg;

    FullscreenEvalInput eval_in{};
    eval_in.now_ms = now_ms;
    eval_in.session_locked = all_locked;
    eval_in.overview_open = ov_open;
    eval_in.popover_open = pop_open;
    eval_in.currently_hidden = entry.hidden;
    eval_in.raw_fullscreen_mon = raw_for_panel;
    eval_in.tracked_fs_mon = (entry.tracked_fs_hwnd != nullptr) ? pmon : nullptr;
    if (raw_for_panel == nullptr && !all_locked && !ov_open && !pop_open &&
        entry.hidden && entry.tracked_fs_hwnd != nullptr) {
      eval_in.fg_is_transient_or_null = fg_transient_or_null;
      eval_in.fg_owned_by_tracked_fs =
          !fg_is_self && is_owned_by_window(fg, entry.tracked_fs_hwnd);
      eval_in.tracked_fs_still_covering =
          is_tracked_fullscreen_still_active(entry.tracked_fs_hwnd, pmon);
    }

    const FullscreenEvalResult eval_out = entry.hysteresis.step(eval_in);
    if (!eval_out.hide_all_locked &&
        eval_out.effective_fullscreen_mon == nullptr &&
        entry.hysteresis.pending_hide_deadline_ms < 0) {
      entry.tracked_fs_hwnd = nullptr;
    }

    if (entry.hysteresis.pending_unhide_deadline_ms >= 0) {
      if (earliest_unhide_ms < 0 ||
          entry.hysteresis.pending_unhide_deadline_ms < earliest_unhide_ms) {
        earliest_unhide_ms = entry.hysteresis.pending_unhide_deadline_ms;
      }
    }
    if (entry.hysteresis.pending_hide_deadline_ms >= 0) {
      if (earliest_hide_ms < 0 ||
          entry.hysteresis.pending_hide_deadline_ms < earliest_hide_ms) {
        earliest_hide_ms = entry.hysteresis.pending_hide_deadline_ms;
      }
    }

    const bool want_hide =
        eval_out.hide_all_locked ||
        (eval_out.effective_fullscreen_mon != nullptr &&
         pmon == eval_out.effective_fullscreen_mon);
    if (!want_hide)
      all_panels_hidden = false;
    if (want_hide != entry.hidden) {
      entry.hidden = want_hide;
      g_print("[fs] panel %s primary=%d locked=%d fs_mon=%p ov=%d\n",
              want_hide ? "hide" : "show",
              entry.is_primary ? 1 : 0,
              all_locked ? 1 : 0,
              static_cast<void*>(eval_out.effective_fullscreen_mon),
              ov_open ? 1 : 0);
      ShowWindow(entry.hwnd, want_hide ? SW_HIDE : SW_SHOWNOACTIVATE);
      if (!want_hide) {
        any_shown = true;
        if (entry.is_primary)
          schedule_workarea_retry(80);
      }
    }
  }

  // Only suppress the global hot-corner hook when the session is locked or
  // EVERY monitor's panel is hidden (so a fullscreen app on monitor 1 never
  // disables the hot corner on monitor 2).
  hot_corner_set_suppressed(g_session_locked ||
                            (any_valid_panel && all_panels_hidden));

  if (earliest_unhide_ms >= 0) {
    const int64_t rem = earliest_unhide_ms - monotonic_ms();
    schedule_unhide_debounce(static_cast<int>(std::max<int64_t>(20, rem + 4)));
  } else {
    cancel_unhide_debounce();
  }

  if (earliest_hide_ms >= 0) {
    const int64_t rem = earliest_hide_ms - monotonic_ms();
    schedule_hide_debounce(static_cast<int>(std::max<int64_t>(20, rem + 4)));
  } else {
    cancel_hide_debounce();
  }

  if (any_shown)
    winome::overview_restack();
}

void register_panel_window(HWND panel_hwnd) {
  if (panel_hwnd == nullptr)
    return;
  for (const auto& entry : g_panels) {
    if (entry.hwnd == panel_hwnd)
      return;
  }
  PanelTrackEntry entry{};
  entry.hwnd = panel_hwnd;
  entry.is_primary = g_panels.empty();
  g_panels.push_back(entry);
  evaluate_fullscreen_state();
}

void unregister_panel_window(HWND panel_hwnd) {
  g_panels.erase(
      std::remove_if(g_panels.begin(), g_panels.end(),
                     [&](const PanelTrackEntry& e) {
                       return e.hwnd == panel_hwnd;
                     }),
      g_panels.end());
}

void set_session_locked(bool locked) {
  g_session_locked = locked;
  if (locked)
    hot_corner_set_suppressed(true);
  evaluate_fullscreen_state();
}

void reassert_panel_workarea_now(void) {
  reassert_primary_workarea_and_styles();
}

bool panel_hidden(void) {
  if (g_session_locked)
    return true;
  for (const auto& entry : g_panels) {
    if (entry.is_primary)
      return entry.hidden;
  }
  return false;
}

bool panel_hidden_for_monitor(HMONITOR mon) {
  if (g_session_locked)
    return true;
  for (const auto& entry : g_panels) {
    if (MonitorFromWindow(entry.hwnd, MONITOR_DEFAULTTONEAREST) == mon)
      return entry.hidden;
  }
  return false;
}

void start_fullscreen_watcher(HWND primary_panel_hwnd) {
  g_panels.clear();
  if (primary_panel_hwnd != nullptr) {
    PanelTrackEntry entry{};
    entry.hwnd = primary_panel_hwnd;
    entry.is_primary = true;
    g_panels.push_back(entry);
  }

  if (g_fg_hook == nullptr) {
    g_fg_hook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
        fg_win_event_proc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  }
  if (g_loc_hook == nullptr) {
    g_loc_hook = SetWinEventHook(
        EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
        fg_win_event_proc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  }
  if (g_state_hook == nullptr) {
    g_state_hook = SetWinEventHook(
        EVENT_SYSTEM_MINIMIZEEND, EVENT_SYSTEM_MINIMIZEEND, nullptr,
        fg_win_event_proc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  }
  if (g_movesize_hook == nullptr) {
    g_movesize_hook = SetWinEventHook(
        EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, nullptr,
        fg_win_event_proc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  }

  reassert_primary_workarea_and_styles();
  evaluate_fullscreen_state();
  // Low-frequency 10s safety backstop (foreground, location, minimize, lock,
  // and SPI_SETWORKAREA events drive immediate transitions with zero 600ms churn).
  if (g_tick_source == 0)
    g_tick_source = g_timeout_add(10000, maintenance_tick, nullptr);
}

void stop_fullscreen_watcher(void) {
  cancel_unhide_debounce();
  cancel_hide_debounce();
  cancel_workarea_retry();
  cancel_workarea_settle_eval();
  cancel_drag_trailing_eval();
  safe_remove_source(&g_tick_source);
  if (g_fg_hook != nullptr) {
    UnhookWinEvent(g_fg_hook);
    g_fg_hook = nullptr;
  }
  if (g_loc_hook != nullptr) {
    UnhookWinEvent(g_loc_hook);
    g_loc_hook = nullptr;
  }
  if (g_state_hook != nullptr) {
    UnhookWinEvent(g_state_hook);
    g_state_hook = nullptr;
  }
  if (g_movesize_hook != nullptr) {
    UnhookWinEvent(g_movesize_hook);
    g_movesize_hook = nullptr;
  }
  g_panels.clear();
  g_session_locked = false;
}

}  // namespace winome
