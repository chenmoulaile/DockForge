// DockForge - .NET sidecar bridge implementation (T-068, C-23).
#include "finderbar/media_sidecar.h"

#include "core/paths.h"
#include "core/log.h"

#include <nlohmann/json.hpp>

#include <cstdio>

namespace dockforge::finderbar {

bool ParseSidecarLine(const std::string& line, SidecarState* out, bool* is_push,
                      std::string* push_kind, std::wstring* error) {
    if (out) out->valid = false;
    if (is_push) *is_push = false;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(line);
    } catch (...) {
        return false;
    }
    if (!j.is_object()) return false;

    if (j.contains("push") && j["push"].is_string()) {
        if (is_push) *is_push = true;
        if (push_kind) *push_kind = j["push"].get<std::string>();
        return true;
    }
    if (j.contains("ok")) {
        const bool ok = j["ok"].get<bool>();
        if (!ok) {
            if (error) {
                std::string raw;
                if (j.contains("error") && j["error"].is_string()) {
                    raw = j["error"].get<std::string>();
                }
                *error = core::Utf8ToWide(raw);
            }
            return true;  // a well-formed failure answer
        }
    } else {
        return false;
    }

    // Ready banner ({"ok":true,"sidecar":"ready","pid":N}) and plain
    // {"ok":true} command acks are valid lines but carry no state.
    if (!j.contains("media") || !j.contains("radios") || !out) return true;

    const auto& media = j["media"];
    if (media.is_object()) {
        out->media.has_session = media.value("has_session", false);
        out->media.aumid = core::Utf8ToWide(media.value("aumid", std::string()));
        out->media.title = core::Utf8ToWide(media.value("title", std::string()));
        out->media.artist = core::Utf8ToWide(media.value("artist", std::string()));
        out->media.playing = media.value("playing", false);
        out->media.position_ticks = media.value("pos", 0LL);
        out->media.duration_ticks = media.value("dur", 0LL);
    }
    const auto& radios = j["radios"];
    if (radios.is_array()) {
        for (const auto& r : radios) {
            if (!r.is_object()) continue;
            SidecarRadioState s;
            s.name = core::Utf8ToWide(r.value("name", std::string()));
            s.kind = r.value("kind", 0);
            s.state = r.value("state", 0);
            out->radios.push_back(std::move(s));
        }
    }
    out->valid = true;
    return true;
}

const SidecarRadioState* FindRadio(const SidecarState& state, int kind) {
    for (const auto& r : state.radios) {
        if (r.kind == kind) return &r;
    }
    return nullptr;
}

RadioStateChanged RadioEventFrom(const SidecarRadioState& radio) {
    RadioStateChanged event;
    event.radio_kind = radio.kind;
    event.on = radio.state == 1;  // RadioState::On
    return event;
}

MediaSessionChanged MediaEventFrom(const SidecarMediaState& media) {
    MediaSessionChanged event;
    event.session_id = media.aumid;
    event.title = media.title;
    event.artist = media.artist;
    event.playing = media.playing;
    event.has_session = media.has_session;
    return event;
}

std::wstring FormatMediaClock(long long ticks) {
    if (ticks <= 0) return L"--:--";
    const long long total_seconds = ticks / 10000000LL;
    const long long seconds = total_seconds % 60;
    const long long minutes = (total_seconds / 60) % 60;
    const long long hours = total_seconds / 3600;
    wchar_t buf[32];
    if (hours > 0) {
        swprintf(buf, 32, L"%lld:%02lld:%02lld", hours, minutes, seconds);
    } else {
        swprintf(buf, 32, L"%lld:%02lld", minutes, seconds);
    }
    return buf;
}

MediaSidecarBridge::~MediaSidecarBridge() { Stop(); }

bool MediaSidecarBridge::Start(const std::wstring& exe_path) {
    if (running()) return true;
    std::wstring path = exe_path;
    if (path.empty()) {
        wchar_t self[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) return false;
        std::wstring full(self);
        const std::size_t slash = full.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return false;
        path = full.substr(0, slash) + L"\\hw_sidecar.exe";
    }
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        DF_WARN("finderbar", "hw_sidecar.exe not found at %s",
                core::WideToUtf8(path).c_str());
        return false;
    }

    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE in_read = nullptr, out_write = nullptr;
    if (!CreatePipe(&in_read, &in_write_, &sa, 0)) return false;
    if (!CreatePipe(&out_read_, &out_write, &sa, 0)) {
        CloseHandle(in_read);
        CloseHandle(in_write_);
        in_write_ = nullptr;
        return false;
    }
    // Non-blocking reads on our end of the stdout pipe.
    SetHandleInformation(out_read_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(in_write_, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_read;
    si.hStdOutput = out_write;
    si.hStdError = out_write;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + path + L"\"";
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    // The child owns its ends now regardless of the spawn result.
    CloseHandle(in_read);
    CloseHandle(out_write);
    if (!ok) {
        DF_WARN("finderbar", "CreateProcess(hw_sidecar) failed err=%lu", GetLastError());
        CloseHandle(out_read_);
        out_read_ = nullptr;
        CloseHandle(in_write_);
        in_write_ = nullptr;
        return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    DF_INFO("finderbar", "hw_sidecar started pid=%lu", pi.dwProcessId);
    return true;
}

void MediaSidecarBridge::Stop() {
    if (in_write_) {
        // Graceful: let the sidecar see EOF and exit on its own.
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
    buffer_.clear();
}

bool MediaSidecarBridge::SendCommand(const std::string& line) {
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

std::vector<std::string> MediaSidecarBridge::DrainLines() {
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

SidecarState MediaSidecarBridge::PollSync(unsigned timeout_ms) {
    SidecarState state;
    if (!running() || !SendCommand("poll")) return state;
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < timeout_ms) {
        for (const auto& line : DrainLines()) {
            bool is_push = false;
            std::string push_kind;
            std::wstring error;
            SidecarState parsed;
            if (!ParseSidecarLine(line, &parsed, &is_push, &push_kind, &error)) continue;
            if (parsed.valid) return parsed;
        }
        Sleep(50);
    }
    return state;
}

bool MediaSidecarBridge::ChildDied() {
    if (!process_) return true;
    return WaitForSingleObject(process_, 0) != WAIT_TIMEOUT;
}

}  // namespace dockforge::finderbar
