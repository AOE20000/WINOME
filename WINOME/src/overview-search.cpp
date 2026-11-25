// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Dedicated GNOME Shell SearchResultsView & SearchController port
// (reference: upstream/gnome-shell/js/ui/search.js, searchController.js,
// appDisplay.js AppSearchProvider, misc/systemActions.js, misc/util.js).

#include "overview-search.h"

#include "app-index.h"
#include "end-session-dialog.h"
#include "i18n.h"
#include "monitor-manager.h"
#include "system-status.h"

#include <shlobj.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace winome {

namespace {

constexpr int kMaxGridResults = 6;
constexpr int kMaxListResults = 5;        // search.js MAX_LIST_SEARCH_RESULTS_ROWS
constexpr int kProviderIconSize = 32;     // search.js ProviderInfo.PROVIDER_ICON_SIZE
constexpr int kListItemIconSize = 24;     // search.js ListSearchResult.ICON_SIZE
constexpr guint kAsyncSearchDebounceMs = 90;
constexpr int kMaxFileScanEntries = 1400;

std::string
utf8_lower_local (const std::string &s)
{
  if (s.empty ())
    return {};
  gchar *down = g_utf8_strdown (s.c_str (), static_cast<gssize> (s.size ()));
  if (down == nullptr)
    return {};
  std::string out (down);
  g_free (down);
  return out;
}

std::string
trim_utf8 (const std::string &s)
{
  size_t b = 0;
  while (b < s.size () && std::isspace (static_cast<unsigned char> (s[b])))
    ++b;
  size_t e = s.size ();
  while (e > b && std::isspace (static_cast<unsigned char> (s[e - 1])))
    --e;
  return s.substr (b, e - b);
}

std::string
url_encode_utf8 (const std::string &s)
{
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve (s.size () * 3);
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      out.push_back (static_cast<char> (c));
    } else if (c == ' ') {
      out.push_back ('+');
    } else {
      out.push_back ('%');
      out.push_back (kHex[(c >> 4) & 0xF]);
      out.push_back (kHex[c & 0xF]);
    }
  }
  return out;
}

void
copy_utf8_to_clipboard (const std::string &text)
{
  std::wstring wide = app_utf8_to_wide (text);
  if (wide.empty ())
    return;
  if (!OpenClipboard (nullptr))
    return;
  EmptyClipboard ();
  const size_t bytes = (wide.size () + 1) * sizeof (wchar_t);
  HGLOBAL hmem = GlobalAlloc (GMEM_MOVEABLE, bytes);
  if (hmem != nullptr) {
    void *dst = GlobalLock (hmem);
    if (dst != nullptr) {
      std::memcpy (dst, wide.c_str (), bytes);
      GlobalUnlock (hmem);
      SetClipboardData (CF_UNICODETEXT, hmem);
    } else {
      GlobalFree (hmem);
    }
  }
  CloseClipboard ();
}

void
shell_open_utf8 (const std::string &target, HMONITOR target_monitor = nullptr)
{
  if (target.empty ())
    return;
  std::wstring wide = app_utf8_to_wide (target);
  AllowSetForegroundWindow (ASFW_ANY);
  HMONITOR mon = (target_monitor != nullptr)
                     ? target_monitor
                     : monitor_manager_active_monitor ();
  watch_launched_window_for_monitor (mon);
  SHELLEXECUTEINFOW sei {};
  sei.cbSize = sizeof (sei);
  sei.fMask = SEE_MASK_FLAG_NO_UI;
  if (mon != nullptr) {
    sei.fMask |= SEE_MASK_HMONITOR;
    sei.hMonitor = mon;
  }
  sei.lpVerb = L"open";
  sei.lpFile = wide.c_str ();
  sei.nShow = SW_SHOWNORMAL;
  ShellExecuteExW (&sei);
}

void
on_end_session_confirmed (EndSessionAction action, void *user_data G_GNUC_UNUSED)
{
  switch (action) {
    case EndSessionAction::PowerOff:
      shutdown_system ();
      break;
    case EndSessionAction::Restart:
      restart_system ();
      break;
    case EndSessionAction::LogOut:
      log_out ();
      break;
  }
}

// --- Scientific / Arithmetic / Hex / Binary Expression Parser ----------------

class MathParser {
public:
  explicit MathParser (const std::string &s) : s_ (s) {}

  bool parse (double *out, bool *was_hex_or_bin) {
    skip_ws ();
    double v = 0.0;
    if (!parse_expr (&v))
      return false;
    skip_ws ();
    if (pos_ != s_.size () || !std::isfinite (v))
      return false;
    if (!has_op_ && !has_func_ && !has_radix_)
      return false;
    *out = v;
    if (was_hex_or_bin != nullptr)
      *was_hex_or_bin = has_radix_;
    return true;
  }

private:
  void skip_ws () {
    while (pos_ < s_.size () &&
           std::isspace (static_cast<unsigned char> (s_[pos_]))) {
      ++pos_;
    }
  }

  bool parse_expr (double *out) {
    double lhs = 0.0;
    if (!parse_term (&lhs))
      return false;
    while (true) {
      skip_ws ();
      if (pos_ < s_.size () && (s_[pos_] == '+' || s_[pos_] == '-')) {
        char op = s_[pos_++];
        has_op_ = true;
        double rhs = 0.0;
        if (!parse_term (&rhs))
          return false;
        lhs = (op == '+') ? (lhs + rhs) : (lhs - rhs);
      } else {
        break;
      }
    }
    *out = lhs;
    return true;
  }

  bool parse_term (double *out) {
    double lhs = 0.0;
    if (!parse_power (&lhs))
      return false;
    while (true) {
      skip_ws ();
      if (pos_ < s_.size () &&
          (s_[pos_] == '*' || s_[pos_] == '/' || s_[pos_] == '%')) {
        char op = s_[pos_++];
        has_op_ = true;
        double rhs = 0.0;
        if (!parse_power (&rhs))
          return false;
        if (op == '*') {
          lhs *= rhs;
        } else if (op == '/') {
          if (std::fabs (rhs) < 1e-15)
            return false;
          lhs /= rhs;
        } else {
          if (std::fabs (rhs) < 1e-15)
            return false;
          lhs = std::fmod (lhs, rhs);
        }
      } else {
        break;
      }
    }
    *out = lhs;
    return true;
  }

  bool parse_power (double *out) {
    double base = 0.0;
    if (!parse_unary (&base))
      return false;
    skip_ws ();
    if (pos_ < s_.size () && s_[pos_] == '^') {
      ++pos_;
      has_op_ = true;
      double exp = 0.0;
      if (!parse_power (&exp))
        return false;
      base = std::pow (base, exp);
    }
    *out = base;
    return true;
  }

  bool parse_unary (double *out) {
    skip_ws ();
    if (pos_ >= s_.size ())
      return false;
    if (s_[pos_] == '+') {
      ++pos_;
      return parse_unary (out);
    }
    if (s_[pos_] == '-') {
      ++pos_;
      double v = 0.0;
      if (!parse_unary (&v))
        return false;
      *out = -v;
      return true;
    }
    return parse_primary (out);
  }

  bool parse_primary (double *out) {
    skip_ws ();
    if (pos_ >= s_.size ())
      return false;
    if (s_[pos_] == '(') {
      ++pos_;
      double v = 0.0;
      if (!parse_expr (&v))
        return false;
      skip_ws ();
      if (pos_ >= s_.size () || s_[pos_] != ')')
        return false;
      ++pos_;
      *out = v;
      return true;
    }

    // Hex (0x...) or Binary (0b...) integer literal.
    if (pos_ + 2 < s_.size () && s_[pos_] == '0' &&
        (s_[pos_ + 1] == 'x' || s_[pos_ + 1] == 'X' || s_[pos_ + 1] == 'b' ||
         s_[pos_ + 1] == 'B')) {
      const bool is_hex = (s_[pos_ + 1] == 'x' || s_[pos_ + 1] == 'X');
      pos_ += 2;
      size_t start = pos_;
      uint64_t val = 0;
      while (pos_ < s_.size ()) {
        char c = s_[pos_];
        int digit = -1;
        if (c >= '0' && c <= '9')
          digit = c - '0';
        else if (is_hex && c >= 'a' && c <= 'f')
          digit = 10 + (c - 'a');
        else if (is_hex && c >= 'A' && c <= 'F')
          digit = 10 + (c - 'A');
        if (digit < 0 || (!is_hex && digit > 1))
          break;
        val = val * (is_hex ? 16ULL : 2ULL) + static_cast<uint64_t> (digit);
        ++pos_;
      }
      if (pos_ == start)
        return false;
      has_radix_ = true;
      *out = static_cast<double> (val);
      return true;
    }

    // Identifier: constant (pi, e) or math function (sqrt, sin, cos, ...).
    if (std::isalpha (static_cast<unsigned char> (s_[pos_]))) {
      size_t start = pos_;
      while (pos_ < s_.size () &&
             std::isalpha (static_cast<unsigned char> (s_[pos_]))) {
        ++pos_;
      }
      std::string ident = utf8_lower_local (s_.substr (start, pos_ - start));
      if (ident == "pi") {
        *out = 3.14159265358979323846;
        return true;
      }
      if (ident == "e") {
        *out = 2.71828182845904523536;
        return true;
      }
      skip_ws ();
      if (pos_ >= s_.size () || s_[pos_] != '(')
        return false;
      ++pos_;
      double arg = 0.0;
      if (!parse_expr (&arg))
        return false;
      skip_ws ();
      if (pos_ >= s_.size () || s_[pos_] != ')')
        return false;
      ++pos_;
      has_func_ = true;
      if (ident == "sqrt") {
        if (arg < 0.0)
          return false;
        *out = std::sqrt (arg);
        return true;
      }
      if (ident == "abs") {
        *out = std::fabs (arg);
        return true;
      }
      if (ident == "sin") {
        *out = std::sin (arg);
        return true;
      }
      if (ident == "cos") {
        *out = std::cos (arg);
        return true;
      }
      if (ident == "tan") {
        *out = std::tan (arg);
        return true;
      }
      if (ident == "ln") {
        if (arg <= 0.0)
          return false;
        *out = std::log (arg);
        return true;
      }
      if (ident == "log") {
        if (arg <= 0.0)
          return false;
        *out = std::log10 (arg);
        return true;
      }
      if (ident == "round") {
        *out = std::round (arg);
        return true;
      }
      if (ident == "floor") {
        *out = std::floor (arg);
        return true;
      }
      if (ident == "ceil") {
        *out = std::ceil (arg);
        return true;
      }
      return false;
    }

    // Decimal number literal.
    size_t start = pos_;
    bool saw_digit = false;
    bool saw_dot = false;
    while (pos_ < s_.size ()) {
      char c = s_[pos_];
      if (c >= '0' && c <= '9') {
        saw_digit = true;
        ++pos_;
      } else if (c == '.' && !saw_dot) {
        saw_dot = true;
        ++pos_;
      } else {
        break;
      }
    }
    if (!saw_digit)
      return false;
    try {
      *out = std::stod (s_.substr (start, pos_ - start));
      return true;
    } catch (...) {
      return false;
    }
  }

