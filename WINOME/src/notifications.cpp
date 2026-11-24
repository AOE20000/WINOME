// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Asynchronous SMTC & Toast notification state plus GNOME top-center banner.

#include "notifications.h"

#include "app-index.h"
#include "easing.h"
#include "fonts.h"
#include "i18n.h"
#include "monitor-manager.h"
#include "st-engine.h"
#include "system-status.h"

#include <gtk/gtk.h>
#include <gdk/win32/gdkwin32.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
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
std::atomic<bool> g_toast_allowed_last{false};
std::atomic<bool> g_media_active_last{false};
guint g_poll_timer_id = 0;
int g_poll_interval_sec = 2;

struct NotifWorker {
  std::mutex mu;
  std::condition_variable cv;
  std::thread thread;
  bool started = false;
  bool stop = false;
  bool poll_requested = false;
  bool clear_all_pending = false;
  std::vector<uint32_t> remove_ids;
  std::vector<WinomeMediaAction> media_actions;
};

NotifWorker g_worker;

constexpr int kBannerAnimMs = 200; // messageTray.js ANIMATION_TIME
constexpr int kBannerTimeoutMs = 4500;

GtkWidget* g_banner_window = nullptr;
GtkWidget* g_banner_wrap = nullptr;
GtkWidget* g_banner_app_label = nullptr;
GtkWidget* g_banner_title_label = nullptr;
GtkWidget* g_banner_body_label = nullptr;
GtkWidget* g_banner_actions_box = nullptr;
guint g_banner_hide_timer = 0;
uint64_t g_banner_current_key = 0;
uint64_t g_banner_anim_gen = 0;
bool g_banner_hovered = false;

void cancel_banner_hide_timer() {
  if (g_banner_hide_timer != 0) {
    g_source_remove(g_banner_hide_timer);
    g_banner_hide_timer = 0;
  }
}

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
  ++g_banner_anim_gen;
  cancel_banner_hide_timer();
  g_banner_hovered = false;
  if (g_banner_window != nullptr)
    gtk_widget_set_visible(g_banner_window, FALSE);
  g_banner_current_key = 0;
}

gboolean on_banner_timeout(gpointer G_GNUC_UNUSED data) {
  g_banner_hide_timer = 0;
  if (g_banner_window == nullptr || g_banner_wrap == nullptr)
    return G_SOURCE_REMOVE;
  if (g_banner_hovered)
    return G_SOURCE_REMOVE;

  const uint64_t gen = ++g_banner_anim_gen;
  gtk_widget_set_sensitive(g_banner_wrap, FALSE);
  tween_opacity(g_banner_wrap, gtk_widget_get_opacity(g_banner_wrap), 0.0,
                kBannerAnimMs, Ease::ease_out_quad, [gen] {
                  if (gen != g_banner_anim_gen)
                    return;
                  if (g_banner_window != nullptr)
                    gtk_widget_set_visible(g_banner_window, FALSE);
                  g_banner_current_key = 0;
                });
  return G_SOURCE_REMOVE;
}

void schedule_banner_hide() {
  cancel_banner_hide_timer();
  if (!g_banner_hovered)
    g_banner_hide_timer = g_timeout_add(kBannerTimeoutMs, on_banner_timeout,
                                        nullptr);
}

void on_banner_motion_enter(GtkEventControllerMotion* ctrl G_GNUC_UNUSED,
                            double x G_GNUC_UNUSED,
                            double y G_GNUC_UNUSED,
                            gpointer user_data G_GNUC_UNUSED) {
  g_banner_hovered = true;
  cancel_banner_hide_timer();
}

void on_banner_motion_leave(GtkEventControllerMotion* ctrl G_GNUC_UNUSED,
                            gpointer user_data G_GNUC_UNUSED) {
  g_banner_hovered = false;
  if (g_banner_window != nullptr && gtk_widget_get_visible(g_banner_window) &&
      g_banner_wrap != nullptr && gtk_widget_get_sensitive(g_banner_wrap) &&
      g_banner_current_key != 0) {
    schedule_banner_hide();
  }
}

void on_banner_close_clicked(GtkButton* btn G_GNUC_UNUSED,
                             gpointer user_data G_GNUC_UNUSED) {
  uint64_t key = g_banner_current_key;
  dismiss_banner_immediately();
  if (key != 0)
    notifications_dismiss(key);
}

void on_banner_action_clicked(GtkButton* btn, gpointer user_data G_GNUC_UNUSED) {
  const uint64_t key = g_banner_current_key;
  const int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "action-index"));
  dismiss_banner_immediately();
  if (key != 0)
    notifications_activate(key, idx);
}

