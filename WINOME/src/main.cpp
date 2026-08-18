// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// WINOME host process entry point.

#include <gtk/gtk.h>
#include <gdk/win32/gdkwin32.h>

#include <windows.h>
#include <cstdio>

#include "host-utils.h"
#include "native-taskbar.h"
#include "overview-trigger.h"
#include "overview.h"
#include "shell-panel.h"
#include "win-event-hook.h"
#include "fullscreen-watcher.h"
#include "hot-corner.h"

// The host runs as a GUI application (no console window). Route diagnostics
// to a log file so startup errors are still visible.
static void
setup_log_file (void)
{
  char path[MAX_PATH];
  if (GetTempPathA (MAX_PATH, path) == 0)
    return;
  strcat_s (path, "winome.log");
  // Append keeps previous sessions' logs; nothing is shown on screen.
  freopen (path, "a", stdout);
  freopen (path, "a", stderr);
}

// Primary monitor bounds captured when the panel is positioned; restored in
// on_shutdown so the shell recalculates the work area around the taskbar.
static RECT g_primary_monitor = {0, 0, 0, 0};

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
  MONITORINFO mi = {sizeof (mi)};
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
  g_primary_monitor = mi.rcMonitor;
  RECT work = mi.rcMonitor;
  work.top += height;
  SystemParametersInfoW (SPI_SETWORKAREA, 0, &work, 0);

  // Create the in-host Activities overview, z-ordered below the panel.
  winome::overview_init (hwnd);

  // Auto-hide the panel while a fullscreen window or the lock screen is
  // active; it stays visible everywhere else, including over the overview.
  winome::start_fullscreen_watcher (hwnd);

  // GNOME-style top-left hot corner opens the overview.
  winome::start_hot_corner (hwnd);
}

static void
on_activate (GtkApplication      *app,
             gpointer G_GNUC_UNUSED user_data)
{
  GtkWidget *panel = winome_shell_panel_new ();

  gtk_window_set_application (GTK_WINDOW (panel), app);
  gtk_widget_set_visible (panel, TRUE);

  position_panel (panel);
}

static void
on_shutdown (GtkApplication      *app G_GNUC_UNUSED,
             gpointer G_GNUC_UNUSED user_data)
{
  // Release the panel-strip work area reservation before restoring the
  // taskbar, so the shell recalculates the normal work area around it.
  if (g_primary_monitor.right > g_primary_monitor.left)
    SystemParametersInfoW (SPI_SETWORKAREA, 0, &g_primary_monitor, 0);

  // Restore the native taskbar on exit (only if we hid it).
  winome::NativeTaskbar::restore ();
}

int
main (int argc, char **argv)
{
  // Route stdout/stderr to a log file first: this process is a GUI app and
  // must never pop a console window.
  setup_log_file ();

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

  int status = g_application_run (G_APPLICATION (app), argc, argv);

  g_object_unref (app);

  return status;
}
