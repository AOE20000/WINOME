// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// GNOME-style calendar (matches js/ui/calendar.js Calendar widget).

#include "quick-settings.h"
#include "i18n.h"
#include "notifications.h"
#include "st-engine.h"
#include "system-status.h"

#include <windows.h>
#include <shellapi.h>

#include <time.h>
#include <string.h>
#include <ctime>
#include <string>
#include <vector>

// Convenience for the language check used throughout this file.
static bool
is_zh (void)
{
  return winome::is_zh ();
}

static int
lookup_cal_length (const char *parent_cls, const char *cls, const char *prop,
                   int fallback)
{
  winome::StEngine *e = winome::StEngine::get ();
  if (e == nullptr)
    return fallback;
  double v = 0.0;
  if (parent_cls != nullptr) {
    winome::StEngine::Node parent (*e, nullptr, "", parent_cls, "");
    winome::StEngine::Node node (*e, &parent, "", cls, "");
    if (node.lookup_length (prop, &v) && v > 0.0)
      return (int)(v + 0.5);
  } else {
    winome::StEngine::Node node (*e, nullptr, "", cls, "");
    if (node.lookup_length (prop, &v) && v > 0.0)
      return (int)(v + 0.5);
  }
  return fallback;
}

static int
cal_col_spacing (void)
{
  return lookup_cal_length (nullptr, "datemenu-calendar-column", "spacing", 6);
}

static int
cal_displays_spacing (void)
{
  return lookup_cal_length ("datemenu-calendar-column",
                            "datemenu-displays-box", "spacing", 6);
}

static int
cal_events_list_spacing (void)
{
  winome::StEngine *e = winome::StEngine::get ();
  if (e == nullptr)
    return 6;
  winome::StEngine::Node btn (*e, nullptr, "", "events-button", "");
  winome::StEngine::Node box (*e, &btn, "", "events-box", "");
  winome::StEngine::Node list (*e, &box, "", "events-list", "");
  double v = 0.0;
  if (list.lookup_length ("spacing", &v) && v > 0.0)
    return (int)(v + 0.5);
  return 6;
}

static int
cal_msg_controls_spacing (void)
{
  return lookup_cal_length (nullptr, "message-list-controls", "spacing", 6);
}

static int
cal_msg_header_spacing (void)
{
  return lookup_cal_length ("message", "message-header", "spacing", 6);
}

static int
cal_msg_source_icon_size (void)
{
  winome::StEngine *e = winome::StEngine::get ();
  if (e == nullptr)
    return 16;
  winome::StEngine::Node msg (*e, nullptr, "", "message", "");
  winome::StEngine::Node hdr (*e, &msg, "", "message-header", "");
  winome::StEngine::Node icon (*e, &hdr, "", "message-source-icon", "");
  double v = 0.0;
  if (icon.lookup_length ("icon-size", &v) && v > 0.0)
    return (int)(v + 0.5);
  return 16;
}

static int
cal_msg_box_spacing (void)
{
  return lookup_cal_length ("message", "message-box", "spacing", 6);
}

static int
cal_msg_content_spacing (void)
{
  winome::StEngine *e = winome::StEngine::get ();
  if (e == nullptr)
    return 4;
  winome::StEngine::Node msg (*e, nullptr, "", "message", "");
  winome::StEngine::Node box (*e, &msg, "", "message-box", "");
  winome::StEngine::Node content (*e, &box, "", "message-content", "");
  double v = 0.0;
  if (content.lookup_length ("spacing", &v) && v > 0.0)
    return (int)(v + 0.5);
  return 4;
}

static int
cal_media_icon_size (void)
{
  return lookup_cal_length ("media-message", "message-icon message-themed-icon",
                            "icon-size", 32);
}

// Calendar state.
typedef struct {
  int selected_year;
  int selected_month;   // 0..11
  int selected_day;

  GtkWidget *grid;
  GtkWidget *month_label;
  GtkWidget *day_header_label;
  GtkWidget *date_header_label;
  GtkWidget *dnd_switch;
  GtkWidget *utc_time_label;

  GtkWidget *message_scroll;
  GtkWidget *message_view;
  GtkWidget *placeholder;
  GtkWidget *clear_btn;
  uint64_t last_notif_gen;
} CalendarData;

