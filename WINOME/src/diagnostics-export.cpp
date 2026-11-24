// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors

#include "diagnostics-export.h"

#include "winome-version.h"

#include <shlobj.h>
#include <shellapi.h>
#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace winome {
namespace {

constexpr uint16_t kZipUtf8Flag = 0x0800u;
constexpr uint16_t kZipMethodStore = 0u;
constexpr uint16_t kZipMethodDeflate = 8u;
constexpr uint32_t kSigLocalHeader = 0x04034b50u;
constexpr uint32_t kSigCentralHeader = 0x02014b50u;
constexpr uint32_t kSigEndCentral = 0x06054b50u;

std::string wide_to_utf8(const std::wstring& w) {
  if (w.empty())
    return {};
  int need = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                 static_cast<int>(w.size()), nullptr, 0,
                                 nullptr, nullptr);
  if (need <= 0)
    return {};
  std::string out(static_cast<size_t>(need), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                      out.data(), need, nullptr, nullptr);
  return out;
}

void current_dos_datetime(uint16_t* out_time, uint16_t* out_date) {
  SYSTEMTIME st{};
  GetLocalTime(&st);
  const uint16_t year =
      (st.wYear >= 1980) ? static_cast<uint16_t>(st.wYear - 1980) : 0u;
  *out_date = static_cast<uint16_t>(((year & 0x7Fu) << 9) |
                                    ((st.wMonth & 0x0Fu) << 5) |
                                    (st.wDay & 0x1Fu));
  *out_time = static_cast<uint16_t>(((st.wHour & 0x1Fu) << 11) |
                                    ((st.wMinute & 0x3Fu) << 5) |
                                    ((st.wSecond / 2) & 0x1Fu));
}

bool read_file_shared(const std::wstring& path,
                      std::vector<uint8_t>* out,
                      uint64_t max_bytes = 128ULL * 1024 * 1024) {
  if (out == nullptr || path.empty())
    return false;
  out->clear();
  HANDLE h = CreateFileW(
      path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return false;

  LARGE_INTEGER sz{};
  if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0) {
    CloseHandle(h);
    return false;
  }

  uint64_t to_read = static_cast<uint64_t>(sz.QuadPart);
  if (to_read > max_bytes) {
    // Keep the newest tail when an unusually huge file exceeds the cap.
    LARGE_INTEGER seek_pos{};
    seek_pos.QuadPart = static_cast<LONGLONG>(to_read - max_bytes);
    SetFilePointerEx(h, seek_pos, nullptr, FILE_BEGIN);
    to_read = max_bytes;
  }

  out->resize(static_cast<size_t>(to_read));
  size_t total = 0;
  while (total < out->size()) {
    const DWORD chunk = static_cast<DWORD>(
        std::min<size_t>(out->size() - total, 4u * 1024u * 1024u));
    DWORD got = 0;
    if (!ReadFile(h, out->data() + total, chunk, &got, nullptr) || got == 0)
      break;
    total += got;
  }
  out->resize(total);
  CloseHandle(h);
  return true;
}

std::wstring default_data_dir() {
  wchar_t buf[MAX_PATH] = {};
  DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  std::wstring dir = (len > 0 && len < MAX_PATH) ? std::wstring(buf) : L".";
  dir += L"\\WINOME";
  return dir;
}

std::wstring default_output_zip_path(const std::wstring& data_dir) {
  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t fname[96];
  std::swprintf(fname, sizeof(fname) / sizeof(fname[0]),
                L"WINOME-diagnostics-%04u%02u%02u-%02u%02u%02u.zip",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

  PWSTR desktop = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)) &&
      desktop != nullptr) {
    std::wstring path(desktop);
    CoTaskMemFree(desktop);
    if (!path.empty() && path.back() != L'\\')
      path.push_back(L'\\');
    path += fname;
    return path;
  }
  if (desktop != nullptr)
    CoTaskMemFree(desktop);

  std::wstring fallback = data_dir;
  if (!fallback.empty() && fallback.back() != L'\\')
    fallback.push_back(L'\\');
  fallback += L"exports";
  CreateDirectoryW(data_dir.c_str(), nullptr);
  CreateDirectoryW(fallback.c_str(), nullptr);
  fallback.push_back(L'\\');
  fallback += fname;
  return fallback;
}

