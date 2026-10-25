// DockForge - leveled logger (CONTRACTS.md C-07, rotation per T-062).
#include "core/log.h"

#include "core/paths.h"
#include "platform/win_common.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <share.h>
#include <vector>

namespace dockforge::core {
namespace {

// Keeps the file open in append mode and reopens when the date rolls over.
std::tm LocalNow() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    return local;
}

std::string FormatTimestamp() {
    const std::tm local = LocalNow();
    SYSTEMTIME st{};
    GetLocalTime(&st);

    char buffer[64];
    // ISO8601 with millisecond precision and explicit offset.
    long offset_minutes = 0;
    TIME_ZONE_INFORMATION tz{};
    const DWORD tz_result = GetTimeZoneInformation(&tz);
    if (tz_result != TIME_ZONE_ID_INVALID) {
        offset_minutes = -static_cast<long>(tz.Bias);
        if (tz_result == TIME_ZONE_ID_DAYLIGHT) offset_minutes -= static_cast<long>(tz.DaylightBias);
        if (tz_result == TIME_ZONE_ID_STANDARD) offset_minutes -= static_cast<long>(tz.StandardBias);
    }
    const char sign = offset_minutes < 0 ? '-' : '+';
    const long abs_off = offset_minutes < 0 ? -offset_minutes : offset_minutes;

    std::snprintf(buffer, sizeof(buffer),
                  "%04d-%02d-%02dT%02d:%02d:%02d.%03d%c%02ld:%02ld", local.tm_year + 1900,
                  local.tm_mon + 1, local.tm_mday, st.wHour, st.wMinute, st.wSecond,
                  st.wMilliseconds, sign, abs_off / 60, abs_off % 60);
    return std::string(buffer);
}

}  // namespace

const char* LogLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "INFO";
}

bool ParseLogLevel(std::string_view name, LogLevel& out) {
    // Case-insensitive: DOCKFORGE_LOG_LEVEL is set by hand in shells and CI.
    char upper[16]{};
    const std::size_t n = name.size() < sizeof(upper) - 1 ? name.size() : sizeof(upper) - 1;
    for (std::size_t i = 0; i < n; ++i) {
        const char c = name[i];
        upper[i] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    }
    const std::string_view key(upper, n);

    if (key == "TRACE") { out = LogLevel::Trace; return true; }
    if (key == "DEBUG") { out = LogLevel::Debug; return true; }
    if (key == "INFO") { out = LogLevel::Info; return true; }
    if (key == "WARN" || key == "WARNING") { out = LogLevel::Warn; return true; }
    if (key == "ERROR") { out = LogLevel::Error; return true; }
    if (key == "FATAL") { out = LogLevel::Fatal; return true; }
    return false;
}

Logger& Logger::Instance() {
    static Logger logger;
    return logger;
}

Logger::~Logger() { Shutdown(); }

std::wstring Logger::CurrentPathForToday() const {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return log_dir_ + L"\\" +
           LogFileNameFor(st.wYear, st.wMonth, st.wDay, sequence_);
}

// Opens the current sequence file in append mode and records its size, so the
// T-062 size roll knows how full it already is (a file may pre-exist from a
// previous process run of the same day).
bool Logger::OpenCurrentFile() {
    if (file_) {
        std::fclose(static_cast<FILE*>(file_));
        file_ = nullptr;
    }
    FILE* f = _wfsopen(file_path_.c_str(), L"ab", _SH_DENYWR);
    if (!f) {
        current_bytes_ = 0;
        return false;
    }
    file_ = f;
    if (_fseeki64(f, 0, SEEK_END) == 0) {
        current_bytes_ = static_cast<uint64_t>(_ftelli64(f));
    } else {
        current_bytes_ = 0;
    }
    return true;
}

// Deletes daily log files (both shapes: dockforge-YYYYMMDD.log and
// dockforge-YYYYMMDD-N.log) older than the keep window. Unrelated files in the
// directory never match ParseLogFileName and are never touched. Runs under the
// logger mutex from Init and from the day-rollover branch of RotateIfNeeded.
void Logger::PruneOldLogs() {
    SYSTEMTIME st{};
    GetLocalTime(&st);

    std::wstring pattern = log_dir_ + L"\\dockforge-*.log";
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;

    std::vector<std::pair<std::wstring, LogFileDate>> candidates;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        LogFileDate date{};
        if (ParseLogFileName(data.cFileName, date)) {
            candidates.emplace_back(data.cFileName, date);
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);

    const std::vector<std::wstring> expired =
        ExpiredLogFiles(candidates, st.wYear, st.wMonth, st.wDay, policy_.keep_days);
    for (const std::wstring& name : expired) {
        const std::wstring full = log_dir_ + L"\\" + name;
        if (full == file_path_) continue;  // never delete the open file
        DeleteFileW(full.c_str());
    }
    if (!expired.empty()) {
        // Deferred: Log() re-enters the mutex we already hold in the rollover
        // path. Plain OutputDebugStringW keeps this re-entrancy-safe; the
        // startup path (Init) reports pruning through the banner instead.
        OutputDebugStringW((L"[DockForge] pruned " + std::to_wstring(expired.size()) +
                            L" expired log file(s)\n")
                               .c_str());
    }
}

