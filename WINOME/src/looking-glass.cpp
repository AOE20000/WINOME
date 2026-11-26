// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors

#include "looking-glass.h"

#include "config.h"
#include "debug-runtime.h"
#include "fonts.h"
#include "host-utils.h"
#include "i18n.h"
#include "perf-metrics.h"
#include "plugin-host.h"
#include "st-engine.h"

#include <dwmapi.h>
#include <gdk/win32/gdkwin32.h>
#include <gtk/gtk.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace winome {
namespace {

constexpr UINT kDwmUseImmersiveDarkMode = 20;

struct LookingGlassState {
  LookingGlassCallbacks cbs{};
  std::shared_ptr<std::atomic<bool>> alive;
  GtkWidget* window = nullptr;
  GtkWidget* stack = nullptr;

  // Console page
  GtkWidget* cmd_entry = nullptr;
  GtkTextBuffer* cmd_buf = nullptr;
  GtkWidget* cmd_timing_lbl = nullptr;

  // Test Runner page
  GtkWidget* test_steps_entry = nullptr;
  GtkWidget* test_gap_spin = nullptr;
  GtkWidget* test_settle_spin = nullptr;
  GtkWidget* test_repeat_spin = nullptr;
  GtkWidget* test_tag_entry = nullptr;
  GtkWidget* test_status_lbl = nullptr;
  GtkTextBuffer* test_buf = nullptr;
  bool test_busy = false;

  // Logs page
  GtkWidget* log_tag_drop = nullptr;
  GtkWidget* log_pause_btn = nullptr;
  GtkTextBuffer* log_buf = nullptr;
  GtkWidget* log_view = nullptr;
  guint log_timer_id = 0;
  uint64_t log_last_seq = 0;

  // CSS Inspector page
  GtkWidget* css_sel_entry = nullptr;
  GtkWidget* css_prop_entry = nullptr;
  GtkWidget* css_result_lbl = nullptr;

  // Windows & Z-Order page
  GtkWidget* win_list_box = nullptr;

  // Flags & Subsystems & Plugins page
  GtkWidget* subsys_box = nullptr;
  GtkWidget* plugins_box = nullptr;

  bool css_loaded = false;
};

LookingGlassState g_lg;

void ensure_lg_css() {
  if (g_lg.css_loaded)
    return;
  g_lg.css_loaded = true;

  static const char* kCss = R"(
#LookingGlassDialog {
  background-color: #1c1c1e;
  color: #f3f3f3;
}
#winomeLgHeader {
  background-color: #242428;
  border-bottom: 1px solid rgba(255,255,255,0.09);
  padding: 8px 12px;
}
#winomeLgBody {
  padding: 12px;
}
#winomeLgCard {
  background-color: #28282c;
  border: 1px solid rgba(255,255,255,0.09);
  border-radius: 10px;
  padding: 10px 12px;
}
#winomeLgMono {
  font-family: Consolas, "Cascadia Mono", monospace;
  font-size: 12px;
  background-color: #141416;
  color: #e6e6eb;
  border: 1px solid rgba(255,255,255,0.1);
  border-radius: 8px;
  padding: 6px;
}
#winomeLgMono text {
  background-color: #141416;
  color: #e6e6eb;
}
#winomeLgDim {
  color: #9e9ea6;
  font-size: 12px;
}
)";
  GtkCssProvider* p = gtk_css_provider_new();
  gtk_css_provider_load_from_string(p, kCss);
  if (GdkDisplay* disp = gdk_display_get_default()) {
    gtk_style_context_add_provider_for_display(
        disp, GTK_STYLE_PROVIDER(p),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  }
  g_object_unref(p);
}

void apply_dark_frame(GtkWidget* win) {
  GdkSurface* surf = gtk_native_get_surface(GTK_NATIVE(win));
  if (surf == nullptr)
    return;
  HWND hwnd = static_cast<HWND>(gdk_win32_surface_get_handle(surf));
  if (hwnd != nullptr) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
  }
}

std::string escape_lg_json(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '"' || c == '\\')
      out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

std::string normalize_cmd_to_json(const std::string& raw) {
  size_t s = 0;
  while (s < raw.size() && std::isspace(static_cast<unsigned char>(raw[s])))
    ++s;
  size_t e = raw.size();
  while (e > s && std::isspace(static_cast<unsigned char>(raw[e - 1])))
    --e;
  std::string trimmed = raw.substr(s, e - s);
  if (trimmed.empty())
    return {};
  if (trimmed[0] == '{')
    return trimmed;

  // Support simple CLI form: "<cmd>" or "<cmd> <arg>".
  size_t sp = trimmed.find(' ');
  if (sp == std::string::npos)
    return std::string("{\"cmd\":\"") + trimmed + "\"}";
  const std::string cmd = trimmed.substr(0, sp);
  const std::string arg = trimmed.substr(sp + 1);
  if (cmd == "desktop.switch" || cmd == "desktop.remove")
    return "{\"cmd\":\"" + cmd + "\",\"index\":" + arg + "}";
  if (cmd == "test.trigger")
    return "{\"cmd\":\"test.trigger\",\"action\":\"" + escape_lg_json(arg) +
           "\",\"wait_ms\":450,\"tag\":\"trig,ov,perf,dev-test\"}";
  if (cmd == "test.run") {
    if (arg == "slow-double-win" || arg == "overview-bench") {
      return "{\"cmd\":\"test.run\",\"preset\":\"" + arg +
             "\",\"tag\":\"trig,ov,perf,dev-test\"}";
    }
    return "{\"cmd\":\"test.run\",\"steps\":\"" + escape_lg_json(arg) +
           "\",\"tag\":\"trig,ov,perf,dev-test\"}";
  }
  if (cmd == "test.begin")
    return "{\"cmd\":\"test.begin\",\"tag\":\"" + escape_lg_json(arg) + "\"}";
  if (cmd == "dev.op") {
    return "{\"cmd\":\"dev.op\",\"action\":\"" + escape_lg_json(arg) +
           "\",\"capture_logs\":true}";
  }
  if (cmd == "dev.batch") {
    return "{\"cmd\":\"dev.batch\",\"ops\":\"" + escape_lg_json(arg) +
           "\",\"capture_logs\":true}";
  }
  if (cmd == "subsystem.trip" || cmd == "subsystem.recover" ||
      cmd == "plugin.restart" || cmd == "plugin.enable" ||
      cmd == "plugin.disable" || cmd == "plugin.reset" ||
      cmd == "run.exec") {
    return "{\"cmd\":\"" + cmd + "\",\"name\":\"" + escape_lg_json(arg) + "\"}";
  }
  return "{\"cmd\":\"" + cmd + "\",\"page\":\"" + escape_lg_json(arg) + "\"}";
}

struct AsyncLgCmdResult {
  std::shared_ptr<std::atomic<bool>> alive;
  std::string req;
  std::string resp;
  double elapsed_ms = 0.0;
  bool to_test_tab = false;
};

