// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// WINOME host process entry point.

#include <gtk/gtk.h>
#include <gdk/win32/gdkwin32.h>
#include <epoxy/gl.h>

#include <windows.h>
#include <dbghelp.h>
#include <shellapi.h>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include "config.h"
#include "app-index.h"
#include "crash-dialog.h"
#include "debug-runtime.h"
#include "end-session-dialog.h"
#include "host-utils.h"
#include "i18n.h"
#include "looking-glass.h"
#include "native-taskbar.h"
#include "overview-trigger.h"
#include "overview.h"
#include "perf-metrics.h"
#include "plugin-host.h"
#include "quick-settings.h"
#include "run-dialog.h"
#include "shell-ipc.h"
#include "shell-panel.h"
#include "win-event-hook.h"
#include "fullscreen-watcher.h"
#include "hot-corner.h"
#include "monitor-manager.h"
#include "notifications.h"
#include "settings-window.h"
#include "systray.h"
#include "system-status.h"
#include "workspace-manager.h"
#include "wifi-networks.h"
#include "window-theming.h"
#include "ws-osd.h"
#include "virtual-desktop.h"
#include "winrt-bridge.h"

// The host runs as a GUI application (no console window). Route diagnostics
// to a rotating log file under %LOCALAPPDATA%\WINOME.
static void
setup_log_file (void)
{
  std::wstring dir = winome::app_data_dir ();
  if (dir.empty ())
    return;

  std::wstring log_path = dir + L"\\winome.log";
  WIN32_FILE_ATTRIBUTE_DATA fad {};
  if (GetFileAttributesExW (log_path.c_str (), GetFileExInfoStandard, &fad)) {
    ULONGLONG size = (static_cast<ULONGLONG> (fad.nFileSizeHigh) << 32) |
                     fad.nFileSizeLow;
    constexpr ULONGLONG kMaxLogBytes = 5ULL * 1024 * 1024;
    if (size > kMaxLogBytes) {
      std::wstring prev_path = dir + L"\\winome.prev.log";
      MoveFileExW (log_path.c_str (), prev_path.c_str (),
                   MOVEFILE_REPLACE_EXISTING);
    }
  }

  // Append keeps previous sessions' logs up to kMaxLogBytes.
  _wfreopen (log_path.c_str (), L"a", stdout);
  _wfreopen (log_path.c_str (), L"a", stderr);
  winome::debug_configure_log_rotation (log_path, 5ULL * 1024 * 1024);
}

// Primary monitor bounds captured when the panel is positioned; restored in
// on_shutdown so the shell recalculates the work area around the taskbar.
static RECT g_primary_monitor = {0, 0, 0, 0};
static wchar_t g_crash_state_path[MAX_PATH] = {};
static wchar_t g_log_file_path[MAX_PATH] = {};
static wchar_t g_app_data_path[MAX_PATH] = {};
static std::atomic_bool g_crash_recorded{false};

static void
init_crash_paths (void)
{
  std::wstring dir = winome::app_data_dir ();
  if (dir.empty ())
    return;
  wcsncpy (g_app_data_path, dir.c_str (), MAX_PATH - 1);
  _snwprintf (g_crash_state_path, MAX_PATH - 1, L"%ls\\crash-state.json",
              dir.c_str ());
  _snwprintf (g_log_file_path, MAX_PATH - 1, L"%ls\\winome.log",
              dir.c_str ());
}

static std::wstring
recovery_file_path (void)
{
  std::wstring dir = winome::app_data_dir ();
  if (dir.empty ())
    return {};
  return dir + L"\\recovery.dat";
}

static std::wstring
crash_state_file_path (void)
{
  std::wstring dir = winome::app_data_dir ();
  if (dir.empty ())
    return {};
  return dir + L"\\crash-state.json";
}