void on_banner_body_clicked(GtkGestureClick* gesture,
                            int n_press G_GNUC_UNUSED,
                            double x,
                            double y,
                            gpointer user_data G_GNUC_UNUSED) {
  if (g_banner_wrap != nullptr) {
    GtkWidget* picked = gtk_widget_pick(g_banner_wrap, x, y, GTK_PICK_DEFAULT);
    for (GtkWidget* w = picked; w != nullptr && w != g_banner_wrap;
         w = gtk_widget_get_parent(w)) {
      if (GTK_IS_BUTTON(w))
        return;
    }
  }
  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  const uint64_t key = g_banner_current_key;
  dismiss_banner_immediately();
  if (key != 0)
    notifications_activate(key, -1);
}

int lookup_msg_length(const char* child1,
                      const char* child2,
                      const char* prop,
                      int fallback) {
  StEngine* e = StEngine::get();
  if (e == nullptr)
    return fallback;
  double v = 0.0;
  StEngine::Node root(*e, nullptr, "", "message", "");
  if (child1 == nullptr) {
    if (root.lookup_length(prop, &v) && v > 0.0)
      return static_cast<int>(v + 0.5);
    return fallback;
  }
  StEngine::Node n1(*e, &root, "", child1, "");
  if (child2 == nullptr) {
    if (n1.lookup_length(prop, &v) && v > 0.0)
      return static_cast<int>(v + 0.5);
    return fallback;
  }
  StEngine::Node n2(*e, &n1, "", child2, "");
  if (n2.lookup_length(prop, &v) && v > 0.0)
    return static_cast<int>(v + 0.5);
  return fallback;
}

int banner_header_spacing() {
  return lookup_msg_length("message-header", nullptr, "spacing", 6);
}

int banner_source_icon_size() {
  return lookup_msg_length("message-header", "message-source-icon", "icon-size",
                           16);
}

int banner_box_spacing() {
  return lookup_msg_length("message-box", nullptr, "spacing", 6);
}

int banner_content_spacing() {
  return lookup_msg_length("message-box", "message-content", "spacing", 4);
}

int banner_themed_icon_size() {
  return lookup_msg_length("message-box", "message-icon message-themed-icon",
                           "icon-size", 16);
}

