#include "core/probe_file.h"

#include <windows.h>

#include "core/log.h"

namespace dockforge::core {
namespace {

constexpr DWORD kMaxProbeFileBytes = 4095;

}  // namespace

std::string ReadProbeFileContent(const std::wstring& path) {
    if (path.empty()) return std::string();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    char raw[kMaxProbeFileBytes + 1]{};
    DWORD got = 0;
    const BOOL read_ok = ReadFile(h, raw, kMaxProbeFileBytes, &got, nullptr);
    CloseHandle(h);
    if (!read_ok || got == 0) return std::string();
    raw[got] = '\0';
    std::string content(raw);
    while (!content.empty() &&
           (content.back() == '\n' || content.back() == '\r' || content.back() == ' ')) {
        content.pop_back();
    }
    return content;
}

std::string ProbeCommandOf(const std::string& content) {
    std::string command = content;
    const std::size_t nl = command.find('\n');
    if (nl != std::string::npos) command.resize(nl);
    while (!command.empty() && (command.back() == '\r' || command.back() == ' ')) {
        command.pop_back();
    }
    return command;
}

void ProbeCommandGate::Prime(const std::string& existing_content) {
    last_content_ = existing_content;
    if (!existing_content.empty()) {
        DF_DEBUG("probe", "primed with existing content, first command is inert: %s",
                 ProbeCommandOf(existing_content).c_str());
    }
}

bool ProbeCommandGate::Accept(const std::string& content) {
    if (content.empty()) return false;
    if (content == last_content_) return false;
    last_content_ = content;
    return true;
}

}  // namespace dockforge::core
