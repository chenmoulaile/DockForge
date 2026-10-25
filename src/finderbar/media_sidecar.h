// DockForge - .NET sidecar bridge for GSMTC media + Radios (T-068, C-23).
//
// The main process cannot drive these WinRT areas directly: their entry points
// are IAsyncOperation, which never completes in a MinGW console process
// (measured in spikes/T-068_hw_probe/winrt_mingw.cpp - 15s, handler never
// fires; same boundary as T-065). The .NET projection works (winrt_probe.cs,
// hw_sidecar.cs manual run). So: spawn tools/hw_sidecar.exe (built next to the
// app binary) and speak its one-JSON-object-per-line protocol over pipes.
//
// Lifecycle: started when the hardware panel opens, stopped when it closes -
// no sidecar process outlives a closed panel. If the exe is missing or its
// GSMTC bootstrap fails, media/radio show as unavailable in the panel (never a
// crash, never a fake success).
#pragma once

#include "finderbar/hardware_types.h"

#include <string>
#include <vector>

#include "platform/win_common.h"

namespace dockforge::finderbar {

// Media session snapshot from the sidecar's poll answer.
struct SidecarMediaState {
    bool has_session = false;
    std::wstring aumid;
    std::wstring title;
    std::wstring artist;
    bool playing = false;
    long long position_ticks = 0;
    long long duration_ticks = 0;
};

// One radio from the sidecar's poll answer. `state`: 0=Unknown 1=On 2=Off
// 3=Disabled (Windows.Devices.Radios.RadioState values).
struct SidecarRadioState {
    std::wstring name;
    int kind = 0;   // C-23 RadioKind: 0=Other 1=WiFi 2=MBB 3=Bluetooth
    int state = 0;
};

// Full answer of one `poll`.
struct SidecarState {
    SidecarMediaState media;
    std::vector<SidecarRadioState> radios;
    bool valid = false;
};

// Parses one sidecar output line. Three shapes:
//   {"ok":true,"media":{...},"radios":[...]}   -> poll answer (out filled)
//   {"push":"media"|"radio"}                   -> event ping (*is_push)
//   {"ok":false,"error":"..."}                 -> failure (*error filled)
// Returns false for anything unparsable. Pure: unit-tested without a process.
bool ParseSidecarLine(const std::string& line, SidecarState* out, bool* is_push,
                      std::string* push_kind, std::wstring* error);

// Finds the radio of `kind` in `state`, or nullptr. Exposed for tests/panel.
const SidecarRadioState* FindRadio(const SidecarState& state, int kind);

// C-23 RadioStateChanged from a sidecar radio snapshot.
RadioStateChanged RadioEventFrom(const SidecarRadioState& radio);

// C-23 MediaSessionChanged from a sidecar media snapshot.
MediaSessionChanged MediaEventFrom(const SidecarMediaState& media);

// "12:34" from ticks (100ns units), or "--:--" when duration is 0.
std::wstring FormatMediaClock(long long ticks);

class MediaSidecarBridge {
  public:
    MediaSidecarBridge() = default;
    ~MediaSidecarBridge();

    MediaSidecarBridge(const MediaSidecarBridge&) = delete;
    MediaSidecarBridge& operator=(const MediaSidecarBridge&) = delete;

    // Spawns the sidecar exe. `exe_path` explicit so tests can point at a
    // stub; empty means "next to the app binary". Returns false when the
    // process could not be started.
    bool Start(const std::wstring& exe_path = L"");

    // Terminates the sidecar (close pipes + wait, hard-kill as backstop).
    void Stop();

    bool running() const { return process_ != nullptr; }

    // Writes one command line ("poll" / "media play" / "radio 1 1").
    bool SendCommand(const std::string& line);

    // Non-blocking: returns every complete line currently in the pipe.
    std::vector<std::string> DrainLines();

    // Convenience for the panel tick: send `poll`, then pump DrainLines for up
    // to `timeout_ms`; the first poll answer wins. Returns the parsed state
    // (valid=false on timeout).
    SidecarState PollSync(unsigned timeout_ms);

    // Exit code / death detection: true once the sidecar has died.
    bool ChildDied();

  private:
    HANDLE out_read_ = nullptr;   // sidecar stdout
    HANDLE in_write_ = nullptr;   // sidecar stdin
    HANDLE process_ = nullptr;
    std::string buffer_;
};

}  // namespace dockforge::finderbar