static void calendar_rebuild (CalendarData *data);

// Weekday abbreviation for a day index (0=Sunday). Matches
// _getCalendarDayAbbreviation in calendar.js.
static const char *
weekday_abbrev (int day)
{
  static const char *zh[] = {"日", "一", "二", "三", "四", "五", "六"};
  static const char *en[] = {"S", "M", "T", "W", "T", "F", "S"};
  return is_zh () ? zh[day] : en[day];
}

static const char *
month_name (int month)
{
  static const char *zh[] = {"一月", "二月", "三月", "四月", "五月", "六月",
                             "七月", "八月", "九月", "十月", "十一月", "十二月"};
  static const char *en[] = {"January", "February", "March", "April", "May",
                             "June", "July", "August", "September", "October",
                             "November", "December"};
  return is_zh () ? zh[month] : en[month];
}

static void
update_month_label (CalendarData *data)
{
  gchar text[64];
  if (is_zh ())
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
on_today_button_clicked (GtkButton *b G_GNUC_UNUSED, gpointer user_data)
{
  CalendarData *data = static_cast<CalendarData *>(user_data);
  GDateTime *now = g_date_time_new_now_local ();
  data->selected_year = g_date_time_get_year (now);
  data->selected_month = g_date_time_get_month (now) - 1;
  data->selected_day = g_date_time_get_day_of_month (now);
  g_date_time_unref (now);
  update_month_label (data);
  calendar_rebuild (data);
}

static void
on_events_or_clocks_clicked (GtkButton *b G_GNUC_UNUSED,
                             gpointer G_GNUC_UNUSED user_data)
{
  ShellExecuteW (nullptr, L"open", L"ms-clock:", nullptr, nullptr,
                 SW_SHOWNORMAL);
}

static gboolean
on_datemenu_dnd_state_set (GtkSwitch *sw G_GNUC_UNUSED,
                           gboolean state,
                           gpointer G_GNUC_UNUSED user_data)
{
  winome::set_do_not_disturb (state != FALSE);
  return FALSE;
}

static void
on_media_prev_clicked (GtkButton *b G_GNUC_UNUSED,
                       gpointer G_GNUC_UNUSED user_data)
{
  winome::notifications_media_action (winome::WinomeMediaAction::Previous);
}

static void
on_media_play_pause_clicked (GtkButton *b G_GNUC_UNUSED,
                             gpointer G_GNUC_UNUSED user_data)
{
  winome::notifications_media_action (winome::WinomeMediaAction::PlayPause);
}

static void
on_media_next_clicked (GtkButton *b G_GNUC_UNUSED,
                       gpointer G_GNUC_UNUSED user_data)
{
  winome::notifications_media_action (winome::WinomeMediaAction::Next);
}

static void
on_notif_close_clicked (GtkButton *btn, gpointer G_GNUC_UNUSED user_data)
{
  uint64_t *key = static_cast<uint64_t *> (
      g_object_get_data (G_OBJECT (btn), "notif-key"));
  if (key != nullptr)
    winome::notifications_dismiss (*key);
}

static void
on_clear_notifs_clicked (GtkButton *b G_GNUC_UNUSED,
                         gpointer G_GNUC_UNUSED user_data)
{
  winome::notifications_clear_all ();
}

static void
free_notif_key (gpointer data)
{
  delete static_cast<uint64_t *> (data);
}

static GtkWidget *
create_media_card (const winome::WinomeMediaInfo &media)
{
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class (card, "message");
  gtk_widget_add_css_class (card, "media-message");

  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, cal_msg_box_spacing ());
  gtk_widget_add_css_class (row, "message-box");

  GtkWidget *art = gtk_image_new_from_icon_name ("audio-x-generic-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (art), cal_media_icon_size ());
  gtk_widget_add_css_class (art, "message-icon");
  gtk_widget_add_css_class (art, "message-themed-icon");
  gtk_box_append (GTK_BOX (row), art);

  GtkWidget *info = gtk_box_new (GTK_ORIENTATION_VERTICAL, cal_msg_content_spacing ());
  gtk_widget_add_css_class (info, "message-content");
  gtk_widget_set_hexpand (info, TRUE);
  gtk_widget_set_valign (info, GTK_ALIGN_CENTER);

  const char *title_text = media.title.empty ()
      ? (is_zh () ? "正在播放媒体" : "Media Playback")
      : media.title.c_str ();
  GtkWidget *title = gtk_label_new (title_text);
  gtk_widget_add_css_class (title, "message-title");
  gtk_label_set_xalign (GTK_LABEL (title), 0.0f);
  gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (info), title);

  std::string sub = media.artist;
  if (!media.album.empty ()) {
    if (!sub.empty ())
      sub += " — ";
    sub += media.album;
  }
  if (!sub.empty ()) {
    GtkWidget *artist = gtk_label_new (sub.c_str ());
    gtk_label_set_xalign (GTK_LABEL (artist), 0.0f);
    gtk_label_set_ellipsize (GTK_LABEL (artist), PANGO_ELLIPSIZE_END);
    gtk_box_append (GTK_BOX (info), artist);
  }
  gtk_box_append (GTK_BOX (row), info);

  GtkWidget *btns = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_valign (btns, GTK_ALIGN_CENTER);

  GtkWidget *prev = gtk_button_new_from_icon_name ("media-skip-backward-symbolic");
  gtk_widget_add_css_class (prev, "message-media-control");
  gtk_widget_set_focus_on_click (prev, FALSE);
  g_signal_connect (prev, "clicked", G_CALLBACK (on_media_prev_clicked), nullptr);
  gtk_box_append (GTK_BOX (btns), prev);

  GtkWidget *play = gtk_button_new_from_icon_name (
      media.playing ? "media-playback-pause-symbolic"
                    : "media-playback-start-symbolic");
  gtk_widget_add_css_class (play, "message-media-control");
  gtk_widget_set_focus_on_click (play, FALSE);
  g_signal_connect (play, "clicked",
                    G_CALLBACK (on_media_play_pause_clicked), nullptr);
  gtk_box_append (GTK_BOX (btns), play);

  GtkWidget *next = gtk_button_new_from_icon_name ("media-skip-forward-symbolic");
  gtk_widget_add_css_class (next, "message-media-control");
  gtk_widget_set_focus_on_click (next, FALSE);
  g_signal_connect (next, "clicked", G_CALLBACK (on_media_next_clicked), nullptr);
  gtk_box_append (GTK_BOX (btns), next);

  gtk_box_append (GTK_BOX (row), btns);
  gtk_box_append (GTK_BOX (card), row);
  return card;
}

