// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// GNOME-style calendar (matches js/ui/calendar.js Calendar widget).

#include "quick-settings.h"
#include "system-status.h"

#include <time.h>
#include <string.h>

// Convenience for the language check used throughout this file.
static const char *
sys_lang (void)
{
  return winome::get_system_language ();
}

// Calendar state.
typedef struct {
  int selected_year;
  int selected_month;   // 0..11
  int selected_day;

  GtkWidget *grid;
  GtkWidget *month_label;
} CalendarData;

static void calendar_rebuild (CalendarData *data);

// Weekday abbreviation for a day index (0=Sunday). Matches
// _getCalendarDayAbbreviation in calendar.js.
static const char *
weekday_abbrev (int day)
{
  static const char *zh[] = {"日", "一", "二", "三", "四", "五", "六"};
  static const char *en[] = {"S", "M", "T", "W", "T", "F", "S"};
  const char *lang = sys_lang ();
  if (lang && strcmp (lang, "zh") == 0)
    return zh[day];
  return en[day];
}

static const char *
month_name (int month)
{
  static const char *zh[] = {"一月", "二月", "三月", "四月", "五月", "六月",
                             "七月", "八月", "九月", "十月", "十一月", "十二月"};
  static const char *en[] = {"January", "February", "March", "April", "May",
                             "June", "July", "August", "September", "October",
                             "November", "December"};
  const char *lang = sys_lang ();
  if (lang && strcmp (lang, "zh") == 0)
    return zh[month];
  return en[month];
}

static void
update_month_label (CalendarData *data)
{
  gchar text[64];
  const char *lang = sys_lang ();
  if (lang && strcmp (lang, "zh") == 0)
    snprintf (text, sizeof (text), "%d年%s", data->selected_year,
              month_name (data->selected_month));
  else
    snprintf (text, sizeof (text), "%s %d", month_name (data->selected_month),
              data->selected_year);
  gtk_label_set_text (GTK_LABEL (data->month_label), text);
}

static void
on_prev_month (GtkButton *b G_GNUC_UNUSED, gpointer user_data)
{
  CalendarData *data = static_cast<CalendarData *>(user_data);
  if (data->selected_month == 0) {
    data->selected_month = 11;
    data->selected_year--;
  } else {
    data->selected_month--;
  }
  data->selected_day = 1;
  update_month_label (data);
  calendar_rebuild (data);
}

static void
on_next_month (GtkButton *b G_GNUC_UNUSED, gpointer user_data)
{
  CalendarData *data = static_cast<CalendarData *>(user_data);
  if (data->selected_month == 11) {
    data->selected_month = 0;
    data->selected_year++;
  } else {
    data->selected_month++;
  }
  data->selected_day = 1;
  update_month_label (data);
  calendar_rebuild (data);
}

static void
on_day_clicked (GtkButton *button, gpointer user_data)
{
  CalendarData *data = static_cast<CalendarData *>(user_data);
  int day = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "day"));
  int month = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "month"));
  int year = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "year"));

  data->selected_day = day;
  data->selected_month = month - 1;  // GDateTime month is 1-based
  data->selected_year = year;
  update_month_label (data);
  calendar_rebuild (data);
}

