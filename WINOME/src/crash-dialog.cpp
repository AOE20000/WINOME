// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors

#include "crash-dialog.h"

#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace winome {
namespace {

constexpr wchar_t kCrashWndClass[] = L"WINOME_CrashRecoveryDialog";
constexpr UINT kDwmDarkModeAttr = 20;

enum CtrlId : int {
  IDC_STATUS_LABEL = 1001,
  IDC_DETAILS_EDIT = 1002,
  IDC_BTN_RESTORE = 2001,
  IDC_BTN_SAFE_RESTART = 2002,
  IDC_BTN_RESTART = 2003,
  IDC_BTN_OPEN_DIR = 2004,
  IDC_BTN_CLOSE = 2005,
};

void append_json_escaped(const std::string& s, std::string* out) {
  out->push_back('"');
  for (unsigned char ch : s) {
    switch (ch) {
      case '"':  *out += "\\\""; break;
      case '\\': *out += "\\\\"; break;
      case '\b': *out += "\\b"; break;
      case '\f': *out += "\\f"; break;
      case '\n': *out += "\\n"; break;
      case '\r': *out += "\\r"; break;
      case '\t': *out += "\\t"; break;
      default:
        if (ch < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
          *out += buf;
        } else {
          out->push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  out->push_back('"');
}

void skip_ws(const std::string& s, size_t* pos) {
  while (*pos < s.size() &&
         std::isspace(static_cast<unsigned char>(s[*pos]))) {
    ++(*pos);
  }
}

bool parse_json_str(const std::string& s, size_t* pos, std::string* out) {
  skip_ws(s, pos);
  if (*pos >= s.size() || s[*pos] != '"')
    return false;
  ++(*pos);
  std::string res;
  while (*pos < s.size()) {
    char ch = s[(*pos)++];
    if (ch == '"') {
      *out = std::move(res);
      return true;
    }
    if (ch == '\\') {
      if (*pos >= s.size())
        return false;
      char esc = s[(*pos)++];
      switch (esc) {
        case '"':  res.push_back('"'); break;
        case '\\': res.push_back('\\'); break;
        case '/':  res.push_back('/'); break;
        case 'b':  res.push_back('\b'); break;
        case 'f':  res.push_back('\f'); break;
        case 'n':  res.push_back('\n'); break;
        case 'r':  res.push_back('\r'); break;
        case 't':  res.push_back('\t'); break;
        default:   res.push_back(esc); break;
      }
    } else {
      res.push_back(ch);
    }
  }
  return false;
}

bool parse_json_bool(const std::string& s, size_t* pos, bool* out) {
  skip_ws(s, pos);
  if (s.compare(*pos, 4, "true") == 0) {
    *pos += 4;
    *out = true;
    return true;
  }
  if (s.compare(*pos, 5, "false") == 0) {
    *pos += 5;
    *out = false;
    return true;
  }
  return false;
}

bool parse_json_int64(const std::string& s, size_t* pos, int64_t* out) {
  skip_ws(s, pos);
  size_t start = *pos;
  if (*pos < s.size() && (s[*pos] == '-' || s[*pos] == '+'))
    ++(*pos);
  size_t d0 = *pos;
  while (*pos < s.size() && std::isdigit(static_cast<unsigned char>(s[*pos])))
    ++(*pos);
  if (*pos == d0)
    return false;
  *out = std::strtoll(s.substr(start, *pos - start).c_str(), nullptr, 10);
  return true;
}

bool skip_json_val(const std::string& s, size_t* pos) {
  skip_ws(s, pos);
  if (*pos >= s.size())
    return false;
  char c = s[*pos];
  if (c == '"') {
    std::string dummy;
    return parse_json_str(s, pos, &dummy);
  }
  if (c == '{' || c == '[') {
    char open = c;
    char close = (c == '{') ? '}' : ']';
    int depth = 1;
    ++(*pos);
    while (*pos < s.size() && depth > 0) {
      if (s[*pos] == '"') {
        std::string dummy;
        if (!parse_json_str(s, pos, &dummy))
          return false;
        continue;
      }
      if (s[*pos] == open)
        ++depth;
      else if (s[*pos] == close)
        --depth;
      ++(*pos);
    }
    return depth == 0;
  }
  while (*pos < s.size() && s[*pos] != ',' && s[*pos] != '}' &&
         !std::isspace(static_cast<unsigned char>(s[*pos]))) {
    ++(*pos);
  }
  return true;
}

void write_json_escaped_cstr(const char* src, char* dst, size_t cap, size_t* idx) {
  if (dst == nullptr || cap == 0 || *idx + 2 >= cap)
    return;
  dst[(*idx)++] = '"';
  if (src != nullptr) {
    for (size_t i = 0; src[i] != '\0' && *idx + 8 < cap; ++i) {
      unsigned char ch = static_cast<unsigned char>(src[i]);
      if (ch == '"' || ch == '\\') {
        dst[(*idx)++] = '\\';
        dst[(*idx)++] = static_cast<char>(ch);
      } else if (ch == '\n') {
        dst[(*idx)++] = '\\';
        dst[(*idx)++] = 'n';
      } else if (ch == '\r') {
        dst[(*idx)++] = '\\';
        dst[(*idx)++] = 'r';
      } else if (ch >= 0x20) {
        dst[(*idx)++] = static_cast<char>(ch);
      }
    }
  }
  dst[(*idx)++] = '"';
}

std::wstring utf8_to_wide(const std::string& utf8) {
  if (utf8.empty())
    return {};
  int need = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                 static_cast<int>(utf8.size()), nullptr, 0);
  if (need <= 0)
    return {};
  std::wstring out(static_cast<size_t>(need), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                      static_cast<int>(utf8.size()), out.data(), need);
  return out;
}

std::string to_crlf_preview(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 32);
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r'))
      out.push_back('\r');
    out.push_back(s[i]);
  }
  return out;
}

struct DialogRuntime {
  const CrashRecoveryDialogInput* input = nullptr;
  CrashDialogAction action = CrashDialogAction::ExitKeepTaskbar;
  bool restored = false;
  HWND status_hwnd = nullptr;
  HFONT font_regular = nullptr;
  HFONT font_bold = nullptr;
  int dpi = 96;
};

int scale_px(int px, int dpi) {
  return MulDiv(px, dpi, 96);
}

std::wstring format_status_text(bool restored, bool zh) {
  if (zh) {
    return restored
               ? L"[\x2713] Windows \u539f\u751f\u4efb\u52a1\u680f\u4e0e\u684c\u9762\u5de5\u4f5c\u533a\u5df2\u81ea\u52a8\u8fd8\u539f\u3002"
               : L"[!] \u672a\u68c0\u6d4b\u5230\u53ef\u89c1\u4efb\u52a1\u680f\uff0c\u8bf7\u70b9\u51fb\u4e0b\u65b9\u300c\u8fd8\u539f\u4efb\u52a1\u680f\u4e0e\u5de5\u4f5c\u533a\u300d\u91cd\u8bd5\u3002";
  }
  return restored
             ? L"[OK] Windows native taskbar and desktop work area have been restored."
             : L"[!] Native taskbar visibility was not confirmed yet. Click 'Restore Taskbar' below.";
}

std::wstring build_details_text(const CrashState& st, bool zh) {
  std::string text;
  char hdr[512];
  std::snprintf(hdr, sizeof(hdr),
                "%s: %s\n"
                "%s: %lu (0x%08lX)   |   PID: %lu   |   Safe Mode: %s\n"
                "%s: %s\n"
                "%s: %s\n",
                zh ? "异常原因" : "Reason",
                st.reason.empty() ? (zh ? "进程异常终止" : "Unexpected termination")
                                  : st.reason.c_str(),
                zh ? "退出/异常码" : "Code",
                static_cast<unsigned long>(st.exit_code),
                static_cast<unsigned long>(st.exit_code),
                static_cast<unsigned long>(st.pid),
                st.safe_mode ? "true" : "false",
                zh ? "发生时间(UTC)" : "Timestamp (UTC)",
                st.timestamp_utc.empty() ? "-" : st.timestamp_utc.c_str(),
                zh ? "转储文件" : "Minidump",
                st.minidump_path.empty() ? "-" : st.minidump_path.c_str());
  text += hdr;
  if (!st.log_tail.empty()) {
    text += zh ? "\n--- 最近运行日志 ---\n" : "\n--- Recent Log Tail ---\n";
    text += st.log_tail;
  }
  return utf8_to_wide(to_crlf_preview(text));
}

LRESULT CALLBACK crash_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  auto* rt = reinterpret_cast<DialogRuntime*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_CREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
      rt = reinterpret_cast<DialogRuntime*>(cs->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(rt));
      const int dpi = rt->dpi;
      const bool zh = rt->input->zh;