std::string query_windows_version() {
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll != nullptr) {
    auto fn = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
    if (fn != nullptr) {
      OSVERSIONINFOEXW osv{};
      osv.dwOSVersionInfoSize = sizeof(osv);
      if (fn(&osv) == 0) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "Windows %lu.%lu (Build %lu, SP %u.%u)",
                      osv.dwMajorVersion, osv.dwMinorVersion,
                      osv.dwBuildNumber, osv.wServicePackMajor,
                      osv.wServicePackMinor);
        return buf;
      }
    }
  }
  return "Windows x86_64";
}

struct MonitorLineCtx {
  std::string* out = nullptr;
  int index = 0;
};

BOOL CALLBACK enum_diag_monitors_proc(HMONITOR mon,
                                      HDC /*dc*/,
                                      LPRECT /*rc*/,
                                      LPARAM lp) {
  auto* ctx = reinterpret_cast<MonitorLineCtx*>(lp);
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfoW(mon, &mi)) {
    char buf[256];
    std::snprintf(
        buf, sizeof(buf),
        "  [%d] %s rect=(%ld,%ld)-(%ld,%ld) [%ldx%ld] work=(%ld,%ld)-(%ld,%ld)%s\n",
        ctx->index++, wide_to_utf8(mi.szDevice).c_str(),
        mi.rcMonitor.left, mi.rcMonitor.top,
        mi.rcMonitor.right, mi.rcMonitor.bottom,
        mi.rcMonitor.right - mi.rcMonitor.left,
        mi.rcMonitor.bottom - mi.rcMonitor.top,
        mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom,
        (mi.dwFlags & MONITORINFOF_PRIMARY) ? " [PRIMARY]" : "");
    *ctx->out += buf;
  }
  return TRUE;
}

struct CrashFileEntry {
  std::wstring full_path;
  std::wstring file_name;
  uint64_t write_time = 0;
  uint64_t file_size = 0;
  bool is_dump = false;
};

bool has_extension_ci(const std::wstring& name, const wchar_t* ext) {
  const size_t elen = std::wcslen(ext);
  if (name.size() < elen)
    return false;
  return _wcsicmp(name.c_str() + name.size() - elen, ext) == 0;
}

std::vector<CrashFileEntry> collect_crash_dir_files(const std::wstring& crash_dir) {
  std::vector<CrashFileEntry> files;
  std::wstring pattern = crash_dir;
  if (!pattern.empty() && pattern.back() != L'\\')
    pattern.push_back(L'\\');
  pattern += L"*";

  WIN32_FIND_DATAW fd{};
  HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
  if (find == INVALID_HANDLE_VALUE)
    return files;

  do {
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
      continue;
    std::wstring name(fd.cFileName);
    const bool is_dmp = has_extension_ci(name, L".dmp");
    const bool keep = is_dmp || has_extension_ci(name, L".json") ||
                      has_extension_ci(name, L".log") ||
                      has_extension_ci(name, L".txt");
    if (!keep)
      continue;
    CrashFileEntry e{};
    e.file_name = name;
    e.full_path = crash_dir + L"\\" + name;
    e.write_time = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                   fd.ftLastWriteTime.dwLowDateTime;
    e.file_size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) |
                  fd.nFileSizeLow;
    e.is_dump = is_dmp;
    files.push_back(std::move(e));
  } while (FindNextFileW(find, &fd));
  FindClose(find);

  // Newest files first so recent crash dumps are always prioritized.
  std::sort(files.begin(), files.end(),
            [](const CrashFileEntry& a, const CrashFileEntry& b) {
              return a.write_time > b.write_time;
            });
  return files;
}

}  // namespace

bool DiagnosticsZipWriter::open(const std::wstring& path) {
  abort();
  path_ = path;
  file_ = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file_ == INVALID_HANDLE_VALUE)
    return false;
  offset_ = 0;
  total_uncompressed_ = 0;
  total_compressed_ = 0;
  entries_.clear();
  return true;
}