gboolean on_async_lg_cmd_done(gpointer ud) {
  std::unique_ptr<AsyncLgCmdResult> res(static_cast<AsyncLgCmdResult*>(ud));
  if (!res || !res->alive || !res->alive->load(std::memory_order_acquire) ||
      g_lg.window == nullptr) {
    return G_SOURCE_REMOVE;
  }
  char hdr[256];
  std::snprintf(hdr, sizeof(hdr), "%.2f ms  |  %s", res->elapsed_ms,
                res->req.c_str());
  if (res->to_test_tab) {
    g_lg.test_busy = false;
    if (g_lg.test_status_lbl != nullptr)
      gtk_label_set_text(GTK_LABEL(g_lg.test_status_lbl), hdr);
    if (g_lg.test_buf != nullptr) {
      gtk_text_buffer_set_text(g_lg.test_buf, res->resp.c_str(),
                               static_cast<int>(res->resp.size()));
    }
  } else {
    if (g_lg.cmd_timing_lbl != nullptr)
      gtk_label_set_text(GTK_LABEL(g_lg.cmd_timing_lbl), hdr);
    if (g_lg.cmd_buf != nullptr) {
      gtk_text_buffer_set_text(g_lg.cmd_buf, res->resp.c_str(),
                               static_cast<int>(res->resp.size()));
    }
  }
  return G_SOURCE_REMOVE;
}

void dispatch_lg_request_async(const std::string& req, bool to_test_tab) {
  if (g_lg.window == nullptr || req.empty())
    return;
  if (to_test_tab) {
    if (g_lg.test_busy)
      return;
    g_lg.test_busy = true;
    if (g_lg.test_status_lbl != nullptr) {
      gtk_label_set_text(GTK_LABEL(g_lg.test_status_lbl),
                         tr("Running test sequence...", "正在执行测试序列..."));
    }
  } else if (g_lg.cmd_timing_lbl != nullptr) {
    gtk_label_set_text(GTK_LABEL(g_lg.cmd_timing_lbl),
                       tr("Executing asynchronously...", "正在后台异步执行..."));
  }

  auto dispatch_fn = g_lg.cbs.dispatch_cmd;
  auto alive = g_lg.alive;
  std::thread([dispatch_fn, alive, req, to_test_tab]() {
    const gint64 t0 = g_get_monotonic_time();
    const std::string resp =
        dispatch_fn ? dispatch_fn(req)
                    : "{\"ok\":false,\"error\":\"no_dispatcher\"}";
    const double elapsed_ms =
        static_cast<double>(g_get_monotonic_time() - t0) / 1000.0;
    auto* payload = new AsyncLgCmdResult();
    payload->alive = alive;
    payload->req = req;
    payload->resp = resp;
    payload->elapsed_ms = elapsed_ms;
    payload->to_test_tab = to_test_tab;
    g_idle_add(on_async_lg_cmd_done, payload);
  }).detach();
}

void exec_console_command(const char* text) {
  if (g_lg.window == nullptr || g_lg.cmd_buf == nullptr || text == nullptr)
    return;
  const std::string req = normalize_cmd_to_json(text);
  if (req.empty())
    return;

  // test.trigger / test.run / dev.op / dev.batch may sleep or drive OS input
  // while GTK animates and collects logs; run off the GTK main thread so
  // animations remain 100% live & responsive.
  if (req.find("\"test.trigger\"") != std::string::npos ||
      req.find("\"test.run\"") != std::string::npos ||
      req.find("\"dev.op\"") != std::string::npos ||
      req.find("\"dev.batch\"") != std::string::npos) {
    dispatch_lg_request_async(req, false);
    return;
  }

  const gint64 t0 = g_get_monotonic_time();
  const std::string resp =
      g_lg.cbs.dispatch_cmd
          ? g_lg.cbs.dispatch_cmd(req)
          : "{\"ok\":false,\"error\":\"no_dispatcher\"}";
  const double elapsed_ms = static_cast<double>(g_get_monotonic_time() - t0) / 1000.0;

  char hdr[128];
  std::snprintf(hdr, sizeof(hdr), "%.2f ms  |  %s", elapsed_ms, req.c_str());
  if (g_lg.cmd_timing_lbl != nullptr)
    gtk_label_set_text(GTK_LABEL(g_lg.cmd_timing_lbl), hdr);

  gtk_text_buffer_set_text(g_lg.cmd_buf, resp.c_str(), static_cast<int>(resp.size()));
}

void on_cmd_run_clicked(GtkButton* btn G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  if (g_lg.cmd_entry == nullptr)
    return;
  exec_console_command(gtk_editable_get_text(GTK_EDITABLE(g_lg.cmd_entry)));
}

void on_cmd_entry_activate(GtkEntry* entry, gpointer ud G_GNUC_UNUSED) {
  exec_console_command(gtk_editable_get_text(GTK_EDITABLE(entry)));
}

void on_quick_cmd_clicked(GtkButton* btn G_GNUC_UNUSED, gpointer data) {
  const char* cmd = static_cast<const char*>(data);
  if (cmd == nullptr)
    return;
  if (g_lg.cmd_entry != nullptr)
    gtk_editable_set_text(GTK_EDITABLE(g_lg.cmd_entry), cmd);
  exec_console_command(cmd);
}

std::string current_log_filter() {
  if (g_lg.log_tag_drop == nullptr)
    return "all";
  guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(g_lg.log_tag_drop));
  static const char* const kTags[] = {
      "all", "perf", "ov", "ov-focus", "trig", "dev-test", "vd", "ipc", "abi",
      "plugin", "run"};
  if (idx < std::size(kTags))
    return kTags[idx];
  return "all";
}

void append_logs_incremental(bool reset_all) {
  if (g_lg.window == nullptr || g_lg.log_buf == nullptr)
    return;
  if (g_lg.log_pause_btn != nullptr &&
      gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_lg.log_pause_btn))) {
    return;
  }

  if (reset_all) {
    gtk_text_buffer_set_text(g_lg.log_buf, "", 0);
    g_lg.log_last_seq = 0;
  }

  const auto entries =
      debug_get_logs_since(g_lg.log_last_seq, current_log_filter(), 250);
  if (entries.empty())
    return;

  GtkTextIter end_it;
  gtk_text_buffer_get_end_iter(g_lg.log_buf, &end_it);
  std::string chunk;
  for (const auto& e : entries) {
    char prefix[48];
    std::snprintf(prefix, sizeof(prefix), "[#%llu][%s] ",
                  static_cast<unsigned long long>(e.seq), e.tag.c_str());
    chunk += prefix;
    chunk += e.text;
    chunk.push_back('\n');
    g_lg.log_last_seq = std::max(g_lg.log_last_seq, e.seq);
  }
  gtk_text_buffer_insert(g_lg.log_buf, &end_it, chunk.c_str(),
                         static_cast<int>(chunk.size()));

  if (g_lg.log_view != nullptr) {
    gtk_text_buffer_get_end_iter(g_lg.log_buf, &end_it);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(g_lg.log_view), &end_it,
                                 0.0, FALSE, 0.0, 1.0);
  }
}

gboolean on_log_poll_tick(gpointer ud G_GNUC_UNUSED) {
  if (g_lg.window == nullptr) {
    g_lg.log_timer_id = 0;
    return G_SOURCE_REMOVE;
  }
  append_logs_incremental(false);
  return G_SOURCE_CONTINUE;
}