static void
save_recovery_workarea (const RECT &rc)
{
  std::wstring path = recovery_file_path ();
  if (path.empty () || rc.right <= rc.left || rc.bottom <= rc.top)
    return;

  winome::RecoverySnapshot snap;
  snap.valid = true;
  snap.taskbar_state = winome::NativeTaskbar::original_state ();
  snap.primary_work = rc;
  const std::string payload = winome::serialize_recovery_snapshot (snap);

  HANDLE file = CreateFileW (path.c_str (), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return;

  DWORD written = 0;
  WriteFile (file, payload.data (), static_cast<DWORD> (payload.size ()),
             &written, nullptr);
  CloseHandle (file);
}

static void
clear_recovery_file (void)
{
  std::wstring path = recovery_file_path ();
  if (!path.empty ())
    DeleteFileW (path.c_str ());
}

static void
clear_crash_state_file (void)
{
  std::wstring path = crash_state_file_path ();
  if (!path.empty ())
    DeleteFileW (path.c_str ());
}

static bool
has_recovery_file (void)
{
  std::wstring path = recovery_file_path ();
  if (path.empty ())
    return false;
  return GetFileAttributesW (path.c_str ()) != INVALID_FILE_ATTRIBUTES;
}

static bool
apply_recovery_keep_or_clear (bool force_taskbar_restore, bool clear_files)
{
  std::wstring path = recovery_file_path ();
  bool recovered = false;
  int32_t desired_state = ABS_ALWAYSONTOP;

  if (!path.empty ()) {
    HANDLE file = CreateFileW (path.c_str (), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      LARGE_INTEGER sz{};
      if (GetFileSizeEx (file, &sz) && sz.QuadPart > 0 && sz.QuadPart <= 65536) {
        std::string raw (static_cast<size_t> (sz.QuadPart), '\0');
        DWORD read_bytes = 0;
        if (ReadFile (file, raw.data (), static_cast<DWORD> (sz.QuadPart),
                      &read_bytes, nullptr)) {
          raw.resize (read_bytes);
          winome::RecoverySnapshot snap;
          if (winome::parse_recovery_snapshot (raw, &snap) && snap.valid) {
            RECT rc = snap.primary_work;
            SystemParametersInfoW (SPI_SETWORKAREA, 0, &rc, SPIF_SENDCHANGE);
            recovered = true;
            if (!force_taskbar_restore)
              desired_state = snap.taskbar_state;
          }
        }
      }
      CloseHandle (file);
    }
  }

  if (!recovered && g_primary_monitor.right > g_primary_monitor.left) {
    RECT rc = g_primary_monitor;
    SystemParametersInfoW (SPI_SETWORKAREA, 0, &rc, SPIF_SENDCHANGE);
  }

  bool taskbar_visible = true;
  if (recovered || force_taskbar_restore) {
    taskbar_visible = winome::NativeTaskbar::force_restore_all (
        force_taskbar_restore ? ABS_ALWAYSONTOP : desired_state);
  }

  if (clear_files)
    clear_recovery_file ();
  return taskbar_visible;
}

static void
apply_and_clear_recovery (bool force_taskbar_restore)
{
  apply_recovery_keep_or_clear (force_taskbar_restore, true);
}

static void
restore_environment_emergency (void)
{
  if (g_primary_monitor.right > g_primary_monitor.left)
    SystemParametersInfoW (SPI_SETWORKAREA, 0, &g_primary_monitor, SPIF_SENDCHANGE);
  winome::NativeTaskbar::restore ();
  clear_recovery_file ();
  clear_crash_state_file ();
}

using MiniDumpWriteDumpFn = BOOL (WINAPI *)(
    HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION,
    PMINIDUMP_USER_STREAM_INFORMATION,
    PMINIDUMP_CALLBACK_INFORMATION);

static void
write_crash_minidump (EXCEPTION_POINTERS *info,
                      char *out_utf8,
                      size_t out_utf8_cap)
{
  if (out_utf8 != nullptr && out_utf8_cap > 0)
    out_utf8[0] = '\0';

  HMODULE dbghelp = LoadLibraryW (L"dbghelp.dll");
  if (dbghelp == nullptr)
    return;

  auto write_dump = reinterpret_cast<MiniDumpWriteDumpFn> (
      reinterpret_cast<void *> (GetProcAddress (dbghelp, "MiniDumpWriteDump")));
  if (write_dump == nullptr) {
    FreeLibrary (dbghelp);
    return;
  }

  wchar_t crash_dir[MAX_PATH] = {};
  if (g_app_data_path[0] == L'\0') {
    FreeLibrary (dbghelp);
    return;
  }
  CreateDirectoryW (g_app_data_path, nullptr);
  _snwprintf (crash_dir, MAX_PATH - 1, L"%ls\\crashes", g_app_data_path);
  CreateDirectoryW (crash_dir, nullptr);

  SYSTEMTIME st {};
  GetLocalTime (&st);
  wchar_t path[MAX_PATH] = {};
  _snwprintf (path, MAX_PATH - 1,
              L"%ls\\winome-%04u%02u%02u-%02u%02u%02u-%lu.dmp",
              crash_dir,
              st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond,
              static_cast<unsigned long> (GetCurrentProcessId ()));

  HANDLE file = CreateFileW (path, GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mei {};
    mei.ThreadId = GetCurrentThreadId ();
    mei.ExceptionPointers = info;
    mei.ClientPointers = FALSE;
    if (write_dump (GetCurrentProcess (), GetCurrentProcessId (), file,
                    MiniDumpNormal, info ? &mei : nullptr, nullptr, nullptr) &&
        out_utf8 != nullptr && out_utf8_cap > 1) {
      int n = WideCharToMultiByte (CP_UTF8, 0, path, -1, out_utf8,
                                   static_cast<int> (out_utf8_cap),
                                   nullptr, nullptr);
      if (n <= 0)
        out_utf8[0] = '\0';
    }
    CloseHandle (file);
  }
  FreeLibrary (dbghelp);
}

static void
handle_fatal_crash_and_keep_recovery (const char *reason,
                                      uint32_t code,
                                      EXCEPTION_POINTERS *info)
{
  if (g_crash_recorded.exchange (true, std::memory_order_acq_rel))
    return;

  char dump_utf8[MAX_PATH * 3] = {};
  write_crash_minidump (info, dump_utf8, sizeof (dump_utf8));
  winome::write_crash_state_emergency_file (
      g_crash_state_path, reason, code, GetCurrentProcessId (), dump_utf8,
      winome::is_safe_mode ());

  // Lock/join-free best-effort restoration; preserve recovery.dat so the
  // detached watchdog confirms full restoration and presents the recovery UI.
  if (g_primary_monitor.right > g_primary_monitor.left)
    SystemParametersInfoW (SPI_SETWORKAREA, 0, &g_primary_monitor, 0);
  winome::NativeTaskbar::restore_emergency_no_join ();
}

static LONG WINAPI
unhandled_exception_filter (EXCEPTION_POINTERS *info)
{
  const DWORD code = (info != nullptr && info->ExceptionRecord != nullptr)
                         ? info->ExceptionRecord->ExceptionCode
                         : 0xFFFFFFFFu;
  char msg[128];
  std::snprintf (msg, sizeof (msg),
                 "Win32 Unhandled Structured Exception (0x%08lX)",
                 static_cast<unsigned long> (code));
  handle_fatal_crash_and_keep_recovery (msg, static_cast<uint32_t> (code), info);

  if (IsDebuggerPresent ())
    return EXCEPTION_CONTINUE_SEARCH;
  // Terminate cleanly right away so --watchdog restores taskbar immediately
  // without stalling behind Windows Error Reporting dialogs.
  return EXCEPTION_EXECUTE_HANDLER;
}

static void
on_crt_signal (int sig)
{
  char msg[96];
  std::snprintf (msg, sizeof (msg), "CRT Fatal Signal (%d)", sig);
  handle_fatal_crash_and_keep_recovery (
      msg, static_cast<uint32_t> (128 + sig), nullptr);
  std::signal (sig, SIG_DFL);
  std::_Exit (128 + sig);
}

static void
on_cpp_terminate (void)
{
  handle_fatal_crash_and_keep_recovery (
      "Unhandled C++ Exception (std::terminate)", 3, nullptr);
  std::_Exit (3);
}

static void
on_glib_log (const gchar *log_domain,
             GLogLevelFlags log_level,
             const gchar *message,
             gpointer user_data)
{
  if ((log_level & G_LOG_LEVEL_ERROR) != 0) {
    char msg[256];
    std::snprintf (msg, sizeof (msg), "GLib Fatal Error [%s]: %s",
                   log_domain != nullptr ? log_domain : "GLib",
                   message != nullptr ? message : "(null)");
    handle_fatal_crash_and_keep_recovery (msg, 3, nullptr);
  }
  g_log_default_handler (log_domain, log_level, message, user_data);
}

static BOOL WINAPI
console_ctrl_handler (DWORD ctrl_type)
{
  switch (ctrl_type) {
  case CTRL_C_EVENT:
  case CTRL_BREAK_EVENT:
  case CTRL_CLOSE_EVENT:
  case CTRL_LOGOFF_EVENT:
  case CTRL_SHUTDOWN_EVENT:
    restore_environment_emergency ();
    break;
  default:
    break;
  }
  return FALSE;
}

static void
spawn_self_with_args (const wchar_t *extra_args)
{
  wchar_t exe_path[MAX_PATH] {};
  DWORD len = GetModuleFileNameW (nullptr, exe_path, MAX_PATH);
  if (len == 0 || len >= MAX_PATH)
    return;

  std::wstring cmd = L"\"";
  cmd += exe_path;
  cmd += L"\"";
  if (extra_args != nullptr && extra_args[0] != L'\0') {
    cmd += L" ";
    cmd += extra_args;
  }

  STARTUPINFOW si {};
  si.cb = sizeof (si);
  PROCESS_INFORMATION pi {};
  if (CreateProcessW (exe_path, cmd.data (), nullptr, nullptr, FALSE,
                      DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
    CloseHandle (pi.hThread);
    CloseHandle (pi.hProcess);
  }
}

static void
spawn_watchdog (void)
{
  wchar_t exe_path[MAX_PATH] {};
  DWORD len = GetModuleFileNameW (nullptr, exe_path, MAX_PATH);
  if (len == 0 || len >= MAX_PATH)
    return;

  DWORD pid = GetCurrentProcessId ();
  std::wstring cmd = L"\"";
  cmd += exe_path;
  cmd += L"\" --watchdog ";
  cmd += std::to_wstring (pid);

  STARTUPINFOW si {};
  si.cb = sizeof (si);
  PROCESS_INFORMATION pi {};
  if (CreateProcessW (exe_path, cmd.data (), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr,
                      &si, &pi)) {
    CloseHandle (pi.hThread);
    CloseHandle (pi.hProcess);
  }
}

// Pin all of this process's top-level windows to every virtual desktop
// ("show on all desktops", like the taskbar). Without this the shell chrome
// belongs to one desktop: it slides away with it on Win+Ctrl+Left/Right and
// only reappears when the periodic z-order re-assert pulls it onto the new
// desktop. Runs late (COM services are lazy; on a deadlocking explorer RPC
// the first call blocks up to the module's 8s timeout before self-disabling
// — keep that off the startup path) and sweeps every visible window created
// so far; windows realized later pin themselves in their realize handlers.
static BOOL CALLBACK
pin_enum_proc (HWND hwnd, LPARAM lParam)
{
  (void)lParam;
  DWORD pid = 0;
  GetWindowThreadProcessId (hwnd, &pid);
  if (pid == GetCurrentProcessId () && IsWindowVisible (hwnd))
    winome::vd::pin_window_async (hwnd);
  return TRUE;
}

static gboolean
pin_shell_windows (gpointer user_data)
{
  (void)user_data;
  EnumWindows (pin_enum_proc, 0);
  return G_SOURCE_REMOVE;
}

// Late windows (popovers, overview chrome, the OSD) pin themselves when
// shown: EVENT_OBJECT_SHOW for this process's top-level windows. Runs on the
// GTK main thread (WINEVENT_OUTOFCONTEXT + our message loop).
static HWINEVENTHOOK g_pin_show_hook = nullptr;

static gboolean
pin_window_idle (gpointer data)
{
  winome::vd::pin_window_async ((HWND) (guintptr) data);
  return G_SOURCE_REMOVE;
}

static void CALLBACK
pin_show_event_proc (HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject,
                     LONG idChild, DWORD thread, DWORD time)
{
  (void)hook;
  (void)event;
  (void)idChild;
  (void)thread;
  (void)time;
  if (idObject != OBJID_WINDOW || hwnd == nullptr)
    return;
  if (GetAncestor (hwnd, GA_ROOT) != hwnd)
    return;  // only top-level windows
  g_idle_add_full (G_PRIORITY_LOW, pin_window_idle,
                   (gpointer) (guintptr) hwnd, nullptr);
}

// Log which GSK renderer GTK selects for a realized surface. GSK_DEBUG logs
// do not reach winome.log, so query the renderer type directly instead:
// gsk_renderer_new_for_surface walks the same selection path GTK uses for
// the real renderer (display + GSK_RENDERER env), so the type name reflects
// what is actually painting the windows. "GskGLRenderer" means GPU (OpenGL
// on the hardware ICD); "GskCairoRenderer" means software. This MSYS2 GTK
// build ships only these two (d3d11 is not compiled in).
static gboolean
idle_log_gsk_probe (gpointer data)
{
  GtkWidget *widget = GTK_WIDGET (data);
  if (widget == nullptr || !gtk_widget_get_realized (widget))
    return G_SOURCE_REMOVE;
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (widget));
  if (surface == nullptr)
    return G_SOURCE_REMOVE;
  GskRenderer *renderer = gsk_renderer_new_for_surface (surface);
  if (renderer == nullptr)
    return G_SOURCE_REMOVE;

  const char *type = G_OBJECT_TYPE_NAME (renderer);
  const char *want = g_getenv ("GSK_RENDERER");
  g_print ("[gpu] GSK_RENDERER env=%s renderer=%s\n",
           want != nullptr ? want : "(unset)", type);

  // If GTK picked the GL renderer, identify the underlying GL
  // implementation: a hardware ICD (NVIDIA/Intel/AMD) means the widgets are
  // genuinely GPU-accelerated, while llvmpipe/Microsoft GDI Generic means
  // software rendering.
  if (strstr (type, "GL") != nullptr) {
    GError *error = nullptr;
    GdkGLContext *ctx = gdk_surface_create_gl_context (surface, &error);
    if (ctx != nullptr) {
      gdk_gl_context_make_current (ctx);
      const guchar *vendor = glGetString (GL_VENDOR);
      const guchar *r = glGetString (GL_RENDERER);
      const guchar *v = glGetString (GL_VERSION);
      g_print ("[gpu] GL vendor=%s renderer=%s version=%s\n",
               vendor ? (const char *)vendor : "?",
               r ? (const char *)r : "?",
               v ? (const char *)v : "?");
      g_object_unref (ctx);
    } else {
      g_print ("[gpu] gl-context failed: %s\n",
               error != nullptr ? error->message : "?");
      if (error != nullptr)
        g_error_free (error);
    }
  }

  g_object_unref (renderer);
  return G_SOURCE_REMOVE;
}

static void
log_gsk_renderer (GtkWidget *widget)
{
  if (widget != nullptr)
    g_idle_add_full (G_PRIORITY_LOW, idle_log_gsk_probe,
                     g_object_ref (widget), g_object_unref);
}

static GtkApplication *g_gtk_app = nullptr;

static void
position_panel (GtkWidget *panel)
{
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (panel));
  if (surface == NULL)
    return;

  HWND hwnd = static_cast<HWND> (gdk_win32_surface_get_handle (surface));
  if (hwnd == NULL)
    return;

  // Position the panel at the very top of the primary monitor. It is placed
  // at the monitor origin, NOT inside the work area: below, the work area is
  // shrunk by the panel height so maximized/fullscreen apps treat the strip
  // under the panel as the top of the screen, exactly like the taskbar
  // reserves the bottom.
  HMONITOR monitor = MonitorFromWindow (hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi {};
  mi.cbSize = sizeof (mi);
  if (!GetMonitorInfoW (monitor, &mi))
    return;

  int width = mi.rcMonitor.right - mi.rcMonitor.left;
  int height = winome_shell_panel_height ();

  SetWindowPos (hwnd, HWND_TOPMOST,
                mi.rcMonitor.left, mi.rcMonitor.top,
                width, height,
                SWP_SHOWWINDOW);

  // The panel is shell chrome: keep it out of Alt-Tab and the taskbar. Set
  // this last so GDK's configure handling (triggered above) cannot overwrite
  // it; fullscreen-watcher re-asserts it periodically for the same reason.
  LONG_PTR ex_style = GetWindowLongPtrW (hwnd, GWL_EXSTYLE);
  ex_style &= ~WS_EX_APPWINDOW;
  ex_style |= WS_EX_TOOLWINDOW;
  SetWindowLongPtrW (hwnd, GWL_EXSTYLE, ex_style);

  // Reserve the panel strip in the primary work area (session-only). Maximized
  // windows and apps that fill the work area now stop below the panel instead
  // of sliding their title bars underneath it.
  if (g_primary_monitor.right <= g_primary_monitor.left) {
    g_primary_monitor = mi.rcMonitor;
    save_recovery_workarea (g_primary_monitor);
  }
  RECT work = g_primary_monitor;
  work.top += height;
  SystemParametersInfoW (SPI_SETWORKAREA, 0, &work, 0);

  // Create the in-host Activities overview, z-ordered below the panel.
  const gint64 ov_t0 = g_get_monotonic_time ();
  winome::overview_init (hwnd);
  winome::perf_record_phase_ms (
      "overview_init", (g_get_monotonic_time () - ov_t0) / 1000.0);

  // Auto-hide the panel while a fullscreen window or the lock screen is
  // active; it stays visible everywhere else, including over the overview.
  winome::start_fullscreen_watcher (hwnd);

  // GNOME-style top-left hot corner opens the overview.
  winome::start_hot_corner (hwnd);

  // Multi-monitor & OS display/wallpaper/session-lock event manager.
  winome::MonitorRuntimeCallbacks mm_cbs{};
  mm_cbs.panel_height = winome_shell_panel_height;
  mm_cbs.create_secondary_panel = [] () -> GtkWidget * {
    GtkWidget *sec = winome_shell_panel_new_secondary ();
    if (sec != nullptr && g_gtk_app != nullptr)
      gtk_window_set_application (GTK_WINDOW (sec), g_gtk_app);
    return sec;
  };
  mm_cbs.register_panel = winome::register_panel_window;
  mm_cbs.unregister_panel = winome::unregister_panel_window;
  mm_cbs.is_panel_hidden_for_monitor = winome::panel_hidden_for_monitor;
  mm_cbs.pin_window = [] (HWND w) { winome::vd::pin_window (w); };
  mm_cbs.on_session_lock_changed = [] (bool locked) {
    winome::set_session_locked (locked);
    if (!locked)
      winome::NativeTaskbar::hide ();
  };
  mm_cbs.on_display_changed = [] (const RECT &primary_rect) {
    g_primary_monitor = primary_rect;
    save_recovery_workarea (g_primary_monitor);
    winome::hot_corner_refresh_bounds ();
    winome::evaluate_fullscreen_state ();
    winome::ws::on_monitors_changed ();
    winome::overview_reposition ();
  };
  mm_cbs.on_workarea_changed = winome::reassert_panel_workarea_now;
  mm_cbs.on_wallpaper_changed = winome::overview_invalidate_wallpaper;
  winome::monitor_manager_init (hwnd, mm_cbs);
}

constexpr const wchar_t *kControlWindowClass = L"WINOME_ControlWindow";
constexpr UINT WINOME_CMD_SHOW_OVERVIEW = WM_APP + 101;
constexpr UINT WINOME_CMD_RELOAD_CONFIG = WM_APP + 102;
constexpr UINT WINOME_CMD_QUIT = WM_APP + 103;
constexpr UINT WINOME_CMD_OPEN_SETTINGS = WM_APP + 104;

static HWND g_control_hwnd = nullptr;
static UINT g_msg_taskbar_created = 0;
static guint g_deferred_plugins_idle = 0;
static bool g_open_overview_on_start = false;
static bool g_open_settings_on_start = false;

static gboolean
idle_open_overview (gpointer G_GNUC_UNUSED data)
{
  winome::open_overview ("cli");
  return G_SOURCE_REMOVE;
}

static gboolean
idle_reload_config (gpointer G_GNUC_UNUSED data)
{
  winome::reload_config ();
  return G_SOURCE_REMOVE;
}

static gboolean
idle_open_settings (gpointer G_GNUC_UNUSED data)
{
  winome::open_settings_window ();
  return G_SOURCE_REMOVE;
}

static gboolean
idle_quit_app (gpointer G_GNUC_UNUSED data)
{
  if (g_gtk_app != nullptr)
    g_application_quit (G_APPLICATION (g_gtk_app));
  return G_SOURCE_REMOVE;
}

static gboolean
idle_on_explorer_restarted (gpointer G_GNUC_UNUSED data)
{
  g_print ("[shell] TaskbarCreated received; re-suppressing taskbar & recovering VD\n");
  winome::NativeTaskbar::on_explorer_restarted ();
  winome::reassert_panel_workarea_now ();
  winome::vd::reset_circuit_breaker ();
  g_timeout_add (1000, pin_shell_windows, nullptr);
  return G_SOURCE_REMOVE;
}

static LRESULT CALLBACK
control_wnd_proc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  if (g_msg_taskbar_created != 0 && msg == g_msg_taskbar_created) {
    g_idle_add (idle_on_explorer_restarted, nullptr);
    return 0;
  }
  switch (msg) {
  case WINOME_CMD_SHOW_OVERVIEW:
    g_idle_add (idle_open_overview, nullptr);
    return 0;
  case WINOME_CMD_RELOAD_CONFIG:
    g_idle_add (idle_reload_config, nullptr);
    return 0;
  case WINOME_CMD_QUIT:
    g_idle_add (idle_quit_app, nullptr);
    return 0;
  case WINOME_CMD_OPEN_SETTINGS:
    g_idle_add (idle_open_settings, nullptr);
    return 0;
  case WM_QUERYENDSESSION:
    return TRUE;
  case WM_ENDSESSION:
    if (wParam != FALSE) {
      restore_environment_emergency ();
      if (g_gtk_app != nullptr)
        g_application_quit (G_APPLICATION (g_gtk_app));
    }
    return 0;
  default:
    return DefWindowProcW (hwnd, msg, wParam, lParam);
  }
}

