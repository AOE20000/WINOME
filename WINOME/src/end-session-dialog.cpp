// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// GNOME-style 60s End Session confirmation modal dialog (Restart/Power Off/Log Out).

#include "end-session-dialog.h"

#include "fonts.h"
#include "i18n.h"

#include <gtk/gtk.h>
#include <gdk/win32/gdkwin32.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace winome {
namespace {

constexpr int kDefaultCountdownSeconds = 60;

struct EndSessionState {
  GtkWidget* window = nullptr;
  GtkWidget* desc_label = nullptr;
  GtkWidget* confirm_btn = nullptr;
  guint timer_id = 0;
  int seconds_left = kDefaultCountdownSeconds;
  EndSessionAction action = EndSessionAction::PowerOff;
  EndSessionConfirmCallback on_confirm = nullptr;
  void* user_data = nullptr;
  bool completed = false;
};

EndSessionState g_dlg;

const char* action_icon_name(EndSessionAction action) {
  switch (action) {
    case EndSessionAction::Restart:  return "system-reboot-symbolic";
    case EndSessionAction::PowerOff: return "system-shutdown-symbolic";
    case EndSessionAction::LogOut:   return "system-log-out-symbolic";
  }
  return "system-shutdown-symbolic";
}

void center_modal_on_workarea(GtkWidget* dlg, int dlg_w, int dlg_h) {
  GdkSurface* surf = gtk_native_get_surface(GTK_NATIVE(dlg));
  if (surf == nullptr)
    return;
  HWND hwnd = static_cast<HWND>(gdk_win32_surface_get_handle(surf));
  if (hwnd == nullptr)
    return;

  LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                    (ex | WS_EX_TOOLWINDOW | WS_EX_TOPMOST) & ~WS_EX_APPWINDOW);