void on_log_filter_changed(GObject* o G_GNUC_UNUSED,
                           GParamSpec* p G_GNUC_UNUSED,
                           gpointer ud G_GNUC_UNUSED) {
  append_logs_incremental(true);
}

void on_log_clear_clicked(GtkButton* b G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  debug_clear_logs();
  if (g_lg.log_buf != nullptr)
    gtk_text_buffer_set_text(g_lg.log_buf, "", 0);
  g_lg.log_last_seq = debug_latest_log_seq();
}

void on_css_probe_clicked(GtkButton* b G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  if (g_lg.css_sel_entry == nullptr || g_lg.css_prop_entry == nullptr ||
      g_lg.css_result_lbl == nullptr) {
    return;
  }
  StEngine* eng = StEngine::get();
  if (eng == nullptr) {
    gtk_label_set_text(GTK_LABEL(g_lg.css_result_lbl), "StEngine unavailable");
    return;
  }

  const std::string chain =
      gtk_editable_get_text(GTK_EDITABLE(g_lg.css_sel_entry));
  const std::string prop =
      gtk_editable_get_text(GTK_EDITABLE(g_lg.css_prop_entry));

  std::vector<StEngine::Node*> nodes;
  std::istringstream iss(chain);
  std::string tok;
  while (iss >> tok) {
    std::string id;
    std::string cls;
    std::string pseudo;
    size_t p_colon = tok.find(':');
    if (p_colon != std::string::npos) {
      pseudo = tok.substr(p_colon + 1);
      tok.resize(p_colon);
    }
    size_t p_dot = tok.find('.');
    if (!tok.empty() && tok[0] == '#') {
      if (p_dot != std::string::npos) {
        id = tok.substr(1, p_dot - 1);
        cls = tok.substr(p_dot + 1);
      } else {
        id = tok.substr(1);
      }
    } else if (p_dot != std::string::npos) {
      cls = tok.substr(p_dot + 1);
    } else {
      cls = tok;
    }
    for (char& c : cls) {
      if (c == '.')
        c = ' ';
    }
    const StEngine::Node* parent = nodes.empty() ? nullptr : nodes.back();
    nodes.push_back(eng->create_node(parent, id, cls, pseudo));
  }

  std::string msg = "Not found for selector";
  if (!nodes.empty()) {
    double px = 0.0;
    std::string color;
    if (nodes.back()->lookup_color(prop, &color)) {
      msg = prop + " = " + color;
    } else if (nodes.back()->lookup_length(prop, &px)) {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "%s = %.2f px", prop.c_str(), px);
      msg = buf;
    } else if (nodes.back()->lookup_double(prop, &px)) {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "%s = %.3f", prop.c_str(), px);
      msg = buf;
    }
  }
  for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
    delete *it;

  gtk_label_set_text(GTK_LABEL(g_lg.css_result_lbl), msg.c_str());
}

void on_open_gtk_inspector(GtkButton* b G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  gtk_window_set_interactive_debugging(TRUE);
}

void on_reload_theme_clicked(GtkButton* b G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  const bool ok = g_lg.cbs.reload_theme ? g_lg.cbs.reload_theme() : false;
  if (g_lg.css_result_lbl != nullptr) {
    gtk_label_set_text(GTK_LABEL(g_lg.css_result_lbl),
                       ok ? tr("Theme and user-debug.css reloaded", "已重新加载主题与 user-debug.css")
                          : tr("Theme reload failed", "主题重载失败"));
  }
}

void on_open_dev_css_folder(GtkButton* b G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  std::wstring dir = app_data_dir();
  if (!dir.empty())
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void populate_windows_list() {
  if (g_lg.win_list_box == nullptr)
    return;
  while (GtkWidget* ch = gtk_widget_get_first_child(g_lg.win_list_box))
    gtk_box_remove(GTK_BOX(g_lg.win_list_box), ch);

  const auto wins = debug_inspect_windows();
  char buf[384];
  for (const auto& w : wins) {
    std::snprintf(
        buf, sizeof(buf),
        "[z=%2d] 0x%llX  pid=%lu  vd=%d  %s%s%s  (%ld,%ld %ldx%ld)  %s  |  %s",
        w.z_index, static_cast<unsigned long long>(w.hwnd),
        static_cast<unsigned long>(w.pid), w.desktop_index,
        w.is_own_process ? "[WINOME] " : "",
        w.topmost ? "[TOP] " : "",
        w.cloaked ? "[CLOAK] " : "",
        w.left, w.top, w.right - w.left, w.bottom - w.top,
        w.class_name.c_str(),
        w.title.empty() ? "(untitled)" : w.title.c_str());
    GtkWidget* lbl = gtk_label_new(buf);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(lbl), TRUE);
    gtk_box_append(GTK_BOX(g_lg.win_list_box), lbl);
  }
}

void on_windows_refresh_clicked(GtkButton* b G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  populate_windows_list();
}

void on_windows_restack_clicked(GtkButton* b G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  if (g_lg.cbs.restack_windows)
    g_lg.cbs.restack_windows();
  populate_windows_list();
}

void populate_subsys_and_plugins();

void on_flag_toggled(GtkSwitch* sw G_GNUC_UNUSED, gboolean state, gpointer data) {
  const char* name = static_cast<const char*>(data);
  if (name != nullptr)
    debug_set_flag_by_name(name, state != FALSE);
}

void on_trip_subsys_clicked(GtkButton* b G_GNUC_UNUSED, gpointer data) {
  auto id = static_cast<SubsystemId>(reinterpret_cast<intptr_t>(data));
  capability_record_failure(id, "lg-manual-fault-injection", 1);
  populate_subsys_and_plugins();
}

void on_recover_subsys_clicked(GtkButton* b G_GNUC_UNUSED, gpointer data) {
  auto id = static_cast<SubsystemId>(reinterpret_cast<intptr_t>(data));
  capability_set(id, CapabilityState::Active, "lg-manual-recover");
  populate_subsys_and_plugins();
}

struct PluginUiRow {
  std::string id;
  std::string name;
  std::string version;
  std::string state;
  std::string last_error;
  bool enabled = true;
  int pid = 0;
  int crash_count = 0;
};

struct PluginActionBtnData {
  std::string op;
  std::string id;
};

bool skip_json_ws(const std::string& s, size_t* pos) {
  while (*pos < s.size() && std::isspace(static_cast<unsigned char>(s[*pos])))
    ++(*pos);
  return *pos < s.size();
}

bool read_json_str_val(const std::string& s, size_t* pos, std::string* out) {
  if (!skip_json_ws(s, pos) || s[*pos] != '"')
    return false;
  ++(*pos);
  std::string res;
  while (*pos < s.size()) {
    char c = s[(*pos)++];
    if (c == '"') {
      *out = std::move(res);
      return true;
    }
    if (c == '\\' && *pos < s.size()) {
      char e = s[(*pos)++];
      switch (e) {
        case '"':  res.push_back('"'); break;
        case '\\': res.push_back('\\'); break;
        case 'n':  res.push_back('\n'); break;
        case 'r':  res.push_back('\r'); break;
        case 't':  res.push_back('\t'); break;
        default:   res.push_back(e); break;
      }
    } else {
      res.push_back(c);
    }
  }
  return false;
}