static void
create_control_window (void)
{
  if (g_control_hwnd != nullptr)
    return;

  HINSTANCE inst = GetModuleHandleW (nullptr);
  WNDCLASSEXW wc {};
  wc.cbSize = sizeof (wc);
  wc.lpfnWndProc = control_wnd_proc;
  wc.hInstance = inst;
  wc.lpszClassName = kControlWindowClass;
  RegisterClassExW (&wc);

  // Use a hidden top-level WS_POPUP (not HWND_MESSAGE) so both FindWindowW
  // IPC and broadcast session-end messages (WM_QUERYENDSESSION/WM_ENDSESSION)
  // reach the window reliably.
  g_msg_taskbar_created = RegisterWindowMessageW (L"TaskbarCreated");
  g_control_hwnd = CreateWindowExW (0, kControlWindowClass, L"WINOME Control",
                                    WS_POPUP, 0, 0, 0, 0,
                                    nullptr, nullptr, inst, nullptr);
}

static void
destroy_control_window (void)
{
  if (g_control_hwnd != nullptr) {
    DestroyWindow (g_control_hwnd);
    g_control_hwnd = nullptr;
  }
  UnregisterClassW (kControlWindowClass, GetModuleHandleW (nullptr));
}

static gboolean
idle_open_settings_page (gpointer data)
{
  auto *page = static_cast<std::string *> (data);
  winome::open_settings_window_page (page != nullptr ? page->c_str () : "");
  delete page;
  return G_SOURCE_REMOVE;
}

