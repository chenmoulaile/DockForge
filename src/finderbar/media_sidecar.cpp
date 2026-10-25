// DockForge - .NET sidecar bridge implementation (T-068, C-23).
//
// The pipe plumbing lives in platform::LineProcess since T-076 needed the same
// transport for the notification sidecar; this file keeps only the protocol.
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
    const std::wstring path = platform::SiblingExecutablePath(exe_path, L"hw_sidecar.exe");
    if (path.empty()) return false;
    if (!process_.Start(path)) {
        DF_WARN("finderbar", "hw_sidecar not started (missing or spawn failed): %s",
                core::WideToUtf8(path).c_str());
        return false;
    }
    DF_INFO("finderbar", "hw_sidecar started pid=%lu", process_.pid());
    return true;
}

void MediaSidecarBridge::Stop() { process_.Stop(); }

bool MediaSidecarBridge::SendCommand(const std::string& line) { return process_.SendLine(line); }

std::vector<std::string> MediaSidecarBridge::DrainLines() { return process_.DrainLines(); }

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

bool MediaSidecarBridge::ChildDied() { return process_.ChildDied(); }

}  // namespace dockforge::finderbar