bool read_json_int_val(const std::string& s, size_t* pos, int* out) {
  if (!skip_json_ws(s, pos))
    return false;
  size_t st = *pos;
  if (s[*pos] == '-') ++(*pos);
  while (*pos < s.size() && std::isdigit(static_cast<unsigned char>(s[*pos])))
    ++(*pos);
  if (*pos == st) return false;
  *out = std::atoi(s.substr(st, *pos - st).c_str());
  return true;
}

bool skip_json_any(const std::string& s, size_t* pos) {
  if (!skip_json_ws(s, pos))
    return false;
  char c = s[*pos];
  if (c == '"') {
    std::string dummy;
    return read_json_str_val(s, pos, &dummy);
  }
  if (c == '{' || c == '[') {
    char open = c;
    char close = (c == '{') ? '}' : ']';
    int depth = 1;
    ++(*pos);
    while (*pos < s.size() && depth > 0) {
      if (s[*pos] == '"') {
        std::string dummy;
        if (!read_json_str_val(s, pos, &dummy)) return false;
        continue;
      }
      if (s[*pos] == open) ++depth;
      else if (s[*pos] == close) --depth;
      ++(*pos);
    }
    return depth == 0;
  }
  while (*pos < s.size() && s[*pos] != ',' && s[*pos] != '}' &&
         s[*pos] != ']' && !std::isspace(static_cast<unsigned char>(s[*pos]))) {
    ++(*pos);
  }
  return true;
}

std::vector<PluginUiRow> parse_plugin_rows_json(const std::string& json,
                                                std::string* out_dir) {
  std::vector<PluginUiRow> rows;
  size_t dir_k = json.find("\"plugins_dir\"");
  if (dir_k != std::string::npos && out_dir != nullptr) {
    size_t col = json.find(':', dir_k + 13);
    if (col != std::string::npos) {
      size_t p = col + 1;
      read_json_str_val(json, &p, out_dir);
    }
  }
  size_t arr_k = json.find("\"plugins\"");
  if (arr_k == std::string::npos)
    return rows;
  size_t pos = json.find('[', arr_k);
  if (pos == std::string::npos)
    return rows;
  ++pos;
  while (pos < json.size()) {
    if (!skip_json_ws(json, &pos)) break;
    if (json[pos] == ']') break;
    if (json[pos] == ',') { ++pos; continue; }
    if (json[pos] != '{') break;
    ++pos;
    PluginUiRow r;
    while (pos < json.size()) {
      if (!skip_json_ws(json, &pos)) break;
      if (json[pos] == '}') { ++pos; break; }
      if (json[pos] == ',') { ++pos; continue; }
      std::string k;
      if (!read_json_str_val(json, &pos, &k)) break;
      if (!skip_json_ws(json, &pos) || json[pos] != ':') break;
      ++pos;
      if (k == "id") read_json_str_val(json, &pos, &r.id);
      else if (k == "name") read_json_str_val(json, &pos, &r.name);
      else if (k == "version") read_json_str_val(json, &pos, &r.version);
      else if (k == "state") read_json_str_val(json, &pos, &r.state);
      else if (k == "last_error") read_json_str_val(json, &pos, &r.last_error);
      else if (k == "pid") read_json_int_val(json, &pos, &r.pid);
      else if (k == "crash_count") read_json_int_val(json, &pos, &r.crash_count);
      else if (k == "enabled") {
        skip_json_ws(json, &pos);
        if (json.compare(pos, 4, "true") == 0) { r.enabled = true; pos += 4; }
        else if (json.compare(pos, 5, "false") == 0) { r.enabled = false; pos += 5; }
        else skip_json_any(json, &pos);
      } else {
        skip_json_any(json, &pos);
      }
    }
    if (!r.id.empty())
      rows.push_back(std::move(r));
  }
  return rows;
}

void on_plugin_action_clicked(GtkButton* b, gpointer ud G_GNUC_UNUSED) {
  auto* d = static_cast<PluginActionBtnData*>(
      g_object_get_data(G_OBJECT(b), "lg-plugin-act"));
  if (d == nullptr || !g_lg.cbs.plugin_action)
    return;
  std::string err;
  if (!g_lg.cbs.plugin_action(d->op, d->id, &err) && !err.empty())
    debug_append_log("plugin", d->op + "(" + d->id + ") failed: " + err);
  populate_subsys_and_plugins();
}

void on_plugins_reload_clicked(GtkButton* b G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  if (g_lg.cbs.plugin_action) {
    std::string err;
    g_lg.cbs.plugin_action("reload", "", &err);
  }
  populate_subsys_and_plugins();
}