static gboolean
idle_ipc_overview_open (gpointer G_GNUC_UNUSED data)
{
  winome::open_overview ("ipc");
  return G_SOURCE_REMOVE;
}

static gboolean
idle_ipc_overview_close (gpointer G_GNUC_UNUSED data)
{
  winome::close_overview ("ipc");
  return G_SOURCE_REMOVE;
}

static gboolean
idle_ipc_overview_toggle (gpointer G_GNUC_UNUSED data)
{
  winome::toggle_overview ("ipc");
  return G_SOURCE_REMOVE;
}

struct IpcNotifyData {
  std::string title;
  std::string body;
  std::string app;
};

static winome::IpcCallbacks g_main_ipc_cbs {};

static gboolean
idle_ipc_notify (gpointer data)
{
  auto *nd = static_cast<IpcNotifyData *> (data);
  if (nd != nullptr) {
    winome::notify_shell (nd->title, nd->body,
                          nd->app.empty () ? "WINOME" : nd->app);
    delete nd;
  }
  return G_SOURCE_REMOVE;
}

static gboolean
idle_ipc_theme_reload (gpointer G_GNUC_UNUSED data)
{
  winome_shell_reload_theme ();
  return G_SOURCE_REMOVE;
}

static gboolean
idle_ipc_lg_open (gpointer G_GNUC_UNUSED data)
{
  winome::open_looking_glass ();
  return G_SOURCE_REMOVE;
}