  const std::string &s_;
  size_t pos_ = 0;
  bool has_op_ = false;
  bool has_func_ = false;
  bool has_radix_ = false;
};

// --- Async File Search Worker ------------------------------------------------

struct FileSearchHit {
  std::wstring full_path;
  std::string name;
  std::string parent_display;
  bool is_dir = false;
  int score = 0;
};

struct AsyncFileSearchResult {
  GtkWidget *view = nullptr;
  std::shared_ptr<std::atomic<bool>> alive;
  std::shared_ptr<std::atomic<uint64_t>> shared_gen;
  uint64_t gen = 0;
  std::vector<FileSearchHit> hits;
  int total_matches = 0;
};

std::wstring
expand_user_path_prefix (const std::string &trimmed)
{
  std::wstring w = app_utf8_to_wide (trimmed);
  for (wchar_t &ch : w) {
    if (ch == L'/')
      ch = L'\\';
  }
  if (!w.empty () && w[0] == L'~' &&
      (w.size () == 1 || w[1] == L'\\')) {
    wchar_t profile[MAX_PATH] = L"";
    if (GetEnvironmentVariableW (L"USERPROFILE", profile, MAX_PATH) > 0) {
      return std::wstring (profile) + w.substr (1);
    }
  }
  if (w.find (L'%') != std::wstring::npos) {
    wchar_t expanded[MAX_PATH * 2] = L"";
    DWORD n = ExpandEnvironmentStringsW (w.c_str (), expanded, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2)
      return std::wstring (expanded);
  }
  return w;
}

void
scan_directory_for_files (const std::wstring &dir,
                          const std::string &parent_label,
                          const std::vector<std::string> &terms, int depth,
                          int max_depth, int *scanned_budget,
                          const std::atomic<bool> &alive,
                          const std::atomic<uint64_t> &shared_gen, uint64_t gen,
                          std::unordered_set<std::string> *seen_paths,
                          std::vector<FileSearchHit> *out)
{
  if (depth > max_depth || *scanned_budget <= 0)
    return;
  if (!alive.load (std::memory_order_acquire) ||
      shared_gen.load (std::memory_order_acquire) != gen) {
    return;
  }

  std::wstring pattern = dir;
  if (!pattern.empty () && pattern.back () != L'\\')
    pattern.push_back (L'\\');
  pattern.push_back (L'*');

  WIN32_FIND_DATAW ffd{};
  HANDLE h = FindFirstFileExW (pattern.c_str (), FindExInfoBasic, &ffd,
                               FindExSearchNameMatch, nullptr,
                               FIND_FIRST_EX_LARGE_FETCH);
  if (h == INVALID_HANDLE_VALUE)
    return;

  std::vector<std::pair<std::wstring, std::string>> subdirs;
  do {
    if (--(*scanned_budget) <= 0)
      break;
    if (!alive.load (std::memory_order_acquire) ||
        shared_gen.load (std::memory_order_acquire) != gen) {
      break;
    }
    if (wcscmp (ffd.cFileName, L".") == 0 || wcscmp (ffd.cFileName, L"..") == 0)
      continue;
    if ((ffd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) !=
        0) {
      continue;
    }
    const bool is_dir = (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    std::string fname = app_wide_to_utf8 (ffd.cFileName);
    if (fname.empty ())
      continue;

    std::wstring full = dir;
    if (!full.empty () && full.back () != L'\\')
      full.push_back (L'\\');
    full += ffd.cFileName;

    std::string display_name = fname;
    // Strip .lnk suffix for Recent items readability.
    if (!is_dir && display_name.size () > 4) {
      std::string ext =
          utf8_lower_local (display_name.substr (display_name.size () - 4));
      if (ext == ".lnk")
        display_name.resize (display_name.size () - 4);
    }

    std::string hay = utf8_lower_local (display_name + " " + parent_label);
    if (match_all_search_terms (hay, terms)) {
      std::string key = utf8_lower_local (app_wide_to_utf8 (full));
      if (seen_paths->insert (key).second) {
        std::string name_low = utf8_lower_local (display_name);
        int score = is_dir ? 55 : 50;
        if (!terms.empty () && name_low == terms[0])
          score += 50;
        else if (!terms.empty () && name_low.rfind (terms[0], 0) == 0)
          score += 30;
        else if (!terms.empty () && name_low.find (terms[0]) != std::string::npos)
          score += 15;
        if (depth == 0)
          score += 8;
        out->push_back (FileSearchHit{full, display_name, parent_label, is_dir,
                                      score});
      }
    }

    if (is_dir && depth < max_depth && subdirs.size () < 24) {
      std::string sub_label =
          parent_label.empty () ? fname : (parent_label + " / " + fname);
      subdirs.emplace_back (full, sub_label);
    }
  } while (FindNextFileW (h, &ffd));
  FindClose (h);

  for (const auto &sd : subdirs) {
    if (*scanned_budget <= 0)
      break;
    scan_directory_for_files (sd.first, sd.second, terms, depth + 1, max_depth,
                              scanned_budget, alive, shared_gen, gen,
                              seen_paths, out);
  }
}

void
run_file_search_worker (std::string raw_query, std::vector<std::string> terms,
                        GtkWidget *view,
                        std::shared_ptr<std::atomic<bool>> alive,
                        std::shared_ptr<std::atomic<uint64_t>> shared_gen,
                        uint64_t gen, gboolean (*deliver_cb) (gpointer))
{
  std::vector<FileSearchHit> hits;
  std::unordered_set<std::string> seen;
  int budget = kMaxFileScanEntries;

  std::string trimmed = trim_utf8 (raw_query);
  if (is_path_like_query (trimmed)) {
    std::wstring expanded = expand_user_path_prefix (trimmed);
    DWORD attr = GetFileAttributesW (expanded.c_str ());
    if (attr != INVALID_FILE_ATTRIBUTES) {
      bool is_dir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
      std::string u8 = app_wide_to_utf8 (expanded);
      hits.push_back (FileSearchHit{expanded, u8,
                                    winome::tr ("Open path", "打开路径"),
                                    is_dir, 120});
      if (is_dir) {
        std::vector<std::string> empty_terms;
        scan_directory_for_files (expanded, u8, empty_terms, 0, 0, &budget,
                                  *alive, *shared_gen, gen, &seen, &hits);
      }
    } else {
      size_t slash = expanded.find_last_of (L'\\');
      if (slash != std::wstring::npos && slash >= 2) {
        std::wstring parent_dir = expanded.substr (0, slash + 1);
        std::string leaf =
            utf8_lower_local (app_wide_to_utf8 (expanded.substr (slash + 1)));
        std::vector<std::string> leaf_terms;
        if (!leaf.empty ())
          leaf_terms.push_back (leaf);
        scan_directory_for_files (parent_dir, app_wide_to_utf8 (parent_dir),
                                  leaf_terms, 0, 0, &budget, *alive,
                                  *shared_gen, gen, &seen, &hits);
      }
    }
  } else if (!terms.empty ()) {
    struct KnownRoot {
      KNOWNFOLDERID id;
      const char *en;
      const char *zh;
      int max_depth;
    };
    static const KnownRoot kRoots[] = {
        {FOLDERID_Desktop, "Desktop", "桌面", 1},
        {FOLDERID_Downloads, "Downloads", "下载", 1},
        {FOLDERID_Documents, "Documents", "文档", 1},
        {FOLDERID_Pictures, "Pictures", "图片", 1},
        {FOLDERID_Videos, "Videos", "视频", 1},
        {FOLDERID_Music, "Music", "音乐", 1},
        {FOLDERID_Recent, "Recent", "最近使用", 0},
    };
    for (const KnownRoot &kr : kRoots) {
      if (budget <= 0 || !alive->load (std::memory_order_acquire) ||
          shared_gen->load (std::memory_order_acquire) != gen) {
        break;
      }
      PWSTR wpath = nullptr;
      if (SUCCEEDED (SHGetKnownFolderPath (kr.id, 0, nullptr, &wpath)) &&
          wpath != nullptr) {
        std::wstring root_dir (wpath);
        CoTaskMemFree (wpath);
        scan_directory_for_files (root_dir, winome::tr (kr.en, kr.zh), terms, 0,
                                  kr.max_depth, &budget, *alive, *shared_gen,
                                  gen, &seen, &hits);
      }
    }
  }

  std::stable_sort (hits.begin (), hits.end (),
                    [] (const FileSearchHit &a, const FileSearchHit &b) {
                      return a.score > b.score;
                    });
  const int total_matches = static_cast<int> (hits.size ());
  if (hits.size () > static_cast<size_t> (kMaxListResults))
    hits.resize (static_cast<size_t> (kMaxListResults));

  auto *payload = new AsyncFileSearchResult ();
  payload->view = view;
  payload->alive = std::move (alive);
  payload->shared_gen = std::move (shared_gen);
  payload->gen = gen;
  payload->hits = std::move (hits);
  payload->total_matches = total_matches;
  g_idle_add (deliver_cb, payload);
}

GdkTexture *
file_icon_texture_for_path (const std::wstring &path, bool is_dir)
{
  SHFILEINFOW sfi{};
  UINT flags = SHGFI_ICON | SHGFI_SMALLICON;
  DWORD attrs = is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
  if (SHGetFileInfoW (path.c_str (), attrs, &sfi, sizeof (sfi), flags) == 0 ||
      sfi.hIcon == nullptr) {
    SHGetFileInfoW (path.c_str (), attrs, &sfi, sizeof (sfi),
                    flags | SHGFI_USEFILEATTRIBUTES);
  }
  if (sfi.hIcon == nullptr)
    return nullptr;
  GdkTexture *tex = hicon_to_texture (sfi.hIcon);
  DestroyIcon (sfi.hIcon);
  return tex;
}

// --- Search View Widget State ------------------------------------------------

enum class SearchItemKind {
  App,
  SystemAction,
  Window,
  Calculator,
  File,
  Setting,
  UrlOrCommand,
};

struct SearchItem {
  SearchItemKind kind = SearchItemKind::App;
  bool is_grid = false;
  GtkWidget *button = nullptr;
  AppEntry app_entry;
  HWND window_hwnd = nullptr;
  int window_desktop = -1;
  std::string payload;
  std::wstring wide_path;
};

struct WinomeSearchView {
  GtkBox parent_instance;

  GtkWidget *scroll = nullptr;
  GtkWidget *content = nullptr;
  GtkWidget *status_box = nullptr;
  GtkWidget *status_spinner = nullptr;
  GtkWidget *status_label = nullptr;

  // Dynamic section slot for async File search results so it inserts cleanly
  // in GNOME provider order without tearing down other sections.
  GtkWidget *files_section_host = nullptr;
  size_t files_insert_idx = 0;

  OverviewSearchCallbacks cbs{};
  std::string raw_query;
  std::vector<std::string> terms;
  std::vector<SearchItem> items;
  int grid_count = 0;
  int selected_idx = -1;
  bool async_files_pending = false;
  guint async_debounce_timer = 0;

  uint64_t gen = 0;
  std::shared_ptr<std::atomic<bool>> alive;
  std::shared_ptr<std::atomic<uint64_t>> shared_gen;
};

struct WinomeSearchViewClass {
  GtkBoxClass parent_class;
};

#define WINOME_SEARCH_VIEW(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), winome_search_view_get_type (), \
                               WinomeSearchView))