void on_open_plugins_folder_clicked(GtkButton* b G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  const std::wstring pdir = plugin_host_directory();
  if (!pdir.empty()) {
    CreateDirectoryW(pdir.c_str(), nullptr);
    ShellExecuteW(nullptr, L"explore", pdir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
}

void populate_subsys_and_plugins() {
  if (g_lg.subsys_box != nullptr) {
    while (GtkWidget* ch = gtk_widget_get_first_child(g_lg.subsys_box))
      gtk_box_remove(GTK_BOX(g_lg.subsys_box), ch);

    const auto caps = capability_snapshot_all();
    for (const auto& c : caps) {
      GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
      char txt[256];
      std::snprintf(txt, sizeof(txt), "%-18s  [%s | err=%lu]  %s",
                    c.name ? c.name : "?",
                    capability_state_label(c.state),
                    static_cast<unsigned long>(c.failure_count),
                    c.detail.empty() ? "-" : c.detail.c_str());
      GtkWidget* lbl = gtk_label_new(txt);
      gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
      gtk_widget_set_hexpand(lbl, TRUE);

      GtkWidget* trip_btn = gtk_button_new_with_label(tr("Trip", "模拟故障"));
      g_signal_connect(trip_btn, "clicked", G_CALLBACK(on_trip_subsys_clicked),
                       reinterpret_cast<gpointer>(static_cast<intptr_t>(c.id)));
      GtkWidget* rec_btn = gtk_button_new_with_label(tr("Recover", "恢复"));
      g_signal_connect(rec_btn, "clicked", G_CALLBACK(on_recover_subsys_clicked),
                       reinterpret_cast<gpointer>(static_cast<intptr_t>(c.id)));
      gtk_box_append(GTK_BOX(row), lbl);
      gtk_box_append(GTK_BOX(row), trip_btn);
      gtk_box_append(GTK_BOX(row), rec_btn);
      gtk_box_append(GTK_BOX(g_lg.subsys_box), row);
    }
  }

  if (g_lg.plugins_box != nullptr) {
    while (GtkWidget* ch = gtk_widget_get_first_child(g_lg.plugins_box))
      gtk_box_remove(GTK_BOX(g_lg.plugins_box), ch);

    const std::string pjson =
        g_lg.cbs.plugin_list_json ? g_lg.cbs.plugin_list_json() : "{\"plugins\":[]}";
    std::string pdir;
    const auto plugins = parse_plugin_rows_json(pjson, &pdir);

    GtkWidget* hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    std::string hdr_txt = std::string(tr("Plugins dir: ", "插件目录: ")) +
                          (pdir.empty() ? "%LOCALAPPDATA%\\WINOME\\plugins" : pdir);
    GtkWidget* dir_lbl = gtk_label_new(hdr_txt.c_str());
    gtk_label_set_xalign(GTK_LABEL(dir_lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(dir_lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(dir_lbl, TRUE);

    GtkWidget* reload_btn = gtk_button_new_with_label(tr("Reload", "重载插件"));
    g_signal_connect(reload_btn, "clicked",
                     G_CALLBACK(on_plugins_reload_clicked), nullptr);
    GtkWidget* open_btn = gtk_button_new_with_label(tr("Open Dir", "打开目录"));
    g_signal_connect(open_btn, "clicked",
                     G_CALLBACK(on_open_plugins_folder_clicked), nullptr);
    gtk_box_append(GTK_BOX(hdr), dir_lbl);
    gtk_box_append(GTK_BOX(hdr), reload_btn);
    gtk_box_append(GTK_BOX(hdr), open_btn);
    gtk_box_append(GTK_BOX(g_lg.plugins_box), hdr);

    if (plugins.empty()) {
      GtkWidget* empty_lbl = gtk_label_new(
          tr("No plugins discovered. Place <id>\\manifest.json in the directory above and click Reload.",
             "未检测到插件。请将 <id>\\manifest.json 放入上述目录并点击「重载插件」。"));
      gtk_label_set_xalign(GTK_LABEL(empty_lbl), 0.0f);
      gtk_label_set_wrap(GTK_LABEL(empty_lbl), TRUE);
      gtk_box_append(GTK_BOX(g_lg.plugins_box), empty_lbl);
      return;
    }

    auto make_btn = [&](const char* label, const std::string& op, const std::string& id) {
      GtkWidget* btn = gtk_button_new_with_label(label);
      auto* act = new PluginActionBtnData{op, id};
      g_object_set_data_full(
          G_OBJECT(btn), "lg-plugin-act", act,
          +[](gpointer p) { delete static_cast<PluginActionBtnData*>(p); });
      g_signal_connect(btn, "clicked", G_CALLBACK(on_plugin_action_clicked), nullptr);
      return btn;
    };

    for (const auto& p : plugins) {
      GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      char summary[512];
      std::snprintf(summary, sizeof(summary), "%s (%s)  [%s | pid=%d | crash=%d]%s%s",
                    p.id.c_str(), p.version.c_str(), p.state.c_str(),
                    p.pid, p.crash_count,
                    p.last_error.empty() ? "" : " - ",
                    p.last_error.c_str());
      GtkWidget* lbl = gtk_label_new(summary);
      gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
      gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
      gtk_widget_set_hexpand(lbl, TRUE);
      gtk_box_append(GTK_BOX(row), lbl);

      const bool is_running = (p.state == "Running" || p.state == "Starting");
      gtk_box_append(GTK_BOX(row),
                     make_btn(is_running ? tr("Restart", "重启") : tr("Start", "启动"),
                              is_running ? "restart" : "start", p.id));
      if (is_running)
        gtk_box_append(GTK_BOX(row), make_btn(tr("Stop", "停止"), "stop", p.id));
      if (p.state == "Tripped" || p.crash_count > 0)
        gtk_box_append(GTK_BOX(row), make_btn(tr("Reset", "重置熔断"), "reset", p.id));
      gtk_box_append(GTK_BOX(row),
                     make_btn(p.enabled ? tr("Disable", "禁用") : tr("Enable", "启用"),
                              p.enabled ? "disable" : "enable", p.id));
      gtk_box_append(GTK_BOX(g_lg.plugins_box), row);
    }
  }
}

void on_subsys_refresh_clicked(GtkButton* b G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  populate_subsys_and_plugins();
}

void on_lg_destroy(GtkWidget* w G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  if (g_lg.alive)
    g_lg.alive->store(false, std::memory_order_release);
  if (g_lg.log_timer_id != 0) {
    g_source_remove(g_lg.log_timer_id);
    g_lg.log_timer_id = 0;
  }
  g_lg.window = nullptr;
  g_lg.stack = nullptr;
  g_lg.cmd_entry = nullptr;
  g_lg.cmd_buf = nullptr;
  g_lg.cmd_timing_lbl = nullptr;
  g_lg.test_steps_entry = nullptr;
  g_lg.test_gap_spin = nullptr;
  g_lg.test_settle_spin = nullptr;
  g_lg.test_repeat_spin = nullptr;
  g_lg.test_tag_entry = nullptr;
  g_lg.test_status_lbl = nullptr;
  g_lg.test_buf = nullptr;
  g_lg.test_busy = false;
  g_lg.log_tag_drop = nullptr;
  g_lg.log_pause_btn = nullptr;
  g_lg.log_buf = nullptr;
  g_lg.log_view = nullptr;
  g_lg.css_sel_entry = nullptr;
  g_lg.css_prop_entry = nullptr;
  g_lg.css_result_lbl = nullptr;
  g_lg.win_list_box = nullptr;
  g_lg.subsys_box = nullptr;
  g_lg.plugins_box = nullptr;
}

std::string current_test_tags() {
  if (g_lg.test_tag_entry == nullptr)
    return "trig,ov,perf,dev-test";
  const char* txt = gtk_editable_get_text(GTK_EDITABLE(g_lg.test_tag_entry));
  return txt != nullptr ? std::string(txt) : std::string();
}

void on_test_single_clicked(GtkButton* btn G_GNUC_UNUSED, gpointer data) {
  const char* action = static_cast<const char*>(data);
  if (action == nullptr)
    return;
  const int settle =
      g_lg.test_settle_spin != nullptr
          ? gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_lg.test_settle_spin))
          : 500;
  char req[512];
  std::snprintf(
      req, sizeof(req),
      "{\"cmd\":\"test.trigger\",\"action\":\"%s\",\"wait_ms\":%d,\"tag\":\"%s\"}",
      action, settle, escape_lg_json(current_test_tags()).c_str());
  dispatch_lg_request_async(req, true);
}

void on_test_preset_clicked(GtkButton* btn G_GNUC_UNUSED, gpointer data) {
  const char* preset = static_cast<const char*>(data);
  if (preset == nullptr)
    return;
  const int gap =
      g_lg.test_gap_spin != nullptr
          ? gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_lg.test_gap_spin))
          : 500;
  const int settle =
      g_lg.test_settle_spin != nullptr
          ? gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_lg.test_settle_spin))
          : 600;
  const int repeat =
      g_lg.test_repeat_spin != nullptr
          ? gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_lg.test_repeat_spin))
          : 1;
  char req[512];
  std::snprintf(
      req, sizeof(req),
      "{\"cmd\":\"test.run\",\"preset\":\"%s\",\"gap_ms\":%d,"
      "\"settle_ms\":%d,\"repeat\":%d,\"tag\":\"%s\"}",
      preset, gap, settle, repeat, escape_lg_json(current_test_tags()).c_str());
  dispatch_lg_request_async(req, true);
}