static gboolean
idle_ipc_lg_close (gpointer G_GNUC_UNUSED data)
{
  winome::close_looking_glass ();
  return G_SOURCE_REMOVE;
}

struct IpcConfigSetData {
  std::string key;
  std::string val;
};

static bool
apply_config_set_kv (const std::string &key, const std::string &val)
{
  auto parse_bool = [&] (bool *out) {
    if (val == "true" || val == "1" || val == "on") {
      *out = true;
      return true;
    }
    if (val == "false" || val == "0" || val == "off") {
      *out = false;
      return true;
    }
    return false;
  };
  winome::WinomeConfig cfg = winome::get_config ();
  bool ok = false;
  if (key == "developer_mode")
    ok = parse_bool (&cfg.developer_mode);
  else if (key == "hot_corner_enabled")
    ok = parse_bool (&cfg.hot_corner_enabled);
  else if (key == "win_key_overview")
    ok = parse_bool (&cfg.win_key_overview);
  else if (key == "overview_window_blacklist")
    ok = parse_bool (&cfg.overview_window_blacklist);
  else if (key == "multi_monitor_panels")
    ok = parse_bool (&cfg.multi_monitor_panels);
  else if (key == "multi_monitor_overview_linked")
    ok = parse_bool (&cfg.multi_monitor_overview_linked);
  else if (key == "end_session_confirm")
    ok = parse_bool (&cfg.end_session_confirm);
  else if (key == "unify_dark_titlebars")
    ok = parse_bool (&cfg.unify_dark_titlebars);
  else if (key == "unify_rounded_corners")
    ok = parse_bool (&cfg.unify_rounded_corners);
  else if (key == "clock_show_seconds")
    ok = parse_bool (&cfg.clock_show_seconds);
  else if (key == "show_battery_percentage")
    ok = parse_bool (&cfg.show_battery_percentage);
  else if (key == "hot_corner_cooldown_ms") {
    cfg.hot_corner_cooldown_ms = std::atoi (val.c_str ());
    ok = true;
  } else if (key == "language") {
    cfg.language = val;
    ok = true;
  } else if (key == "clock_format") {
    cfg.clock_format = val;
    ok = true;
  } else if (key == "live_wallpaper_mode") {
    cfg.live_wallpaper_mode = val;
    ok = true;
  } else if (key == "multi_monitor_workspace_mode") {
    cfg.multi_monitor_workspace_mode = val;
    ok = true;
  }
  if (!ok)
    return false;
  winome::update_config (cfg);
  if (key == "developer_mode" && !cfg.developer_mode)
    winome::close_looking_glass ();
  if (key == "unify_dark_titlebars" || key == "unify_rounded_corners")
    winome::window_theming_sync_all ();
  return true;
}

static gboolean
idle_ipc_config_set (gpointer data)
{
  auto *d = static_cast<IpcConfigSetData *> (data);
  if (d != nullptr) {
    apply_config_set_kv (d->key, d->val);
    delete d;
  }
  return G_SOURCE_REMOVE;
}

static void
on_vd_changed_for_plugins (int count, int active, void *)
{
  winome::plugin_host_emit_desktop_changed (count, active);
}

