// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Dedicated GNOME Shell SearchResultsView & SearchController port
// (reference: upstream/gnome-shell/js/ui/search.js, searchController.js,
// appDisplay.js AppSearchProvider, misc/systemActions.js, misc/util.js).

#pragma once

#include <gtk/gtk.h>
#include <windows.h>

#include <string>
#include <vector>

namespace winome {

struct SearchWindowCandidate {
  HWND hwnd = nullptr;
  int desktop_index = 0;
  std::string title;
  std::string process_name;
};

struct OverviewSearchCallbacks {
  void (*on_close_overview) (const char *reason, bool discard_focus,
                             void *user_data) = nullptr;
  void (*on_activate_window) (HWND hwnd, int desktop_index,
                              void *user_data) = nullptr;
  std::vector<SearchWindowCandidate> (*get_open_windows) (
      void *user_data) = nullptr;
  void (*on_open_settings) (const char *page, void *user_data) = nullptr;
  void *user_data = nullptr;
};

// --- Pure helpers (unit-tested in winome-tests) ------------------------------

// Split raw UTF-8 query into lowercased non-empty whitespace-delimited terms
// (port of searchController.js getTermsForSearchString).
std::vector<std::string> split_search_terms (const std::string &query_utf8);

// Returns true iff every term in @terms appears in @haystack_lower.
bool match_all_search_terms (const std::string &haystack_lower,
                             const std::vector<std::string> &terms);

// Escapes @text for Pango markup and wraps case-insensitive occurrences of any
// term in @terms with <b>...</b> (port of misc/util.js Highlighter).
std::string highlight_search_terms_markup (
    const std::string &text, const std::vector<std::string> &terms);

// Evaluates arithmetic/scientific/hex/binary expressions (e.g. "12*34",
// "sqrt(144)+2^3", "0xff"). Returns true when @raw_query is a valid expression
// containing an operator, function call, or 0x/0b prefix.
bool try_eval_search_math (const std::string &raw_query,
                           std::string *out_result);

bool is_url_like_query (const std::string &trimmed);
bool is_path_like_query (const std::string &trimmed);

// Returns true for Win32 messages that begin or advance Windows IME
// composition so the search entry can reveal focus without disturbing IMM.
inline bool
is_overview_ime_activation_message (UINT msg, WPARAM wParam)
{
  return msg == WM_IME_STARTCOMPOSITION || msg == WM_IME_COMPOSITION ||
         (msg == WM_KEYDOWN && wParam == VK_PROCESSKEY);
}

// Returns true when a key press arriving outside an editable widget should
// focus the overview search entry and be forwarded to it.
inline bool
should_overview_capture_search_key (guint keyval, GdkModifierType state,
                                    bool search_has_text, bool preedit_active)
{
  const guint mods = static_cast<guint> (state) &
                     (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK);
  if (mods == GDK_CONTROL_MASK &&
      (keyval == GDK_KEY_v || keyval == GDK_KEY_V)) {
    return true;
  }
  if (mods != 0)
    return false;
  const gunichar uc = gdk_keyval_to_unicode (keyval);
  if (uc < 0x20 || uc == 0x7f)
    return false;
  if (uc == ' ' && !search_has_text && !preedit_active)
    return false;
  return true;
}

// Returns true when an empty, dormant search entry should silently ignore
// stray space/backspace/delete keys instead of entering whitespace or beeping.
inline bool
should_overview_consume_dormant_key (guint keyval, GdkModifierType state,
                                     bool search_dormant, bool search_has_text,
                                     bool preedit_active)
{
  if (!search_dormant || search_has_text || preedit_active)
    return false;
  const guint mods = static_cast<guint> (state) &
                     (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK);
  if (mods != 0)
    return false;
  return keyval == GDK_KEY_space || keyval == GDK_KEY_BackSpace ||
         keyval == GDK_KEY_Delete;
}

// --- SearchResultsView widget lifecycle & control ----------------------------

GtkWidget *overview_search_view_new (const OverviewSearchCallbacks &cbs);

// Update query (trimmed/tokenized internally). Empty query resets and clears.
void overview_search_view_set_query (GtkWidget *view,
                                     const std::string &raw_query);

// Cancel pending async searches and clear displayed results.
void overview_search_view_reset (GtkWidget *view);

// Keyboard navigation (returns true if consumed).
// Supports Up/Down/Tab/Shift+Tab and horizontal Left/Right across top grid.
bool overview_search_view_navigate (GtkWidget *view, guint keyval,
                                    bool entry_caret_at_end);

// Activate currently selected result (Enter / Ctrl+Enter for new instance).
bool overview_search_view_activate_selected (GtkWidget *view,
                                             bool new_instance);

// Returns true if at least one result item is currently displayed.
bool overview_search_view_has_results (GtkWidget *view);

}  // namespace winome