G_DEFINE_TYPE (WinomeSearchView, winome_search_view, GTK_TYPE_BOX)

static void clear_box_children (GtkWidget *box)
{
  if (box == nullptr)
    return;
  GtkWidget *c = gtk_widget_get_first_child (box);
  while (c != nullptr) {
    GtkWidget *next = gtk_widget_get_next_sibling (c);
    gtk_box_remove (GTK_BOX (box), c);
    c = next;
  }
}

static void
ensure_selected_visible (WinomeSearchView *self, GtkWidget *btn)
{
  if (self == nullptr || self->scroll == nullptr || self->content == nullptr ||
      btn == nullptr) {
    return;
  }
  graphene_rect_t bounds{};
  if (!gtk_widget_compute_bounds (btn, self->content, &bounds))
    return;
  GtkAdjustment *adj =
      gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scroll));
  if (adj == nullptr)
    return;
  double val = gtk_adjustment_get_value (adj);
  double page = gtk_adjustment_get_page_size (adj);
  double top = static_cast<double> (bounds.origin.y);
  double bot = top + static_cast<double> (bounds.size.height);
  if (top < val + 12.0)
    gtk_adjustment_set_value (adj, std::max (0.0, top - 12.0));
  else if (bot > val + page - 12.0)
    gtk_adjustment_set_value (adj, std::max (0.0, bot - page + 12.0));
}

static void
set_selected_index (WinomeSearchView *self, int idx)
{
  if (self == nullptr || self->items.empty ()) {
    if (self != nullptr)
      self->selected_idx = -1;
    return;
  }
  int clamped =
      std::clamp (idx, 0, static_cast<int> (self->items.size ()) - 1);
  if (self->selected_idx >= 0 &&
      self->selected_idx < static_cast<int> (self->items.size ())) {
    GtkWidget *old_btn =
        self->items[static_cast<size_t> (self->selected_idx)].button;
    if (old_btn != nullptr)
      gtk_widget_remove_css_class (old_btn, "selected");
  }
  self->selected_idx = clamped;
  GtkWidget *btn = self->items[static_cast<size_t> (self->selected_idx)].button;
  if (btn != nullptr) {
    gtk_widget_add_css_class (btn, "selected");
    ensure_selected_visible (self, btn);
  }
}

static void
update_status_visibility (WinomeSearchView *self)
{
  if (self == nullptr)
    return;
  const bool have_results = !self->items.empty ();
  gtk_widget_set_visible (self->scroll, have_results ? TRUE : FALSE);
  gtk_widget_set_visible (self->status_box, have_results ? FALSE : TRUE);

  if (!have_results) {
    if (self->async_files_pending) {
      gtk_widget_set_visible (self->status_spinner, TRUE);
      gtk_spinner_start (GTK_SPINNER (self->status_spinner));
      gtk_label_set_text (GTK_LABEL (self->status_label),
                          winome::tr ("Searching", "正在搜索"));
    } else {
      gtk_spinner_stop (GTK_SPINNER (self->status_spinner));
      gtk_widget_set_visible (self->status_spinner, FALSE);
      gtk_label_set_text (GTK_LABEL (self->status_label),
                          winome::tr ("No results", "无搜索结果"));
    }
  } else {
    gtk_spinner_stop (GTK_SPINNER (self->status_spinner));
  }
}

static void
activate_item (WinomeSearchView *self, int idx, bool new_instance)
{
  if (self == nullptr || idx < 0 ||
      idx >= static_cast<int> (self->items.size ())) {
    return;
  }
  const SearchItem item = self->items[static_cast<size_t> (idx)];

  auto close_ov = [&] (const char *reason, bool discard_focus) {
    if (self->cbs.on_close_overview != nullptr)
      self->cbs.on_close_overview (reason, discard_focus, self->cbs.user_data);
  };

  switch (item.kind) {
    case SearchItemKind::App: {
      AllowSetForegroundWindow (ASFW_ANY);
      // Matching upstream AppIcon.activate(): plain Enter focuses an existing
      // running window of the app if available; Ctrl+Enter launches a new
      // instance.
      if (!new_instance) {
        std::vector<RunningWindow> rw =
            app_index_running_windows_for_entry (item.app_entry);
        if (!rw.empty () && IsWindow (rw[0].hwnd)) {
          if (self->cbs.on_activate_window != nullptr) {
            self->cbs.on_activate_window (rw[0].hwnd, -1, self->cbs.user_data);
            return;
          }
          app_index_activate_hwnd (rw[0].hwnd);
          close_ov ("search-app-focus", true);
          return;
        }
      }
      HMONITOR target_mon =
          monitor_manager_monitor_for_widget (GTK_WIDGET (self));
      if (app_index_launch (item.app_entry, false, target_mon))
        close_ov (new_instance ? "search-app-new" : "search-app-launch", true);
      break;
    }
    case SearchItemKind::SystemAction: {
      if (item.payload == "power-off") {
        close_ov ("search-sys-poweroff", false);
        show_end_session_dialog (EndSessionAction::PowerOff,
                                 on_end_session_confirmed, nullptr);
      } else if (item.payload == "restart") {
        close_ov ("search-sys-restart", false);
        show_end_session_dialog (EndSessionAction::Restart,
                                 on_end_session_confirmed, nullptr);
      } else if (item.payload == "logout") {
        close_ov ("search-sys-logout", false);
        show_end_session_dialog (EndSessionAction::LogOut,
                                 on_end_session_confirmed, nullptr);
      } else if (item.payload == "lock-screen") {
        close_ov ("search-sys-lock", false);
        lock_screen ();
      } else if (item.payload == "suspend") {
        close_ov ("search-sys-suspend", false);
        suspend_system ();
      } else if (item.payload == "screenshot") {
        close_ov ("search-sys-screenshot", true);
        shell_open_utf8 ("ms-screenclip:");
      }
      break;
    }
    case SearchItemKind::Window: {
      if (self->cbs.on_activate_window != nullptr) {
        self->cbs.on_activate_window (item.window_hwnd, item.window_desktop,
                                      self->cbs.user_data);
      } else {
        app_index_activate_hwnd (item.window_hwnd);
        close_ov ("search-window", true);
      }
      break;
    }
    case SearchItemKind::Calculator: {
      copy_utf8_to_clipboard (item.payload);
      close_ov ("search-calc-copy", false);
      break;
    }
    case SearchItemKind::File: {
      HMONITOR target_mon =
          monitor_manager_monitor_for_widget (GTK_WIDGET (self));
      AllowSetForegroundWindow (ASFW_ANY);
      watch_launched_window_for_monitor (target_mon);
      SHELLEXECUTEINFOW sei {};
      sei.cbSize = sizeof (sei);
      sei.fMask = SEE_MASK_FLAG_NO_UI;
      if (target_mon != nullptr) {
        sei.fMask |= SEE_MASK_HMONITOR;
        sei.hMonitor = target_mon;
      }
      sei.lpVerb = L"open";
      sei.lpFile = item.wide_path.c_str ();
      sei.nShow = SW_SHOWNORMAL;
      ShellExecuteExW (&sei);
      close_ov ("search-file-open", true);
      break;
    }
    case SearchItemKind::Setting: {
      if (item.payload.rfind ("winome:settings:", 0) == 0) {
        std::string page = item.payload.substr (16);
        close_ov ("search-winome-settings", true);
        if (self->cbs.on_open_settings != nullptr) {
          self->cbs.on_open_settings (page.empty () ? nullptr : page.c_str (),
                                      self->cbs.user_data);
        }
      } else {
        HMONITOR target_mon =
            monitor_manager_monitor_for_widget (GTK_WIDGET (self));
        close_ov ("search-win-settings", true);
        shell_open_utf8 (item.payload, target_mon);
      }
      break;
    }
    case SearchItemKind::UrlOrCommand: {
      HMONITOR target_mon =
          monitor_manager_monitor_for_widget (GTK_WIDGET (self));
      close_ov ("search-url-cmd", true);
      shell_open_utf8 (item.payload, target_mon);
      break;
    }
  }
}