  RECT wa{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
  RECT wr{};
  GetWindowRect(hwnd, &wr);
  const int scale = std::max(1, gtk_widget_get_scale_factor(dlg));
  const int w = (wr.right - wr.left) > 32 ? (wr.right - wr.left) : dlg_w * scale;
  const int h = (wr.bottom - wr.top) > 32 ? (wr.bottom - wr.top) : dlg_h * scale;
  const int x = wa.left + ((wa.right - wa.left) - w) / 2;
  const int y = wa.top + ((wa.bottom - wa.top) - h) / 2;
  SetWindowPos(hwnd, HWND_TOPMOST, x, y, 0, 0,
               SWP_NOSIZE | SWP_SHOWWINDOW);
}

void finish_confirm_and_close() {
  if (g_dlg.completed)
    return;
  g_dlg.completed = true;
  const EndSessionAction action = g_dlg.action;
  const EndSessionConfirmCallback cb = g_dlg.on_confirm;
  void* ud = g_dlg.user_data;
  close_end_session_dialog();
  if (cb != nullptr)
    cb(action, ud);
}

gboolean on_countdown_tick(gpointer user_data) {
  (void)user_data;
  if (g_dlg.window == nullptr) {
    g_dlg.timer_id = 0;
    return G_SOURCE_REMOVE;
  }
  if (g_dlg.seconds_left > 0)
    --g_dlg.seconds_left;
  if (g_dlg.desc_label != nullptr) {
    const std::string text = end_session_format_description(
        g_dlg.action, g_dlg.seconds_left, is_zh());
    gtk_label_set_text(GTK_LABEL(g_dlg.desc_label), text.c_str());
  }
  if (g_dlg.seconds_left <= 0) {
    g_dlg.timer_id = 0;
    finish_confirm_and_close();
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

void on_cancel_clicked(GtkButton* btn G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  close_end_session_dialog();
}

void on_confirm_clicked(GtkButton* btn G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  finish_confirm_and_close();
}

gboolean on_dialog_key_pressed(GtkEventControllerKey* ctrl G_GNUC_UNUSED,
                               guint keyval,
                               guint keycode G_GNUC_UNUSED,
                               GdkModifierType state G_GNUC_UNUSED,
                               gpointer ud G_GNUC_UNUSED) {
  if (keyval == GDK_KEY_Escape) {
    close_end_session_dialog();
    return TRUE;
  }
  if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
    finish_confirm_and_close();
    return TRUE;
  }
  return FALSE;
}

void on_dialog_destroy(GtkWidget* widget G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  if (g_dlg.timer_id != 0) {
    g_source_remove(g_dlg.timer_id);
    g_dlg.timer_id = 0;
  }
  g_dlg.window = nullptr;
  g_dlg.desc_label = nullptr;
  g_dlg.confirm_btn = nullptr;
  g_dlg.on_confirm = nullptr;
  g_dlg.user_data = nullptr;
}

}  // namespace

std::string end_session_format_title(EndSessionAction action, bool zh) {
  switch (action) {
    case EndSessionAction::Restart:
      return zh ? "重启" : "Restart";
    case EndSessionAction::PowerOff:
      return zh ? "关机" : "Power Off";
    case EndSessionAction::LogOut:
      return zh ? "注销" : "Log Out";
  }
  return zh ? "关机" : "Power Off";
}

std::string end_session_format_description(EndSessionAction action,
                                           int seconds_left,
                                           bool zh) {
  const int sec = std::max(0, seconds_left);
  char buf[192];
  if (zh) {
    switch (action) {
      case EndSessionAction::Restart:
        std::snprintf(buf, sizeof(buf),
                      "系统将在 %d 秒后自动重启。", sec);
        break;
      case EndSessionAction::PowerOff:
        std::snprintf(buf, sizeof(buf),
                      "系统将在 %d 秒后自动关机。", sec);
        break;
      case EndSessionAction::LogOut:
        std::snprintf(buf, sizeof(buf),
                      "您将在 %d 秒后自动注销当前会话。", sec);
        break;
    }
    return buf;
  }

  const char* unit = (sec == 1) ? "second" : "seconds";
  switch (action) {
    case EndSessionAction::Restart:
      std::snprintf(buf, sizeof(buf),
                    "The system will restart automatically in %d %s.",
                    sec, unit);
      break;
    case EndSessionAction::PowerOff:
      std::snprintf(buf, sizeof(buf),
                    "The system will power off automatically in %d %s.",
                    sec, unit);
      break;
    case EndSessionAction::LogOut:
      std::snprintf(buf, sizeof(buf),
                    "You will be logged out automatically in %d %s.",
                    sec, unit);
      break;
  }
  return buf;
}

std::string end_session_confirm_label(EndSessionAction action, bool zh) {
  return end_session_format_title(action, zh);
}

void show_end_session_dialog(EndSessionAction action,
                             EndSessionConfirmCallback on_confirm,
                             void* user_data) {
  close_end_session_dialog();

  const bool zh = is_zh();
  g_dlg.action = action;
  g_dlg.on_confirm = on_confirm;
  g_dlg.user_data = user_data;
  g_dlg.seconds_left = kDefaultCountdownSeconds;
  g_dlg.completed = false;

  GtkWidget* dlg = gtk_window_new();
  g_dlg.window = dlg;
  gtk_widget_set_name(dlg, "winomeModalDialogWindow");
  gtk_window_set_decorated(GTK_WINDOW(dlg), FALSE);
  gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
  gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
  const std::string title = end_session_format_title(action, zh);
  gtk_window_set_title(GTK_WINDOW(dlg), title.c_str());
  install_bundled_fonts(dlg);

  GtkWidget* wrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_start(wrap, 16);
  gtk_widget_set_margin_end(wrap, 16);
  gtk_widget_set_margin_top(wrap, 16);
  gtk_widget_set_margin_bottom(wrap, 16);

  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(box, "modal-dialog");
  gtk_widget_add_css_class(box, "end-session-dialog");
  gtk_widget_set_size_request(box, 360, -1);

  GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class(content, "modal-dialog-content-box");

  GtkWidget* msg_content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_add_css_class(msg_content, "message-dialog-content");

  GtkWidget* icon = gtk_image_new_from_icon_name(action_icon_name(action));
  gtk_image_set_pixel_size(GTK_IMAGE(icon), 48);
  gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(msg_content), icon);

  GtkWidget* title_lbl = gtk_label_new(title.c_str());
  gtk_widget_add_css_class(title_lbl, "message-dialog-title");
  gtk_label_set_justify(GTK_LABEL(title_lbl), GTK_JUSTIFY_CENTER);
  gtk_label_set_xalign(GTK_LABEL(title_lbl), 0.5f);
  gtk_label_set_wrap(GTK_LABEL(title_lbl), TRUE);
  gtk_box_append(GTK_BOX(msg_content), title_lbl);

