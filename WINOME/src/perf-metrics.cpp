// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors

#include "perf-metrics.h"

#include <glib.h>
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstring>
#include <mutex>

namespace winome {
namespace {

struct PerfStore {
  std::mutex mu;
  int64_t t0_us = 0;
  bool startup_logged = false;
  StartupTimings startup{};
  FrameTimeTracker::Stats overview_frames{};
  CapabilityStatus caps[static_cast<size_t>(SubsystemId::Count)]{};
  bool caps_initialized = false;
};

PerfStore& store() {
  static PerfStore s;
  return s;
}

void ensure_caps_init_locked(PerfStore& s) {
  if (s.caps_initialized)
    return;
  s.caps_initialized = true;
  for (size_t i = 0; i < static_cast<size_t>(SubsystemId::Count); ++i) {
    auto id = static_cast<SubsystemId>(i);
    s.caps[i].id = id;
    s.caps[i].name = capability_name(id);
    s.caps[i].state = CapabilityState::Unknown;
    s.caps[i].failure_count = 0;
  }
}

}  // namespace

void perf_mark_process_start() {
  auto& s = store();
  std::lock_guard<std::mutex> lock(s.mu);
  if (s.t0_us == 0)
    s.t0_us = g_get_monotonic_time();
}

void perf_record_phase_ms(const char* phase, double elapsed_ms) {
  if (phase == nullptr)
    return;
  auto& s = store();
  std::lock_guard<std::mutex> lock(s.mu);
  if (std::strcmp(phase, "pre_gtk") == 0) {
    s.startup.pre_gtk_ms = elapsed_ms;
  } else if (std::strcmp(phase, "gtk_init") == 0) {
    s.startup.gtk_init_ms = elapsed_ms;
  } else if (std::strcmp(phase, "idle_wait") == 0) {
    s.startup.idle_wait_ms = elapsed_ms;
  } else if (std::strcmp(phase, "fontmap") == 0) {
    s.startup.fontmap_ms = elapsed_ms;
  } else if (std::strcmp(phase, "panel") == 0) {
    s.startup.panel_ms = elapsed_ms;
  } else if (std::strcmp(phase, "overview") == 0 ||
             std::strcmp(phase, "overview_init") == 0) {
    s.startup.overview_init_ms = elapsed_ms;
  } else if (std::strcmp(phase, "ipc") == 0) {
    s.startup.ipc_ms = elapsed_ms;
  } else if (std::strcmp(phase, "recovery") == 0) {
    s.startup.recovery_ms = elapsed_ms;
  } else if (std::strcmp(phase, "theming") == 0) {
    s.startup.theming_ms = elapsed_ms;
  } else if (std::strcmp(phase, "plugins") == 0) {
    s.startup.plugins_ms = elapsed_ms;
  } else if (std::strcmp(phase, "activate") == 0) {
    s.startup.gtk_activate_ms = elapsed_ms;
  }
}

void perf_record_fonts_stats(int loaded_files, int family_count, double ms) {
  auto& s = store();
  std::lock_guard<std::mutex> lock(s.mu);
  s.startup.fonts_loaded_files = loaded_files;
  s.startup.fonts_family_count = family_count;
  s.startup.fontmap_ms = ms;
}

void perf_finish_startup() {
  StartupTimings copy;
  {
    auto& s = store();
    std::lock_guard<std::mutex> lock(s.mu);
    int64_t now = g_get_monotonic_time();
    if (s.t0_us > 0)
      s.startup.total_startup_ms = static_cast<double>(now - s.t0_us) / 1000.0;
    if (s.startup_logged)
      return;
    s.startup_logged = true;
    copy = s.startup;
  }

  ProcessTelemetry mem = perf_sample_process_telemetry();
  g_print(
      "[perf] startup total=%.1fms (pre_gtk=%.1fms, gtk_init=%.1fms, "
      "recovery=%.1fms, fontmap=%.1fms[%d/%d], panel=%.1fms, overview=%.1fms, "
      "theming=%.1fms, plugins=%.1fms, ipc=%.1fms, activate=%.1fms, "
      "idle_wait=%.1fms) | mem private=%.1fMB ws=%.1fMB gdi=%u user=%u\n",
      copy.total_startup_ms, copy.pre_gtk_ms, copy.gtk_init_ms,
      copy.recovery_ms, copy.fontmap_ms,
      copy.fonts_loaded_files, copy.fonts_family_count, copy.panel_ms,
      copy.overview_init_ms, copy.theming_ms, copy.plugins_ms, copy.ipc_ms,
      copy.gtk_activate_ms, copy.idle_wait_ms,
      static_cast<double>(mem.private_bytes) / (1024.0 * 1024.0),
      static_cast<double>(mem.working_set_bytes) / (1024.0 * 1024.0),
      mem.gdi_objects, mem.user_objects);
}

StartupTimings perf_get_startup_timings() {
  auto& s = store();
  std::lock_guard<std::mutex> lock(s.mu);
  return s.startup;
}

ProcessTelemetry perf_sample_process_telemetry() {
  ProcessTelemetry out{};
  HANDLE proc = GetCurrentProcess();
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  pmc.cb = sizeof(pmc);
  if (GetProcessMemoryInfo(proc, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                           sizeof(pmc))) {
    out.working_set_bytes = pmc.WorkingSetSize;
    out.private_bytes = pmc.PrivateUsage;
  }
  out.gdi_objects = GetGuiResources(proc, GR_GDIOBJECTS);
  out.user_objects = GetGuiResources(proc, GR_USEROBJECTS);
  return out;
}

// --- FrameTimeTracker --------------------------------------------------------

FrameTimeTracker::FrameTimeTracker(int64_t drop_threshold_us)
    : drop_threshold_us_(drop_threshold_us) {}

void FrameTimeTracker::reset() {
  prev_frame_us_ = 0;
  frames_ = 0;
  dropped_ = 0;
  sum_us_ = 0.0;
  max_us_ = 0.0;
  ring_pos_ = 0;
  ring_count_ = 0;
}

void FrameTimeTracker::record_frame_time_us(int64_t frame_time_us) {
  if (frame_time_us <= 0)
    return;
  if (prev_frame_us_ > 0 && frame_time_us > prev_frame_us_) {
    int64_t dt = frame_time_us - prev_frame_us_;
    if (dt < 250000) {  // ignore inter-animation idle pauses
      ++frames_;
      if (dt > drop_threshold_us_)
        ++dropped_;
      double d = static_cast<double>(dt);
      sum_us_ += d;
      if (d > max_us_)
        max_us_ = d;
      ring_us_[ring_pos_] = d;
      ring_pos_ = (ring_pos_ + 1) % kRingSize;
      if (ring_count_ < kRingSize)
        ++ring_count_;
    }
  }
  prev_frame_us_ = frame_time_us;
}

FrameTimeTracker::Stats FrameTimeTracker::snapshot() const {
  Stats st;
  st.frames = frames_;
  st.dropped_frames = dropped_;
  if (frames_ > 0)
    st.avg_frame_ms = (sum_us_ / static_cast<double>(frames_)) / 1000.0;
  st.max_frame_ms = max_us_ / 1000.0;
  if (ring_count_ > 0) {
    std::vector<double> sorted(ring_us_, ring_us_ + ring_count_);
    std::sort(sorted.begin(), sorted.end());
    size_t idx = (sorted.size() * 95) / 100;
    if (idx >= sorted.size())
      idx = sorted.size() - 1;
    st.p95_frame_ms = sorted[idx] / 1000.0;
  }
  return st;
}

void perf_record_overview_frame_stats(const FrameTimeTracker::Stats& stats) {
  auto& s = store();
  std::lock_guard<std::mutex> lock(s.mu);
  s.overview_frames.frames += stats.frames;
  s.overview_frames.dropped_frames += stats.dropped_frames;
  if (stats.max_frame_ms > s.overview_frames.max_frame_ms)
    s.overview_frames.max_frame_ms = stats.max_frame_ms;
  if (stats.frames > 0) {
    s.overview_frames.avg_frame_ms = stats.avg_frame_ms;
    s.overview_frames.p95_frame_ms = stats.p95_frame_ms;
  }
}

FrameTimeTracker::Stats perf_get_overview_frame_stats() {
  auto& s = store();
  std::lock_guard<std::mutex> lock(s.mu);
  return s.overview_frames;
}

// --- Subsystem capability & circuit breaker ---------------------------------

const char* capability_name(SubsystemId id) {
  switch (id) {
    case SubsystemId::VirtualDesktop:
      return "VirtualDesktop";
    case SubsystemId::SysTray:
      return "SysTray";
    case SubsystemId::WinRtBluetooth:
      return "WinRtBluetooth";
    case SubsystemId::WinRtSmtc:
      return "WinRtSmtc";
    case SubsystemId::WinRtNotifications:
      return "WinRtNotifications";
    case SubsystemId::Brightness:
      return "Brightness";
    case SubsystemId::Wlan:
      return "Wlan";
    case SubsystemId::Fonts:
      return "Fonts";
    case SubsystemId::IpcServer:
      return "IpcServer";
    case SubsystemId::Count:
      return "Unknown";
  }
  return "Unknown";
}

const char* capability_state_label(CapabilityState state) {
  switch (state) {
    case CapabilityState::Unknown:
      return "unknown";
    case CapabilityState::Active:
      return "active";
    case CapabilityState::Degraded:
      return "degraded";
    case CapabilityState::Tripped:
      return "tripped";
  }
  return "unknown";
}

void capability_set(SubsystemId id,
                    CapabilityState state,
                    const std::string& detail) {
  size_t idx = static_cast<size_t>(id);
  if (idx >= static_cast<size_t>(SubsystemId::Count))
    return;
  auto& s = store();
  std::lock_guard<std::mutex> lock(s.mu);
  ensure_caps_init_locked(s);
  auto& c = s.caps[idx];
  const bool changed = (c.state != state || c.detail != detail);
  c.state = state;
  if (state == CapabilityState::Active)
    c.failure_count = 0;
  c.detail = detail;
  if (changed && !detail.empty()) {
    g_print("[abi] %s -> %s (%s)\n", c.name, capability_state_label(state),
            detail.c_str());
  }
}

void capability_record_failure(SubsystemId id,
                               const std::string& detail,
                               uint32_t trip_threshold) {
  size_t idx = static_cast<size_t>(id);
  if (idx >= static_cast<size_t>(SubsystemId::Count))
    return;
  auto& s = store();
  std::lock_guard<std::mutex> lock(s.mu);
  ensure_caps_init_locked(s);
  auto& c = s.caps[idx];
  ++c.failure_count;
  c.detail = detail;
  if (trip_threshold > 0 && c.failure_count >= trip_threshold) {
    if (c.state != CapabilityState::Tripped) {
      c.state = CapabilityState::Tripped;
      g_print("[abi] circuit-breaker TRIPPED for %s after %u failures (%s)\n",
              c.name, c.failure_count, detail.c_str());
    }
  } else if (c.state == CapabilityState::Unknown) {
    c.state = CapabilityState::Degraded;
  }
}

CapabilityStatus capability_get(SubsystemId id) {
  size_t idx = static_cast<size_t>(id);
  auto& s = store();
  std::lock_guard<std::mutex> lock(s.mu);
  ensure_caps_init_locked(s);
  if (idx >= static_cast<size_t>(SubsystemId::Count))
    return {};
  return s.caps[idx];
}

std::vector<CapabilityStatus> capability_snapshot_all() {
  auto& s = store();
  std::lock_guard<std::mutex> lock(s.mu);
  ensure_caps_init_locked(s);
  std::vector<CapabilityStatus> out;
  out.reserve(static_cast<size_t>(SubsystemId::Count));
  for (size_t i = 0; i < static_cast<size_t>(SubsystemId::Count); ++i)
    out.push_back(s.caps[i]);
  return out;
}

void capability_log_summary() {
  auto all = capability_snapshot_all();
  for (const auto& c : all) {
    g_print("[abi] %-18s %-8s fails=%u%s%s\n",
            c.name, capability_state_label(c.state), c.failure_count,
            c.detail.empty() ? "" : " : ", c.detail.c_str());
  }
}

}  // namespace winome