static void
on_activate (GtkApplication      *app,
             gpointer G_GNUC_UNUSED user_data)
{
  g_gtk_app = app;
  // Match GNOME Shell / Mutter behavior on Windows: prevent GtkText boundary
  // edits (e.g. Backspace on empty search, arrow bounds, keynav-failed) from
  // invoking gdk_surface_beep() -> Windows MessageBeep(-1) warning chimes.
  if (GtkSettings *settings = gtk_settings_get_default ())
    g_object_set (settings, "gtk-error-bell", FALSE, nullptr);
  create_control_window ();

  const gint64 panel_t0 = g_get_monotonic_time ();
  const gint64 act_t0 = panel_t0;
  GtkWidget *panel = winome_shell_panel_new ();

  gtk_window_set_application (GTK_WINDOW (panel), app);
  g_signal_connect (panel, "realize", G_CALLBACK (log_gsk_renderer), nullptr);
  gtk_widget_set_visible (panel, TRUE);

  position_panel (panel);
  const double panel_and_ov_ms = (g_get_monotonic_time () - panel_t0) / 1000.0;
  const double ov_ms = winome::perf_get_startup_timings ().overview_init_ms;
  winome::perf_record_phase_ms ("panel", std::max (0.0, panel_and_ov_ms - ov_ms));

  // Multi-monitor workspace manager (Independent / PrimaryOnly / Span).
  winome::ws::init ();

  // Workspace-switch OSD (workspaceSwitcherPopup.js) + event-driven VD sync.
  winome::ws_osd_init ();

  // Live WinRT notifications + toast banner + SMTC store.
  winome::notifications_init ();

  // Compatible system tray collector (classic toolbar + Win11 UIA).
  winome::systray_init ();

  // Event-driven WASAPI master volume & default audio endpoint monitor.
  winome::start_volume_monitor ();

  // Opt-in DWM window unification (dark titlebars / Win11 rounded corners).
  const gint64 theme_t0 = g_get_monotonic_time ();
  winome::window_theming_init ();
  winome::perf_record_phase_ms (
      "theming", (g_get_monotonic_time () - theme_t0) / 1000.0);

  // Bidirectional Named Pipe IPC server (\\.\pipe\WINOME_Shell_IPC).
  const gint64 ipc_t0 = g_get_monotonic_time ();
  g_main_ipc_cbs = winome::IpcCallbacks {};
  g_main_ipc_cbs.overview_open = +[] () {
    g_idle_add (idle_ipc_overview_open, nullptr);
  };
  g_main_ipc_cbs.overview_close = +[] () {
    g_idle_add (idle_ipc_overview_close, nullptr);
  };
  g_main_ipc_cbs.overview_toggle = +[] () {
    g_idle_add (idle_ipc_overview_toggle, nullptr);
  };
  g_main_ipc_cbs.overview_app_grid = +[] () {
    winome::overview_shift_to_app_grid_async ();
  };
  g_main_ipc_cbs.overview_is_open = +[] () -> bool {
    return winome::overview_active ();
  };
  g_main_ipc_cbs.settings_open = +[] (const std::string &page) {
    g_idle_add (idle_open_settings_page, new std::string (page));
  };
  g_main_ipc_cbs.config_reload = +[] () {
    g_idle_add (idle_reload_config, nullptr);
  };
  g_main_ipc_cbs.config_set = +[] (const std::string &key,
                                   const std::string &val) -> bool {
    // Validate on caller thread; execute listener updates on GTK main thread.
    static const char *const kAllowed[] = {
        "developer_mode", "hot_corner_enabled", "win_key_overview",
        "overview_window_blacklist", "multi_monitor_panels",
        "multi_monitor_overview_linked", "end_session_confirm", "unify_dark_titlebars", "unify_rounded_corners",
        "clock_show_seconds", "show_battery_percentage",
        "hot_corner_cooldown_ms", "language", "clock_format",
        "live_wallpaper_mode", "multi_monitor_workspace_mode"};
    bool known = false;
    for (const char *k : kAllowed) {
      if (key == k) {
        known = true;
        break;
      }
    }
    if (!known)
      return false;
    g_idle_add (idle_ipc_config_set, new IpcConfigSetData {key, val});
    return true;
  };
  g_main_ipc_cbs.desktop_switch = +[] (int index) -> bool {
    const bool ok = winome::vd::switch_to (index);
    if (ok)
      winome::vd::notify_change_hint_async ();
    return ok;
  };
  g_main_ipc_cbs.desktop_list = +[] () {
    return winome::vd::desktops ();
  };
  g_main_ipc_cbs.desktop_active = +[] () -> int {
    return winome::vd::current_index ();
  };
  g_main_ipc_cbs.desktop_create = +[] () -> bool {
    const bool ok = winome::vd::create_desktop ();
    if (ok)
      winome::vd::notify_change_hint_async ();
    return ok;
  };
  g_main_ipc_cbs.desktop_remove = +[] (int index) -> bool {
    const bool ok = winome::vd::remove_desktop (index);
    if (ok)
      winome::vd::notify_change_hint_async ();
    return ok;
  };
  g_main_ipc_cbs.notify = +[] (const std::string &title,
                               const std::string &body,
                               const std::string &app) {
    g_idle_add (idle_ipc_notify, new IpcNotifyData {title, body, app});
  };
  g_main_ipc_cbs.theme_reload = +[] () -> bool {
    g_idle_add (idle_ipc_theme_reload, nullptr);
    return true;
  };
  g_main_ipc_cbs.looking_glass_open = +[] () -> bool {
    if (!winome::get_config ().developer_mode)
      return false;
    g_idle_add (idle_ipc_lg_open, nullptr);
    return true;
  };
  g_main_ipc_cbs.looking_glass_close = +[] () -> bool {
    g_idle_add (idle_ipc_lg_close, nullptr);
    return true;
  };
  g_main_ipc_cbs.run_exec = +[] (const std::string &cmdline,
                                 std::string *out_err) -> bool {
    winome::RunDialogCallbacks rcbs {};
    rcbs.open_looking_glass = winome::open_looking_glass;
    rcbs.reload_theme = winome_shell_reload_theme;
    rcbs.restart_shell = +[] () {
      std::wstring args = L"--after-pid " + std::to_wstring (GetCurrentProcessId ());
      if (winome::is_safe_mode ())
        args += L" --safe-mode";
      spawn_self_with_args (args.c_str ());
      g_idle_add (idle_quit_app, nullptr);
    };
    rcbs.quit_shell = +[] () {
      g_idle_add (idle_quit_app, nullptr);
    };
    // Marshal internal commands safely via g_idle_add when invoked from IPC.
    const auto act = winome::classify_run_internal_command (
        cmdline, winome::get_config ().developer_mode);
    if (act == winome::RunInternalAction::OpenLookingGlass) {
      g_idle_add (idle_ipc_lg_open, nullptr);
      return true;
    }
    if (act == winome::RunInternalAction::ReloadTheme) {
      g_idle_add (idle_ipc_theme_reload, nullptr);
      return true;
    }
    return winome::execute_run_command_string (cmdline, false, rcbs, out_err);
  };
  g_main_ipc_cbs.plugin_list_json = winome::plugin_host_list_json;
  g_main_ipc_cbs.plugin_action = winome::plugin_host_action;
  g_main_ipc_cbs.quit = +[] () {
    g_idle_add (idle_quit_app, nullptr);
  };

  winome::RunDialogCallbacks run_cbs {};
  run_cbs.open_looking_glass = winome::open_looking_glass;
  run_cbs.reload_theme = winome_shell_reload_theme;
  run_cbs.restart_shell = +[] () {
    std::wstring args = L"--after-pid " + std::to_wstring (GetCurrentProcessId ());
    if (winome::is_safe_mode ())
      args += L" --safe-mode";
    spawn_self_with_args (args.c_str ());
    if (g_gtk_app != nullptr)
      g_application_quit (G_APPLICATION (g_gtk_app));
  };
  run_cbs.quit_shell = +[] () {
    if (g_gtk_app != nullptr)
      g_application_quit (G_APPLICATION (g_gtk_app));
  };
  winome::run_dialog_init (run_cbs);

  winome::LookingGlassCallbacks lg_cbs {};
  lg_cbs.dispatch_cmd = +[] (const std::string &req) -> std::string {
    return winome::ipc_dispatch_request_json (req, g_main_ipc_cbs);
  };
  lg_cbs.restack_windows = +[] () {
    winome::overview_restack ();
  };
  lg_cbs.reload_theme = +[] () -> bool {
    return winome_shell_reload_theme ();
  };
  lg_cbs.plugin_list_json = winome::plugin_host_list_json;
  lg_cbs.plugin_action = winome::plugin_host_action;
  winome::looking_glass_init (lg_cbs);

  winome_shell_panel_set_plugin_click_cb (
      +[] (const char *plugin_id, const char *slot) {
        winome::plugin_host_emit_topbar_click (
            plugin_id ? plugin_id : "", slot ? slot : "");
      });
  winome_quick_settings_set_plugin_toggle_cb (
      +[] (const char *plugin_id, const char *slot, gboolean active) {
        winome::plugin_host_emit_quicktoggle (
            plugin_id ? plugin_id : "", slot ? slot : "", active != FALSE);
      });

  static winome::PluginHostCallbacks plugin_cbs {};
  plugin_cbs.topbar_upsert = +[] (const winome::PluginTopBarSpec &spec) {
    winome_shell_panel_plugin_upsert (
        spec.key.c_str (), spec.plugin_id.c_str (), spec.slot.c_str (),
        spec.icon.c_str (), spec.text.c_str (), spec.tooltip.c_str ());
  };
  plugin_cbs.topbar_remove = +[] (const std::string &, const std::string &key) {
    winome_shell_panel_plugin_remove (key.c_str ());
  };
  plugin_cbs.quicktoggle_upsert =
      +[] (const winome::PluginQuickToggleSpec &spec) {
        winome_quick_settings_plugin_upsert (
            spec.key.c_str (), spec.plugin_id.c_str (), spec.slot.c_str (),
            spec.icon.c_str (), spec.title.c_str (), spec.subtitle.c_str (),
            spec.active ? TRUE : FALSE);
      };
  plugin_cbs.quicktoggle_remove =
      +[] (const std::string &, const std::string &key) {
        winome_quick_settings_plugin_remove (key.c_str ());
      };
  plugin_cbs.notify = +[] (const std::string &title,
                           const std::string &body,
                           const std::string &app_name) {
    winome::notify_shell (title, body, app_name);
  };
  plugin_cbs.plugins_changed = winome::refresh_looking_glass_plugins;

  winome::ipc_server_start (g_main_ipc_cbs);
  winome::perf_record_phase_ms (
      "ipc", (g_get_monotonic_time () - ipc_t0) / 1000.0);

  // Pin the shell chrome to all virtual desktops (see pin_shell_windows).
  // Delayed: the first COM call can block up to the vd module's 8s timeout
  // when the explorer RPC deadlocks — off the startup path.
  g_timeout_add (3000, pin_shell_windows, nullptr);
  g_pin_show_hook = SetWinEventHook (EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW,
                                     nullptr, pin_show_event_proc,
                                     GetCurrentProcessId (), 0,
                                     WINEVENT_OUTOFCONTEXT);

  if (g_open_overview_on_start) {
    g_open_overview_on_start = false;
    g_idle_add (idle_open_overview, nullptr);
  }
  if (g_open_settings_on_start) {
    g_open_settings_on_start = false;
    g_idle_add (idle_open_settings, nullptr);
  }

  winome::perf_record_phase_ms (
      "activate", (g_get_monotonic_time () - act_t0) / 1000.0);

  g_deferred_plugins_idle = g_idle_add (
      +[] (gpointer data) -> gboolean {
        g_deferred_plugins_idle = 0;
        const auto *cbs =
            static_cast<const winome::PluginHostCallbacks *> (data);
        const gint64 plug_t0 = g_get_monotonic_time ();
        winome::plugin_host_init (*cbs);
        winome::vd::add_change_listener (on_vd_changed_for_plugins, nullptr);
        winome::perf_record_phase_ms (
            "plugins", (g_get_monotonic_time () - plug_t0) / 1000.0);
        winome::perf_finish_startup ();
        return G_SOURCE_REMOVE;
      },
      &plugin_cbs);
}

