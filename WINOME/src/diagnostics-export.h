// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Self-contained troubleshooting bundle (.zip) exporter for both the GTK4
// Settings window and the pure-Win32 Crash Recovery dialog. Uses zlib raw
// Deflate with UTF-8 PKZIP headers (zero subprocesses, full Unicode path
// safety on Windows).

#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace winome {

struct DiagnosticsExtraFile {
  std::string archive_name;  // UTF-8 forward-slash path inside zip
  std::string content;       // raw bytes
};

struct DiagnosticsExportOptions {
  // Directory containing WINOME logs, config.json, last-crash.json, crashes/.
  // If empty, defaults to %LOCALAPPDATA%\WINOME.
  std::wstring data_dir;

  // Explicit output .zip file path. If empty, automatically generates
  // WINOME-diagnostics-YYYYMMDD-HHMMSS.zip on the user's Desktop (or under
  // data_dir\exports as fallback).
  std::wstring output_zip_path;

  // Optional caller-generated report sections (e.g. live telemetry, crash state).
  std::string extra_summary_text;

  // Optional extra in-memory files to embed in the bundle.
  std::vector<DiagnosticsExtraFile> extra_files;

  // Bounds on crash dump inclusion so the zip stays GitHub-upload-friendly.
  size_t max_crash_dumps = 5;
  uint64_t max_total_dump_bytes = 64ULL * 1024 * 1024;

  // When true, flush stdio buffers before reading active log files.
  bool flush_stdio = true;
};

struct DiagnosticsExportResult {
  bool ok = false;
  std::wstring zip_path;
  size_t file_count = 0;
  uint64_t uncompressed_bytes = 0;
  uint64_t compressed_bytes = 0;
  std::string error;
};

// Low-level in-memory/stream PKZIP writer (exposed for unit tests).
class DiagnosticsZipWriter {
 public:
  bool open(const std::wstring& path);
  bool add_entry(const std::string& utf8_name,
                 const void* data,
                 size_t size,
                 bool compress = true);
  bool finish();
  void abort();

  size_t entry_count() const { return entries_.size(); }
  uint64_t total_uncompressed() const { return total_uncompressed_; }
  uint64_t total_compressed() const { return total_compressed_; }

 private:
  struct CentralEntry {
    std::string name;
    uint16_t method = 0;
    uint16_t dos_time = 0;
    uint16_t dos_date = 0;
    uint32_t crc32_val = 0;
    uint32_t comp_size = 0;
    uint32_t uncomp_size = 0;
    uint32_t local_header_offset = 0;
  };

  bool write_all(const void* data, size_t size);
  bool write_u16(uint16_t v);
  bool write_u32(uint32_t v);

  HANDLE file_ = INVALID_HANDLE_VALUE;
  std::wstring path_;
  uint64_t offset_ = 0;
  uint64_t total_uncompressed_ = 0;
  uint64_t total_compressed_ = 0;
  std::vector<CentralEntry> entries_;
};

// Exports WINOME logs, crash dumps, config, and system/monitor summary into a
// single .zip archive. Safe to call from both the main GTK process and the
// Win32 crash-recovery process.
DiagnosticsExportResult export_diagnostics_bundle(
    const DiagnosticsExportOptions& opts);

// Opens Windows Explorer with the exported .zip file pre-selected.
bool reveal_file_in_explorer(const std::wstring& file_path);

}  // namespace winome
