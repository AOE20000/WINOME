// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// C++ bridge over the extracted St CSS engine. Lets the GTK4 host query any
// GNOME Shell stylesheet property (icon-size, spacing, padding, colors, ...)
// at runtime, replacing the build-time CSS-text patching approach.

#pragma once

#include <string>

namespace winome {

// A loaded GNOME Shell stylesheet + theme context.
class StEngine {
 public:
  StEngine();
  ~StEngine();

  StEngine(const StEngine&) = delete;
  StEngine& operator=(const StEngine&) = delete;

  // Load a stylesheet from a file path (UTF-8).
  bool load_stylesheet(const std::string& path);

  // Load a stylesheet from a GResource URI (resource://...).
  bool load_stylesheet_resource(const std::string& resource_uri);

  // A resolved style for one CSS node, mirroring GNOME's StThemeNode.
  class Node {
   public:
    Node(const StEngine& engine,
         const Node* parent,
         const std::string& element_id,
         const std::string& element_class,
         const std::string& pseudo_class);
    ~Node();

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    // Look up a length property in physical pixels (e.g. "icon-size",
    // "spacing", "-natural-hpadding"). Returns false if not found.
    bool lookup_length(const std::string& property, double* out) const;
    // Look up a plain double property.
    bool lookup_double(const std::string& property, double* out) const;
    // Look up a color property into #RRGGBB[AA].
    bool lookup_color(const std::string& property, std::string* out) const;

   private:
    void* node_;
  };

  // Convenience: resolve a node with an optional parent node.
  // Caller owns the returned Node.
  Node* create_node(const Node* parent,
                    const std::string& element_id,
                    const std::string& element_class,
                    const std::string& pseudo_class) const;

  // Global singleton, preloaded with the GNOME dark theme from GResource.
  static StEngine* get();

 private:
  void* context_;
  void* theme_;
};

}  // namespace winome