      BOOL dark = TRUE;
      DwmSetWindowAttribute(hwnd, kDwmDarkModeAttr, &dark, sizeof(dark));

      NONCLIENTMETRICSW ncm{};
      ncm.cbSize = sizeof(ncm);
      if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        ncm.lfMessageFont.lfHeight = -scale_px(13, dpi);
        rt->font_regular = CreateFontIndirectW(&ncm.lfMessageFont);
        ncm.lfMessageFont.lfHeight = -scale_px(14, dpi);
        ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
        rt->font_bold = CreateFontIndirectW(&ncm.lfMessageFont);
      }

      HINSTANCE inst = cs->hInstance;
      const int pad = scale_px(16, dpi);
      const int w = scale_px(640, dpi);
      const int inner_w = w - 2 * pad;

      std::wstring status_txt = format_status_text(rt->restored, zh);
      rt->status_hwnd = CreateWindowExW(
          0, L"STATIC", status_txt.c_str(),
          WS_CHILD | WS_VISIBLE,
          pad, pad, inner_w, scale_px(26, dpi),
          hwnd, reinterpret_cast<HMENU>(IDC_STATUS_LABEL), inst, nullptr);
      if (rt->font_bold)
        SendMessageW(rt->status_hwnd, WM_SETFONT,
                     reinterpret_cast<WPARAM>(rt->font_bold), TRUE);

      std::wstring hint_txt =
          zh ? L"WINOME 桌面外壳意外退出，您可以从下方立即恢复桌面、打开诊断或以安全模式重启："
             : L"WINOME exited unexpectedly. Use the recovery actions below to restore or restart:";
      HWND hint_hwnd = CreateWindowExW(
          0, L"STATIC", hint_txt.c_str(),
          WS_CHILD | WS_VISIBLE,
          pad, scale_px(44, dpi), inner_w, scale_px(22, dpi),
          hwnd, nullptr, inst, nullptr);
      if (rt->font_regular)
        SendMessageW(hint_hwnd, WM_SETFONT,
                     reinterpret_cast<WPARAM>(rt->font_regular), TRUE);

      std::wstring details_txt = build_details_text(rt->input->state, zh);
      HWND edit_hwnd = CreateWindowExW(
          WS_EX_CLIENTEDGE, L"EDIT", details_txt.c_str(),
          WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE |
              ES_READONLY | ES_AUTOVSCROLL,
          pad, scale_px(72, dpi), inner_w, scale_px(250, dpi),
          hwnd, reinterpret_cast<HMENU>(IDC_DETAILS_EDIT), inst, nullptr);
      if (rt->font_regular)
        SendMessageW(edit_hwnd, WM_SETFONT,
                     reinterpret_cast<WPARAM>(rt->font_regular), TRUE);

      const int btn_h = scale_px(32, dpi);
      const int row1_y = scale_px(336, dpi);
      const int row2_y = scale_px(376, dpi);
      const int col3_w = (inner_w - scale_px(16, dpi)) / 3;
      const int col2_w = (inner_w - scale_px(8, dpi)) / 2;

      auto add_btn = [&](int id, const wchar_t* label, int x, int y, int bw) {
        HWND b = CreateWindowExW(
            0, L"BUTTON", label,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            x, y, bw, btn_h,
            hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(id)),
            inst, nullptr);
        if (rt->font_regular)
          SendMessageW(b, WM_SETFONT,
                       reinterpret_cast<WPARAM>(rt->font_regular), TRUE);
      };

      add_btn(IDC_BTN_RESTORE,
              zh ? L"还原任务栏与工作区" : L"Restore Taskbar",
              pad, row1_y, col3_w);
      add_btn(IDC_BTN_SAFE_RESTART,
              zh ? L"安全模式重启 (禁插件)" : L"Restart Safe Mode",
              pad + col3_w + scale_px(8, dpi), row1_y, col3_w);
      add_btn(IDC_BTN_RESTART,
              zh ? L"正常重启 WINOME" : L"Restart WINOME",
              pad + 2 * (col3_w + scale_px(8, dpi)), row1_y, col3_w);

      add_btn(IDC_BTN_OPEN_DIR,
              zh ? L"打开日志与转储目录" : L"Open Logs & Crashes",
              pad, row2_y, col2_w);
      add_btn(IDC_BTN_CLOSE,
              zh ? L"关闭（保留原生任务栏）" : L"Close (Keep Taskbar)",
              pad + col2_w + scale_px(8, dpi), row2_y, col2_w);
      return 0;
    }
    case WM_COMMAND: {
      if (rt == nullptr)
        break;
      const int id = LOWORD(wParam);
      if (id == IDC_BTN_RESTORE) {
        if (rt->input->on_restore_now != nullptr) {
          rt->restored = rt->input->on_restore_now(rt->input->user_data);
          std::wstring status_txt = format_status_text(rt->restored, rt->input->zh);
          SetWindowTextW(rt->status_hwnd, status_txt.c_str());
        }
        return 0;
      }
      if (id == IDC_BTN_SAFE_RESTART) {
        rt->action = CrashDialogAction::RestartSafeMode;
        DestroyWindow(hwnd);
        return 0;
      }
      if (id == IDC_BTN_RESTART) {
        rt->action = CrashDialogAction::RestartNormal;
        DestroyWindow(hwnd);
        return 0;
      }
      if (id == IDC_BTN_OPEN_DIR) {
        if (!rt->input->data_dir.empty()) {
          ShellExecuteW(hwnd, L"open", rt->input->data_dir.c_str(),
                        nullptr, nullptr, SW_SHOWNORMAL);
        }
        return 0;
      }
      if (id == IDC_BTN_CLOSE || id == IDCANCEL) {
        rt->action = CrashDialogAction::ExitKeepTaskbar;
        DestroyWindow(hwnd);
        return 0;
      }
      break;
    }
    case WM_CLOSE:
      if (rt != nullptr)
        rt->action = CrashDialogAction::ExitKeepTaskbar;
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      if (rt != nullptr) {
        if (rt->font_regular) {
          DeleteObject(rt->font_regular);
          rt->font_regular = nullptr;
        }
        if (rt->font_bold) {
          DeleteObject(rt->font_bold);
          rt->font_bold = nullptr;
        }
      }
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