static GtkWidget *
create_icon_button (const char *icon_name,
                    int pixel_size,
                    const char *cls1,
                    const char *cls2,
                    const char *cls3 = nullptr,
                    const char *cls4 = nullptr)
{
  GtkWidget *btn = gtk_button_new ();
  GtkWidget *img = gtk_image_new_from_icon_name (icon_name);
  if (pixel_size > 0)
    gtk_image_set_pixel_size (GTK_IMAGE (img), pixel_size);
  gtk_button_set_child (GTK_BUTTON (btn), img);
  const char *classes[4] = {cls1, cls2, cls3, cls4};
  for (const char *cls : classes) {
    if (cls != nullptr && cls[0] != '\0')
      gtk_widget_add_css_class (btn, cls);
  }
  gtk_widget_set_halign (btn, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (btn, GTK_ALIGN_CENTER);
  gtk_widget_set_focus_on_click (btn, FALSE);
  return btn;
}

static void
on_notif_expand_clicked (GtkButton *btn, gpointer G_GNUC_UNUSED user_data)
{
  GtkLabel *body = GTK_LABEL (g_object_get_data (G_OBJECT (btn), "body-label"));
  if (body == nullptr)
    return;
  const bool collapsed = gtk_label_get_lines (body) >= 0;
  gtk_label_set_lines (body, collapsed ? -1 : 3);
  gtk_label_set_ellipsize (body, collapsed ? PANGO_ELLIPSIZE_NONE
                                          : PANGO_ELLIPSIZE_END);
  if (collapsed)
    gtk_widget_add_css_class (GTK_WIDGET (btn), "expanded");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (btn), "expanded");
}

