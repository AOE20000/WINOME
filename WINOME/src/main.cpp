// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// WINOME host process entry point.

#include <gtk/gtk.h>
#include <gdk/win32/gdkwin32.h>
#include <epoxy/gl.h>

#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "config.h"
#include "app-index.h"
#include "host-utils.h"
#include "native-taskbar.h"
#include "overview-trigger.h"
#include "overview.h"
#include "shell-panel.h"
#include "win-event-hook.h"
#include "fullscreen-watcher.h"
#include "hot-corner.h"
#include "monitor-manager.h"
#include "notifications.h"
#include "settings-window.h"
#include "systray.h"
#include "system-status.h"
#include "wifi-networks.h"
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
}

// Primary monitor bounds captured when the panel is positioned; restored in
// on_shutdown so the shell recalculates the work area around the taskbar.
static RECT g_primary_monitor = {0, 0, 0, 0};

static std::wstring
recovery_file_path (void)
{
  std::wstring dir = winome::app_data_dir ();
  if (dir.empty ())
    return {};
  return dir + L"\\recovery.dat";
}

static void
save_recovery_workarea (const RECT &rc)
{
  std::wstring path = recovery_file_path ();
  if (path.empty () || rc.right <= rc.left || rc.bottom <= rc.top)
    return;

  HANDLE file = CreateFileW (path.c_str (), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return;

  DWORD written = 0;
  WriteFile (file, &rc, sizeof (rc), &written, nullptr);
  CloseHandle (file);
}

static void
clear_recovery_file (void)
{
  std::wstring path = recovery_file_path ();
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

static void
apply_and_clear_recovery (bool force_taskbar_restore)
{
  std::wstring path = recovery_file_path ();
  bool recovered = false;

  if (!path.empty ()) {
    HANDLE file = CreateFileW (path.c_str (), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      RECT rc {};
      DWORD read_bytes = 0;
      if (ReadFile (file, &rc, sizeof (rc), &read_bytes, nullptr) &&
          read_bytes == sizeof (rc) &&
          rc.right > rc.left && rc.bottom > rc.top) {
        SystemParametersInfoW (SPI_SETWORKAREA, 0, &rc, 0);
        recovered = true;
      }
      CloseHandle (file);
    }
  }

  if (recovered || force_taskbar_restore)
    winome::NativeTaskbar::force_restore_all ();

  clear_recovery_file ();
}

static void
restore_environment_emergency (void)
{
  if (g_primary_monitor.right > g_primary_monitor.left)
    SystemParametersInfoW (SPI_SETWORKAREA, 0, &g_primary_monitor, 0);
  winome::NativeTaskbar::restore ();
  clear_recovery_file ();
}

using MiniDumpWriteDumpFn = BOOL (WINAPI *)(
    HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION,
    PMINIDUMP_USER_STREAM_INFORMATION,
    PMINIDUMP_CALLBACK_INFORMATION);

static void
write_crash_minidump (EXCEPTION_POINTERS *info)
{
  HMODULE dbghelp = LoadLibraryW (L"dbghelp.dll");
  if (dbghelp == nullptr)
    return;

  auto write_dump = reinterpret_cast<MiniDumpWriteDumpFn> (
      reinterpret_cast<void *> (GetProcAddress (dbghelp, "MiniDumpWriteDump")));
  if (write_dump == nullptr) {
    FreeLibrary (dbghelp);
    return;
  }

  std::wstring dir = winome::app_data_dir ();
  if (dir.empty ()) {
    FreeLibrary (dbghelp);
    return;
  }
  CreateDirectoryW (dir.c_str (), nullptr);
  std::wstring crash_dir = dir + L"\\crashes";
  CreateDirectoryW (crash_dir.c_str (), nullptr);

  SYSTEMTIME st {};
  GetLocalTime (&st);
  wchar_t path[MAX_PATH] = {};
  _snwprintf (path, MAX_PATH - 1,
              L"%ls\\winome-%04u%02u%02u-%02u%02u%02u-%lu.dmp",
              crash_dir.c_str (),
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
    write_dump (GetCurrentProcess (), GetCurrentProcessId (), file,
                MiniDumpNormal, info ? &mei : nullptr, nullptr, nullptr);
    CloseHandle (file);
  }
  FreeLibrary (dbghelp);
}

static LONG WINAPI
unhandled_exception_filter (EXCEPTION_POINTERS *info)
{
  write_crash_minidump (info);
  restore_environment_emergency ();
  return EXCEPTION_CONTINUE_SEARCH;
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
    winome::vd::pin_window (hwnd);
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
  winome::vd::pin_window ((HWND) (guintptr) data);
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
static void
log_gsk_renderer (GtkWidget *widget)
{
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (widget));
  if (surface == nullptr)
    return;
  GskRenderer *renderer = gsk_renderer_new_for_surface (surface);
  if (renderer == nullptr)
    return;

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
  winome::overview_init (hwnd);

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
  mm_cbs.pin_window = [] (HWND w) { winome::vd::pin_window (w); };
  mm_cbs.on_session_lock_changed = [] (bool locked) {
    winome::set_session_locked (locked);
    if (!locked)
      winome::NativeTaskbar::hide ();
  };
  mm_cbs.on_display_changed = [] (const RECT &primary_rect) {
    g_primary_monitor = primary_rect;
    save_recovery_workarea (primary_rect);
    winome::evaluate_fullscreen_state ();
    winome::overview_reposition ();
  };
  mm_cbs.on_wallpaper_changed = winome::overview_invalidate_wallpaper;
  winome::monitor_manager_init (hwnd, mm_cbs);
}

constexpr const wchar_t *kControlWindowClass = L"WINOME_ControlWindow";
constexpr UINT WINOME_CMD_SHOW_OVERVIEW = WM_APP + 101;
constexpr UINT WINOME_CMD_RELOAD_CONFIG = WM_APP + 102;
constexpr UINT WINOME_CMD_QUIT = WM_APP + 103;
constexpr UINT WINOME_CMD_OPEN_SETTINGS = WM_APP + 104;

static HWND g_control_hwnd = nullptr;
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

static LRESULT CALLBACK
control_wnd_proc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
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

static void
on_activate (GtkApplication      *app,
             gpointer G_GNUC_UNUSED user_data)
{
  g_gtk_app = app;
  create_control_window ();

  GtkWidget *panel = winome_shell_panel_new ();

  gtk_window_set_application (GTK_WINDOW (panel), app);
  g_signal_connect (panel, "realize", G_CALLBACK (log_gsk_renderer), nullptr);
  gtk_widget_set_visible (panel, TRUE);

  position_panel (panel);

  // Workspace-switch OSD (workspaceSwitcherPopup.js) + desktop poll.
  winome::ws_osd_init ();

  // Live WinRT notifications + toast banner + SMTC store.
  winome::notifications_init ();

  // Compatible system tray collector (classic toolbar + Win11 UIA).
  winome::systray_init ();

  // Event-driven WASAPI master volume & default audio endpoint monitor.
  winome::start_volume_monitor ();

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
}

static void
on_shutdown (GtkApplication      *app G_GNUC_UNUSED,
             gpointer G_GNUC_UNUSED user_data)
{
  destroy_control_window ();
  g_gtk_app = nullptr;

  winome::close_settings_window ();
  winome::app_icon_cache_clear ();
  winome::monitor_manager_shutdown ();
  winome::stop_fullscreen_watcher ();
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

  // Release the panel-strip work area reservation before restoring the
  // taskbar, so the shell recalculates the work area around it.
  if (g_primary_monitor.right > g_primary_monitor.left)
    SystemParametersInfoW (SPI_SETWORKAREA, 0, &g_primary_monitor, 0);

  // Restore the native taskbar on exit (only if we hid it).
  winome::NativeTaskbar::restore ();
  clear_recovery_file ();
}

int
main (int argc, char **argv)
{
  // Recovery mode: restore native taskbar and any saved work area, then exit.
  for (int i = 1; i < argc; ++i) {
    if (std::string (argv[i]) == "--restore") {
      apply_and_clear_recovery (true);
      return 0;
    }
    if (std::string (argv[i]) == "--watchdog" && i + 1 < argc) {
      DWORD pid = static_cast<DWORD> (std::strtoul (argv[i + 1], nullptr, 10));
      if (pid != 0) {
        HANDLE proc = OpenProcess (SYNCHRONIZE, FALSE, pid);
        if (proc != nullptr) {
          WaitForSingleObject (proc, INFINITE);
          CloseHandle (proc);
        }
        if (has_recovery_file ())
          apply_and_clear_recovery (true);
      }
      return 0;
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
  setup_log_file ();

  // Recover taskbar and work area if a prior session terminated abnormally.
  if (has_recovery_file ())
    apply_and_clear_recovery (true);

  SetUnhandledExceptionFilter (unhandled_exception_filter);
  SetConsoleCtrlHandler (console_ctrl_handler, TRUE);

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
                             G_APPLICATION_DEFAULT_FLAGS);

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
