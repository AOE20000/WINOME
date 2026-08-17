// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// WINOME host process entry point.

#include <gtk/gtk.h>
#include <gdk/win32/gdkwin32.h>

#include <windows.h>

#include "host-utils.h"
#include "native-taskbar.h"
#include "overview-trigger.h"
#include "shell-panel.h"
#include "win-event-hook.h"

static void
position_panel (GtkWidget *panel)
{
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (panel));
  if (surface == NULL)
    return;

  HWND hwnd = static_cast<HWND> (gdk_win32_surface_get_handle (surface));
  if (hwnd == NULL)
    return;

  // Position the panel as a top-most, full-width top bar across the primary
  // work area (the taskbar is hidden, so the work area spans the full screen).
  RECT work_area;
  if (!SystemParametersInfoW (SPI_GETWORKAREA, 0, &work_area, 0))
    return;

  int width = work_area.right - work_area.left;

  SetWindowPos (hwnd, HWND_TOPMOST,
                work_area.left, work_area.top,
                width, winome_shell_panel_height (),
                SWP_SHOWWINDOW);
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
  // Restore the native taskbar on exit (only if we hid it).
  winome::NativeTaskbar::restore ();
}

int
main (int argc, char **argv)
{
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
