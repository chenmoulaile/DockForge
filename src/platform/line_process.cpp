// DockForge - line-oriented child process transport (see line_process.h).
#include "platform/line_process.h"

#include <cstdio>

namespace dockforge::platform {

std::wstring SiblingExecutablePath(const std::wstring& explicit_path,
                                   const wchar_t* fallback_name) {
    if (!explicit_path.empty()) return explicit_path;
    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) return {};
    std::wstring full(self);
    const std::size_t slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return full.substr(0, slash) + L"\\" + fallback_name;
}

LineProcess::~LineProcess() { Stop(); }

bool LineProcess::Start(const std::wstring& exe_path) {
    if (running()) return true;
    if (exe_path.empty()) return false;
    const DWORD attrs = GetFileAttributesW(exe_path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }

    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE in_read = nullptr;
    HANDLE out_write = nullptr;
    if (!CreatePipe(&in_read, &in_write_, &sa, 0)) return false;
    if (!CreatePipe(&out_read_, &out_write, &sa, 0)) {
        CloseHandle(in_read);
        CloseHandle(in_write_);
        in_write_ = nullptr;
        return false;
    }
    // Our ends must not be inherited by anything else we spawn later.
    SetHandleInformation(out_read_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(in_write_, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_read;
    si.hStdOutput = out_write;
    si.hStdError = out_write;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe_path + L"\"";
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    // The child owns its ends now regardless of the spawn result.
    CloseHandle(in_read);
    CloseHandle(out_write);
    if (!ok) {
        CloseHandle(out_read_);
        out_read_ = nullptr;
        CloseHandle(in_write_);
        in_write_ = nullptr;
        return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    pid_ = pi.dwProcessId;
    return true;
}

void LineProcess::Stop() {
    if (in_write_) {
        // Graceful: the child sees EOF on stdin and exits on its own.
        CloseHandle(in_write_);
        in_write_ = nullptr;
        if (process_) WaitForSingleObject(process_, 2000);
    }
    if (process_) {
        if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
            TerminateProcess(process_, 0);
            WaitForSingleObject(process_, 2000);
        }
        CloseHandle(process_);
        process_ = nullptr;
    }
    if (out_read_) {
        CloseHandle(out_read_);
        out_read_ = nullptr;
    }
    pid_ = 0;
    buffer_.clear();
}

bool LineProcess::SendLine(const std::string& line) {
    if (!in_write_) return false;
    const std::string raw = line + "\n";
    DWORD written = 0;
    if (!WriteFile(in_write_, raw.data(), static_cast<DWORD>(raw.size()), &written,
                   nullptr) ||
        written != raw.size()) {
        return false;
    }
    FlushFileBuffers(in_write_);
    return true;
}

std::vector<std::string> LineProcess::DrainLines() {
    std::vector<std::string> lines;
    if (!out_read_) return lines;
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(out_read_, nullptr, 0, nullptr, &available, nullptr)) {
            return lines;  // pipe gone: child died
        }
        if (available == 0) break;
        char chunk[4096];
        const DWORD want = available < sizeof(chunk) ? available : sizeof(chunk);
        DWORD got = 0;
        if (!ReadFile(out_read_, chunk, want, &got, nullptr) || got == 0) break;
        buffer_.append(chunk, got);
    }
    std::size_t nl;
    while ((nl = buffer_.find('\n')) != std::string::npos) {
        std::string line = buffer_.substr(0, nl);
        buffer_.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
    }
    return lines;
}

bool LineProcess::ChildDied() {
    if (!process_) return true;
    return WaitForSingleObject(process_, 0) != WAIT_TIMEOUT;
}

}  // namespace dockforge::platform