void on_test_custom_run_clicked(GtkButton* btn G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  if (g_lg.test_steps_entry == nullptr)
    return;
  const char* steps = gtk_editable_get_text(GTK_EDITABLE(g_lg.test_steps_entry));
  if (steps == nullptr || steps[0] == '\0')
    return;
  const int settle =
      g_lg.test_settle_spin != nullptr
          ? gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_lg.test_settle_spin))
          : 500;
  const int repeat =
      g_lg.test_repeat_spin != nullptr
          ? gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_lg.test_repeat_spin))
          : 1;
  std::string req =
      std::string("{\"cmd\":\"test.run\",\"steps\":\"") +
      escape_lg_json(steps) + "\",\"settle_ms\":" + std::to_string(settle) +
      ",\"repeat\":" + std::to_string(repeat) + ",\"tag\":\"" +
      escape_lg_json(current_test_tags()) + "\"}";
  dispatch_lg_request_async(req, true);
}

void on_test_session_begin_clicked(GtkButton* btn G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  std::string req = std::string("{\"cmd\":\"test.begin\",\"tag\":\"") +
                    escape_lg_json(current_test_tags()) + "\"}";
  dispatch_lg_request_async(req, true);
}

void on_test_session_end_clicked(GtkButton* btn G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  dispatch_lg_request_async("{\"cmd\":\"test.end\"}", true);
}

GtkWidget* build_tests_page() {
  GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_name(page, "winomeLgBody");

  // 1. Preset & session bar
  GtkWidget* preset_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget* slow_win_btn = gtk_button_new_with_label(
      tr("Slow Double Win (OS)", "慢速双击 Win 键 (OS)"));
  g_signal_connect(slow_win_btn, "clicked", G_CALLBACK(on_test_preset_clicked),
                   const_cast<char*>("slow-double-win"));
  GtkWidget* bench_btn = gtk_button_new_with_label(
      tr("Overview Bench", "概览开合压测"));
  g_signal_connect(bench_btn, "clicked", G_CALLBACK(on_test_preset_clicked),
                   const_cast<char*>("overview-bench"));
  GtkWidget* beg_btn = gtk_button_new_with_label(tr("Begin Session", "开始会话"));
  g_signal_connect(beg_btn, "clicked",
                   G_CALLBACK(on_test_session_begin_clicked), nullptr);
  GtkWidget* end_btn = gtk_button_new_with_label(tr("End & Aggregate", "结束并聚合"));
  g_signal_connect(end_btn, "clicked",
                   G_CALLBACK(on_test_session_end_clicked), nullptr);
  gtk_box_append(GTK_BOX(preset_row), slow_win_btn);
  gtk_box_append(GTK_BOX(preset_row), bench_btn);
  gtk_box_append(GTK_BOX(preset_row), beg_btn);
  gtk_box_append(GTK_BOX(preset_row), end_btn);
  gtk_box_append(GTK_BOX(page), preset_row);

  // 2. Single-trigger buttons
  GtkWidget* single_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  struct SingleSpec {
    const char* label;
    const char* action;
  } singles[] = {
      {"win_tap_os", "win_tap_os"},
      {"win_tap", "win_tap"},
      {"esc_os", "esc_os"},
      {"overview_open", "overview_open"},
      {"overview_close", "overview_close"},
      {"overview_grid", "overview_grid"},
  };
  for (const auto& sp : singles) {
    GtkWidget* b = gtk_button_new_with_label(sp.label);
    g_signal_connect(b, "clicked", G_CALLBACK(on_test_single_clicked),
                     const_cast<char*>(sp.action));
    gtk_box_append(GTK_BOX(single_row), b);
  }
  gtk_box_append(GTK_BOX(page), single_row);

  // 2b. Universal developer debug channel quick ops (dev.help / dev.op)
  GtkWidget* dev_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  struct DevQuickSpec {
    const char* label;
    const char* json;
  } dev_quicks[] = {
      {tr("GTK Windows", "GTK 窗口/渲染器"),
       "{\"cmd\":\"dev.op\",\"op\":\"gtk.windows\"}"},
      {tr("Perf Snapshot", "性能快照"),
       "{\"cmd\":\"dev.op\",\"op\":\"perf.snapshot\"}"},
      {tr("Foreground Win", "前景窗口"),
       "{\"cmd\":\"dev.op\",\"op\":\"win.fg\"}"},
      {tr("Clear Icon Cache", "清图标缓存"),
       "{\"cmd\":\"dev.op\",\"op\":\"shell.cache\",\"args\":{\"action\":\"icon_clear\"}}"},
      {tr("Dev Ops Help", "通用操作手册"),
       "{\"cmd\":\"dev.help\"}"},
  };
  for (const auto& dq : dev_quicks) {
    GtkWidget* b = gtk_button_new_with_label(dq.label);
    g_signal_connect(
        b, "clicked",
        G_CALLBACK(+[](GtkButton* G_GNUC_UNUSED, gpointer data) {
          const char* j = static_cast<const char*>(data);
          if (j != nullptr)
            dispatch_lg_request_async(j, true);
        }),
        const_cast<char*>(dq.json));
    gtk_box_append(GTK_BOX(dev_row), b);
  }
  gtk_box_append(GTK_BOX(page), dev_row);

  // 3. Parameters & custom step sequence
  GtkWidget* param_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  g_lg.test_gap_spin = gtk_spin_button_new_with_range(50, 5000, 50);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_lg.test_gap_spin), 500);
  g_lg.test_settle_spin = gtk_spin_button_new_with_range(0, 5000, 50);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_lg.test_settle_spin), 600);
  g_lg.test_repeat_spin = gtk_spin_button_new_with_range(1, 20, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_lg.test_repeat_spin), 1);
  g_lg.test_tag_entry = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(g_lg.test_tag_entry),
                        "trig,ov,perf,dev-test");
  gtk_widget_set_hexpand(g_lg.test_tag_entry, TRUE);

  gtk_box_append(GTK_BOX(param_row), gtk_label_new("gap(ms):"));
  gtk_box_append(GTK_BOX(param_row), g_lg.test_gap_spin);
  gtk_box_append(GTK_BOX(param_row), gtk_label_new("settle(ms):"));
  gtk_box_append(GTK_BOX(param_row), g_lg.test_settle_spin);
  gtk_box_append(GTK_BOX(param_row), gtk_label_new("repeat:"));
  gtk_box_append(GTK_BOX(param_row), g_lg.test_repeat_spin);
  gtk_box_append(GTK_BOX(param_row), gtk_label_new("tags:"));
  gtk_box_append(GTK_BOX(param_row), g_lg.test_tag_entry);
  gtk_box_append(GTK_BOX(page), param_row);

  GtkWidget* steps_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  g_lg.test_steps_entry = gtk_entry_new();
  gtk_widget_set_hexpand(g_lg.test_steps_entry, TRUE);
  gtk_editable_set_text(GTK_EDITABLE(g_lg.test_steps_entry),
                        "win_tap_os,sleep:500,win_tap_os");
  GtkWidget* run_steps_btn =
      gtk_button_new_with_label(tr("Run Steps", "运行步骤序列"));
  g_signal_connect(run_steps_btn, "clicked",
                   G_CALLBACK(on_test_custom_run_clicked), nullptr);
  gtk_box_append(GTK_BOX(steps_row), g_lg.test_steps_entry);
  gtk_box_append(GTK_BOX(steps_row), run_steps_btn);
  gtk_box_append(GTK_BOX(page), steps_row);

  g_lg.test_status_lbl = gtk_label_new(
      tr("Ready — runs asynchronously while WINOME stays live",
         "就绪 — 后台异步触发，保持 WINOME 实时运行并返回过程日志与帧率汇总"));
  gtk_widget_set_name(g_lg.test_status_lbl, "winomeLgDim");
  gtk_label_set_xalign(GTK_LABEL(g_lg.test_status_lbl), 0.0f);
  gtk_box_append(GTK_BOX(page), g_lg.test_status_lbl);

  GtkWidget* tv = gtk_text_view_new();
  gtk_widget_set_name(tv, "winomeLgMono");
  gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
  g_lg.test_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));

  GtkWidget* sc = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(sc, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), tv);
  gtk_box_append(GTK_BOX(page), sc);
  return page;
}