std::string serialize_recovery_snapshot(const RecoverySnapshot& snap) {
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "{\"version\":1,\"taskbar_state\":%d,"
                "\"work\":[%ld,%ld,%ld,%ld]}\n",
                static_cast<int>(snap.taskbar_state),
                snap.primary_work.left, snap.primary_work.top,
                snap.primary_work.right, snap.primary_work.bottom);
  return buf;
}

bool parse_recovery_snapshot(const std::string& raw, RecoverySnapshot* out) {
  if (out == nullptr || raw.empty())
    return false;

  // Backward compatibility with legacy raw binary RECT recovery.dat.
  if (raw.size() == sizeof(RECT) && raw[0] != '{') {
    RECT rc{};
    std::memcpy(&rc, raw.data(), sizeof(rc));
    if (rc.right <= rc.left || rc.bottom <= rc.top)
      return false;
    out->valid = true;
    out->taskbar_state = 2;  // ABS_ALWAYSONTOP
    out->primary_work = rc;
    return true;
  }

  RecoverySnapshot parsed;
  size_t pos = 0;
  skip_ws(raw, &pos);
  if (pos >= raw.size() || raw[pos] != '{')
    return false;
  ++pos;
  skip_ws(raw, &pos);
  if (pos < raw.size() && raw[pos] == '}')
    return false;

  bool has_work = false;
  for (;;) {
    std::string key;
    if (!parse_json_str(raw, &pos, &key))
      return false;
    skip_ws(raw, &pos);
    if (pos >= raw.size() || raw[pos] != ':')
      return false;
    ++pos;

    bool ok = true;
    if (key == "taskbar_state") {
      int64_t v = 2;
      ok = parse_json_int64(raw, &pos, &v);
      parsed.taskbar_state = static_cast<int32_t>(v);
    } else if (key == "work") {
      skip_ws(raw, &pos);
      if (pos >= raw.size() || raw[pos] != '[') {
        ok = false;
      } else {
        ++pos;
        int64_t vals[4]{};
        for (int i = 0; i < 4; ++i) {
          if (!parse_json_int64(raw, &pos, &vals[i])) {
            ok = false;
            break;
          }
          skip_ws(raw, &pos);
          if (i < 3) {
            if (pos >= raw.size() || raw[pos] != ',') {
              ok = false;
              break;
            }
            ++pos;
          }
        }
        if (ok) {
          skip_ws(raw, &pos);
          if (pos >= raw.size() || raw[pos] != ']') {
            ok = false;
          } else {
            ++pos;
            parsed.primary_work.left = static_cast<LONG>(vals[0]);
            parsed.primary_work.top = static_cast<LONG>(vals[1]);
            parsed.primary_work.right = static_cast<LONG>(vals[2]);
            parsed.primary_work.bottom = static_cast<LONG>(vals[3]);
            has_work = (parsed.primary_work.right > parsed.primary_work.left &&
                        parsed.primary_work.bottom > parsed.primary_work.top);
          }
        }
      }
    } else {
      ok = skip_json_val(raw, &pos);
    }
    if (!ok)
      return false;
    skip_ws(raw, &pos);
    if (pos < raw.size() && raw[pos] == ',') {
      ++pos;
      continue;
    }
    if (pos < raw.size() && raw[pos] == '}') {
      ++pos;
      break;
    }
    return false;
  }

  parsed.valid = has_work;
  if (!parsed.valid)
    return false;
  *out = parsed;
  return true;
}

