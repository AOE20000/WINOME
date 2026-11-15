// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Startup phase profiling, animation frame-time tracking, process resource
// telemetry, and cross-version subsystem ABI capability / circuit-breaker
// health tracking.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace winome {

// --- Startup & runtime performance telemetry ---------------------------------

struct StartupTimings {
  double fontmap_ms = 0.0;
  double panel_ms = 0.0;
  double overview_init_ms = 0.0;
  double ipc_ms = 0.0;
  double total_startup_ms = 0.0;
  int fonts_loaded_files = 0;
  int fonts_family_count = 0;
};

struct ProcessTelemetry {
  uint64_t working_set_bytes = 0;
  uint64_t private_bytes = 0;
  uint32_t gdi_objects = 0;
  uint32_t user_objects = 0;
};

// Marks overall process startup t0 and records individual phase durations.
void perf_mark_process_start();
void perf_record_phase_ms(const char* phase, double elapsed_ms);
void perf_record_fonts_stats(int loaded_files, int family_count, double ms);
void perf_finish_startup();
StartupTimings perf_get_startup_timings();

// Collects current process RSS / private commit and Win32 GDI/USER handles.
ProcessTelemetry perf_sample_process_telemetry();

// Lightweight tick-callback frame-time aggregator (no heap allocation per frame).
class FrameTimeTracker {
 public:
  struct Stats {
    uint64_t frames = 0;
    uint64_t dropped_frames = 0;  // interval > drop_threshold_us
    double avg_frame_ms = 0.0;
    double max_frame_ms = 0.0;
    double p95_frame_ms = 0.0;
  };

  explicit FrameTimeTracker(int64_t drop_threshold_us = 20000);

  void reset();
  // Pass frame clock timestamp in microseconds. Ignores huge pauses > 250ms
  // so idle gaps between separate animations do not skew frame statistics.
  void record_frame_time_us(int64_t frame_time_us);
  Stats snapshot() const;

 private:
  static constexpr int kRingSize = 128;
  int64_t drop_threshold_us_;
  int64_t prev_frame_us_ = 0;
  uint64_t frames_ = 0;
  uint64_t dropped_ = 0;
  double sum_us_ = 0.0;
  double max_us_ = 0.0;
  double ring_us_[kRingSize]{};
  int ring_pos_ = 0;
  int ring_count_ = 0;
};

void perf_record_overview_frame_stats(const FrameTimeTracker::Stats& stats);
FrameTimeTracker::Stats perf_get_overview_frame_stats();

// --- Subsystem ABI probe & circuit-breaker registry --------------------------

enum class SubsystemId {
  VirtualDesktop = 0,
  SysTray,
  WinRtBluetooth,
  WinRtSmtc,
  WinRtNotifications,
  Brightness,
  Wlan,
  Fonts,
  IpcServer,
  Count
};

enum class CapabilityState {
  Unknown = 0,
  Active,
  Degraded,  // Clean fallback due to OS capability / policy / unsupported build
  Tripped,   // Disabled cleanly after runtime timeout / repeated error / probe failure
};

struct CapabilityStatus {
  SubsystemId id = SubsystemId::VirtualDesktop;
  const char* name = "";
  CapabilityState state = CapabilityState::Unknown;
  uint32_t failure_count = 0;
  std::string detail;
};

const char* capability_name(SubsystemId id);
const char* capability_state_label(CapabilityState state);

void capability_set(SubsystemId id,
                    CapabilityState state,
                    const std::string& detail = {});
// Increments failure count; transitions cleanly to Tripped once threshold hit.
void capability_record_failure(SubsystemId id,
                               const std::string& detail,
                               uint32_t trip_threshold = 3);
CapabilityStatus capability_get(SubsystemId id);
std::vector<CapabilityStatus> capability_snapshot_all();
void capability_log_summary();

}  // namespace winome