GtkWidget* build_console_page() {
  GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_name(page, "winomeLgBody");

  GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  g_lg.cmd_entry = gtk_entry_new();
  gtk_widget_set_hexpand(g_lg.cmd_entry, TRUE);
  gtk_entry_set_placeholder_text(
      GTK_ENTRY(g_lg.cmd_entry),
      tr("Command or JSON, e.g. status | test.run slow-double-win | test.trigger win_tap_os",
         "输入指令或 JSON，例如 status | test.run slow-double-win | test.trigger win_tap_os"));
  g_signal_connect(g_lg.cmd_entry, "activate",
                   G_CALLBACK(on_cmd_entry_activate), nullptr);

  GtkWidget* run_btn = gtk_button_new_with_label(tr("Execute", "执行"));
  g_signal_connect(run_btn, "clicked", G_CALLBACK(on_cmd_run_clicked), nullptr);
  gtk_box_append(GTK_BOX(bar), g_lg.cmd_entry);
  gtk_box_append(GTK_BOX(bar), run_btn);
  gtk_box_append(GTK_BOX(page), bar);

  GtkWidget* quick = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  static const char* const kQuickCmds[] = {
      "status", "overview.toggle", "test.run slow-double-win", "test.trigger win_tap_os", "inspect.windows", "theme.reload"};
  for (const char* qc : kQuickCmds) {
    GtkWidget* b = gtk_button_new_with_label(qc);
    g_signal_connect(b, "clicked", G_CALLBACK(on_quick_cmd_clicked),
                     const_cast<char*>(qc));
    gtk_box_append(GTK_BOX(quick), b);
  }
  gtk_box_append(GTK_BOX(page), quick);

  g_lg.cmd_timing_lbl = gtk_label_new(tr("Ready", "就绪"));
  gtk_widget_set_name(g_lg.cmd_timing_lbl, "winomeLgDim");
  gtk_label_set_xalign(GTK_LABEL(g_lg.cmd_timing_lbl), 0.0f);
  gtk_box_append(GTK_BOX(page), g_lg.cmd_timing_lbl);

  GtkWidget* tv = gtk_text_view_new();
  gtk_widget_set_name(tv, "winomeLgMono");
  gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
  g_lg.cmd_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));

  GtkWidget* sc = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(sc, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), tv);
  gtk_box_append(GTK_BOX(page), sc);
  return page;
}

GtkWidget* build_logs_page() {
  GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_name(page, "winomeLgBody");

  GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  const char* const tags[] = {
      "all", "perf", "ov", "ov-focus", "trig", "dev-test", "vd", "ipc", "abi",
      "plugin", "run", nullptr};
  g_lg.log_tag_drop = gtk_drop_down_new_from_strings(tags);
  g_signal_connect(g_lg.log_tag_drop, "notify::selected",
                   G_CALLBACK(on_log_filter_changed), nullptr);

  g_lg.log_pause_btn = gtk_toggle_button_new_with_label(tr("Pause", "暂停"));
  GtkWidget* clr_btn = gtk_button_new_with_label(tr("Clear", "清空"));
  g_signal_connect(clr_btn, "clicked", G_CALLBACK(on_log_clear_clicked), nullptr);

  gtk_box_append(GTK_BOX(bar), gtk_label_new(tr("Filter:", "模块过滤：")));
  gtk_box_append(GTK_BOX(bar), g_lg.log_tag_drop);
  gtk_box_append(GTK_BOX(bar), g_lg.log_pause_btn);
  gtk_box_append(GTK_BOX(bar), clr_btn);
  gtk_box_append(GTK_BOX(page), bar);

  GtkWidget* tv = gtk_text_view_new();
  gtk_widget_set_name(tv, "winomeLgMono");
  gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
  g_lg.log_view = tv;
  g_lg.log_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));

  GtkWidget* sc = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(sc, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), tv);
  gtk_box_append(GTK_BOX(page), sc);

  append_logs_incremental(true);
  g_lg.log_timer_id = g_timeout_add(300, on_log_poll_tick, nullptr);
  return page;
}

GtkWidget* build_inspector_page() {
  GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_name(page, "winomeLgBody");

  GtkWidget* tools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget* gtk_insp = gtk_button_new_with_label(
      tr("Open GTK Interactive Inspector", "打开 GTK4 交互式控件检查器"));
  g_signal_connect(gtk_insp, "clicked", G_CALLBACK(on_open_gtk_inspector), nullptr);
  GtkWidget* reload_btn = gtk_button_new_with_label(
      tr("Hot-Reload Theme & user-debug.css", "热重载主题与 user-debug.css"));
  g_signal_connect(reload_btn, "clicked", G_CALLBACK(on_reload_theme_clicked), nullptr);
  GtkWidget* dir_btn = gtk_button_new_with_label(
      tr("Open Config Dir", "打开配置目录"));
  g_signal_connect(dir_btn, "clicked", G_CALLBACK(on_open_dev_css_folder), nullptr);
  gtk_box_append(GTK_BOX(tools), gtk_insp);
  gtk_box_append(GTK_BOX(tools), reload_btn);
  gtk_box_append(GTK_BOX(tools), dir_btn);
  gtk_box_append(GTK_BOX(page), tools);

  GtkWidget* probe_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_name(probe_card, "winomeLgCard");

  GtkWidget* title = gtk_label_new(tr("StEngine Live Selector Probe", "StEngine 样式实时查询探针"));
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_box_append(GTK_BOX(probe_card), title);

  GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  g_lg.css_sel_entry = gtk_entry_new();
  gtk_widget_set_hexpand(g_lg.css_sel_entry, TRUE);
  gtk_editable_set_text(GTK_EDITABLE(g_lg.css_sel_entry),
                        "#panel .panel-button .system-status-icon");
  g_lg.css_prop_entry = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(g_lg.css_prop_entry), "icon-size");
  GtkWidget* probe_btn = gtk_button_new_with_label(tr("Lookup", "查询"));
  g_signal_connect(probe_btn, "clicked", G_CALLBACK(on_css_probe_clicked), nullptr);
  gtk_box_append(GTK_BOX(row), g_lg.css_sel_entry);
  gtk_box_append(GTK_BOX(row), g_lg.css_prop_entry);
  gtk_box_append(GTK_BOX(row), probe_btn);
  gtk_box_append(GTK_BOX(probe_card), row);

  g_lg.css_result_lbl = gtk_label_new("-");
  gtk_label_set_xalign(GTK_LABEL(g_lg.css_result_lbl), 0.0f);
  gtk_label_set_selectable(GTK_LABEL(g_lg.css_result_lbl), TRUE);
  gtk_box_append(GTK_BOX(probe_card), g_lg.css_result_lbl);
  gtk_box_append(GTK_BOX(page), probe_card);
  return page;
}