static void
on_shutdown (GtkApplication      *app G_GNUC_UNUSED,
             gpointer G_GNUC_UNUSED user_data)
{
  if (g_deferred_plugins_idle != 0) {
    g_source_remove (g_deferred_plugins_idle);
    g_deferred_plugins_idle = 0;
  }
  winome::vd::remove_change_listener (on_vd_changed_for_plugins, nullptr);
  winome::plugin_host_shutdown ();
  winome::ipc_server_stop ();
  destroy_control_window ();
  g_gtk_app = nullptr;

  winome::run_dialog_close ();
  winome::close_looking_glass ();
  winome::close_end_session_dialog ();
  winome::close_settings_window ();
  winome::ws_osd_shutdown ();
  winome::window_theming_shutdown ();
  winome::app_icon_cache_clear ();
  winome::monitor_manager_shutdown ();
  winome::stop_hot_corner ();
  winome::stop_fullscreen_watcher ();
  winome::app_index_stop_watch ();
  winome::systray_shutdown ();
  winome::stop_volume_monitor ();
  winome_wifi_shutdown_notifications ();
  winome::notifications_shutdown ();
  winome::winrt_shutdown ();

  if (g_pin_show_hook != nullptr) {
    UnhookWinEvent (g_pin_show_hook);
    g_pin_show_hook = nullptr;
  }

  winome::stop_overview_trigger ();
  winome::stop_win_event_hook ();
  winome::ws::shutdown ();
  winome::vd::shutdown ();

  // Release the panel-strip work area reservation before restoring the
  // taskbar, so the shell recalculates the work area around it.
  if (g_primary_monitor.right > g_primary_monitor.left)
    SystemParametersInfoW (SPI_SETWORKAREA, 0, &g_primary_monitor, SPIF_SENDCHANGE);

  // Restore the native taskbar on exit (only if we hid it).
  winome::NativeTaskbar::restore ();
  clear_recovery_file ();
  clear_crash_state_file ();
  winome::debug_flush_log_repeats ();
}

static bool
watchdog_restore_callback (void *user_data)
{
  (void)user_data;
  return apply_recovery_keep_or_clear (true, true);
}