std::string serialize_crash_state(const CrashState& state) {
  std::string out = "{\"reason\":";
  append_json_escaped(state.reason, &out);
  char num[128];
  std::snprintf(num, sizeof(num), ",\"exit_code\":%lu,\"pid\":%lu,",
                static_cast<unsigned long>(state.exit_code),
                static_cast<unsigned long>(state.pid));
  out += num;
  out += "\"timestamp_utc\":";
  append_json_escaped(state.timestamp_utc, &out);
  out += ",\"minidump_path\":";
  append_json_escaped(state.minidump_path, &out);
  out += ",\"log_tail\":";
  append_json_escaped(state.log_tail, &out);
  out += ",\"taskbar_restored\":";
  out += state.taskbar_restored ? "true" : "false";
  out += ",\"safe_mode\":";
  out += state.safe_mode ? "true" : "false";
  out += "}\n";
  return out;
}

bool parse_crash_state(const std::string& json, CrashState* out) {
  if (out == nullptr)
    return false;
  CrashState parsed;
  size_t pos = 0;
  skip_ws(json, &pos);
  if (pos >= json.size() || json[pos] != '{')
    return false;
  ++pos;
  skip_ws(json, &pos);
  if (pos < json.size() && json[pos] == '}') {
    *out = parsed;
    return true;
  }

  for (;;) {
    std::string key;
    if (!parse_json_str(json, &pos, &key))
      return false;
    skip_ws(json, &pos);
    if (pos >= json.size() || json[pos] != ':')
      return false;
    ++pos;

    bool ok = true;
    if (key == "reason") {
      ok = parse_json_str(json, &pos, &parsed.reason);
    } else if (key == "exit_code") {
      int64_t v = 0;
      ok = parse_json_int64(json, &pos, &v);
      parsed.exit_code = static_cast<uint32_t>(std::max<int64_t>(0, v));
    } else if (key == "pid") {
      int64_t v = 0;
      ok = parse_json_int64(json, &pos, &v);
      parsed.pid = static_cast<uint32_t>(std::max<int64_t>(0, v));
    } else if (key == "timestamp_utc") {
      ok = parse_json_str(json, &pos, &parsed.timestamp_utc);
    } else if (key == "minidump_path") {
      ok = parse_json_str(json, &pos, &parsed.minidump_path);
    } else if (key == "log_tail") {
      ok = parse_json_str(json, &pos, &parsed.log_tail);
    } else if (key == "taskbar_restored") {
      ok = parse_json_bool(json, &pos, &parsed.taskbar_restored);
    } else if (key == "safe_mode") {
      ok = parse_json_bool(json, &pos, &parsed.safe_mode);
    } else {
      ok = skip_json_val(json, &pos);
    }
    if (!ok)
      return false;
    skip_ws(json, &pos);
    if (pos < json.size() && json[pos] == ',') {
      ++pos;
      continue;
    }
    if (pos < json.size() && json[pos] == '}') {
      ++pos;
      break;
    }
    return false;
  }

  *out = std::move(parsed);
  return true;
}