static GtkWidget *
create_notification_card (const winome::ShellNotification &notif,
                          int64_t now_unix)
{
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_add_css_class (card, "message");

  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, cal_msg_header_spacing ());
  gtk_widget_add_css_class (header, "message-header");

  GtkWidget *src_icon = gtk_image_new_from_icon_name ("dialog-information-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (src_icon), cal_msg_source_icon_size ());
  gtk_widget_add_css_class (src_icon, "message-source-icon");
  gtk_box_append (GTK_BOX (header), src_icon);

  GtkWidget *hc = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class (hc, "message-header-content");
  gtk_widget_set_hexpand (hc, TRUE);

  GtkWidget *app_lbl = gtk_label_new (
      notif.app_name.empty () ? "Windows" : notif.app_name.c_str ());
  gtk_widget_add_css_class (app_lbl, "message-source-title");
  gtk_label_set_xalign (GTK_LABEL (app_lbl), 0.0f);
  gtk_label_set_ellipsize (GTK_LABEL (app_lbl), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (hc), app_lbl);

  std::string age = winome::format_notification_age (
      notif.timestamp_unix, now_unix, is_zh ());
  GtkWidget *time_lbl = gtk_label_new (age.c_str ());
  gtk_widget_add_css_class (time_lbl, "event-time");
  gtk_box_append (GTK_BOX (hc), time_lbl);
  gtk_box_append (GTK_BOX (header), hc);

  GtkWidget *body = nullptr;
  if (!notif.body.empty ()) {
    body = gtk_label_new (notif.body.c_str ());
    gtk_widget_add_css_class (body, "message-body");
    gtk_label_set_xalign (GTK_LABEL (body), 0.0f);
    gtk_label_set_wrap (GTK_LABEL (body), TRUE);
    gtk_label_set_wrap_mode (GTK_LABEL (body), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines (GTK_LABEL (body), 3);
    gtk_label_set_ellipsize (GTK_LABEL (body), PANGO_ELLIPSIZE_END);
  }

  const bool needs_expand =
      body != nullptr &&
      (notif.body.size () > 110 || notif.body.find ('\n') != std::string::npos);
  if (needs_expand) {
    GtkWidget *expand_btn = create_icon_button (
        "notification-expand-symbolic", 16,
        "message-expand-button", "icon-button");
    g_object_set_data (G_OBJECT (expand_btn), "body-label", body);
    g_signal_connect (expand_btn, "clicked",
                      G_CALLBACK (on_notif_expand_clicked), nullptr);
    gtk_box_append (GTK_BOX (header), expand_btn);
  }

  GtkWidget *close_btn = create_icon_button (
      "window-close-symbolic", 16,
      "message-close-button", "icon-button");
  g_object_set_data_full (G_OBJECT (close_btn), "notif-key",
                          new uint64_t (notif.key), free_notif_key);
  g_signal_connect (close_btn, "clicked",
                    G_CALLBACK (on_notif_close_clicked), nullptr);
  gtk_box_append (GTK_BOX (header), close_btn);
  gtk_box_append (GTK_BOX (card), header);

  GtkWidget *body_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, cal_msg_box_spacing ());
  gtk_widget_add_css_class (body_row, "message-box");

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, cal_msg_content_spacing ());
  gtk_widget_add_css_class (content, "message-content");
  gtk_widget_set_hexpand (content, TRUE);

  if (!notif.title.empty ()) {
    GtkWidget *title = gtk_label_new (notif.title.c_str ());
    gtk_widget_add_css_class (title, "message-title");
    gtk_label_set_xalign (GTK_LABEL (title), 0.0f);
    gtk_label_set_wrap (GTK_LABEL (title), TRUE);
    gtk_label_set_wrap_mode (GTK_LABEL (title), PANGO_WRAP_WORD_CHAR);
    gtk_box_append (GTK_BOX (content), title);
  }
  if (body != nullptr)
    gtk_box_append (GTK_BOX (content), body);

  gtk_box_append (GTK_BOX (body_row), content);
  gtk_box_append (GTK_BOX (card), body_row);
  return card;
}