void Logger::Init(std::wstring_view module) {
    std::string banner;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialized_) return;

        // DOCKFORGE_LOG_LEVEL=trace|debug|info|warn|error lets a session raise
        // verbosity without a rebuild (used to capture frame stats evidence).
        wchar_t level_buffer[16]{};
        if (GetEnvironmentVariableW(L"DOCKFORGE_LOG_LEVEL", level_buffer, 16) > 0) {
            LogLevel parsed{};
            const std::string narrow = WideToUtf8(level_buffer);
            if (ParseLogLevel(narrow, parsed)) {
                min_level_ = parsed;
            }
        }

        // T-062 limits, env-overridable so probes exercise real rotations
        // without waiting real megabytes or real days.
        wchar_t value[32]{};
        if (GetEnvironmentVariableW(L"DOCKFORGE_LOG_MAX_BYTES", value, 32) > 0) {
            const unsigned long long parsed = _wcstoui64(value, nullptr, 10);
            if (parsed > 0) policy_.max_file_bytes = parsed;
        }
        if (GetEnvironmentVariableW(L"DOCKFORGE_LOG_KEEP_DAYS", value, 32) > 0) {
            const int parsed = static_cast<int>(_wtoi(value));
            if (parsed > 0) policy_.keep_days = parsed;
        }

        log_dir_ = LogsDir();
        if (!EnsureDirectory(log_dir_)) {
            // Fall back to the executable directory so logging never silently dies.
            log_dir_ = ExecutableDir();
            EnsureDirectory(log_dir_);
        }

        SYSTEMTIME st{};
        GetLocalTime(&st);
        current_ordinal_ = DayOrdinal(st.wYear, st.wMonth, st.wDay);
        sequence_ = 0;
        file_path_ = CurrentPathForToday();
        OpenCurrentFile();
        initialized_ = true;
        banner = WideToUtf8(file_path_);
        if (!file_) OutputDebugStringW(L"[DockForge] log file unavailable\n");

        // Age-based cleanup runs once per process start (the rollover path
        // covers long-running processes). Inside the lock by design: Find* on
        // our own directory cannot deadlock with Log() writers.
        PruneOldLogs();
    }
    // Log() takes the same mutex; emit the banner outside the lock.
    if (!banner.empty()) {
        Log(LogLevel::Info, WideToUtf8(std::wstring(module)),
            std::string("logger started, file=") + banner);
    }
}

void Logger::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
        std::fclose(static_cast<FILE*>(file_));
        file_ = nullptr;
    }
    initialized_ = false;
}

void Logger::RotateIfNeeded() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    const int64_t today = DayOrdinal(st.wYear, st.wMonth, st.wDay);

    // Day rollover: restart the sequence at the new date and re-run the age
    // prune so a long-lived process cleans up without a restart.
    if (today != current_ordinal_) {
        current_ordinal_ = today;
        sequence_ = 0;
        file_path_ = CurrentPathForToday();
        OpenCurrentFile();
        PruneOldLogs();
    }
}

// T-062 size roll: appending this line would exceed the cap -> continue into
// dockforge-YYYYMMDD-N.log (pure rule in log_rotation.cpp, unit-tested).
void Logger::RollBySizeIfNeeded(uint64_t incoming_bytes) {
    if (!ShouldRollBySize(current_bytes_, incoming_bytes, policy_.max_file_bytes)) return;
    ++sequence_;
    file_path_ = CurrentPathForToday();
    OpenCurrentFile();
}

void Logger::Log(LogLevel level, std::string_view module, std::string_view message) {
    if (static_cast<int>(level) < static_cast<int>(min_level_)) return;

    std::string line = FormatTimestamp();
    line += ' ';
    line += LogLevelName(level);
    line += " [";
    line.append(module.data(), module.size());
    line += "] ";
    line.append(message.data(), message.size());
    line += '\n';

    std::lock_guard<std::mutex> lock(mutex_);
    RotateIfNeeded();
    RollBySizeIfNeeded(line.size());

    if (file_) {
        std::fwrite(line.data(), 1, line.size(), static_cast<FILE*>(file_));
        std::fflush(static_cast<FILE*>(file_));
        current_bytes_ += line.size();
    }
    if (level >= LogLevel::Warn) {
        OutputDebugStringA(line.c_str());
    }
}

namespace detail {

void Write(LogLevel level, const char* module, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char stack_buffer[1024];
    int written = std::vsnprintf(stack_buffer, sizeof(stack_buffer), fmt, args);
    va_end(args);

    if (written < 0) return;
    if (written < static_cast<int>(sizeof(stack_buffer))) {
        Logger::Instance().Log(level, module ? module : "app",
                               std::string_view(stack_buffer, static_cast<size_t>(written)));
        return;
    }

    std::vector<char> heap(static_cast<size_t>(written) + 1);
    va_start(args, fmt);
    std::vsnprintf(heap.data(), heap.size(), fmt, args);
    va_end(args);
    Logger::Instance().Log(level, module ? module : "app",
                           std::string_view(heap.data(), static_cast<size_t>(written)));
}

}  // namespace detail

}  // namespace dockforge::core