struct ItemBtnCtx {
  WinomeSearchView *self = nullptr;
  int idx = -1;
};

static void
on_item_btn_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  auto *ctx = static_cast<ItemBtnCtx *> (user_data);
  if (ctx == nullptr || ctx->self == nullptr)
    return;
  activate_item (ctx->self, ctx->idx, false);
}

struct ProviderLaunchCtx {
  WinomeSearchView *self = nullptr;
  std::string launch_cmd;
};

static void
on_provider_info_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  auto *ctx = static_cast<ProviderLaunchCtx *> (user_data);
  if (ctx == nullptr || ctx->self == nullptr || ctx->launch_cmd.empty ())
    return;
  if (ctx->self->cbs.on_close_overview != nullptr) {
    ctx->self->cbs.on_close_overview ("search-provider-launch", true,
                                      ctx->self->cbs.user_data);
  }
  if (ctx->launch_cmd == "winome:settings") {
    if (ctx->self->cbs.on_open_settings != nullptr)
      ctx->self->cbs.on_open_settings (nullptr, ctx->self->cbs.user_data);
  } else {
    shell_open_utf8 (ctx->launch_cmd);
  }
}

static GtkWidget *
make_provider_section_card (WinomeSearchView *self, const char *icon_name,
                            const char *provider_title, int more_count,
                            const std::string &provider_launch_cmd,
                            GtkWidget **out_list_box)
{
  GtkWidget *section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (section, "search-section");
  gtk_widget_set_hexpand (section, TRUE);

  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 14);
  gtk_widget_add_css_class (card, "search-section-content");
  gtk_widget_set_hexpand (card, TRUE);

  // ProviderInfo button (upstream search.js:966-1029).
  GtkWidget *prov_btn = gtk_button_new ();
  gtk_widget_add_css_class (prov_btn, "search-provider-icon");
  gtk_widget_set_focus_on_click (prov_btn, FALSE);
  gtk_widget_set_valign (prov_btn, GTK_ALIGN_START);
  gtk_widget_set_size_request (prov_btn, 168, -1);

  GtkWidget *prov_content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class (prov_content, "list-search-provider-content");

  GtkWidget *picon = gtk_image_new_from_icon_name (icon_name);
  gtk_image_set_pixel_size (GTK_IMAGE (picon), kProviderIconSize);
  gtk_widget_set_valign (picon, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (prov_content), picon);

  GtkWidget *details = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_add_css_class (details, "list-search-provider-details");
  gtk_widget_set_hexpand (details, TRUE);
  gtk_widget_set_valign (details, GTK_ALIGN_CENTER);

  GtkWidget *name_lbl = gtk_label_new (provider_title);
  gtk_label_set_xalign (GTK_LABEL (name_lbl), 0.0f);
  gtk_label_set_ellipsize (GTK_LABEL (name_lbl), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (details), name_lbl);

  if (more_count > 0) {
    char buf[64];
    std::snprintf (buf, sizeof (buf),
                   winome::tr ("%d more", "还有 %d 项"), more_count);
    GtkWidget *more_lbl = gtk_label_new (buf);
    gtk_widget_add_css_class (more_lbl, "list-search-result-description");
    gtk_label_set_xalign (GTK_LABEL (more_lbl), 0.0f);
    gtk_box_append (GTK_BOX (details), more_lbl);
  }

  gtk_box_append (GTK_BOX (prov_content), details);
  gtk_button_set_child (GTK_BUTTON (prov_btn), prov_content);

  if (!provider_launch_cmd.empty ()) {
    auto *pctx = new ProviderLaunchCtx{self, provider_launch_cmd};
    g_object_set_data_full (
        G_OBJECT (prov_btn), "winome-prov-ctx", pctx,
        [] (gpointer p) { delete static_cast<ProviderLaunchCtx *> (p); });
    g_signal_connect (prov_btn, "clicked",
                      G_CALLBACK (on_provider_info_clicked), pctx);
  } else {
    gtk_widget_set_sensitive (prov_btn, FALSE);
  }
  gtk_box_append (GTK_BOX (card), prov_btn);

  GtkWidget *list_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class (list_box, "list-search-results");
  gtk_widget_set_hexpand (list_box, TRUE);
  gtk_box_append (GTK_BOX (card), list_box);

  gtk_box_append (GTK_BOX (section), card);
  if (out_list_box != nullptr)
    *out_list_box = list_box;
  return section;
}