GtkWidget* build_windows_page() {
  GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_name(page, "winomeLgBody");

  GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget* ref_btn = gtk_button_new_with_label(tr("Refresh Windows", "刷新窗口列表"));
  g_signal_connect(ref_btn, "clicked", G_CALLBACK(on_windows_refresh_clicked), nullptr);
  GtkWidget* restack_btn = gtk_button_new_with_label(tr("Restack Z-Order", "强制重排 Z-Order"));
  g_signal_connect(restack_btn, "clicked", G_CALLBACK(on_windows_restack_clicked), nullptr);
  gtk_box_append(GTK_BOX(bar), ref_btn);
  gtk_box_append(GTK_BOX(bar), restack_btn);
  gtk_box_append(GTK_BOX(page), bar);

  g_lg.win_list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_name(g_lg.win_list_box, "winomeLgMono");

  GtkWidget* sc = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(sc, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), g_lg.win_list_box);
  gtk_box_append(GTK_BOX(page), sc);
  populate_windows_list();
  return page;
}

GtkWidget* build_flags_page() {
  GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_name(page, "winomeLgBody");

  GtkWidget* flags_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_name(flags_card, "winomeLgCard");
  const DebugFlags cur = debug_get_flags();
  struct FlagSpec {
    const char* key;
    const char* label;
    bool val;
  } specs[] = {
      {"zorder_trace", tr("Z-Order Restack Trace", "Z-Order 重排日志追踪"), cur.zorder_trace},
      {"hotkey_trace", tr("Hotkey / Trigger Trace", "热键与触发器日志追踪"), cur.hotkey_trace},
      {"ipc_trace", tr("IPC Request Trace", "IPC 指令请求追踪"), cur.ipc_trace},
      {"frame_overlay", tr("Animation Frame Telemetry", "概览帧耗时诊断标志"), cur.frame_overlay},
  };
  for (const auto& sp : specs) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget* lbl = gtk_label_new(sp.label);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_widget_set_hexpand(lbl, TRUE);
    GtkWidget* sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sw), sp.val ? TRUE : FALSE);
    g_signal_connect(sw, "state-set", G_CALLBACK(on_flag_toggled),
                     const_cast<char*>(sp.key));
    gtk_box_append(GTK_BOX(row), lbl);
    gtk_box_append(GTK_BOX(row), sw);
    gtk_box_append(GTK_BOX(flags_card), row);
  }
  gtk_box_append(GTK_BOX(page), flags_card);

  GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget* hdr = gtk_label_new(tr("Subsystems & Out-of-Process Plugins", "子系统熔断器与外部插件状态"));
  gtk_widget_set_hexpand(hdr, TRUE);
  gtk_label_set_xalign(GTK_LABEL(hdr), 0.0f);
  GtkWidget* ref = gtk_button_new_with_label(tr("Refresh", "刷新"));
  g_signal_connect(ref, "clicked", G_CALLBACK(on_subsys_refresh_clicked), nullptr);
  gtk_box_append(GTK_BOX(bar), hdr);
  gtk_box_append(GTK_BOX(bar), ref);
  gtk_box_append(GTK_BOX(page), bar);

  g_lg.subsys_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_box_append(GTK_BOX(page), g_lg.subsys_box);

  g_lg.plugins_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_name(g_lg.plugins_box, "winomeLgCard");
  gtk_box_append(GTK_BOX(page), g_lg.plugins_box);

  populate_subsys_and_plugins();
  return page;
}

}  // namespace

void looking_glass_init(const LookingGlassCallbacks& cbs) {
  g_lg.cbs = cbs;
}

bool open_looking_glass() {
  if (!get_config().developer_mode) {
    close_looking_glass();
    return false;
  }
  if (g_lg.window != nullptr) {
    gtk_window_present(GTK_WINDOW(g_lg.window));
    populate_windows_list();
    populate_subsys_and_plugins();
    return true;
  }

  ensure_lg_css();
  g_lg.alive = std::make_shared<std::atomic<bool>>(true);

  GtkWidget* win = gtk_window_new();
  g_lg.window = win;
  gtk_widget_set_name(win, "LookingGlassDialog");
  gtk_window_set_title(GTK_WINDOW(win),
                       tr("WINOME Developer Console (Looking Glass)",
                          "WINOME 开发者控制台 (Looking Glass)"));
  gtk_window_set_default_size(GTK_WINDOW(win), 920, 580);
  gtk_window_set_resizable(GTK_WINDOW(win), TRUE);
  install_bundled_fonts(win);

  GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child(GTK_WINDOW(win), root);

  GtkWidget* stack = gtk_stack_new();
  g_lg.stack = stack;
  gtk_stack_set_transition_type(GTK_STACK(stack),
                                GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_widget_set_hexpand(stack, TRUE);
  gtk_widget_set_vexpand(stack, TRUE);

  gtk_stack_add_titled(GTK_STACK(stack), build_console_page(),
                       "console", tr("Command", "指令台"));
  gtk_stack_add_titled(GTK_STACK(stack), build_tests_page(),
                       "tests", tr("Actions & Tests", "动作测试"));
  gtk_stack_add_titled(GTK_STACK(stack), build_logs_page(),
                       "logs", tr("Logs", "实时日志"));
  gtk_stack_add_titled(GTK_STACK(stack), build_inspector_page(),
                       "inspector", tr("Inspector", "样式与控件"));
  gtk_stack_add_titled(GTK_STACK(stack), build_windows_page(),
                       "windows", tr("Windows", "窗口层级"));
  gtk_stack_add_titled(GTK_STACK(stack), build_flags_page(),
                       "flags", tr("Plugins & Flags", "插件与熔断"));

  GtkWidget* hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_name(hdr, "winomeLgHeader");
  GtkWidget* switcher = gtk_stack_switcher_new();
  gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(stack));
  gtk_widget_set_hexpand(switcher, TRUE);
  gtk_box_append(GTK_BOX(hdr), switcher);

  gtk_box_append(GTK_BOX(root), hdr);
  gtk_box_append(GTK_BOX(root), stack);

  g_signal_connect(win, "destroy", G_CALLBACK(on_lg_destroy), nullptr);
  gtk_widget_realize(win);
  apply_dark_frame(win);
  gtk_window_present(GTK_WINDOW(win));
  return true;
}

void close_looking_glass() {
  if (g_lg.window != nullptr)
    gtk_window_destroy(GTK_WINDOW(g_lg.window));
}

bool looking_glass_is_open() {
  return g_lg.window != nullptr;
}

void refresh_looking_glass_plugins() {
  if (g_lg.window != nullptr)
    populate_subsys_and_plugins();
}

}  // namespace winome
