// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Hybrid third-party live wallpaper engine detector & Windows.Graphics.Capture
// (WGC) frame provider for the Activities Overview.
//
// When a third-party live wallpaper engine (Wallpaper Engine, Lively Wallpaper,
// Sucrose, etc.) hosts a render child inside WorkerW / Progman, this module
// captures it borderlessly on a dedicated MTA D3D11/WGC worker thread and
// delivers opaque BGRX GdkTextures to the GTK main thread — preserving GNOME's
// 30px rounded corners, drop shadows, multi-workspace strip, and exact Z-order.
// When no live wallpaper engine is active, detection returns immediately (<50us)
// with zero D3D11/WinRT initialization and zero background overhead.

#pragma once

#include <windows.h>

#include <gtk/gtk.h>

#include <cstdint>
#include <string>

namespace winome {

struct LiveWallpaperTarget {
  bool active = false;
  HWND engine_child_hwnd = nullptr;
  HWND capture_hwnd = nullptr;
  RECT host_rect{};
  RECT child_rect{};
  std::wstring process_name;
};

// Pure helpers (unit-tested in winome-tests).
std::string sanitize_live_wallpaper_mode(const std::string& mode);

bool is_live_wallpaper_candidate(const std::wstring& class_name,
                                 const std::wstring& process_basename,
                                 const RECT& child_rect,
                                 const RECT& monitor_rect,
                                 bool visible);

RECT compute_wgc_crop_rect(const RECT& host_rect,
                           const RECT& monitor_rect,
                           int tex_w,
                           int tex_h);

// Probe Progman / WorkerW for a visible third-party wallpaper engine covering
// @monitor_rect. Fast (<50us) and safe to call on the GTK main thread.
LiveWallpaperTarget live_wallpaper_detect_target(const RECT& monitor_rect);

// Callback fired on the GTK main thread (low priority) whenever a fresh frame
// arrives. Textures are owned by the caller for the duration of the callback;
// call g_object_ref() if retaining them. @thumb_tex may be nullptr on
// intermediate frames when only the full-resolution frame updated.
using LiveWallpaperFrameCb = void (*)(GdkTexture* full_tex,
                                      GdkTexture* thumb_tex,
                                      void* user_data);

// Start (or restart if target/mode changed) capturing @target for @monitor_rect.
// If @live_stream is false ("snapshot" mode), captures a single fresh frame and
// automatically closes the WGC session.
bool live_wallpaper_start(const LiveWallpaperTarget& target,
                          const RECT& monitor_rect,
                          bool live_stream,
                          LiveWallpaperFrameCb on_frame,
                          void* user_data);

// Pause or resume frame readbacks while keeping the WGC session alive (used to
// guarantee zero D3D11 contention during 250ms Overview animations). Frame 0
// still delivers immediately even when paused so opening Overview shows fresh
// content without delay.
void live_wallpaper_set_paused(bool paused);

// Stop any active capture session and join the worker thread cleanly.
void live_wallpaper_stop();

// True when a capture session is currently running or recently delivered a live
// frame for the active engine.
bool live_wallpaper_is_running();

}  // namespace winome