static int
run_watchdog_mode (DWORD pid, bool show_ui)
{
  winome::set_process_dpi_aware ();
  init_crash_paths ();

  DWORD exit_code = 0;
  if (pid != 0) {
    HANDLE proc = OpenProcess (
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc != nullptr) {
      WaitForSingleObject (proc, INFINITE);
      GetExitCodeProcess (proc, &exit_code);
      CloseHandle (proc);
    }
  }

  const bool had_recovery = has_recovery_file ();
  const bool had_crash_file =
      !crash_state_file_path ().empty () &&
      GetFileAttributesW (crash_state_file_path ().c_str ()) != INVALID_FILE_ATTRIBUTES;
  const bool abnormal = had_recovery || had_crash_file ||
                        (pid != 0 && exit_code != 0 && exit_code != STILL_ACTIVE);
  if (!abnormal)
    return 0;

  // Always restore workarea & native taskbar FIRST before showing any UI.
  const bool restored = apply_recovery_keep_or_clear (true, true);
  if (!show_ui) {
    clear_crash_state_file ();
    return 0;
  }

  (void)winome::get_config ();
  const bool zh = winome::is_zh ();

  winome::CrashState st;
  {
    std::wstring cpath = crash_state_file_path ();
    if (!cpath.empty ()) {
      HANDLE f = CreateFileW (cpath.c_str (), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
      if (f != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz{};
        if (GetFileSizeEx (f, &sz) && sz.QuadPart > 0 && sz.QuadPart <= 65536) {
          std::string raw (static_cast<size_t> (sz.QuadPart), '\0');
          DWORD rbytes = 0;
          if (ReadFile (f, raw.data (), static_cast<DWORD> (sz.QuadPart),
                        &rbytes, nullptr)) {
            raw.resize (rbytes);
            winome::parse_crash_state (raw, &st);
          }
        }
        CloseHandle (f);
      }
      clear_crash_state_file ();
    }
  }

  if (st.reason.empty ()) {
    st.reason = zh ? "进程被外部强行终止或未捕获异常退出"
                   : "Process terminated unexpectedly or was killed externally";
  }
  if (st.exit_code == 0 && exit_code != 0)
    st.exit_code = exit_code;
  if (st.pid == 0)
    st.pid = pid;
  st.taskbar_restored = restored;
  st.log_tail = winome::read_log_tail_utf8 (g_log_file_path, 3500);

  winome::CrashRecoveryDialogInput input;
  input.state = std::move (st);
  input.data_dir = winome::app_data_dir ();
  input.zh = zh;
  input.on_restore_now = watchdog_restore_callback;
  input.user_data = nullptr;

  const auto action = winome::show_crash_recovery_dialog (input);
  if (action == winome::CrashDialogAction::RestartSafeMode)
    spawn_self_with_args (L"--safe-mode");
  else if (action == winome::CrashDialogAction::RestartNormal)
    spawn_self_with_args (nullptr);

  return 0;
}

int
main (int argc, char **argv)
{
  winome::perf_mark_process_start ();

  // Parse headless / recovery / watchdog flags first.
  bool no_crash_ui = false;
  DWORD wait_prev_pid = 0;
  for (int i = 1; i < argc; ++i) {
    if (std::string (argv[i]) == "--no-crash-ui")
      no_crash_ui = true;
    else if (std::string (argv[i]) == "--safe-mode")
      winome::set_safe_mode (true);
    else if (std::string (argv[i]) == "--after-pid" && i + 1 < argc)
      wait_prev_pid = static_cast<DWORD> (std::strtoul (argv[i + 1], nullptr, 10));
  }
  if (wait_prev_pid != 0) {
    HANDLE prev = OpenProcess (SYNCHRONIZE, FALSE, wait_prev_pid);
    if (prev != nullptr) {
      WaitForSingleObject (prev, 5000);
      CloseHandle (prev);
    }
  }

  for (int i = 1; i < argc; ++i) {
    if (std::string (argv[i]) == "--restore") {
      apply_and_clear_recovery (true);
      clear_crash_state_file ();
      return 0;
    }
    if (std::string (argv[i]) == "--watchdog" && i + 1 < argc) {
      DWORD pid = static_cast<DWORD> (std::strtoul (argv[i + 1], nullptr, 10));
      return run_watchdog_mode (pid, !no_crash_ui);
    }
    if (std::string (argv[i]) == "--crash-ui") {
      return run_watchdog_mode (0, true);
    }
    if (std::string (argv[i]) == "--status") {
      if (GetFileType (GetStdHandle (STD_OUTPUT_HANDLE)) == FILE_TYPE_UNKNOWN &&
          AttachConsole (ATTACH_PARENT_PROCESS)) {
        std::freopen ("CONOUT$", "w", stdout);
      }
      std::string resp;
      if (winome::ipc_query_running_shell ("{\"cmd\":\"status\"}", &resp)) {
        std::fwrite (resp.data (), 1, resp.size (), stdout);
        std::fputc ('\n', stdout);
        std::fflush (stdout);
        return 0;
      }
      std::printf ("{\"ok\":false,\"error\":\"not_running\"}\n");
      std::fflush (stdout);
      return 1;
    }
  }

  // Forward single-instance CLI commands to an already-running WINOME shell.
  bool want_overview = false;
  bool want_settings = false;
  UINT remote_cmd = 0;
  for (int i = 1; i < argc; ++i) {
    std::string arg (argv[i]);
    if (arg == "--show-overview") {
      remote_cmd = WINOME_CMD_SHOW_OVERVIEW;
      want_overview = true;
    } else if (arg == "--reload") {
      remote_cmd = WINOME_CMD_RELOAD_CONFIG;
    } else if (arg == "--quit") {
      remote_cmd = WINOME_CMD_QUIT;
    } else if (arg == "--settings") {
      remote_cmd = WINOME_CMD_OPEN_SETTINGS;
      want_settings = true;
    }
  }
  if (remote_cmd != 0) {
    HWND ctrl = FindWindowW (kControlWindowClass, nullptr);
    if (ctrl != nullptr) {
      PostMessageW (ctrl, remote_cmd, 0, 0);
      return 0;
    }
    if (remote_cmd == WINOME_CMD_RELOAD_CONFIG || remote_cmd == WINOME_CMD_QUIT)
      return 0;
  }
  g_open_overview_on_start = want_overview;
  g_open_settings_on_start = want_settings;

  // Single-instance guard so two shell panels do not fight over the taskbar.
  HANDLE instance_mutex = CreateMutexW (nullptr, TRUE,
                                        L"Local\\WINOME_Shell_SingleInstance");
  if (instance_mutex != nullptr && GetLastError () == ERROR_ALREADY_EXISTS) {
    CloseHandle (instance_mutex);
    return 0;
  }

  // Load user configuration before initializing subsystems.
  (void)winome::get_config ();

  // Route stdout/stderr to a log file first: this process is a GUI app and
  // must never pop a console window.
  init_crash_paths ();
  setup_log_file ();
  winome::debug_install_log_capture ();

  // Recover taskbar and work area if a prior session terminated abnormally.
  const gint64 rec_t0 = g_get_monotonic_time ();
  if (has_recovery_file ())
    apply_and_clear_recovery (true);
  clear_crash_state_file ();
  winome::perf_record_phase_ms (
      "recovery", (g_get_monotonic_time () - rec_t0) / 1000.0);

  SetUnhandledExceptionFilter (unhandled_exception_filter);
  SetConsoleCtrlHandler (console_ctrl_handler, TRUE);
  std::set_terminate (on_cpp_terminate);
  std::signal (SIGABRT, on_crt_signal);
  std::signal (SIGSEGV, on_crt_signal);
  std::signal (SIGILL, on_crt_signal);
  std::signal (SIGFPE, on_crt_signal);
  g_log_set_default_handler (on_glib_log, nullptr);

  // Snapshot the clean work area before hiding the taskbar so watchdog can
  // recover even if the process exits before on_activate completes.
  RECT initial_work {};
  if (SystemParametersInfoW (SPI_GETWORKAREA, 0, &initial_work, 0))
    save_recovery_workarea (initial_work);

  spawn_watchdog ();

  // Initialize Windows integration before creating the GTK app.
  winome::set_process_dpi_aware ();
  winome::enable_privilege (L"SeTcbPrivilege");
  winome::NativeTaskbar::hide ();
  winome::start_win_event_hook ();
  winome::start_overview_trigger ();

  GtkApplication *app;

  app = gtk_application_new ("org.winome.Shell",
                             G_APPLICATION_NON_UNIQUE);

  g_signal_connect (app, "activate",
                    G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "shutdown",
                    G_CALLBACK (on_shutdown), NULL);

  int status = g_application_run (G_APPLICATION (app), 1, argv);

  g_object_unref (app);
  if (instance_mutex != nullptr)
    CloseHandle (instance_mutex);

  return status;
}