bool DiagnosticsZipWriter::write_all(const void* data, size_t size) {
  if (file_ == INVALID_HANDLE_VALUE)
    return false;
  const auto* ptr = static_cast<const uint8_t*>(data);
  size_t remaining = size;
  while (remaining > 0) {
    const DWORD chunk =
        static_cast<DWORD>(std::min<size_t>(remaining, 4u * 1024u * 1024u));
    DWORD written = 0;
    if (!WriteFile(file_, ptr, chunk, &written, nullptr) || written != chunk)
      return false;
    ptr += written;
    remaining -= written;
    offset_ += written;
  }
  return true;
}

bool DiagnosticsZipWriter::write_u16(uint16_t v) {
  uint8_t b[2] = {static_cast<uint8_t>(v & 0xFFu),
                  static_cast<uint8_t>((v >> 8) & 0xFFu)};
  return write_all(b, 2);
}

bool DiagnosticsZipWriter::write_u32(uint32_t v) {
  uint8_t b[4] = {static_cast<uint8_t>(v & 0xFFu),
                  static_cast<uint8_t>((v >> 8) & 0xFFu),
                  static_cast<uint8_t>((v >> 16) & 0xFFu),
                  static_cast<uint8_t>((v >> 24) & 0xFFu)};
  return write_all(b, 4);
}

bool DiagnosticsZipWriter::add_entry(const std::string& utf8_name,
                                     const void* data,
                                     size_t size,
                                     bool compress) {
  if (file_ == INVALID_HANDLE_VALUE || utf8_name.empty() ||
      utf8_name.size() > 0xFFFFu || size > 0xFFFFFFFFULL ||
      offset_ > 0xFFFFFFFFULL) {
    return false;
  }

  const auto* raw = static_cast<const uint8_t*>(data);
  const uint32_t crc =
      (size > 0 && raw != nullptr)
          ? static_cast<uint32_t>(
                crc32(crc32(0L, Z_NULL, 0), raw, static_cast<uInt>(size)))
          : 0u;

  std::vector<uint8_t> deflated;
  uint16_t method = kZipMethodStore;
  const uint8_t* payload = raw;
  size_t payload_size = size;

  if (compress && size >= 64 && raw != nullptr) {
    z_stream zs{};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                     Z_DEFAULT_STRATEGY) == Z_OK) {
      deflated.resize(static_cast<size_t>(deflateBound(&zs, static_cast<uLong>(size))) + 16u);
      zs.next_in = const_cast<Bytef*>(raw);
      zs.avail_in = static_cast<uInt>(size);
      zs.next_out = deflated.data();
      zs.avail_out = static_cast<uInt>(deflated.size());
      const int rc = deflate(&zs, Z_FINISH);
      deflateEnd(&zs);
      if (rc == Z_STREAM_END && zs.total_out < size) {
        deflated.resize(zs.total_out);
        method = kZipMethodDeflate;
        payload = deflated.data();
        payload_size = deflated.size();
      }
    }
  }

  CentralEntry ce{};
  ce.name = utf8_name;
  ce.method = method;
  current_dos_datetime(&ce.dos_time, &ce.dos_date);
  ce.crc32_val = crc;
  ce.comp_size = static_cast<uint32_t>(payload_size);
  ce.uncomp_size = static_cast<uint32_t>(size);
  ce.local_header_offset = static_cast<uint32_t>(offset_);

  // Local file header (30 bytes + filename + payload).
  if (!write_u32(kSigLocalHeader) || !write_u16(20u) ||
      !write_u16(kZipUtf8Flag) || !write_u16(ce.method) ||
      !write_u16(ce.dos_time) || !write_u16(ce.dos_date) ||
      !write_u32(ce.crc32_val) || !write_u32(ce.comp_size) ||
      !write_u32(ce.uncomp_size) ||
      !write_u16(static_cast<uint16_t>(ce.name.size())) || !write_u16(0u) ||
      !write_all(ce.name.data(), ce.name.size())) {
    return false;
  }
  if (payload_size > 0 && !write_all(payload, payload_size))
    return false;

  total_uncompressed_ += size;
  total_compressed_ += payload_size;
  entries_.push_back(std::move(ce));
  return true;
}