void write_crash_state_emergency_file(const wchar_t* path,
                                      const char* reason,
                                      uint32_t code,
                                      uint32_t pid,
                                      const char* minidump_utf8,
                                      bool safe_mode) {
  if (path == nullptr || path[0] == L'\0')
    return;

  SYSTEMTIME st{};
  GetSystemTime(&st);
  char ts[64];
  std::snprintf(ts, sizeof(ts), "%04u-%02u-%02uT%02u:%02u:%02uZ",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

  char buf[2048];
  size_t idx = 0;
  auto append_lit = [&](const char* lit) {
    while (*lit && idx + 4 < sizeof(buf))
      buf[idx++] = *lit++;
  };

  append_lit("{\"reason\":");
  write_json_escaped_cstr(reason, buf, sizeof(buf), &idx);
  char nums[128];
  std::snprintf(nums, sizeof(nums), ",\"exit_code\":%lu,\"pid\":%lu,",
                static_cast<unsigned long>(code),
                static_cast<unsigned long>(pid));
  append_lit(nums);
  append_lit("\"timestamp_utc\":");
  write_json_escaped_cstr(ts, buf, sizeof(buf), &idx);
  append_lit(",\"minidump_path\":");
  write_json_escaped_cstr(minidump_utf8, buf, sizeof(buf), &idx);
  append_lit(",\"safe_mode\":");
  append_lit(safe_mode ? "true" : "false");
  append_lit("}\n");

  HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(file, buf, static_cast<DWORD>(idx), &written, nullptr);
    CloseHandle(file);
  }
}