static GtkWidget *
make_list_row_button (WinomeSearchView *self, GdkTexture *tex,
                      const char *fallback_icon, const std::string &title,
                      const std::string &description, int item_idx)
{
  GtkWidget *btn = gtk_button_new ();
  gtk_widget_add_css_class (btn, "list-search-result");
  gtk_widget_set_focus_on_click (btn, FALSE);
  gtk_widget_set_hexpand (btn, TRUE);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class (content, "list-search-result-content");
  gtk_widget_set_hexpand (content, TRUE);

  GtkWidget *icon_w = nullptr;
  if (tex != nullptr) {
    icon_w = gtk_image_new_from_paintable (GDK_PAINTABLE (tex));
  } else {
    icon_w = gtk_image_new_from_icon_name (
        fallback_icon != nullptr ? fallback_icon : "application-x-executable");
  }
  gtk_image_set_pixel_size (GTK_IMAGE (icon_w), kListItemIconSize);
  gtk_widget_set_valign (icon_w, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (content), icon_w);

  GtkWidget *title_lbl = gtk_label_new (nullptr);
  gtk_widget_add_css_class (title_lbl, "list-search-result-title");
  std::string title_markup = highlight_search_terms_markup (title, self->terms);
  gtk_label_set_markup (GTK_LABEL (title_lbl), title_markup.c_str ());
  gtk_label_set_xalign (GTK_LABEL (title_lbl), 0.0f);
  gtk_label_set_ellipsize (GTK_LABEL (title_lbl), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode (GTK_LABEL (title_lbl), TRUE);
  gtk_widget_set_hexpand (title_lbl, TRUE);
  gtk_widget_set_valign (title_lbl, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (content), title_lbl);

  if (!description.empty ()) {
    GtkWidget *desc_lbl = gtk_label_new (nullptr);
    gtk_widget_add_css_class (desc_lbl, "list-search-result-description");
    std::string desc_markup =
        highlight_search_terms_markup (description, self->terms);
    gtk_label_set_markup (GTK_LABEL (desc_lbl), desc_markup.c_str ());
    gtk_label_set_xalign (GTK_LABEL (desc_lbl), 1.0f);
    gtk_label_set_ellipsize (GTK_LABEL (desc_lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_single_line_mode (GTK_LABEL (desc_lbl), TRUE);
    gtk_widget_set_valign (desc_lbl, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (content), desc_lbl);
  }

  gtk_button_set_child (GTK_BUTTON (btn), content);

  auto *ctx = new ItemBtnCtx{self, item_idx};
  g_object_set_data_full (
      G_OBJECT (btn), "winome-item-ctx", ctx,
      [] (gpointer p) { delete static_cast<ItemBtnCtx *> (p); });
  g_signal_connect (btn, "clicked", G_CALLBACK (on_item_btn_clicked), ctx);
  return btn;
}

static gboolean
on_async_file_search_done (gpointer user_data)
{
  std::unique_ptr<AsyncFileSearchResult> res (
      static_cast<AsyncFileSearchResult *> (user_data));
  if (res == nullptr || res->alive == nullptr ||
      !res->alive->load (std::memory_order_acquire) ||
      res->shared_gen->load (std::memory_order_acquire) != res->gen ||
      res->view == nullptr) {
    return G_SOURCE_REMOVE;
  }

  WinomeSearchView *self = WINOME_SEARCH_VIEW (res->view);
  self->async_files_pending = false;

  if (self->files_section_host != nullptr) {
    clear_box_children (self->files_section_host);
    if (!res->hits.empty ()) {
      const int more =
          std::max (0, res->total_matches - static_cast<int> (res->hits.size ()));
      GtkWidget *list_box = nullptr;
      GtkWidget *card = make_provider_section_card (
          self, "system-file-manager-symbolic",
          winome::tr ("Files", "文件"), more, "explorer.exe", &list_box);
      const size_t insert_at =
          std::min (self->files_insert_idx, self->items.size ());
      std::vector<SearchItem> file_items;
      file_items.reserve (res->hits.size ());
      for (const FileSearchHit &hit : res->hits) {
        int item_idx = static_cast<int> (insert_at + file_items.size ());
        GdkTexture *tex = file_icon_texture_for_path (hit.full_path, hit.is_dir);
        GtkWidget *row_btn = make_list_row_button (
            self, tex,
            hit.is_dir ? "folder-symbolic" : "text-x-generic-symbolic",
            hit.name, hit.parent_display, item_idx);
        if (tex != nullptr)
          g_object_unref (tex);
        gtk_box_append (GTK_BOX (list_box), row_btn);

        SearchItem item;
        item.kind = SearchItemKind::File;
        item.is_grid = false;
        item.button = row_btn;
        item.wide_path = hit.full_path;
        file_items.push_back (std::move (item));
      }
      const int inserted_n = static_cast<int> (file_items.size ());
      self->items.insert (self->items.begin () + static_cast<ptrdiff_t> (insert_at),
                          std::make_move_iterator (file_items.begin ()),
                          std::make_move_iterator (file_items.end ()));
      // Keep ItemBtnCtx::idx and selected_idx in sync with visual order.
      for (size_t i = insert_at; i < self->items.size (); ++i) {
        GtkWidget *b = self->items[i].button;
        if (b != nullptr) {
          auto *ctx = static_cast<ItemBtnCtx *> (
              g_object_get_data (G_OBJECT (b), "winome-item-ctx"));
          if (ctx != nullptr)
            ctx->idx = static_cast<int> (i);
        }
      }
      if (self->selected_idx >= static_cast<int> (insert_at))
        self->selected_idx += inserted_n;
      gtk_box_append (GTK_BOX (self->files_section_host), card);
      gtk_widget_set_visible (self->files_section_host, TRUE);
    } else {
      gtk_widget_set_visible (self->files_section_host, FALSE);
    }
  }

  if (self->selected_idx < 0 && !self->items.empty ())
    set_selected_index (self, 0);
  update_status_visibility (self);
  return G_SOURCE_REMOVE;
}

static gboolean
on_async_debounce_fire (gpointer user_data)
{
  WinomeSearchView *self = WINOME_SEARCH_VIEW (user_data);
  self->async_debounce_timer = 0;
  if (self->terms.empty ()) {
    self->async_files_pending = false;
    update_status_visibility (self);
    return G_SOURCE_REMOVE;
  }

  std::string raw = self->raw_query;
  std::vector<std::string> terms = self->terms;
  uint64_t gen = self->gen;
  auto alive = self->alive;
  auto shared_gen = self->shared_gen;
  GtkWidget *view_widget = GTK_WIDGET (self);

  std::thread ([raw = std::move (raw), terms = std::move (terms), view_widget,
                alive = std::move (alive), shared_gen = std::move (shared_gen),
                gen] () {
    HRESULT hr = CoInitializeEx (
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    run_file_search_worker (raw, terms, view_widget, alive, shared_gen, gen,
                            on_async_file_search_done);
    if (SUCCEEDED (hr))
      CoUninitialize ();
  }).detach ();

  return G_SOURCE_REMOVE;
}

static void
cancel_async_search (WinomeSearchView *self)
{
  if (self == nullptr)
    return;
  if (self->async_debounce_timer != 0) {
    g_source_remove (self->async_debounce_timer);
    self->async_debounce_timer = 0;
  }
  uint64_t next_gen = ++self->gen;
  if (self->shared_gen != nullptr)
    self->shared_gen->store (next_gen, std::memory_order_release);
  self->async_files_pending = false;
}

static void
rebuild_sync_providers (WinomeSearchView *self)
{
  if (self == nullptr || self->content == nullptr)
    return;
  cancel_async_search (self);
  clear_box_children (self->content);
  self->files_section_host = nullptr;
  self->items.clear ();
  self->grid_count = 0;
  self->files_insert_idx = 0;
  self->selected_idx = -1;

  if (self->terms.empty ()) {
    update_status_visibility (self);
    return;
  }

  const int scale =
      std::max (gtk_widget_get_scale_factor (GTK_WIDGET (self)), 1);
  const std::string trimmed = trim_utf8 (self->raw_query);
  const std::string joined_lower = utf8_lower_local (trimmed);

  // 1. Top Row: AppSearchProvider + SystemActions (GridSearchResults).
  //    Reference: appDisplay.js:1830-1846 & misc/systemActions.js:96-158.
  std::vector<AppEntry> all_apps = app_index_get_all ();
  std::vector<AppEntry> matched_apps;
  if (self->terms.size () == 1) {
    matched_apps = filter_and_sort_apps (all_apps, self->terms[0]);
  } else {
    for (const AppEntry &a : all_apps) {
      std::string hay = a.name_lower + " " + a.target_lower + " " +
                        utf8_lower_local (a.folder_name);
      if (match_all_search_terms (hay, self->terms))
        matched_apps.push_back (a);
    }
  }

  struct SysActionSpec {
    const char *id;
    const char *icon;
    const char *en;
    const char *zh;
    const char *keywords;
  };
  static const SysActionSpec kSysActions[] = {
      {"lock-screen", "system-lock-screen-symbolic", "Lock Screen", "锁定屏幕",
       "lock screen lockscreen 锁屏 锁定 屏幕"},
      {"screenshot", "record-screen-symbolic", "Take a Screenshot", "截屏",
       "screenshot screencast snip capture record 截屏 截图 录屏 捕捉"},
      {"suspend", "media-playback-pause-symbolic", "Suspend", "休眠",
       "suspend sleep hibernate 休眠 睡眠 待机"},
      {"logout", "system-log-out-symbolic", "Log Out", "注销",
       "logout log out sign off 注销 登出 退出登录"},
      {"restart", "system-reboot-symbolic", "Restart", "重启",
       "restart reboot 重新启动 重启"},
      {"power-off", "system-shutdown-symbolic", "Power Off", "关机",
       "power off poweroff shutdown halt stop 关机 关闭电源"},
  };

  std::vector<const SysActionSpec *> matched_sys_tiles;
  for (const SysActionSpec &sa : kSysActions) {
    std::string kw = utf8_lower_local (sa.keywords);
    if (match_all_search_terms (kw, self->terms))
      matched_sys_tiles.push_back (&sa);
  }

  const size_t total_grid_candidates =
      matched_apps.size () + matched_sys_tiles.size ();
  if (total_grid_candidates > 0) {
    GtkWidget *grid_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 20);
    gtk_widget_add_css_class (grid_row, "grid-search-results");
    gtk_widget_set_halign (grid_row, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top (grid_row, 8);
    gtk_widget_set_margin_bottom (grid_row, 10);

    const std::vector<RunningWindow> running_wins =
        app_index_enumerate_running_windows ();
    const int icon_px = 76;
    const int load_px = std::clamp (icon_px * std::max (scale, 2), 96, 192);

    size_t added = 0;
    for (size_t i = 0;
         i < matched_apps.size () && added < static_cast<size_t> (kMaxGridResults);
         ++i, ++added) {
      const AppEntry &app = matched_apps[i];
      int item_idx = static_cast<int> (self->items.size ());

      GtkWidget *btn = gtk_button_new ();
      gtk_widget_add_css_class (btn, "overview-tile");
      gtk_widget_add_css_class (btn, "grid-search-result");
      gtk_widget_set_focus_on_click (btn, FALSE);
      gtk_widget_set_size_request (btn, 132, 132);
      gtk_widget_set_tooltip_text (btn, app.name.c_str ());

      GtkWidget *overlay = gtk_overlay_new ();
      gtk_widget_set_hexpand (overlay, TRUE);
      gtk_widget_set_vexpand (overlay, TRUE);

      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
      gtk_widget_add_css_class (box, "overview-icon");
      gtk_widget_add_css_class (box, "overview-icon-with-label");
      gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
      gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
      gtk_widget_set_margin_bottom (box, 6);

      GdkTexture *tex = app_icon_for_entry (app, load_px);
      GtkWidget *img = nullptr;
      if (tex != nullptr) {
        img = gtk_image_new_from_paintable (GDK_PAINTABLE (tex));
        g_object_unref (tex);
      } else {
        img = gtk_image_new_from_icon_name ("application-x-executable");
      }
      gtk_image_set_pixel_size (GTK_IMAGE (img), icon_px);
      gtk_widget_set_halign (img, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (box), img);

      GtkWidget *lbl = gtk_label_new (app.name.c_str ());
      gtk_label_set_lines (GTK_LABEL (lbl), 1);
      gtk_label_set_wrap (GTK_LABEL (lbl), FALSE);
      gtk_label_set_ellipsize (GTK_LABEL (lbl), PANGO_ELLIPSIZE_END);
      gtk_label_set_max_width_chars (GTK_LABEL (lbl), 13);
      gtk_widget_set_halign (lbl, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (box), lbl);

      gtk_overlay_set_child (GTK_OVERLAY (overlay), box);

      if (!match_entry_running_windows (app, running_wins).empty ()) {
        GtkWidget *dot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class (dot, "app-grid-running-dot");
        gtk_widget_set_size_request (dot, 5, 5);
        gtk_widget_set_halign (dot, GTK_ALIGN_CENTER);
        gtk_widget_set_valign (dot, GTK_ALIGN_END);
        gtk_overlay_add_overlay (GTK_OVERLAY (overlay), dot);
      }

      gtk_button_set_child (GTK_BUTTON (btn), overlay);
      auto *ctx = new ItemBtnCtx{self, item_idx};
      g_object_set_data_full (
          G_OBJECT (btn), "winome-item-ctx", ctx,
          [] (gpointer p) { delete static_cast<ItemBtnCtx *> (p); });
      g_signal_connect (btn, "clicked", G_CALLBACK (on_item_btn_clicked), ctx);
      gtk_box_append (GTK_BOX (grid_row), btn);

      SearchItem item;
      item.kind = SearchItemKind::App;
      item.is_grid = true;
      item.button = btn;
      item.app_entry = app;
      self->items.push_back (std::move (item));
      ++self->grid_count;
    }

    // SystemActionIcon circular tiles (.system-action-icon).
    for (size_t i = 0;
         i < matched_sys_tiles.size () &&
         added < static_cast<size_t> (kMaxGridResults);
         ++i, ++added) {
      const SysActionSpec *sa = matched_sys_tiles[i];
      int item_idx = static_cast<int> (self->items.size ());

      GtkWidget *btn = gtk_button_new ();
      gtk_widget_add_css_class (btn, "overview-tile");
      gtk_widget_add_css_class (btn, "grid-search-result");
      gtk_widget_set_focus_on_click (btn, FALSE);
      gtk_widget_set_size_request (btn, 132, 132);

      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
      gtk_widget_add_css_class (box, "overview-icon");
      gtk_widget_add_css_class (box, "overview-icon-with-label");
      gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
      gtk_widget_set_valign (box, GTK_ALIGN_CENTER);

      GtkWidget *circle = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_add_css_class (circle, "system-action-icon");
      gtk_widget_set_size_request (circle, icon_px, icon_px);
      gtk_widget_set_halign (circle, GTK_ALIGN_CENTER);
      gtk_widget_set_valign (circle, GTK_ALIGN_CENTER);

      GtkWidget *img = gtk_image_new_from_icon_name (sa->icon);
      gtk_image_set_pixel_size (GTK_IMAGE (img), 38);
      gtk_widget_set_hexpand (img, TRUE);
      gtk_widget_set_vexpand (img, TRUE);
      gtk_widget_set_halign (img, GTK_ALIGN_CENTER);
      gtk_widget_set_valign (img, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (circle), img);
      gtk_box_append (GTK_BOX (box), circle);

      GtkWidget *lbl = gtk_label_new (winome::tr (sa->en, sa->zh));
      gtk_label_set_lines (GTK_LABEL (lbl), 1);
      gtk_label_set_wrap (GTK_LABEL (lbl), FALSE);
      gtk_label_set_ellipsize (GTK_LABEL (lbl), PANGO_ELLIPSIZE_END);
      gtk_label_set_max_width_chars (GTK_LABEL (lbl), 13);
      gtk_widget_set_halign (lbl, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (box), lbl);

      gtk_button_set_child (GTK_BUTTON (btn), box);
      auto *ctx = new ItemBtnCtx{self, item_idx};
      g_object_set_data_full (
          G_OBJECT (btn), "winome-item-ctx", ctx,
          [] (gpointer p) { delete static_cast<ItemBtnCtx *> (p); });
      g_signal_connect (btn, "clicked", G_CALLBACK (on_item_btn_clicked), ctx);
      gtk_box_append (GTK_BOX (grid_row), btn);

      SearchItem item;
      item.kind = SearchItemKind::SystemAction;
      item.is_grid = true;
      item.button = btn;
      item.payload = sa->id;
      self->items.push_back (std::move (item));
      ++self->grid_count;
    }

    gtk_box_append (GTK_BOX (self->content), grid_row);
  }

  // 2. Instant Calculator Provider.
  std::string calc_res;
  if (try_eval_search_math (trimmed, &calc_res)) {
    GtkWidget *list_box = nullptr;
    GtkWidget *sec = make_provider_section_card (
        self, "accessories-calculator-symbolic",
        winome::tr ("Calculator", "计算器"), 0, "calc.exe", &list_box);
    int item_idx = static_cast<int> (self->items.size ());
    std::string expr_line = trimmed + "  =  " + calc_res;
    GtkWidget *row = make_list_row_button (
        self, nullptr, "accessories-calculator-symbolic", expr_line,
        winome::tr ("Click or press Enter to copy", "点击或按回车复制结果"),
        item_idx);
    gtk_box_append (GTK_BOX (list_box), row);
    gtk_box_append (GTK_BOX (self->content), sec);

    SearchItem item;
    item.kind = SearchItemKind::Calculator;
    item.button = row;
    item.payload = calc_res;
    self->items.push_back (std::move (item));
  }

  // 3. Open Windows Provider (across all virtual workspaces).
  if (self->cbs.get_open_windows != nullptr) {
    std::vector<SearchWindowCandidate> all_wins =
        self->cbs.get_open_windows (self->cbs.user_data);
    std::vector<SearchWindowCandidate> matched_wins;
    for (const SearchWindowCandidate &wc : all_wins) {
      std::string hay = utf8_lower_local (wc.title + " " + wc.process_name);
      if (match_all_search_terms (hay, self->terms))
        matched_wins.push_back (wc);
    }
    if (!matched_wins.empty ()) {
      const size_t shown =
          std::min<size_t> (matched_wins.size (), kMaxListResults);
      const int more =
          static_cast<int> (matched_wins.size () - shown);
      GtkWidget *list_box = nullptr;
      GtkWidget *sec = make_provider_section_card (
          self, "preferences-system-windows-symbolic",
          winome::tr ("Windows", "窗口"), more, "", &list_box);
      for (size_t i = 0; i < shown; ++i) {
        const SearchWindowCandidate &wc = matched_wins[i];
        int item_idx = static_cast<int> (self->items.size ());
        std::string title = wc.title.empty () ? wc.process_name : wc.title;
        std::string desc =
            std::string (winome::tr ("Workspace ", "工作区 ")) +
            std::to_string (wc.desktop_index + 1);
        if (!wc.process_name.empty ())
          desc += " · " + wc.process_name;

        GdkTexture *tex =
            app_icon_for_window (wc.hwnd, L"", kListItemIconSize * scale);
        GtkWidget *row = make_list_row_button (
            self, tex, "application-x-executable", title, desc, item_idx);
        if (tex != nullptr)
          g_object_unref (tex);
        gtk_box_append (GTK_BOX (list_box), row);

        SearchItem item;
        item.kind = SearchItemKind::Window;
        item.button = row;
        item.window_hwnd = wc.hwnd;
        item.window_desktop = wc.desktop_index;
        self->items.push_back (std::move (item));
      }
      gtk_box_append (GTK_BOX (self->content), sec);
    }
  }

  // 4. Placeholder slot for Async Files Provider (keeps provider order stable).
  self->files_insert_idx = self->items.size ();
  self->files_section_host = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand (self->files_section_host, TRUE);
  gtk_widget_set_visible (self->files_section_host, FALSE);
  gtk_box_append (GTK_BOX (self->content), self->files_section_host);

  // 5. Settings & System Tools Provider.
  struct SettingSpec {
    const char *icon;
    const char *en;
    const char *zh;
    const char *desc_en;
    const char *desc_zh;
    const char *keywords;
    const char *cmd;
  };
  static const SettingSpec kSettings[] = {
      {"emblem-system-symbolic", "WINOME Settings", "WINOME 设置",
       "Appearance, Top Bar & Overview", "外观、顶栏与概览设置",
       "winome settings preferences config 设置 偏好 配置", "winome:settings:"},
      {"preferences-desktop-appearance-symbolic", "Appearance", "外观设置",
       "WINOME Settings · Theme & Font", "WINOME 设置 · 主题与字体",
       "winome appearance theme dark light font 外观 主题 深色 浅色 字体",
       "winome:settings:appearance"},
      {"preferences-desktop-workspaces-symbolic", "Behavior & Workspaces",
       "行为与工作区", "WINOME Settings · Hot Corner & Overview",
       "WINOME 设置 · 热角与概览行为",
       "winome behavior workspace hot corner overview 行为 工作区 热角 概览 快捷键",
       "winome:settings:behavior"},
      {"utilities-system-monitor-symbolic", "Diagnostics & Logs", "诊断与日志",
       "WINOME Settings · Logs & Safe Mode", "WINOME 设置 · 日志与诊断",
       "winome diagnostics logs crash debug 诊断 日志 调试 性能",
       "winome:settings:diagnostics"},
      {"preferences-desktop-display-symbolic", "Display", "显示",
       "Windows Settings · Resolution & Scaling", "Windows 设置 · 分辨率与缩放",
       "display screen monitor resolution scale hdr night light 显示 分辨率 缩放 屏幕 刷新率 夜间模式",
       "ms-settings:display"},
      {"audio-volume-high-symbolic", "Sound", "声音",
       "Windows Settings · Output & Microphone", "Windows 设置 · 音量与麦克风",
       "sound audio volume speaker microphone output input 声音 音量 音频 麦克风 扬声器",
       "ms-settings:sound"},
      {"network-wireless-signal-good-symbolic", "Network & Internet",
       "网络和 Internet", "Windows Settings · Wi-Fi, VPN & Proxy",
       "Windows 设置 · Wi-Fi、VPN 与代理",
       "network wifi wlan ethernet vpn proxy internet 网络 无线 代理 以太网",
       "ms-settings:network"},
      {"bluetooth-active-symbolic", "Bluetooth & Devices", "蓝牙和其他设备",
       "Windows Settings · Mouse, Keyboard & Audio", "Windows 设置 · 蓝牙、鼠标与键盘",
       "bluetooth mouse keyboard devices printer camera 蓝牙 鼠标 键盘 设备 打印机",
       "ms-settings:bluetooth"},
      {"preferences-desktop-wallpaper-symbolic", "Personalization", "个性化",
       "Windows Settings · Wallpaper & Colors", "Windows 设置 · 壁纸、主题与颜色",
       "personalization wallpaper background theme color lockscreen 个性化 壁纸 背景 主题 颜色",
       "ms-settings:personalization"},
      {"battery-level-100-symbolic", "Power & Battery", "电源和电池",
       "Windows Settings · Sleep & Power Mode", "Windows 设置 · 电源模式与睡眠",
       "power battery sleep energy mode 电源 电池 睡眠 节能 性能模式",
       "ms-settings:powersleep"},
      {"drive-harddisk-symbolic", "Storage", "存储",
       "Windows Settings · Disk Space & Cleanup", "Windows 设置 · 磁盘空间与清理",
       "storage disk drive space cleanup 存储 磁盘 空间 清理 硬盘",
       "ms-settings:storagesense"},
      {"system-software-install-symbolic", "Installed Apps", "已安装的应用",
       "Windows Settings · Apps & Uninstall", "Windows 设置 · 应用管理与卸载",
       "apps applications uninstall programs default 应用 卸载 程序 默认应用",
       "ms-settings:appsfeatures"},
      {"preferences-desktop-locale-symbolic", "Time & Language", "时间和语言",
       "Windows Settings · Date, Region & IME", "Windows 设置 · 时区、区域与输入法",
       "time date language keyboard ime region 时间 日期 语言 输入法 时区",
       "ms-settings:dateandtime"},
      {"preferences-desktop-accessibility-symbolic", "Accessibility", "辅助功能",
       "Windows Settings · Text Size & Contrast", "Windows 设置 · 文本大小与对比度",
       "accessibility narrator magnifier contrast 辅助功能 放大镜 高对比度",
       "ms-settings:easeofaccess"},
      {"security-high-symbolic", "Privacy & Security", "隐私和安全性",
       "Windows Settings · Defender & Permissions", "Windows 设置 · 安全中心与权限",
       "privacy security defender firewall permissions 隐私 安全 防火墙 权限",
       "ms-settings:privacy"},
      {"software-update-available-symbolic", "Windows Update", "Windows 更新",
       "Windows Settings · Check for Updates", "Windows 设置 · 检查系统更新",
       "update upgrade windows update 更新 升级 系统更新 补丁",
       "ms-settings:windowsupdate"},
      {"utilities-system-monitor-symbolic", "Task Manager", "任务管理器",
       "System Tool · Processes & Performance", "系统工具 · 进程与资源监控",
       "task manager process performance cpu memory gpu taskmgr 任务管理器 进程 性能 内存",
       "taskmgr.exe"},
      {"preferences-system-symbolic", "Control Panel", "控制面板",
       "System Tool · Classic Windows Settings", "系统工具 · 传统控制面板",
       "control panel classic settings control 控制面板", "control.exe"},
      {"computer-symbolic", "Device Manager", "设备管理器",
       "System Tool · Hardware & Drivers", "系统工具 · 硬件与驱动管理",
       "device manager driver hardware devmgmt 设备管理器 驱动 硬件",
       "devmgmt.msc"},
  };

  std::vector<const SettingSpec *> matched_settings;
  for (const SettingSpec &sp : kSettings) {
    std::string hay = utf8_lower_local (std::string (sp.en) + " " + sp.zh + " " +
                                        sp.desc_en + " " + sp.desc_zh + " " +
                                        sp.keywords);
    if (match_all_search_terms (hay, self->terms))
      matched_settings.push_back (&sp);
  }
  if (!matched_settings.empty ()) {
    const size_t shown =
        std::min<size_t> (matched_settings.size (), kMaxListResults);
    const int more = static_cast<int> (matched_settings.size () - shown);
    GtkWidget *list_box = nullptr;
    GtkWidget *sec = make_provider_section_card (
        self, "emblem-system-symbolic", winome::tr ("Settings", "设置"), more,
        "winome:settings", &list_box);
    for (size_t i = 0; i < shown; ++i) {
      const SettingSpec *sp = matched_settings[i];
      int item_idx = static_cast<int> (self->items.size ());
      GtkWidget *row = make_list_row_button (
          self, nullptr, sp->icon, winome::tr (sp->en, sp->zh),
          winome::tr (sp->desc_en, sp->desc_zh), item_idx);
      gtk_box_append (GTK_BOX (list_box), row);

      SearchItem item;
      item.kind = SearchItemKind::Setting;
      item.button = row;
      item.payload = sp->cmd;
      self->items.push_back (std::move (item));
    }
    gtk_box_append (GTK_BOX (self->content), sec);
  }

  // 6. Direct URL / Command ("> ...") / Web Search Provider.
  const bool is_url = is_url_like_query (trimmed);
  const bool is_cmd_prefix =
      trimmed.size () >= 2 && trimmed[0] == '>' &&
      !trim_utf8 (trimmed.substr (1)).empty ();
  const bool is_web_prefix =
      (joined_lower.rfind ("? ", 0) == 0 || joined_lower.rfind ("g ", 0) == 0 ||
       joined_lower.rfind ("b ", 0) == 0) &&
      trimmed.size () > 2;

  if (is_url || is_cmd_prefix || is_web_prefix || self->items.size () <= 2) {
    GtkWidget *list_box = nullptr;
    std::string web_query =
        is_web_prefix ? trim_utf8 (trimmed.substr (2)) : trimmed;
    std::string search_url =
        "https://www.bing.com/search?q=" + url_encode_utf8 (web_query);
    if (joined_lower.rfind ("g ", 0) == 0) {
      search_url =
          "https://www.google.com/search?q=" + url_encode_utf8 (web_query);
    }

    if (is_cmd_prefix) {
      std::string cmd = trim_utf8 (trimmed.substr (1));
      GtkWidget *sec = make_provider_section_card (
          self, "utilities-terminal-symbolic",
          winome::tr ("Run Command", "运行命令"), 0, "cmd.exe", &list_box);
      int item_idx = static_cast<int> (self->items.size ());
      GtkWidget *row = make_list_row_button (
          self, nullptr, "utilities-terminal-symbolic", cmd,
          winome::tr ("Execute command", "执行命令"), item_idx);
      gtk_box_append (GTK_BOX (list_box), row);
      gtk_box_append (GTK_BOX (self->content), sec);

      SearchItem item;
      item.kind = SearchItemKind::UrlOrCommand;
      item.button = row;
      item.payload = cmd;
      self->items.push_back (std::move (item));
    } else if (is_url) {
      std::string full_url = trimmed;
      if (joined_lower.rfind ("http://", 0) != 0 &&
          joined_lower.rfind ("https://", 0) != 0) {
        full_url = "https://" + trimmed;
      }
      GtkWidget *sec = make_provider_section_card (
          self, "web-browser-symbolic", winome::tr ("Web", "网页"), 0,
          full_url, &list_box);
      int item_idx = static_cast<int> (self->items.size ());
      GtkWidget *row = make_list_row_button (
          self, nullptr, "web-browser-symbolic", full_url,
          winome::tr ("Open in default browser", "在默认浏览器中打开"),
          item_idx);
      gtk_box_append (GTK_BOX (list_box), row);
      gtk_box_append (GTK_BOX (self->content), sec);

      SearchItem item;
      item.kind = SearchItemKind::UrlOrCommand;
      item.button = row;
      item.payload = full_url;
      self->items.push_back (std::move (item));
    } else if (!web_query.empty () && !is_path_like_query (trimmed)) {
      GtkWidget *sec = make_provider_section_card (
          self, "web-browser-symbolic", winome::tr ("Web Search", "网页搜索"),
          0, search_url, &list_box);
      int item_idx = static_cast<int> (self->items.size ());
      std::string label =
          std::string (winome::tr ("Search the web for \"", "在浏览器中搜索 “")) +
          web_query + winome::tr ("\"", "”");
      GtkWidget *row = make_list_row_button (
          self, nullptr, "edit-find-symbolic", label,
          winome::tr ("Open in default browser", "在默认浏览器中打开"),
          item_idx);
      gtk_box_append (GTK_BOX (list_box), row);
      gtk_box_append (GTK_BOX (self->content), sec);

      SearchItem item;
      item.kind = SearchItemKind::UrlOrCommand;
      item.button = row;
      item.payload = search_url;
      self->items.push_back (std::move (item));
    }
  }

  // Schedule debounced async File & Folder search worker (search.js:808-809).
  if (trimmed.size () >= 2 || is_path_like_query (trimmed)) {
    self->async_files_pending = true;
    self->async_debounce_timer = g_timeout_add (
        kAsyncSearchDebounceMs, on_async_debounce_fire, self);
  }

  if (!self->items.empty ())
    set_selected_index (self, 0);
  update_status_visibility (self);
}

static void
winome_search_view_dispose (GObject *object)
{
  WinomeSearchView *self = WINOME_SEARCH_VIEW (object);
  cancel_async_search (self);
  if (self->alive != nullptr)
    self->alive->store (false, std::memory_order_release);
  G_OBJECT_CLASS (winome_search_view_parent_class)->dispose (object);
}

static void
winome_search_view_finalize (GObject *object)
{
  WinomeSearchView *self = WINOME_SEARCH_VIEW (object);
  using StrT = std::string;
  using TermsT = std::vector<std::string>;
  using ItemsT = std::vector<SearchItem>;
  using AliveT = std::shared_ptr<std::atomic<bool>>;
  using GenT = std::shared_ptr<std::atomic<uint64_t>>;
  self->shared_gen.~GenT ();
  self->alive.~AliveT ();
  self->items.~ItemsT ();
  self->terms.~TermsT ();
  self->raw_query.~StrT ();
  G_OBJECT_CLASS (winome_search_view_parent_class)->finalize (object);
}

static void
winome_search_view_class_init (WinomeSearchViewClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS (klass);
  oc->dispose = winome_search_view_dispose;
  oc->finalize = winome_search_view_finalize;
}

static void
winome_search_view_init (WinomeSearchView *self)
{
  new (&self->raw_query) std::string ();
  new (&self->terms) std::vector<std::string> ();
  new (&self->items) std::vector<SearchItem> ();
  new (&self->alive) std::shared_ptr<std::atomic<bool>> (
      std::make_shared<std::atomic<bool>> (true));
  new (&self->shared_gen) std::shared_ptr<std::atomic<uint64_t>> (
      std::make_shared<std::atomic<uint64_t>> (0));

  gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                  GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_name (GTK_WIDGET (self), "searchResults");
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  self->scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (self->scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_hexpand (self->scroll, TRUE);
  gtk_widget_set_vexpand (self->scroll, TRUE);

  self->content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_set_name (self->content, "searchResultsContent");
  gtk_widget_set_halign (self->content, GTK_ALIGN_FILL);
  gtk_widget_set_valign (self->content, GTK_ALIGN_START);
  gtk_widget_set_margin_bottom (self->content, 16);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (self->scroll),
                                 self->content);
  gtk_box_append (GTK_BOX (self), self->scroll);

  // Status container (.search-statusbox: spinner + .search-statustext, search.js:637-653).
  self->status_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 20);
  gtk_widget_add_css_class (self->status_box, "search-statusbox");
  gtk_widget_set_halign (self->status_box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (self->status_box, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (self->status_box, TRUE);
  gtk_widget_set_vexpand (self->status_box, TRUE);

  self->status_spinner = gtk_spinner_new ();
  gtk_widget_set_size_request (self->status_spinner, 48, 48);
  gtk_widget_set_halign (self->status_spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (self->status_spinner, FALSE);
  gtk_box_append (GTK_BOX (self->status_box), self->status_spinner);

  self->status_label =
      gtk_label_new (winome::tr ("No results", "无搜索结果"));
  gtk_widget_add_css_class (self->status_label, "search-statustext");
  gtk_widget_set_halign (self->status_label, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (self->status_box), self->status_label);

  gtk_widget_set_visible (self->status_box, FALSE);
  gtk_box_append (GTK_BOX (self), self->status_box);
}

}  // namespace

// --- Pure Public Helpers -----------------------------------------------------

std::vector<std::string>
split_search_terms (const std::string &query_utf8)
{
  std::string lowered = utf8_lower_local (trim_utf8 (query_utf8));
  std::vector<std::string> terms;
  size_t i = 0;
  while (i < lowered.size ()) {
    while (i < lowered.size () &&
           std::isspace (static_cast<unsigned char> (lowered[i]))) {
      ++i;
    }
    size_t start = i;
    while (i < lowered.size () &&
           !std::isspace (static_cast<unsigned char> (lowered[i]))) {
      ++i;
    }
    if (i > start)
      terms.push_back (lowered.substr (start, i - start));
  }
  return terms;
}

bool
match_all_search_terms (const std::string &haystack_lower,
                        const std::vector<std::string> &terms)
{
  if (terms.empty ())
    return true;
  for (const std::string &t : terms) {
    if (t.empty ())
      continue;
    if (haystack_lower.find (t) == std::string::npos)
      return false;
  }
  return true;
}

std::string
highlight_search_terms_markup (const std::string &text,
                               const std::vector<std::string> &terms)
{
  auto escape_slice = [] (const std::string &slice) -> std::string {
    if (slice.empty ())
      return {};
    gchar *esc =
        g_markup_escape_text (slice.c_str (), static_cast<gssize> (slice.size ()));
    if (esc == nullptr)
      return {};
    std::string out (esc);
    g_free (esc);
    return out;
  };

  if (text.empty () || terms.empty ())
    return escape_slice (text);

  std::string lowered = utf8_lower_local (text);
  // Only map byte offsets directly when UTF-8 case folding preserved length
  // (true for ASCII, CJK, and nearly all scripts); fall back safely otherwise.
  if (lowered.size () != text.size ())
    return escape_slice (text);

  std::vector<std::pair<size_t, size_t>> ranges;
  for (const std::string &term : terms) {
    if (term.empty ())
      continue;
    size_t pos = 0;
    while ((pos = lowered.find (term, pos)) != std::string::npos) {
      ranges.emplace_back (pos, pos + term.size ());
      pos += term.size ();
    }
  }
  if (ranges.empty ())
    return escape_slice (text);

  std::sort (ranges.begin (), ranges.end ());
  std::vector<std::pair<size_t, size_t>> merged;
  for (const auto &r : ranges) {
    if (merged.empty () || r.first > merged.back ().second)
      merged.push_back (r);
    else
      merged.back ().second = std::max (merged.back ().second, r.second);
  }

  std::string out;
  out.reserve (text.size () + merged.size () * 8);
  size_t cursor = 0;
  for (const auto &r : merged) {
    if (r.first > cursor)
      out += escape_slice (text.substr (cursor, r.first - cursor));
    out += "<b>";
    out += escape_slice (text.substr (r.first, r.second - r.first));
    out += "</b>";
    cursor = r.second;
  }
  if (cursor < text.size ())
    out += escape_slice (text.substr (cursor));
  return out;
}

bool
try_eval_search_math (const std::string &raw_query, std::string *out_result)
{
  std::string trimmed = trim_utf8 (raw_query);
  if (trimmed.size () < 2 || trimmed.size () > 96)
    return false;
  double val = 0.0;
  bool was_radix = false;
  MathParser parser (trimmed);
  if (!parser.parse (&val, &was_radix))
    return false;
  char buf[96];
  if (std::fabs (val - std::round (val)) < 1e-9 && std::fabs (val) < 1e15) {
    long long iv = static_cast<long long> (std::llround (val));
    if (was_radix && iv >= 0) {
      std::snprintf (buf, sizeof (buf), "%lld (0x%llX)", iv,
                     static_cast<unsigned long long> (iv));
    } else {
      std::snprintf (buf, sizeof (buf), "%lld", iv);
    }
  } else {
    std::snprintf (buf, sizeof (buf), "%.10g", val);
  }
  if (out_result != nullptr)
    *out_result = buf;
  return true;
}

bool
is_url_like_query (const std::string &trimmed)
{
  if (trimmed.size () < 4 || trimmed.find (' ') != std::string::npos)
    return false;
  std::string low = utf8_lower_local (trimmed);
  if (low.rfind ("http://", 0) == 0 || low.rfind ("https://", 0) == 0 ||
      low.rfind ("www.", 0) == 0) {
    return true;
  }
  size_t dot = low.find ('.');
  if (dot == std::string::npos || dot == 0 || dot + 2 >= low.size ())
    return false;
  static const char *kTlds[] = {".com", ".org", ".net", ".cn",  ".io",
                                ".dev", ".edu", ".gov", ".app", ".ai"};
  for (const char *tld : kTlds) {
    size_t pos = low.find (tld, dot);
    if (pos != std::string::npos &&
        (pos + std::strlen (tld) == low.size () ||
         low[pos + std::strlen (tld)] == '/')) {
      return true;
    }
  }
  return false;
}

bool
is_path_like_query (const std::string &trimmed)
{
  if (trimmed.size () >= 2 &&
      std::isalpha (static_cast<unsigned char> (trimmed[0])) &&
      trimmed[1] == ':' &&
      (trimmed.size () == 2 || trimmed[2] == '\\' || trimmed[2] == '/')) {
    return true;
  }
  if (trimmed.size () >= 2 && trimmed[0] == '~' &&
      (trimmed[1] == '\\' || trimmed[1] == '/')) {
    return true;
  }
  if (trimmed.size () >= 3 && trimmed[0] == '%' &&
      trimmed.find ('%', 1) != std::string::npos) {
    return true;
  }
  return false;
}

// --- Public Widget API -------------------------------------------------------

GtkWidget *
overview_search_view_new (const OverviewSearchCallbacks &cbs)
{
  auto *self = WINOME_SEARCH_VIEW (
      g_object_new (winome_search_view_get_type (), nullptr));
  self->cbs = cbs;
  return GTK_WIDGET (self);
}

void
overview_search_view_set_query (GtkWidget *view, const std::string &raw_query)
{
  if (view == nullptr)
    return;
  WinomeSearchView *self = WINOME_SEARCH_VIEW (view);
  std::vector<std::string> next_terms = split_search_terms (raw_query);
  if (next_terms.empty ()) {
    overview_search_view_reset (view);
    return;
  }
  if (raw_query == self->raw_query && next_terms == self->terms)
    return;
  self->raw_query = raw_query;
  self->terms = std::move (next_terms);
  rebuild_sync_providers (self);
}

void
overview_search_view_reset (GtkWidget *view)
{
  if (view == nullptr)
    return;
  WinomeSearchView *self = WINOME_SEARCH_VIEW (view);
  cancel_async_search (self);
  self->raw_query.clear ();
  self->terms.clear ();
  self->items.clear ();
  self->grid_count = 0;
  self->files_insert_idx = 0;
  self->selected_idx = -1;
  clear_box_children (self->content);
  self->files_section_host = nullptr;
  update_status_visibility (self);
}

bool
overview_search_view_navigate (GtkWidget *view, guint keyval,
                               bool entry_caret_at_end)
{
  if (view == nullptr)
    return false;
  WinomeSearchView *self = WINOME_SEARCH_VIEW (view);
  if (self->items.empty ())
    return false;

  const int n = static_cast<int> (self->items.size ());
  const int cur = self->selected_idx < 0 ? 0 : self->selected_idx;

  if (keyval == GDK_KEY_Tab) {
    set_selected_index (self, (cur + 1) % n);
    return true;
  }
  if (keyval == GDK_KEY_ISO_Left_Tab) {
    set_selected_index (self, (cur - 1 + n) % n);
    return true;
  }
  if (keyval == GDK_KEY_Down) {
    // From top GridSearchResults row, jump directly into first ListSearchResult
    // below it (matching GNOME Shell's 2D Down keynav).
    if (cur < self->grid_count && self->grid_count < n)
      set_selected_index (self, self->grid_count);
    else
      set_selected_index (self, (cur + 1) % n);
    return true;
  }
  if (keyval == GDK_KEY_Up) {
    if (cur == self->grid_count && self->grid_count > 0)
      set_selected_index (self, 0);
    else
      set_selected_index (self, (cur - 1 + n) % n);
    return true;
  }
  if (keyval == GDK_KEY_Right && self->grid_count > 1 &&
      cur < self->grid_count && (entry_caret_at_end || cur > 0)) {
    set_selected_index (self, (cur + 1) % self->grid_count);
    return true;
  }
  if (keyval == GDK_KEY_Left && self->grid_count > 1 &&
      cur > 0 && cur < self->grid_count) {
    set_selected_index (self, cur - 1);
    return true;
  }
  return false;
}

bool
overview_search_view_activate_selected (GtkWidget *view, bool new_instance)
{
  if (view == nullptr)
    return false;
  WinomeSearchView *self = WINOME_SEARCH_VIEW (view);
  if (self->items.empty ())
    return false;
  int idx = self->selected_idx >= 0 ? self->selected_idx : 0;
  activate_item (self, idx, new_instance);
  return true;
}

bool
overview_search_view_has_results (GtkWidget *view)
{
  if (view == nullptr)
    return false;
  return !WINOME_SEARCH_VIEW (view)->items.empty ();
}

}  // namespace winome