  const std::string desc = end_session_format_description(
      action, g_dlg.seconds_left, zh);
  GtkWidget* desc_lbl = gtk_label_new(desc.c_str());
  g_dlg.desc_label = desc_lbl;
  gtk_widget_add_css_class(desc_lbl, "message-dialog-description");
  gtk_label_set_justify(GTK_LABEL(desc_lbl), GTK_JUSTIFY_CENTER);
  gtk_label_set_xalign(GTK_LABEL(desc_lbl), 0.5f);
  gtk_label_set_wrap(GTK_LABEL(desc_lbl), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(desc_lbl), PANGO_WRAP_WORD_CHAR);
  gtk_box_append(GTK_BOX(msg_content), desc_lbl);

  gtk_box_append(GTK_BOX(content), msg_content);
  gtk_box_append(GTK_BOX(box), content);

  GtkWidget* btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_set_homogeneous(GTK_BOX(btns), TRUE);
  gtk_widget_add_css_class(btns, "modal-dialog-button-box");

  GtkWidget* cancel_btn = gtk_button_new_with_label(tr("Cancel", "取消"));
  gtk_widget_set_hexpand(cancel_btn, TRUE);
  gtk_widget_add_css_class(cancel_btn, "cancel-button");
  gtk_widget_add_css_class(cancel_btn, "modal-dialog-button");
  g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_cancel_clicked), nullptr);
  gtk_box_append(GTK_BOX(btns), cancel_btn);

  const std::string confirm_txt = end_session_confirm_label(action, zh);
  GtkWidget* confirm_btn = gtk_button_new_with_label(confirm_txt.c_str());
  g_dlg.confirm_btn = confirm_btn;
  gtk_widget_set_hexpand(confirm_btn, TRUE);
  gtk_widget_add_css_class(confirm_btn, "default");
  gtk_widget_add_css_class(confirm_btn, "modal-dialog-button");
  g_signal_connect(confirm_btn, "clicked",
                   G_CALLBACK(on_confirm_clicked), nullptr);
  gtk_box_append(GTK_BOX(btns), confirm_btn);

  gtk_box_append(GTK_BOX(box), btns);
  gtk_box_append(GTK_BOX(wrap), box);

  GtkEventController* key_ctrl = gtk_event_controller_key_new();
  g_signal_connect(key_ctrl, "key-pressed",
                   G_CALLBACK(on_dialog_key_pressed), nullptr);
  gtk_widget_add_controller(dlg, key_ctrl);
  g_signal_connect(dlg, "destroy", G_CALLBACK(on_dialog_destroy), nullptr);

  gtk_window_set_child(GTK_WINDOW(dlg), wrap);

  int min_w = 0, nat_w = 392;
  int min_h = 0, nat_h = 220;
  gtk_widget_measure(wrap, GTK_ORIENTATION_HORIZONTAL, -1,
                     &min_w, &nat_w, nullptr, nullptr);
  const int dlg_w = std::max(nat_w, min_w);
  gtk_widget_measure(wrap, GTK_ORIENTATION_VERTICAL, dlg_w,
                     &min_h, &nat_h, nullptr, nullptr);
  const int dlg_h = std::max(nat_h, min_h);
  gtk_window_set_default_size(GTK_WINDOW(dlg), dlg_w, dlg_h);

  gtk_widget_realize(dlg);
  center_modal_on_workarea(dlg, dlg_w, dlg_h);
  gtk_widget_set_visible(dlg, TRUE);
  gtk_window_present(GTK_WINDOW(dlg));
  gtk_widget_grab_focus(cancel_btn);

  g_dlg.timer_id = g_timeout_add_seconds(1, on_countdown_tick, nullptr);
}

void close_end_session_dialog() {
  if (g_dlg.timer_id != 0) {
    g_source_remove(g_dlg.timer_id);
    g_dlg.timer_id = 0;
  }
  if (g_dlg.window != nullptr) {
    GtkWidget* w = g_dlg.window;
    g_dlg.window = nullptr;
    gtk_window_destroy(GTK_WINDOW(w));
  }
  g_dlg.desc_label = nullptr;
  g_dlg.confirm_btn = nullptr;
  g_dlg.on_confirm = nullptr;
  g_dlg.user_data = nullptr;
}

}  // namespace winome
