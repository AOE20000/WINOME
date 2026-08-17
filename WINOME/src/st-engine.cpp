// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// C++ bridge over the extracted St CSS engine.

#include "st-engine.h"

#include <gtk/gtk.h>

#include "st-compat-impl.h"
#include "st-theme.h"
#include "st-theme-node.h"

namespace winome {

StEngine::StEngine()
    : context_(st_theme_context_new()),
      theme_(st_theme_new(nullptr, nullptr, nullptr)) {
  st_theme_context_set_theme(static_cast<StThemeContext*>(context_),
                             static_cast<StTheme*>(theme_));
}

StEngine::~StEngine() {
  if (theme_)
    g_object_unref(theme_);
  if (context_)
    g_object_unref(context_);
}

bool StEngine::load_stylesheet(const std::string& path) {
  GFile* file = g_file_new_for_path(path.c_str());
  GError* error = nullptr;
  bool ok = st_theme_load_stylesheet(static_cast<StTheme*>(theme_), file,
                                     &error);
  if (!ok) {
    g_printerr("StEngine: failed to load %s: %s\n", path.c_str(),
               error ? error->message : "unknown");
    g_clear_error(&error);
  }
  g_object_unref(file);
  return ok;
}

bool StEngine::load_stylesheet_resource(const std::string& resource_uri) {
  GFile* file = g_file_new_for_uri(resource_uri.c_str());
  GError* error = nullptr;
  bool ok = st_theme_load_stylesheet(static_cast<StTheme*>(theme_), file,
                                     &error);
  if (!ok) {
    g_printerr("StEngine: failed to load %s: %s\n", resource_uri.c_str(),
               error ? error->message : "unknown");
    g_clear_error(&error);
  }
  g_object_unref(file);
  return ok;
}

StEngine::Node::Node(const StEngine& engine,
                     const Node* parent,
                     const std::string& element_id,
                     const std::string& element_class,
                     const std::string& pseudo_class) {
  StThemeNode* parent_node =
      parent ? static_cast<StThemeNode*>(parent->node_) : nullptr;
  node_ = st_theme_node_new(
      static_cast<StThemeContext*>(engine.context_),
      parent_node,
      static_cast<StTheme*>(engine.theme_),
      G_TYPE_OBJECT,
      element_id.empty() ? nullptr : element_id.c_str(),
      element_class.empty() ? nullptr : element_class.c_str(),
      pseudo_class.empty() ? nullptr : pseudo_class.c_str(),
      nullptr /* inline_style */);
}

StEngine::Node::~Node() {
  if (node_)
    g_object_unref(static_cast<StThemeNode*>(node_));
}

bool StEngine::Node::lookup_length(const std::string& property,
                                   double* out) const {
  double v = 0.0;
  gboolean found = st_theme_node_lookup_length(static_cast<StThemeNode*>(node_),
                                               property.c_str(), FALSE, &v);
  if (found && out)
    *out = v;
  return found != FALSE;
}

bool StEngine::Node::lookup_double(const std::string& property,
                                   double* out) const {
  double v = 0.0;
  gboolean found = st_theme_node_lookup_double(static_cast<StThemeNode*>(node_),
                                               property.c_str(), FALSE, &v);
  if (found && out)
    *out = v;
  return found != FALSE;
}

bool StEngine::Node::lookup_color(const std::string& property,
                                  std::string* out) const {
  CoglColor color;
  gboolean found = st_theme_node_lookup_color(static_cast<StThemeNode*>(node_),
                                              property.c_str(), FALSE, &color);
  if (!found || !out)
    return false;

  char buf[16];
  if (color.alpha == 0xff)
    snprintf(buf, sizeof(buf), "#%02x%02x%02x", color.red, color.green,
             color.blue);
  else
    snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", color.red, color.green,
             color.blue, color.alpha);
  *out = buf;
  return true;
}

StEngine::Node* StEngine::create_node(const Node* parent,
                                      const std::string& element_id,
                                      const std::string& element_class,
                                      const std::string& pseudo_class) const {
  return new Node(*this, parent, element_id, element_class, pseudo_class);
}

StEngine* StEngine::get() {
  static StEngine* instance = nullptr;
  if (instance == nullptr) {
    instance = new StEngine();
    // Load the raw GNOME theme (with St properties) from GResource. This is
    // the sassc-compiled gnome-shell-dark.css, distinct from the GTK4-
    // converted gnome-shell-gtk4.css used for rendering.
    if (!instance->load_stylesheet_resource(
            "resource:///org/winome/theme/gnome-shell-dark.css")) {
      g_printerr("StEngine: failed to load GNOME theme resource\n");
    }
  }
  return instance;
}

}  // namespace winome
