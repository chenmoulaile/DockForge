// DockForge - leveled logger with daily file rotation (CONTRACTS.md C-07).
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace dockforge::core {

enum class LogLevel : int { Trace = 0, Debug, Info, Warn, Error, Fatal };

// Directory resolution per CONTRACTS.md C-03:
//   %LOCALAPPDATA%\DockForge\logs\dockforge-YYYYMMDD.log
class Logger {
public:
    static Logger& Instance();

    // Creates the log directory and opens today's file. Safe to call twice.
    void Init(std::wstring_view module = L"core");
    void Shutdown();

    void SetMinLevel(LogLevel level) { min_level_ = level; }
    LogLevel MinLevel() const { return min_level_; }

    void Log(LogLevel level, std::string_view module, std::string_view message);

    // Absolute path of the active log file (empty before Init()).
    const std::wstring& FilePath() const { return file_path_; }

private:
    Logger() = default;
    ~Logger();

    void RotateIfNeeded();
    std::wstring CurrentPathForToday() const;

    std::mutex mutex_;
    LogLevel min_level_ = LogLevel::Info;
    std::wstring log_dir_;
    std::wstring file_path_;
    void* file_ = nullptr;  // FILE*
    int current_day_ = -1;
    bool initialized_ = false;
};

const char* LogLevelName(LogLevel level);
bool ParseLogLevel(std::string_view name, LogLevel& out);

namespace detail {
void Write(LogLevel level, const char* module, const char* fmt, ...);
}  // namespace detail

}  // namespace dockforge::core

#define DF_LOG(level, module, ...) \
    ::dockforge::core::detail::Write(level, module, __VA_ARGS__)

#define DF_TRACE(mod, ...) DF_LOG(::dockforge::core::LogLevel::Trace, mod, __VA_ARGS__)
#define DF_DEBUG(mod, ...) DF_LOG(::dockforge::core::LogLevel::Debug, mod, __VA_ARGS__)
#define DF_INFO(mod, ...)  DF_LOG(::dockforge::core::LogLevel::Info,  mod, __VA_ARGS__)
#define DF_WARN(mod, ...)  DF_LOG(::dockforge::core::LogLevel::Warn,  mod, __VA_ARGS__)
#define DF_ERROR(mod, ...) DF_LOG(::dockforge::core::LogLevel::Error, mod, __VA_ARGS__)
#define DF_FATAL(mod, ...) DF_LOG(::dockforge::core::LogLevel::Fatal, mod, __VA_ARGS__)