bool DiagnosticsZipWriter::finish() {
  if (file_ == INVALID_HANDLE_VALUE)
    return false;
  if (entries_.size() > 0xFFFFu || offset_ > 0xFFFFFFFFULL) {
    abort();
    return false;
  }

  const uint32_t cd_offset = static_cast<uint32_t>(offset_);
  for (const auto& ce : entries_) {
    if (!write_u32(kSigCentralHeader) || !write_u16(20u) || !write_u16(20u) ||
        !write_u16(kZipUtf8Flag) || !write_u16(ce.method) ||
        !write_u16(ce.dos_time) || !write_u16(ce.dos_date) ||
        !write_u32(ce.crc32_val) || !write_u32(ce.comp_size) ||
        !write_u32(ce.uncomp_size) ||
        !write_u16(static_cast<uint16_t>(ce.name.size())) || !write_u16(0u) ||
        !write_u16(0u) || !write_u16(0u) || !write_u16(0u) || !write_u32(0u) ||
        !write_u32(ce.local_header_offset) ||
        !write_all(ce.name.data(), ce.name.size())) {
      abort();
      return false;
    }
  }
  const uint32_t cd_size = static_cast<uint32_t>(offset_ - cd_offset);

  const uint16_t count = static_cast<uint16_t>(entries_.size());
  if (!write_u32(kSigEndCentral) || !write_u16(0u) || !write_u16(0u) ||
      !write_u16(count) || !write_u16(count) || !write_u32(cd_size) ||
      !write_u32(cd_offset) || !write_u16(0u)) {
    abort();
    return false;
  }

  CloseHandle(file_);
  file_ = INVALID_HANDLE_VALUE;
  return true;
}

void DiagnosticsZipWriter::abort() {
  if (file_ != INVALID_HANDLE_VALUE) {
    CloseHandle(file_);
    file_ = INVALID_HANDLE_VALUE;
    if (!path_.empty())
      DeleteFileW(path_.c_str());
  }
}