static void
rebuild_message_list (CalendarData *data, bool force = false)
{
  if (data == nullptr || data->message_view == nullptr)
    return;

  const uint64_t gen = winome::notifications_generation ();
  if (!force && data->last_notif_gen == gen)
    return;
  data->last_notif_gen = gen;

  while (GtkWidget *child = gtk_widget_get_first_child (data->message_view))
    gtk_box_remove (GTK_BOX (data->message_view), child);

  const winome::WinomeMediaInfo media = winome::notifications_get_media ();
  const auto notifs = winome::notifications_get_all ();
  const int64_t now_unix = static_cast<int64_t> (std::time (nullptr));

  if (media.active)
    gtk_box_append (GTK_BOX (data->message_view), create_media_card (media));

  for (const auto &notif : notifs) {
    gtk_box_append (GTK_BOX (data->message_view),
                    create_notification_card (notif, now_unix));
  }

  const bool has_content = media.active || !notifs.empty ();
  if (data->message_scroll != nullptr)
    gtk_widget_set_visible (data->message_scroll, has_content ? TRUE : FALSE);
  if (data->placeholder != nullptr)
    gtk_widget_set_visible (data->placeholder, has_content ? FALSE : TRUE);
  if (data->clear_btn != nullptr) {
    gtk_widget_set_visible (data->clear_btn, has_content ? TRUE : FALSE);
    gtk_widget_set_sensitive (data->clear_btn, !notifs.empty () ? TRUE : FALSE);
  }
}

static void
on_calendar_notifications_changed (void *user_data)
{
  GtkWidget *cal = GTK_WIDGET (user_data);
  if (cal == nullptr)
    return;
  CalendarData *data = static_cast<CalendarData *> (
      g_object_get_data (G_OBJECT (cal), "calendar-data"));
  rebuild_message_list (data, false);
}

