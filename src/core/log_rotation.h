// DockForge - log rotation decisions as pure logic (T-062).
//
// The T-002 logger already splits files per day (dockforge-YYYYMMDD.log). T-062
// adds the two missing dimensions and keeps every rule a pure function so the
// decision table is unit-testable without touching a filesystem:
//   * a per-file size cap (10MB default): same-day overflow continues into
//     sequence files dockforge-YYYYMMDD-N.log instead of growing without bound;
//   * an age limit (14 days default, per T-062 DoD; supersedes C-07's initial
//     "保留 7 天" - the DoD is the later, deliberate choice): older daily files
//     are deleted.
//
// Both limits are env-overridable (DOCKFORGE_LOG_MAX_BYTES / DOCKFORGE_LOG_KEEP_DAYS,
// read by the Logger) so the probe can exercise real rotations without waiting
// for real time or real megabytes.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dockforge::core {

struct LogRotationPolicy {
    uint64_t max_file_bytes = 10ull * 1024 * 1024;  // DoD: 单文件上限 10MB
    int keep_days = 14;                             // DoD: 保留 14 天
};

struct LogFileDate {
    int year = 0;
    int month = 0;
    int day = 0;
    int sequence = 0;  // 0 = primary daily file, >0 = same-day overflow file
};

// Sequence 0 -> "dockforge-YYYYMMDD.log"; sequence N>0 -> "dockforge-YYYYMMDD-N.log".
std::wstring LogFileNameFor(int year, int month, int day, int sequence);

// Parses exactly the two shapes above (case-sensitive, "dockforge-" prefix).
// Returns false for anything else - unrelated files in the logs directory are
// never matched, never deleted.
bool ParseLogFileName(const std::wstring& name, LogFileDate& out);

// True when appending `incoming_bytes` to a file of `current_bytes` must start
// the next sequence file. An EMPTY file never rolls (a single line larger than
// the cap still lands somewhere instead of looping on empty files forever).
bool ShouldRollBySize(uint64_t current_bytes, uint64_t incoming_bytes, uint64_t cap);

// Days-since-epoch ordinal of a proleptic-Gregorian date. Expiry math only ever
// subtracts ordinals, so no timezone and no clock beyond "today" is involved.
int64_t DayOrdinal(int year, int month, int day);

// Files strictly older than today-(keep_days-1) are expired. Nothing dated
// today is ever expired, even with keep_days=1 (a "keep 1 day" policy means
// "keep today"). `files` pairs each name with its already-parsed date.
std::vector<std::wstring> ExpiredLogFiles(
    const std::vector<std::pair<std::wstring, LogFileDate>>& files, int today_year,
    int today_month, int today_day, int keep_days);

}  // namespace dockforge::core