DiagnosticsExportResult export_diagnostics_bundle(
    const DiagnosticsExportOptions& opts) {
  DiagnosticsExportResult res{};

  if (opts.flush_stdio) {
    std::fflush(stdout);
    std::fflush(stderr);
  }

  const std::wstring data_dir =
      opts.data_dir.empty() ? default_data_dir() : opts.data_dir;
  const std::wstring out_path =
      opts.output_zip_path.empty() ? default_output_zip_path(data_dir)
                                   : opts.output_zip_path;

  DiagnosticsZipWriter zip;
  if (!zip.open(out_path)) {
    res.error = "Failed to create zip file: " + wide_to_utf8(out_path);
    return res;
  }

  std::string manifest;
  auto try_pack_file = [&](const std::wstring& disk_path,
                           const std::string& arc_name,
                           uint64_t max_bytes = 32ULL * 1024 * 1024) {
    std::vector<uint8_t> bytes;
    if (!read_file_shared(disk_path, &bytes, max_bytes))
      return false;
    if (!zip.add_entry(arc_name, bytes.data(), bytes.size(), true))
      return false;
    char line[256];
    std::snprintf(line, sizeof(line), "  + %s (%llu bytes)\n",
                  arc_name.c_str(),
                  static_cast<unsigned long long>(bytes.size()));
    manifest += line;
    return true;
  };

  // 1. Logs.
  try_pack_file(data_dir + L"\\winome.log", "logs/winome.log", 16ULL * 1024 * 1024);
  try_pack_file(data_dir + L"\\winome.prev.log", "logs/winome.prev.log", 16ULL * 1024 * 1024);

  // 2. Config & recovery state.
  try_pack_file(data_dir + L"\\config.json", "config/config.json", 4ULL * 1024 * 1024);
  try_pack_file(data_dir + L"\\last-crash.json", "config/last-crash.json", 4ULL * 1024 * 1024);
  try_pack_file(data_dir + L"\\recovery.json", "config/recovery.json", 1ULL * 1024 * 1024);

  // 3. Crash minidumps & reports under data_dir\crashes.
  const std::wstring crash_dir = data_dir + L"\\crashes";
  const auto crash_files = collect_crash_dir_files(crash_dir);
  size_t packed_dumps = 0;
  uint64_t packed_dump_bytes = 0;
  for (const auto& cf : crash_files) {
    if (cf.is_dump) {
      if (packed_dumps >= opts.max_crash_dumps ||
          (packed_dump_bytes + cf.file_size > opts.max_total_dump_bytes &&
           packed_dumps > 0)) {
        char skip[256];
        std::snprintf(skip, sizeof(skip),
                      "  - crashes/%s (skipped: limit reached, %llu bytes)\n",
                      wide_to_utf8(cf.file_name).c_str(),
                      static_cast<unsigned long long>(cf.file_size));
        manifest += skip;
        continue;
      }
      if (try_pack_file(cf.full_path,
                        "crashes/" + wide_to_utf8(cf.file_name),
                        opts.max_total_dump_bytes)) {
        ++packed_dumps;
        packed_dump_bytes += cf.file_size;
      }
    } else {
      try_pack_file(cf.full_path, "crashes/" + wide_to_utf8(cf.file_name),
                    4ULL * 1024 * 1024);
    }
  }

  // 4. Caller-provided in-memory extra files (e.g. live ring logs / Z-order).
  for (const auto& ef : opts.extra_files) {
    if (ef.archive_name.empty())
      continue;
    if (zip.add_entry(ef.archive_name, ef.content.data(), ef.content.size(), true)) {
      char line[256];
      std::snprintf(line, sizeof(line), "  + %s (%llu bytes)\n",
                    ef.archive_name.c_str(),
                    static_cast<unsigned long long>(ef.content.size()));
      manifest += line;
    }
  }

  // 5. Generated system & environment summary (always included).
  SYSTEMTIME st_utc{};
  SYSTEMTIME st_loc{};
  GetSystemTime(&st_utc);
  GetLocalTime(&st_loc);
  char ts[128];
  std::snprintf(ts, sizeof(ts),
                "%04u-%02u-%02u %02u:%02u:%02u (Local) / "
                "%04u-%02u-%02uT%02u:%02u:%02uZ (UTC)",
                st_loc.wYear, st_loc.wMonth, st_loc.wDay,
                st_loc.wHour, st_loc.wMinute, st_loc.wSecond,
                st_utc.wYear, st_utc.wMonth, st_utc.wDay,
                st_utc.wHour, st_utc.wMinute, st_utc.wSecond);

  std::string summary;
  summary += "=== WINOME Diagnostics Bundle ===\n";
  summary += std::string("WINOME Version : ") + WINOME_VERSION + "\n";
  summary += std::string("Exported At    : ") + ts + "\n";
  summary += "Host OS        : " + query_windows_version() + "\n";
  summary += "Data Directory : " + wide_to_utf8(data_dir) + "\n";
  summary += "\n=== Displays ===\n";
  MonitorLineCtx mctx{&summary, 0};
  EnumDisplayMonitors(nullptr, nullptr, enum_diag_monitors_proc,
                      reinterpret_cast<LPARAM>(&mctx));
  if (!opts.extra_summary_text.empty()) {
    summary += "\n=== Runtime / Crash Details ===\n";
    summary += opts.extra_summary_text;
    if (summary.back() != '\n')
      summary.push_back('\n');
  }
  summary += "\n=== Included Files ===\n";
  summary += manifest.empty() ? "  (no existing log/dump files found)\n" : manifest;

  if (!zip.add_entry("system-info.txt", summary.data(), summary.size(), true) ||
      !zip.finish()) {
    zip.abort();
    res.error = "Failed to finalize diagnostics zip archive";
    return res;
  }

  res.ok = true;
  res.zip_path = out_path;
  res.file_count = zip.entry_count();
  res.uncompressed_bytes = zip.total_uncompressed();
  res.compressed_bytes = zip.total_compressed();
  return res;
}

bool reveal_file_in_explorer(const std::wstring& file_path) {
  if (file_path.empty())
    return false;
  std::wstring arg = L"/select,\"" + file_path + L"\"";
  HINSTANCE h = ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(),
                              nullptr, SW_SHOWNORMAL);
  return reinterpret_cast<intptr_t>(h) > 32;
}

}  // namespace winome