int banner_min_width() {
  StEngine* e = StEngine::get();
  if (e == nullptr)
    return 360;
  StEngine::Node n(*e, nullptr, "", "notification-banner", "");
  double v = 0.0;
  if (n.lookup_length("width", &v) && v > 0.0)
    return static_cast<int>(v + 0.5);
  return 360;
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
  const int w = std::max(min_w, banner_min_width());
  gtk_widget_measure(window, GTK_ORIENTATION_VERTICAL, w,
                     &min_h, &nat_h, nullptr, nullptr);
  const int h = std::max(nat_h, 64);
  gtk_window_set_default_size(GTK_WINDOW(window), w, h);

  const int scale = std::max(gtk_widget_get_scale_factor(window), 1);
  const int phys_w = w * scale;
  const int phys_h = h * scale;

  HMONITOR mon = monitor_manager_active_monitor();
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (mon == nullptr || !GetMonitorInfoW(mon, &mi)) {
    mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(mon, &mi);
  }

  // Sit cleanly below this monitor's panel if one is visible; otherwise anchor
  // directly beneath the top edge of the active monitor.
  int top_y = mi.rcMonitor.top + 8 * scale;
  HWND panel_hwnd = monitor_manager_panel_for_monitor(mon);
  if (panel_hwnd == nullptr)
    panel_hwnd = FindWindowW(nullptr, L"WINOME Shell");
  if (panel_hwnd != nullptr && IsWindowVisible(panel_hwnd)) {
    RECT pr{};
    if (GetWindowRect(panel_hwnd, &pr) &&
        MonitorFromWindow(panel_hwnd, MONITOR_DEFAULTTONULL) == mon &&
        pr.bottom > mi.rcMonitor.top) {
      top_y = pr.bottom;
    }
  }

  int x = mi.rcMonitor.left + (mi.rcMonitor.right - mi.rcMonitor.left - phys_w) / 2;
  SetWindowPos(hwnd, HWND_TOPMOST, x, top_y, phys_w, phys_h,
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
  install_bundled_fonts(g_banner_window);

  g_banner_wrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(g_banner_wrap, "notification-banner");
  gtk_widget_add_css_class(g_banner_wrap, "message");

  GtkEventController* motion = gtk_event_controller_motion_new();
  g_signal_connect(motion, "enter", G_CALLBACK(on_banner_motion_enter), nullptr);
  g_signal_connect(motion, "leave", G_CALLBACK(on_banner_motion_leave), nullptr);
  gtk_widget_add_controller(g_banner_wrap, motion);

  GtkGesture* click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
  g_signal_connect(click, "released", G_CALLBACK(on_banner_body_clicked), nullptr);
  gtk_widget_add_controller(g_banner_wrap, GTK_EVENT_CONTROLLER(click));
  gtk_widget_set_cursor_from_name(g_banner_wrap, "pointer");

  GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, banner_header_spacing());
  gtk_widget_add_css_class(header, "message-header");

  GtkWidget* src_icon = gtk_image_new_from_icon_name("dialog-information-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(src_icon), banner_source_icon_size());
  gtk_widget_add_css_class(src_icon, "message-source-icon");
  gtk_box_append(GTK_BOX(header), src_icon);

  GtkWidget* header_content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, banner_header_spacing());
  gtk_widget_add_css_class(header_content, "message-header-content");
  gtk_widget_set_hexpand(header_content, TRUE);

  g_banner_app_label = gtk_label_new("");
  gtk_widget_add_css_class(g_banner_app_label, "message-source-title");
  gtk_label_set_xalign(GTK_LABEL(g_banner_app_label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(g_banner_app_label), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(header_content), g_banner_app_label);
  gtk_box_append(GTK_BOX(header), header_content);

  GtkWidget* close_btn = gtk_button_new();
  GtkWidget* close_icon = gtk_image_new_from_icon_name("window-close-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(close_icon), 16);
  gtk_button_set_child(GTK_BUTTON(close_btn), close_icon);
  gtk_widget_add_css_class(close_btn, "message-close-button");
  gtk_widget_add_css_class(close_btn, "icon-button");
  gtk_widget_set_halign(close_btn, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(close_btn, GTK_ALIGN_CENTER);
  gtk_widget_set_focus_on_click(close_btn, FALSE);
  g_signal_connect(close_btn, "clicked",
                   G_CALLBACK(on_banner_close_clicked), nullptr);
  gtk_box_append(GTK_BOX(header), close_btn);
  gtk_box_append(GTK_BOX(g_banner_wrap), header);

  GtkWidget* msg_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, banner_box_spacing());
  gtk_widget_add_css_class(msg_box, "message-box");

  GtkWidget* icon = gtk_image_new_from_icon_name("preferences-system-notifications-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(icon), banner_themed_icon_size());
  gtk_widget_add_css_class(icon, "message-icon");
  gtk_widget_add_css_class(icon, "message-themed-icon");
  gtk_box_append(GTK_BOX(msg_box), icon);

  GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, banner_content_spacing());
  gtk_widget_add_css_class(content, "message-content");
  gtk_widget_set_hexpand(content, TRUE);

  g_banner_title_label = gtk_label_new("");
  gtk_widget_add_css_class(g_banner_title_label, "message-title");
  gtk_label_set_xalign(GTK_LABEL(g_banner_title_label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(g_banner_title_label), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(content), g_banner_title_label);

  g_banner_body_label = gtk_label_new("");
  gtk_widget_add_css_class(g_banner_body_label, "message-body");
  gtk_label_set_xalign(GTK_LABEL(g_banner_body_label), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(g_banner_body_label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(g_banner_body_label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_lines(GTK_LABEL(g_banner_body_label), 3);
  gtk_label_set_ellipsize(GTK_LABEL(g_banner_body_label), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(content), g_banner_body_label);

  g_banner_actions_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(g_banner_actions_box, "notification-actions");
  gtk_widget_set_visible(g_banner_actions_box, FALSE);
  gtk_box_append(GTK_BOX(content), g_banner_actions_box);

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

  if (g_banner_actions_box != nullptr) {
    while (GtkWidget* ch = gtk_widget_get_first_child(g_banner_actions_box))
      gtk_box_remove(GTK_BOX(g_banner_actions_box), ch);
    for (size_t i = 0; i < notif.actions.size(); ++i) {
      GtkWidget* btn = gtk_button_new_with_label(notif.actions[i].label.c_str());
      gtk_widget_add_css_class(btn, "notification-button");
      gtk_widget_set_hexpand(btn, TRUE);
      gtk_widget_set_focus_on_click(btn, FALSE);
      g_object_set_data(G_OBJECT(btn), "action-index",
                        GINT_TO_POINTER(static_cast<int>(i)));
      g_signal_connect(btn, "clicked", G_CALLBACK(on_banner_action_clicked),
                       nullptr);
      gtk_box_append(GTK_BOX(g_banner_actions_box), btn);
    }
    gtk_widget_set_visible(g_banner_actions_box,
                           !notif.actions.empty() ? TRUE : FALSE);
  }

  cancel_banner_hide_timer();
  ++g_banner_anim_gen;

  gtk_widget_set_sensitive(g_banner_wrap, TRUE);
  gtk_widget_set_opacity(g_banner_wrap, 0.0);
  gtk_widget_set_visible(g_banner_window, TRUE);
  position_banner_window(g_banner_window);
  const uint64_t show_gen = g_banner_anim_gen;
  g_idle_add(
      [](gpointer ud) -> gboolean {
        const auto expected = reinterpret_cast<uintptr_t>(ud);
        if (static_cast<uint64_t>(expected) == g_banner_anim_gen &&
            g_banner_window != nullptr &&
            gtk_widget_get_visible(g_banner_window)) {
          position_banner_window(g_banner_window);
        }
        return G_SOURCE_REMOVE;
      },
      reinterpret_cast<gpointer>(static_cast<uintptr_t>(show_gen)));

  tween_opacity(g_banner_wrap, 0.0, 1.0, kBannerAnimMs, Ease::ease_out_quad);
  schedule_banner_hide();
}

struct PollApplyData {
  WinomeMediaInfo media;
  std::vector<WinomeSystemNotification> sys_notifs;
  bool got_notifs = false;
  bool toast_allowed = false;
};

gboolean poll_tick_forwarder(gpointer data);

bool media_equals(const WinomeMediaInfo& a, const WinomeMediaInfo& b) {
  return a.active == b.active && a.playing == b.playing &&
         a.title == b.title && a.artist == b.artist &&
         a.album == b.album && a.app_id == b.app_id;
}

bool sys_notifs_equal(const std::vector<WinomeSystemNotification>& a,
                      const std::vector<WinomeSystemNotification>& b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].id != b[i].id || a[i].timestamp_unix != b[i].timestamp_unix ||
        a[i].app_name != b[i].app_name || a[i].app_id != b[i].app_id ||
        a[i].title != b[i].title || a[i].body != b[i].body ||
        a[i].launch_args != b[i].launch_args ||
        a[i].activation_type != b[i].activation_type ||
        a[i].url != b[i].url || a[i].actions.size() != b[i].actions.size()) {
      return false;
    }
  }
  return true;
}

gboolean apply_poll_on_main(gpointer user_data) {
  auto* data = static_cast<PollApplyData*>(user_data);
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

      // Prune dismissed IDs that Windows has already removed so the set stays
      // bounded and future reused notification IDs are never suppressed.
      if (!g_state.dismissed_winrt_ids.empty()) {
        std::set<uint32_t> live_ids;
        for (const auto& sn : data->sys_notifs)
          live_ids.insert(sn.id);
        for (auto it = g_state.dismissed_winrt_ids.begin();
             it != g_state.dismissed_winrt_ids.end();) {
          if (live_ids.count(*it) == 0)
            it = g_state.dismissed_winrt_ids.erase(it);
          else
            ++it;
        }
      }

      for (const auto& sn : data->sys_notifs) {
        if (g_state.dismissed_winrt_ids.count(sn.id))
          continue;

        ShellNotification item;
        item.winrt_id = sn.id;
        item.from_winrt = true;
        item.timestamp_unix = sn.timestamp_unix;
        item.app_name = sn.app_name.empty() ? "Windows" : sn.app_name;
        item.app_id = sn.app_id;
        item.title = sn.title;
        item.body = sn.body;
        item.launch_args = sn.launch_args;
        item.activation_type = sn.activation_type;
        item.url = sn.url;
        item.actions = sn.actions;

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
              rebuilt[i].body != g_state.items[i].body ||
              rebuilt[i].app_id != g_state.items[i].app_id ||
              rebuilt[i].launch_args != g_state.items[i].launch_args ||
              rebuilt[i].url != g_state.items[i].url ||
              rebuilt[i].actions.size() != g_state.items[i].actions.size()) {
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

  // Adapt background WinRT polling cadence: 2s when active media or OS toast
  // access is permitted, relaxing to 12s when idle & toast access is denied.
  g_toast_allowed_last.store(data->toast_allowed, std::memory_order_release);
  g_media_active_last.store(data->media.active, std::memory_order_release);
  const int want_sec = (data->toast_allowed || data->media.active) ? 2 : 12;
  if (g_poll_timer_id != 0 && want_sec != g_poll_interval_sec) {
    g_source_remove(g_poll_timer_id);
    g_poll_interval_sec = want_sec;
    g_poll_timer_id = g_timeout_add_seconds(static_cast<guint>(want_sec),
                                            poll_tick_forwarder, nullptr);
  }

  return G_SOURCE_REMOVE;
}

void notif_worker_main() {
  bool has_last_poll = false;
  WinomeMediaInfo last_media{};
  bool last_toast_allowed = false;
  bool last_got_notifs = false;
  std::vector<WinomeSystemNotification> last_notifs;

  for (;;) {
    bool do_poll = false;
    bool do_clear = false;
    std::vector<uint32_t> remove_ids;
    std::vector<WinomeMediaAction> media_actions;
    {
      std::unique_lock<std::mutex> lock(g_worker.mu);
      g_worker.cv.wait(lock, [] {
        return g_worker.stop || g_worker.poll_requested ||
               !g_worker.remove_ids.empty() || g_worker.clear_all_pending ||
               !g_worker.media_actions.empty();
      });
      if (g_worker.stop)
        break;
      do_poll = g_worker.poll_requested;
      g_worker.poll_requested = false;
      do_clear = g_worker.clear_all_pending;
      g_worker.clear_all_pending = false;
      remove_ids.swap(g_worker.remove_ids);
      media_actions.swap(g_worker.media_actions);
    }

    const bool had_removals = !remove_ids.empty() || do_clear;
    std::set<uint32_t> unique_ids(remove_ids.begin(), remove_ids.end());
    for (uint32_t id : unique_ids)
      winrt_remove_notification(id);
    if (do_clear)
      winrt_clear_notifications();
    if (had_removals) {
      Sleep(120);
      do_poll = true;
    }
    if (!media_actions.empty()) {
      for (WinomeMediaAction act : media_actions)
        winrt_media_control(act);
      Sleep(120);
      do_poll = true;
    }

    if (do_poll && g_running.load()) {
      auto* payload = new PollApplyData();
      winrt_get_media_info(&payload->media);
      bool allowed = false;
      payload->got_notifs =
          winrt_get_notifications(&payload->sys_notifs, &allowed);
      payload->toast_allowed = allowed;

      const bool unchanged =
          has_last_poll &&
          media_equals(last_media, payload->media) &&
          last_toast_allowed == payload->toast_allowed &&
          last_got_notifs == payload->got_notifs &&
          (!payload->got_notifs ||
           sys_notifs_equal(last_notifs, payload->sys_notifs));
      if (unchanged) {
        delete payload;
      } else {
        has_last_poll = true;
        last_media = payload->media;
        last_toast_allowed = payload->toast_allowed;
        last_got_notifs = payload->got_notifs;
        if (payload->got_notifs)
          last_notifs = payload->sys_notifs;
        else
          last_notifs.clear();
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE, apply_poll_on_main, payload,
            +[](gpointer ud) { delete static_cast<PollApplyData*>(ud); });
      }
    }
  }
}

void trigger_background_poll() {
  if (!g_running.load())
    return;
  std::lock_guard<std::mutex> lock(g_worker.mu);
  if (g_worker.stop)
    return;
  g_worker.poll_requested = true;
  g_worker.cv.notify_one();
}

gboolean poll_tick(gpointer G_GNUC_UNUSED data) {
  trigger_background_poll();
  return G_SOURCE_CONTINUE;
}

gboolean poll_tick_forwarder(gpointer data) {
  return poll_tick(data);
}

}  // namespace

void notifications_init() {
  if (g_running.exchange(true))
    return;
  {
    std::lock_guard<std::mutex> lock(g_worker.mu);
    if (!g_worker.started) {
      g_worker.stop = false;
      g_worker.poll_requested = true;
      g_worker.started = true;
      g_worker.thread = std::thread(notif_worker_main);
    }
  }
  g_worker.cv.notify_one();
  g_poll_interval_sec = 2;
  g_poll_timer_id = g_timeout_add_seconds(2, poll_tick, nullptr);
}

void notifications_shutdown() {
  if (!g_running.exchange(false))
    return;
  if (g_poll_timer_id != 0) {
    g_source_remove(g_poll_timer_id);
    g_poll_timer_id = 0;
  }
  std::thread to_join;
  {
    std::lock_guard<std::mutex> lock(g_worker.mu);
    if (g_worker.started) {
      g_worker.stop = true;
      g_worker.started = false;
      to_join = std::move(g_worker.thread);
    }
  }
  g_worker.cv.notify_all();
  if (to_join.joinable())
    to_join.join();

  dismiss_banner_immediately();
  if (g_banner_window != nullptr) {
    gtk_window_destroy(GTK_WINDOW(g_banner_window));
    g_banner_window = nullptr;
    g_banner_wrap = nullptr;
    g_banner_actions_box = nullptr;
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
    item.url = extract_notification_url("", "", title, body);
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

void notifications_activate(uint64_t key, int action_index) {
  ShellNotification snap;
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(g_state.mu);
    for (const auto& item : g_state.items) {
      if (item.key == key) {
        snap = item;
        found = true;
        break;
      }
    }
  }
  if (!found)
    return;

  // Match GNOME Shell and Windows Action Center: activating a notification
  // dismisses it from the banner and notification list.
  notifications_dismiss(key);

  std::string args = snap.launch_args;
  std::string act_type = snap.activation_type;
  std::string fallback_url = snap.url;
  if (action_index >= 0 &&
      static_cast<size_t>(action_index) < snap.actions.size()) {
    const auto& act = snap.actions[static_cast<size_t>(action_index)];
    args = act.arguments;
    act_type = act.activation_type;
    fallback_url = extract_notification_url(args, act_type, "", "");
  }

  std::thread([app_id = std::move(snap.app_id),
               app_name = std::move(snap.app_name),
               args = std::move(args),
               act_type = std::move(act_type),
               fallback_url = std::move(fallback_url)]() {
    if (winrt_activate_notification(app_id, app_name, args, act_type,
                                    fallback_url)) {
      return;
    }

    // Fallback for unpackaged Win32 apps whose AUMID or display name matches a
    // Start Menu / AppsFolder entry in AppIndex: focus an existing window or
    // launch the app shortcut.
    AppEntry entry;
    bool matched = !app_id.empty() && app_index_find_by_id(app_id, &entry);
    if (!matched && !app_name.empty()) {
      const std::string want = app_utf8_tolower(app_name);
      for (const auto& cand : app_index_get_all()) {
        if (cand.name_lower == want || cand.target_lower == want ||
            cand.target_lower == want + ".exe") {
          entry = cand;
          matched = true;
          break;
        }
      }
    }
    if (matched) {
      auto wins = app_index_running_windows_for_entry(entry);
      if (!wins.empty()) {
        app_index_activate_hwnd(wins.front().hwnd);
        return;
      }
      app_index_launch(entry, false);
    }
  }).detach();
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
  if (found) {
    if (g_banner_current_key == key)
      dismiss_banner_immediately();
    notify_listeners_main();
  }
  if (remove_winrt) {
    std::lock_guard<std::mutex> lock(g_worker.mu);
    if (!g_worker.stop) {
      g_worker.remove_ids.push_back(winrt_id);
      g_worker.cv.notify_one();
    }
  }
}

void notifications_clear_all() {
  bool had_any = false;
  std::vector<uint32_t> ids_to_remove;
  {
    std::lock_guard<std::mutex> lock(g_state.mu);
    for (const auto& item : g_state.items) {
      if (item.from_winrt) {
        g_state.dismissed_winrt_ids.insert(item.winrt_id);
        ids_to_remove.push_back(item.winrt_id);
      }
    }
    had_any = !g_state.items.empty();
    g_state.items.clear();
    if (had_any)
      ++g_state.generation;
  }
  if (had_any) {
    dismiss_banner_immediately();
    notify_listeners_main();
  }
  {
    std::lock_guard<std::mutex> lock(g_worker.mu);
    if (!g_worker.stop) {
      for (uint32_t id : ids_to_remove)
        g_worker.remove_ids.push_back(id);
      g_worker.clear_all_pending = true;
      g_worker.cv.notify_one();
    }
  }
}

void notifications_media_action(WinomeMediaAction action) {
  std::lock_guard<std::mutex> lock(g_worker.mu);
  if (!g_worker.stop) {
    g_worker.media_actions.push_back(action);
    g_worker.cv.notify_one();
  }
}

void notifications_notify_dnd_changed() {
  bool dnd = false;
  if (get_do_not_disturb(&dnd) && dnd)
    dismiss_banner_immediately();
  notify_listeners_main();
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