std::string read_log_tail_utf8(const std::wstring& log_path, size_t max_bytes) {
  if (log_path.empty() || max_bytes == 0)
    return {};
  HANDLE file = CreateFileW(log_path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return {};

  LARGE_INTEGER sz{};
  std::string out;
  if (GetFileSizeEx(file, &sz) && sz.QuadPart > 0) {
    const LONGLONG want = static_cast<LONGLONG>(max_bytes);
    const LONGLONG start = (sz.QuadPart > want) ? (sz.QuadPart - want) : 0;
    LARGE_INTEGER offset{};
    offset.QuadPart = start;
    if (SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)) {
      const DWORD to_read = static_cast<DWORD>(sz.QuadPart - start);
      out.resize(to_read);
      DWORD got = 0;
      if (ReadFile(file, out.data(), to_read, &got, nullptr)) {
        out.resize(got);
        if (start > 0) {
          size_t nl = out.find('\n');
          if (nl != std::string::npos && nl + 1 < out.size())
            out.erase(0, nl + 1);
        }
      } else {
        out.clear();
      }
    }
  }
  CloseHandle(file);
  return out;
}

CrashDialogAction show_crash_recovery_dialog(
    const CrashRecoveryDialogInput& input) {
  HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = crash_wnd_proc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(nullptr, IDI_WARNING);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kCrashWndClass;
  RegisterClassExW(&wc);

  DialogRuntime rt;
  rt.input = &input;
  rt.restored = input.state.taskbar_restored;
  UINT sys_dpi = 96;
  HDC screen_dc = GetDC(nullptr);
  if (screen_dc != nullptr) {
    sys_dpi = static_cast<UINT>(std::max(96, GetDeviceCaps(screen_dc, LOGPIXELSX)));
    ReleaseDC(nullptr, screen_dc);
  }
  rt.dpi = static_cast<int>(sys_dpi);

  const int client_w = scale_px(640, rt.dpi);
  const int client_h = scale_px(424, rt.dpi);
  RECT wr{0, 0, client_w, client_h};
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
  AdjustWindowRectEx(&wr, style, FALSE, WS_EX_TOPMOST);
  const int win_w = wr.right - wr.left;
  const int win_h = wr.bottom - wr.top;

  RECT wa{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
  const int x = wa.left + ((wa.right - wa.left) - win_w) / 2;
  const int y = wa.top + ((wa.bottom - wa.top) - win_h) / 2;

  const wchar_t* title = input.zh
                             ? L"WINOME \u5f02\u5e38\u6062\u590d\u4e0e\u64cd\u4f5c\u7a97\u53e3"
                             : L"WINOME Crash Recovery & Actions";
  HWND hwnd = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_APPWINDOW,
      kCrashWndClass, title, style,
      x, y, win_w, win_h,
      nullptr, nullptr, inst, &rt);
  if (hwnd != nullptr) {
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      if (!IsDialogMessageW(hwnd, &msg)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    }
  }

  UnregisterClassW(kCrashWndClass, inst);
  return rt.action;
}

}  // namespace winome