static void
calendar_rebuild (CalendarData *data)
{
  // Remove all day buttons (keep header rows: month header + weekday labels).
  GtkWidget *child = gtk_widget_get_first_child (data->grid);
  GtkWidget *next = NULL;
  for (child = gtk_widget_get_first_child (data->grid); child != NULL;
       child = next) {
    next = gtk_widget_get_next_sibling (child);
    int row = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (child), "row"));
    if (row >= 2) {
      gtk_grid_remove (GTK_GRID (data->grid), child);
    }
  }

  GDateTime *now = g_date_time_new_now_local ();
  int now_y = g_date_time_get_year (now);
  int now_m = g_date_time_get_month (now);
  int now_d = g_date_time_get_day_of_month (now);

  // First day of the selected month.
  GDateTime *first = g_date_time_new_local (data->selected_year,
                                            data->selected_month + 1, 1,
                                            0, 0, 0.0);
  int first_dow = g_date_time_get_day_of_week (first);  // 1=Mon..7=Sun

  // GNOME starts the grid at the beginning of the week containing the 1st
  // (week starts Sunday; convert dow so 0=Sunday).
  int sunday_dow = first_dow % 7;  // 0=Sunday
  // Begin date = first - sunday_dow days.
  GDateTime *iter = g_date_time_add_days (first, -sunday_dow);

  for (int row = 2; row < 8; row++) {
    for (int col = 0; col < 7; col++) {
      int y = g_date_time_get_year (iter);
      int m = g_date_time_get_month (iter);
      int d = g_date_time_get_day_of_month (iter);

      GtkWidget *button = gtk_button_new ();
      gchar label[8];
      snprintf (label, sizeof (label), "%d", d);
      gtk_button_set_label (GTK_BUTTON (button), label);

      gtk_widget_add_css_class (button, "calendar-day");
      if (m == data->selected_month + 1)
        gtk_widget_add_css_class (button, "calendar-weekday");
      else
        gtk_widget_add_css_class (button, "calendar-other-month");
      if (y == now_y && m == now_m && d == now_d)
        gtk_widget_add_css_class (button, "calendar-today");

      if (y == data->selected_year && m == data->selected_month + 1 &&
          d == data->selected_day)
        gtk_widget_add_css_class (button, "selected");

      g_object_set_data (G_OBJECT (button), "row", GINT_TO_POINTER (row));
      g_object_set_data (G_OBJECT (button), "day", GINT_TO_POINTER (d));
      g_object_set_data (G_OBJECT (button), "month", GINT_TO_POINTER (m));
      g_object_set_data (G_OBJECT (button), "year", GINT_TO_POINTER (y));
      g_signal_connect (button, "clicked", G_CALLBACK (on_day_clicked), data);
      gtk_widget_set_focus_on_click (button, FALSE);

      gtk_grid_attach (GTK_GRID (data->grid), button, col, row, 1, 1);

      GDateTime *next_day = g_date_time_add_days (iter, 1);
      g_date_time_unref (iter);
      iter = next_day;
    }
  }

  g_date_time_unref (iter);
  g_date_time_unref (first);
  g_date_time_unref (now);
}

GtkWidget *
winome_calendar_new (void)
{
  time_t now_ts = time (NULL);
  struct tm *now = localtime (&now_ts);

  CalendarData *data = g_new0 (CalendarData, 1);
  data->selected_year = now->tm_year + 1900;
  data->selected_month = now->tm_mon;
  data->selected_day = now->tm_mday;

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (box, "calendar");

  data->grid = gtk_grid_new ();
  gtk_widget_add_css_class (data->grid, "calendar-grid");
  gtk_box_append (GTK_BOX (box), data->grid);

  // Month header row: < back | label | forward >
  GtkWidget *top_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (top_box, "calendar-month-header");
  gtk_grid_attach (GTK_GRID (data->grid), top_box, 0, 0, 7, 1);
  g_object_set_data (G_OBJECT (top_box), "row", GINT_TO_POINTER (0));

  GtkWidget *back = gtk_button_new_from_icon_name ("pan-start-symbolic");
  gtk_widget_add_css_class (back, "calendar-change-month-back");
  gtk_widget_add_css_class (back, "pager-button");
  gtk_widget_set_focus_on_click (back, FALSE);
  g_signal_connect (back, "clicked", G_CALLBACK (on_prev_month), data);
  gtk_box_append (GTK_BOX (top_box), back);

  data->month_label = gtk_label_new ("");
  gtk_widget_add_css_class (data->month_label, "calendar-month-label");
  gtk_widget_set_hexpand (data->month_label, TRUE);
  gtk_box_append (GTK_BOX (top_box), data->month_label);

  GtkWidget *forward = gtk_button_new_from_icon_name ("pan-end-symbolic");
  gtk_widget_add_css_class (forward, "calendar-change-month-forward");
  gtk_widget_add_css_class (forward, "pager-button");
  gtk_widget_set_focus_on_click (forward, FALSE);
  g_signal_connect (forward, "clicked", G_CALLBACK (on_next_month), data);
  gtk_box_append (GTK_BOX (top_box), forward);

  // Weekday labels row (0=Sunday).
  for (int col = 0; col < 7; col++) {
    GtkWidget *heading = gtk_label_new (weekday_abbrev (col));
    gtk_widget_add_css_class (heading, "calendar-day-heading");
    gtk_grid_attach (GTK_GRID (data->grid), heading, col, 1, 1, 1);
    g_object_set_data (G_OBJECT (heading), "row", GINT_TO_POINTER (1));
  }

  update_month_label (data);
  calendar_rebuild (data);

  // Attach the CalendarData to the box so it survives.
  g_object_set_data_full (G_OBJECT (box), "calendar-data", data, g_free);

  return box;
}