static void
on_calendar_destroy (GtkWidget *cal, gpointer G_GNUC_UNUSED user_data)
{
  winome::notifications_remove_listener (on_calendar_notifications_changed, cal);
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
      gtk_widget_set_halign (button, GTK_ALIGN_CENTER);
      gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
      if (col == 0 || col == 6)
        gtk_widget_add_css_class (button, "calendar-weekend");
      if (m == data->selected_month + 1)
        gtk_widget_add_css_class (button, "calendar-weekday");
      else
        gtk_widget_add_css_class (button, "calendar-other-month");
      if (y == now_y && m == now_m && d == now_d)
        gtk_widget_add_css_class (button, "calendar-today");

      if (y == data->selected_year && m == data->selected_month + 1 &&
          d == data->selected_day) {
        gtk_widget_add_css_class (button, "selected");
        gtk_widget_set_state_flags (button, GTK_STATE_FLAG_SELECTED, FALSE);
      } else {
        gtk_widget_unset_state_flags (button, GTK_STATE_FLAG_SELECTED);
      }

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

  GtkWidget *back = create_icon_button (
      "pan-start-symbolic", -1,
      "calendar-change-month-back", "pager-button", "icon-button", "flat");
  g_signal_connect (back, "clicked", G_CALLBACK (on_prev_month), data);
  gtk_box_append (GTK_BOX (top_box), back);

  data->month_label = gtk_label_new ("");
  gtk_widget_add_css_class (data->month_label, "calendar-month-label");
  gtk_label_set_justify (GTK_LABEL (data->month_label), GTK_JUSTIFY_CENTER);
  gtk_widget_set_hexpand (data->month_label, TRUE);
  gtk_box_append (GTK_BOX (top_box), data->month_label);

  GtkWidget *forward = create_icon_button (
      "pan-end-symbolic", -1,
      "calendar-change-month-forward", "pager-button", "icon-button", "flat");
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

  GtkWidget *root = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_name (root, "calendarArea");

  // --- Left column: notifications / message list & DND controls ---
  GtkWidget *msg_list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (msg_list, "message-list");
  gtk_widget_set_valign (msg_list, GTK_ALIGN_FILL);

  GtkWidget *msg_view = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class (msg_view, "message-view");
  data->message_view = msg_view;

  GtkWidget *msg_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (msg_scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height (
      GTK_SCROLLED_WINDOW (msg_scroll), TRUE);
  gtk_scrolled_window_set_max_content_height (
      GTK_SCROLLED_WINDOW (msg_scroll), 360);
  gtk_widget_set_vexpand (msg_scroll, TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (msg_scroll), msg_view);
  gtk_widget_set_visible (msg_scroll, FALSE);
  data->message_scroll = msg_scroll;
  gtk_box_append (GTK_BOX (msg_list), msg_scroll);

  GtkWidget *placeholder = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (placeholder, "message-list-placeholder");
  gtk_widget_set_vexpand (placeholder, TRUE);
  gtk_widget_set_valign (placeholder, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (placeholder, GTK_ALIGN_CENTER);
  data->placeholder = placeholder;

  GtkWidget *ph_icon = gtk_image_new_from_icon_name ("no-notifications-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (ph_icon), 96);
  gtk_box_append (GTK_BOX (placeholder), ph_icon);

  GtkWidget *ph_label = gtk_label_new (is_zh () ? "没有通知" : "No Notifications");
  gtk_box_append (GTK_BOX (placeholder), ph_label);
  gtk_box_append (GTK_BOX (msg_list), placeholder);

  GtkWidget *controls = gtk_box_new (GTK_ORIENTATION_HORIZONTAL,
                                   cal_msg_controls_spacing ());
  gtk_widget_add_css_class (controls, "message-list-controls");

  GtkWidget *dnd_label = gtk_label_new (is_zh () ? "勿扰模式" : "Do Not Disturb");
  gtk_box_append (GTK_BOX (controls), dnd_label);

  data->dnd_switch = gtk_switch_new ();
  gtk_widget_add_css_class (data->dnd_switch, "toggle-switch");
  gtk_widget_set_focus_on_click (data->dnd_switch, FALSE);
  gtk_widget_set_valign (data->dnd_switch, GTK_ALIGN_CENTER);
  g_signal_connect (data->dnd_switch, "state-set",
                    G_CALLBACK (on_datemenu_dnd_state_set), NULL);
  gtk_box_append (GTK_BOX (controls), data->dnd_switch);

  GtkWidget *ctrl_spacer = gtk_label_new ("");
  gtk_widget_set_hexpand (ctrl_spacer, TRUE);
  gtk_box_append (GTK_BOX (controls), ctrl_spacer);

  GtkWidget *clear_btn = gtk_button_new_with_label (is_zh () ? "清除" : "Clear");
  gtk_widget_add_css_class (clear_btn, "message-list-clear-button");
  gtk_widget_add_css_class (clear_btn, "button");
  gtk_widget_set_focus_on_click (clear_btn, FALSE);
  gtk_widget_set_visible (clear_btn, FALSE);
  gtk_widget_set_sensitive (clear_btn, FALSE);
  g_signal_connect (clear_btn, "clicked",
                    G_CALLBACK (on_clear_notifs_clicked), NULL);
  data->clear_btn = clear_btn;
  gtk_box_append (GTK_BOX (controls), clear_btn);
  gtk_box_append (GTK_BOX (msg_list), controls);

  gtk_box_append (GTK_BOX (root), msg_list);

  // --- Right column: today header, calendar grid, events & world clocks ---
  GtkWidget *cal_col = gtk_box_new (GTK_ORIENTATION_VERTICAL,
                                  cal_col_spacing ());
  gtk_widget_add_css_class (cal_col, "datemenu-calendar-column");

  GtkWidget *today_btn = gtk_button_new ();
  gtk_widget_add_css_class (today_btn, "datemenu-today-button");
  gtk_widget_set_focus_on_click (today_btn, FALSE);
  g_signal_connect (today_btn, "clicked",
                    G_CALLBACK (on_today_button_clicked), data);

  GtkWidget *today_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  data->day_header_label = gtk_label_new ("");
  gtk_widget_add_css_class (data->day_header_label, "day-label");
  gtk_widget_set_halign (data->day_header_label, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (today_box), data->day_header_label);

  data->date_header_label = gtk_label_new ("");
  gtk_widget_add_css_class (data->date_header_label, "date-label");
  gtk_widget_set_halign (data->date_header_label, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (today_box), data->date_header_label);
  gtk_button_set_child (GTK_BUTTON (today_btn), today_box);
  gtk_box_append (GTK_BOX (cal_col), today_btn);

  gtk_box_append (GTK_BOX (cal_col), box);

  GtkWidget *displays_box = gtk_box_new (GTK_ORIENTATION_VERTICAL,
                                       cal_displays_spacing ());
  gtk_widget_add_css_class (displays_box, "datemenu-displays-box");

  GtkWidget *events_btn = gtk_button_new ();
  gtk_widget_add_css_class (events_btn, "events-button");
  gtk_widget_set_focus_on_click (events_btn, FALSE);
  g_signal_connect (events_btn, "clicked",
                    G_CALLBACK (on_events_or_clocks_clicked), NULL);

  GtkWidget *events_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class (events_box, "events-box");
  GtkWidget *events_title = gtk_label_new (is_zh () ? "今天" : "Today");
  gtk_widget_add_css_class (events_title, "events-title");
  gtk_widget_set_halign (events_title, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (events_box), events_title);

  GtkWidget *events_list = gtk_box_new (GTK_ORIENTATION_VERTICAL,
                                      cal_events_list_spacing ());
  gtk_widget_add_css_class (events_list, "events-list");
  GtkWidget *events_ph = gtk_label_new (is_zh () ? "无事件" : "No Events");
  gtk_widget_add_css_class (events_ph, "event-placeholder");
  gtk_widget_set_halign (events_ph, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (events_list), events_ph);
  gtk_box_append (GTK_BOX (events_box), events_list);
  gtk_button_set_child (GTK_BUTTON (events_btn), events_box);
  gtk_box_append (GTK_BOX (displays_box), events_btn);

  GtkWidget *clocks_btn = gtk_button_new ();
  gtk_widget_add_css_class (clocks_btn, "world-clocks-button");
  gtk_widget_set_focus_on_click (clocks_btn, FALSE);
  g_signal_connect (clocks_btn, "clicked",
                    G_CALLBACK (on_events_or_clocks_clicked), NULL);

  GtkWidget *clocks_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *clocks_header = gtk_label_new (is_zh () ? "世界时钟" : "World Clocks");
  gtk_widget_add_css_class (clocks_header, "world-clocks-header");
  gtk_widget_set_halign (clocks_header, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (clocks_box), clocks_header);

  GtkWidget *clocks_grid = gtk_grid_new ();
  gtk_widget_add_css_class (clocks_grid, "world-clocks-grid");
  gtk_grid_set_column_spacing (
      GTK_GRID (clocks_grid),
      lookup_cal_length ("world-clocks-button", "world-clocks-grid",
                         "spacing-columns", 12));
  gtk_grid_set_row_spacing (
      GTK_GRID (clocks_grid),
      lookup_cal_length ("world-clocks-button", "world-clocks-grid",
                         "spacing-rows", 6));

  GtkWidget *utc_tz = gtk_label_new ("UTC");
  gtk_widget_add_css_class (utc_tz, "world-clocks-timezone");
  gtk_widget_set_halign (utc_tz, GTK_ALIGN_START);
  gtk_widget_set_hexpand (utc_tz, TRUE);
  gtk_grid_attach (GTK_GRID (clocks_grid), utc_tz, 0, 0, 1, 1);

  data->utc_time_label = gtk_label_new ("00:00");
  gtk_widget_add_css_class (data->utc_time_label, "world-clocks-time");
  gtk_widget_set_halign (data->utc_time_label, GTK_ALIGN_END);
  gtk_grid_attach (GTK_GRID (clocks_grid), data->utc_time_label, 1, 0, 1, 1);

  gtk_box_append (GTK_BOX (clocks_box), clocks_grid);
  gtk_button_set_child (GTK_BUTTON (clocks_btn), clocks_box);
  gtk_box_append (GTK_BOX (displays_box), clocks_btn);

  gtk_box_append (GTK_BOX (cal_col), displays_box);
  gtk_box_append (GTK_BOX (root), cal_col);

  // Attach the CalendarData to the root widget so its lifetime matches.
  g_object_set_data_full (G_OBJECT (root), "calendar-data", data, g_free);
  winome::notifications_add_listener (on_calendar_notifications_changed, root);
  g_signal_connect (root, "destroy", G_CALLBACK (on_calendar_destroy), NULL);

  rebuild_message_list (data, true);
  winome_calendar_refresh (root);

  return root;
}

void
winome_calendar_refresh (GtkWidget *calendar)
{
  if (calendar == nullptr)
    return;
  CalendarData *data = static_cast<CalendarData *> (
      g_object_get_data (G_OBJECT (calendar), "calendar-data"));
  if (data == nullptr)
    return;

  GDateTime *now = g_date_time_new_now_local ();
  if (now != nullptr) {
    int dow = g_date_time_get_day_of_week (now); // 1=Mon..7=Sun
    static const char *zh_days[] = {"星期一", "星期二", "星期三", "星期四",
                                    "星期五", "星期六", "星期日"};
    static const char *en_days[] = {"Monday", "Tuesday", "Wednesday",
                                    "Thursday", "Friday", "Saturday", "Sunday"};
    int idx = (dow >= 1 && dow <= 7) ? (dow - 1) : 0;

    if (data->day_header_label != nullptr)
      gtk_label_set_text (GTK_LABEL (data->day_header_label),
                          is_zh () ? zh_days[idx] : en_days[idx]);

    if (data->date_header_label != nullptr) {
      int y = g_date_time_get_year (now);
      int m = g_date_time_get_month (now);
      int d = g_date_time_get_day_of_month (now);
      gchar date_buf[64];
      if (is_zh ()) {
        snprintf (date_buf, sizeof (date_buf), "%d年%d月%d日", y, m, d);
      } else {
        int m_idx = (m >= 1 && m <= 12) ? (m - 1) : 0;
        snprintf (date_buf, sizeof (date_buf), "%s %d, %d",
                  month_name (m_idx), d, y);
      }
      gtk_label_set_text (GTK_LABEL (data->date_header_label), date_buf);
    }
    g_date_time_unref (now);
  }

  if (data->utc_time_label != nullptr) {
    GDateTime *utc = g_date_time_new_now_utc ();
    if (utc != nullptr) {
      gchar utc_buf[16];
      snprintf (utc_buf, sizeof (utc_buf), "%02d:%02d",
                g_date_time_get_hour (utc), g_date_time_get_minute (utc));
      gtk_label_set_text (GTK_LABEL (data->utc_time_label), utc_buf);
      g_date_time_unref (utc);
    }
  }

  if (data->dnd_switch != nullptr) {
    bool dnd_on = false;
    if (winome::get_do_not_disturb (&dnd_on)) {
      gboolean want = dnd_on ? TRUE : FALSE;
      if (gtk_switch_get_active (GTK_SWITCH (data->dnd_switch)) != want) {
        g_signal_handlers_block_by_func (data->dnd_switch,
            (gpointer) on_datemenu_dnd_state_set, NULL);
        gtk_switch_set_active (GTK_SWITCH (data->dnd_switch), want);
        gtk_switch_set_state (GTK_SWITCH (data->dnd_switch), want);
        g_signal_handlers_unblock_by_func (data->dnd_switch,
            (gpointer) on_datemenu_dnd_state_set, NULL);
      }
    }
  }

  rebuild_message_list (data, false);
}
