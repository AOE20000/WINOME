// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Deterministic mock child-process plugin used by winome-tests.

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);

  const std::string mode = (argc > 1) ? argv[1] : "normal";
  if (mode == "crash") {
    std::fprintf(stderr, "mock crashing intentionally\n");
    Sleep(25);
    return 42;
  }

  std::printf("{\"type\":\"ready\"}\n");
  std::printf("{\"type\":\"log\",\"level\":\"info\",\"message\":\"mock booted\"}\n");
  std::fprintf(stderr, "mock stderr hello\n");
  std::printf(
      "{\"type\":\"topbar.upsert\",\"slot\":\"cpu\","
      "\"icon\":\"utilities-system-monitor-symbolic\","
      "\"text\":\"42%%\\u4e2d\",\"tooltip\":\"CPU\"}\n");
  std::printf(
      "{\"type\":\"quicksettings.upsert\",\"slot\":\"turbo\","
      "\"icon\":\"power-profile-performance-symbolic\","
      "\"title\":\"Turbo\",\"subtitle\":\"On\",\"active\":true}\n");
  // Deliberately attempt notify without capability in unit test manifest.
  std::printf(
      "{\"type\":\"notify\",\"title\":\"Denied\",\"body\":\"Unauthorized\"}\n");

  char buf[4096];
  while (std::fgets(buf, sizeof(buf), stdin) != nullptr) {
    const std::string line(buf);
    if (line.find("\"shutdown\"") != std::string::npos) {
      return 0;
    }
    if (line.find("\"ping\"") != std::string::npos) {
      std::printf("{\"type\":\"pong\"}\n");
    } else if (line.find("\"topbar.click\"") != std::string::npos) {
      std::printf(
          "{\"type\":\"topbar.upsert\",\"slot\":\"cpu\","
          "\"icon\":\"utilities-system-monitor-symbolic\","
          "\"text\":\"CLICKED\",\"tooltip\":\"Clicked\"}\n");
    } else if (line.find("\"quicksettings.toggle\"") != std::string::npos) {
      std::printf(
          "{\"type\":\"quicksettings.upsert\",\"slot\":\"turbo\","
          "\"icon\":\"power-profile-performance-symbolic\","
          "\"title\":\"Turbo\",\"subtitle\":\"Off\",\"active\":false}\n");
    } else if (line.find("\"desktop.changed\"") != std::string::npos) {
      std::fprintf(stderr, "mock saw desktop.changed\n");
    }
  }
  return 0;
}
