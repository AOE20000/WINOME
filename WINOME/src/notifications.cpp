// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Asynchronous SMTC & Toast notification state plus GNOME top-center banner.

#include "notifications.h"

#include "easing.h"
#include "i18n.h"
#include "system-status.h"

#include <gtk/gtk.h>
#include <gdk/win32/gdkwin32.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace winome {
namespace {

struct ListenerEntry {
  NotificationsChangedCallback cb;
  void* user_data;
};

struct NotifState {
  std::mutex mu;
  std::vector<ShellNotification> items;
  WinomeMediaInfo media;
  std::set<uint32_t> seen_winrt_ids;
  std::set<uint32_t> dismissed_winrt_ids;
  bool initial_hydration_done = false;
  uint64_t next_key = 1;
  uint64_t generation = 1;
  std::vector<ListenerEntry> listeners;
};

NotifState g_state;
std::atomic<bool> g_running{false};
std::atomic<bool> g_poll_in_flight{false};
guint g_poll_timer_id = 0;

GtkWidget* g_banner_window = nullptr;
GtkWidget* g_banner_wrap = nullptr;
GtkWidget* g_banner_app_label = nullptr;
GtkWidget* g_banner_title_label = nullptr;
GtkWidget* g_banner_body_label = nullptr;
guint g_banner_hide_timer = 0;
uint64_t g_banner_current_key = 0;

void notify_listeners_main() {
  std::vector<ListenerEntry> snap;
  {
    std::lock_guard<std::mutex> lock(g_state.mu);
    snap = g_state.listeners;
  }
  for (const auto& entry : snap) {
    if (entry.cb != nullptr)
      entry.cb(entry.user_data);
  }
}

void dismiss_banner_immediately() {
  if (g_banner_hide_timer != 0) {
    g_source_remove(g_banner_hide_timer);
    g_banner_hide_timer = 0;
  }
  if (g_banner_window != nullptr)
    gtk_widget_set_visible(g_banner_window, FALSE);
  g_banner_current_key = 0;
}

gboolean on_banner_timeout(gpointer G_GNUC_UNUSED data) {
  g_banner_hide_timer = 0;
  if (g_banner_window == nullptr || g_banner_wrap == nullptr)
    return G_SOURCE_REMOVE;

  gtk_widget_set_sensitive(g_banner_wrap, FALSE);
  tween_opacity(g_banner_wrap, gtk_widget_get_opacity(g_banner_wrap), 0.0,
                BOXPOPOVER_ANIMATION_TIME, Ease::ease_out_quad, [] {
                  if (g_banner_window != nullptr)
                    gtk_widget_set_visible(g_banner_window, FALSE);
                  g_banner_current_key = 0;
                });
  return G_SOURCE_REMOVE;
}

void on_banner_close_clicked(GtkButton* btn G_GNUC_UNUSED,
                             gpointer user_data G_GNUC_UNUSED) {
  uint64_t key = g_banner_current_key;
  dismiss_banner_immediately();
  if (key != 0)
    notifications_dismiss(key);
}

void position_banner_window(GtkWidget* window) {
  if (window == nullptr)
    return;
  GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window));
  if (surface == nullptr)
    return;
  HWND hwnd = gdk_win32_surface_get_handle(surface);
  if (hwnd == nullptr)
    return;

  LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                    (ex | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE) &
                        ~WS_EX_APPWINDOW);

  gtk_widget_realize(window);
  int min_w = 0, nat_w = 420;
  int min_h = 0, nat_h = 72;
  gtk_widget_measure(window, GTK_ORIENTATION_HORIZONTAL, -1,
                     &min_w, &nat_w, nullptr, nullptr);
  gtk_widget_measure(window, GTK_ORIENTATION_VERTICAL, nat_w,
                     &min_h, &nat_h, nullptr, nullptr);
  int w = std::max(nat_w, 360);
  int h = std::max(nat_h, 64);

  HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(mon, &mi);

  int top_y = mi.rcMonitor.top + 40;
  HWND panel_hwnd = FindWindowW(nullptr, L"WINOME Shell");
  if (panel_hwnd != nullptr) {
    RECT pr{};
    if (GetWindowRect(panel_hwnd, &pr) && pr.bottom > mi.rcMonitor.top)
      top_y = pr.bottom + 8;
  }

  int x = mi.rcMonitor.left + (mi.rcMonitor.right - mi.rcMonitor.left - w) / 2;
  SetWindowPos(hwnd, HWND_TOPMOST, x, top_y, w, h,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void ensure_banner_window() {
  if (g_banner_window != nullptr)
    return;

  g_banner_window = gtk_window_new();
  gtk_widget_set_name(g_banner_window, "winomeToastWindow");
  gtk_window_set_decorated(GTK_WINDOW(g_banner_window), FALSE);
  gtk_window_set_resizable(GTK_WINDOW(g_banner_window), FALSE);
  gtk_widget_set_focusable(g_banner_window, FALSE);

  // Transparent background for the floating toplevel window only.
  GtkCssProvider* css = gtk_css_provider_new();
  gtk_css_provider_load_from_string(
      css, "#winomeToastWindow { background-color: transparent; box-shadow: none; }");
  gtk_style_context_add_provider_for_display(
      gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(css);

  g_banner_wrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(g_banner_wrap, "notification-banner");
  gtk_widget_add_css_class(g_banner_wrap, "message");

  GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(header, "message-header");

  GtkWidget* src_icon = gtk_image_new_from_icon_name("dialog-information-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(src_icon), 16);
  gtk_widget_add_css_class(src_icon, "message-source-icon");
  gtk_box_append(GTK_BOX(header), src_icon);

  GtkWidget* header_content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(header_content, "message-header-content");
  gtk_widget_set_hexpand(header_content, TRUE);

  g_banner_app_label = gtk_label_new("");
  gtk_widget_add_css_class(g_banner_app_label, "message-source-title");
  gtk_label_set_xalign(GTK_LABEL(g_banner_app_label), 0.0f);
  gtk_box_append(GTK_BOX(header_content), g_banner_app_label);
  gtk_box_append(GTK_BOX(header), header_content);

  GtkWidget* close_btn = gtk_button_new_from_icon_name("window-close-symbolic");
  gtk_widget_add_css_class(close_btn, "message-close-button");
  gtk_widget_set_focus_on_click(close_btn, FALSE);
  g_signal_connect(close_btn, "clicked",
                   G_CALLBACK(on_banner_close_clicked), nullptr);
  gtk_box_append(GTK_BOX(header), close_btn);
  gtk_box_append(GTK_BOX(g_banner_wrap), header);

  GtkWidget* msg_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(msg_box, "message-box");

  GtkWidget* icon = gtk_image_new_from_icon_name("preferences-system-notifications-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(icon), 24);
  gtk_widget_add_css_class(icon, "message-icon");
  gtk_widget_add_css_class(icon, "message-themed-icon");
  gtk_box_append(GTK_BOX(msg_box), icon);

  GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_add_css_class(content, "message-content");
  gtk_widget_set_hexpand(content, TRUE);

  g_banner_title_label = gtk_label_new("");
  gtk_widget_add_css_class(g_banner_title_label, "message-title");
  gtk_label_set_xalign(GTK_LABEL(g_banner_title_label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(g_banner_title_label), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(content), g_banner_title_label);

  g_banner_body_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(g_banner_body_label), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(g_banner_body_label), TRUE);
  gtk_label_set_lines(GTK_LABEL(g_banner_body_label), 3);
  gtk_label_set_ellipsize(GTK_LABEL(g_banner_body_label), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(content), g_banner_body_label);

  gtk_box_append(GTK_BOX(msg_box), content);
  gtk_box_append(GTK_BOX(g_banner_wrap), msg_box);

  gtk_window_set_child(GTK_WINDOW(g_banner_window), g_banner_wrap);
}

void show_banner_main(const ShellNotification& notif) {
  if (!g_running.load())
    return;

  bool dnd = false;
  if (get_do_not_disturb(&dnd) && dnd)
    return;

  ensure_banner_window();
  g_banner_current_key = notif.key;

  gtk_label_set_text(GTK_LABEL(g_banner_app_label),
                     notif.app_name.empty() ? "WINOME" : notif.app_name.c_str());
  gtk_label_set_text(GTK_LABEL(g_banner_title_label), notif.title.c_str());
  gtk_label_set_text(GTK_LABEL(g_banner_body_label), notif.body.c_str());
  gtk_widget_set_visible(g_banner_body_label, !notif.body.empty());

  if (g_banner_hide_timer != 0) {
    g_source_remove(g_banner_hide_timer);
    g_banner_hide_timer = 0;
  }

  gtk_widget_set_sensitive(g_banner_wrap, TRUE);
  gtk_widget_set_opacity(g_banner_wrap, 0.0);
  gtk_widget_set_visible(g_banner_window, TRUE);
  position_banner_window(g_banner_window);

  tween_opacity(g_banner_wrap, 0.0, 1.0, BOXPOPOVER_ANIMATION_TIME,
                Ease::ease_out_quad);
  g_banner_hide_timer = g_timeout_add(4500, on_banner_timeout, nullptr);
}

struct PollApplyData {
  WinomeMediaInfo media;
  std::vector<WinomeSystemNotification> sys_notifs;
  bool got_notifs = false;
};

bool media_equals(const WinomeMediaInfo& a, const WinomeMediaInfo& b) {
  return a.active == b.active && a.playing == b.playing &&
         a.title == b.title && a.artist == b.artist &&
         a.album == b.album && a.app_id == b.app_id;
}

gboolean apply_poll_on_main(gpointer user_data) {
  std::unique_ptr<PollApplyData> data(static_cast<PollApplyData*>(user_data));
  if (!g_running.load())
    return G_SOURCE_REMOVE;

  bool changed = false;
  std::vector<ShellNotification> newly_arrived;

  {
    std::lock_guard<std::mutex> lock(g_state.mu);
    if (!media_equals(g_state.media, data->media)) {
      g_state.media = data->media;
      changed = true;
    }

    if (data->got_notifs) {
      std::vector<ShellNotification> rebuilt;
      // Preserve existing WINOME shell internal notifications.
      for (const auto& existing : g_state.items) {
        if (!existing.from_winrt)
          rebuilt.push_back(existing);
      }

      for (const auto& sn : data->sys_notifs) {
        if (g_state.dismissed_winrt_ids.count(sn.id))
          continue;

        ShellNotification item;
        item.winrt_id = sn.id;
        item.from_winrt = true;
        item.timestamp_unix = sn.timestamp_unix;
        item.app_name = sn.app_name.empty() ? "Windows" : sn.app_name;
        item.title = sn.title;
        item.body = sn.body;

        bool matched_existing = false;
        for (const auto& ex : g_state.items) {
          if (ex.from_winrt && ex.winrt_id == sn.id) {
            item.key = ex.key;
            matched_existing = true;
            break;
          }
        }
        if (!matched_existing)
          item.key = g_state.next_key++;

        rebuilt.push_back(item);
        if (g_state.seen_winrt_ids.insert(sn.id).second &&
            g_state.initial_hydration_done) {
          newly_arrived.push_back(item);
        }
      }
      g_state.initial_hydration_done = true;

      std::sort(rebuilt.begin(), rebuilt.end(),
                [](const ShellNotification& a, const ShellNotification& b) {
                  if (a.timestamp_unix != b.timestamp_unix)
                    return a.timestamp_unix > b.timestamp_unix;
                  return a.key > b.key;
                });
      if (rebuilt.size() > 50)
        rebuilt.resize(50);

      if (rebuilt.size() != g_state.items.size()) {
        changed = true;
      } else {
        for (size_t i = 0; i < rebuilt.size(); ++i) {
          if (rebuilt[i].key != g_state.items[i].key ||
              rebuilt[i].title != g_state.items[i].title ||
              rebuilt[i].body != g_state.items[i].body) {
            changed = true;
            break;
          }
        }
      }
      if (changed)
        g_state.items = std::move(rebuilt);
    }

    if (changed)
      ++g_state.generation;
  }

  if (changed)
    notify_listeners_main();
  if (!newly_arrived.empty())
    show_banner_main(newly_arrived.front());

  return G_SOURCE_REMOVE;
}

void trigger_background_poll() {
  if (!g_running.load())
    return;
  bool expected = false;
  if (!g_poll_in_flight.compare_exchange_strong(expected, true))
    return;

  std::thread([] {
    auto* payload = new PollApplyData();
    winrt_get_media_info(&payload->media);
    bool allowed = false;
    payload->got_notifs = winrt_get_notifications(&payload->sys_notifs, &allowed);
    g_poll_in_flight.store(false);
    g_idle_add(apply_poll_on_main, payload);
  }).detach();
}

gboolean poll_tick(gpointer G_GNUC_UNUSED data) {
  trigger_background_poll();
  return G_SOURCE_CONTINUE;
}

}  // namespace

void notifications_init() {
  if (g_running.exchange(true))
    return;
  trigger_background_poll();
  g_poll_timer_id = g_timeout_add_seconds(2, poll_tick, nullptr);
}

void notifications_shutdown() {
  if (!g_running.exchange(false))
    return;
  if (g_poll_timer_id != 0) {
    g_source_remove(g_poll_timer_id);
    g_poll_timer_id = 0;
  }
  dismiss_banner_immediately();
  if (g_banner_window != nullptr) {
    gtk_window_destroy(GTK_WINDOW(g_banner_window));
    g_banner_window = nullptr;
    g_banner_wrap = nullptr;
  }
}

void notify_shell(const std::string& title,
                  const std::string& body,
                  const std::string& app_name) {
  ShellNotification item;
  {
    std::lock_guard<std::mutex> lock(g_state.mu);
    item.key = g_state.next_key++;
    item.from_winrt = false;
    item.timestamp_unix = static_cast<int64_t>(std::time(nullptr));
    item.app_name = app_name.empty() ? "WINOME" : app_name;
    item.title = title;
    item.body = body;
    g_state.items.insert(g_state.items.begin(), item);
    if (g_state.items.size() > 50)
      g_state.items.resize(50);
    ++g_state.generation;
  }

  g_idle_add([](gpointer ud) -> gboolean {
    std::unique_ptr<ShellNotification> n(static_cast<ShellNotification*>(ud));
    notify_listeners_main();
    show_banner_main(*n);
    return G_SOURCE_REMOVE;
  }, new ShellNotification(item));
}

std::vector<ShellNotification> notifications_get_all() {
  std::lock_guard<std::mutex> lock(g_state.mu);
  return g_state.items;
}

WinomeMediaInfo notifications_get_media() {
  std::lock_guard<std::mutex> lock(g_state.mu);
  return g_state.media;
}

uint64_t notifications_generation() {
  std::lock_guard<std::mutex> lock(g_state.mu);
  return g_state.generation;
}

void notifications_dismiss(uint64_t key) {
  uint32_t winrt_id = 0;
  bool remove_winrt = false;
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(g_state.mu);
    for (auto it = g_state.items.begin(); it != g_state.items.end(); ++it) {
      if (it->key == key) {
        found = true;
        if (it->from_winrt) {
          remove_winrt = true;
          winrt_id = it->winrt_id;
          g_state.dismissed_winrt_ids.insert(winrt_id);
        }
        g_state.items.erase(it);
        ++g_state.generation;
        break;
      }
    }
  }
  if (found)
    notify_listeners_main();
  if (remove_winrt) {
    std::thread([winrt_id] { winrt_remove_notification(winrt_id); }).detach();
  }
}

void notifications_clear_all() {
  bool had_any = false;
  {
    std::lock_guard<std::mutex> lock(g_state.mu);
    for (const auto& item : g_state.items) {
      if (item.from_winrt)
        g_state.dismissed_winrt_ids.insert(item.winrt_id);
    }
    had_any = !g_state.items.empty();
    g_state.items.clear();
    if (had_any)
      ++g_state.generation;
  }
  if (had_any)
    notify_listeners_main();
  std::thread([] { winrt_clear_notifications(); }).detach();
}

void notifications_media_action(WinomeMediaAction action) {
  std::thread([action] {
    winrt_media_control(action);
    Sleep(120);
    trigger_background_poll();
  }).detach();
}

void notifications_add_listener(NotificationsChangedCallback cb, void* user_data) {
  if (cb == nullptr)
    return;
  std::lock_guard<std::mutex> lock(g_state.mu);
  g_state.listeners.push_back({cb, user_data});
}

void notifications_remove_listener(NotificationsChangedCallback cb, void* user_data) {
  std::lock_guard<std::mutex> lock(g_state.mu);
  g_state.listeners.erase(
      std::remove_if(g_state.listeners.begin(), g_state.listeners.end(),
                     [&](const ListenerEntry& e) {
                       return e.cb == cb && e.user_data == user_data;
                     }),
      g_state.listeners.end());
}

std::string format_notification_age(int64_t timestamp_unix,
                                    int64_t now_unix,
                                    bool zh) {
  if (timestamp_unix <= 0 || now_unix < timestamp_unix)
    return zh ? "刚刚" : "Just now";
  int64_t diff = now_unix - timestamp_unix;
  char buf[32];
  if (diff < 60)
    return zh ? "刚刚" : "Just now";
  if (diff < 3600) {
    std::snprintf(buf, sizeof(buf), zh ? "%lld 分钟前" : "%lldm",
                  static_cast<long long>(diff / 60));
    return buf;
  }
  if (diff < 86400) {
    std::snprintf(buf, sizeof(buf), zh ? "%lld 小时前" : "%lldh",
                  static_cast<long long>(diff / 3600));
    return buf;
  }
  std::snprintf(buf, sizeof(buf), zh ? "%lld 天前" : "%lldd",
                static_cast<long long>(diff / 86400));
  return buf;
}

}  // namespace winome
