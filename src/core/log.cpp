// DockForge - leveled logger (CONTRACTS.md C-07).
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
    const std::tm local = LocalNow();
    wchar_t name[64];
    swprintf_s(name, L"dockforge-%04d%02d%02d.log", local.tm_year + 1900, local.tm_mon + 1,
               local.tm_mday);
    return log_dir_ + L"\\" + name;
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

        log_dir_ = LogsDir();
        if (!EnsureDirectory(log_dir_)) {
            // Fall back to the executable directory so logging never silently dies.
            log_dir_ = ExecutableDir();
            EnsureDirectory(log_dir_);
        }

        const std::tm local = LocalNow();
        current_day_ = local.tm_yday;
        file_path_ = CurrentPathForToday();

        // _SH_DENYWR keeps the file readable (tail/CI) while we hold it open.
        FILE* f = _wfsopen(file_path_.c_str(), L"ab", _SH_DENYWR);
        if (f) {
            file_ = f;
        }
        initialized_ = true;
        banner = WideToUtf8(file_path_);
        if (!file_) OutputDebugStringW(L"[DockForge] log file unavailable\n");
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
    const std::tm local = LocalNow();
    if (local.tm_yday == current_day_) return;

    if (file_) {
        std::fclose(static_cast<FILE*>(file_));
        file_ = nullptr;
    }
    current_day_ = local.tm_yday;
    file_path_ = CurrentPathForToday();

    FILE* f = nullptr;
    if (_wfopen_s(&f, file_path_.c_str(), L"ab") == 0 && f) file_ = f;
}

void Logger::Log(LogLevel level, std::string_view module, std::string_view message) {
    if (static_cast<int>(level) < static_cast<int>(min_level_)) return;

    std::lock_guard<std::mutex> lock(mutex_);
    RotateIfNeeded();

    std::string line = FormatTimestamp();
    line += ' ';
    line += LogLevelName(level);
    line += " [";
    line.append(module.data(), module.size());
    line += "] ";
    line.append(message.data(), message.size());
    line += '\n';

    if (file_) {
        std::fwrite(line.data(), 1, line.size(), static_cast<FILE*>(file_));
        std::fflush(static_cast<FILE*>(file_));
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
