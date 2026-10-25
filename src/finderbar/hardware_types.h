// DockForge - hardware control event payloads (T-068, CONTRACTS.md C-23).
//
// The four C-23 events, verbatim field-for-field from the frozen contract, plus
// the shared error-reporting shape every control path must use: a control that
// fails reports an HRESULT and a user-facing message - never a silent drop.
#pragma once

#include <string>

namespace dockforge::finderbar {

// C-23: VolumeChanged. `session_id` empty = endpoint-level
// (IAudioEndpointVolume), non-empty = process-level (ISimpleAudioVolume).
struct VolumeChanged {
    std::wstring device_id;
    std::wstring session_id;
    float level = 0.0f;
    bool muted = false;
};

// C-23: MediaSessionChanged. `session_id` is the media session's
// SourceAppUserModelId; empty when no session is current.
struct MediaSessionChanged {
    std::wstring session_id;
    std::wstring title;
    std::wstring artist;
    bool playing = false;
    bool has_session = false;  // C-23 has no null-word; this distinguishes
                               // "no current session" from "paused session".
};

// C-23: RadioStateChanged. radio_kind follows Windows.Devices.Radios.RadioKind
// values: 0=Other 1=WiFi 2=MobileBroadband 3=Bluetooth.
struct RadioStateChanged {
    int radio_kind = 0;
    bool on = false;
};

// C-23: SystemStatsUpdated. Sampled at most 0.5Hz, never on the render path.
struct SystemStatsUpdated {
    float cpu_percent = 0.0f;
    float memory_percent = 0.0f;
    unsigned long long memory_used_bytes = 0;
    unsigned long long memory_total_bytes = 0;
};

// RadioKind numeric values (C-23 / Windows.Devices.Radios.RadioKind).
enum RadioKindValue {
    kRadioOther = 0,
    kRadioWiFi = 1,
    kRadioMobileBroadband = 2,
    kRadioBluetooth = 3,
};

// One failure of one control action, as shown in the panel's error strip and
// logged. DoD: "control failure must show a Toast or in-panel error, never fail
// silently". The HRESULT is the raw code from the API; the message is Chinese,
// user-facing, and says WHAT failed, not the hex code alone.
struct HardwareControlError {
    std::wstring scope;     // "master volume" / "microphone" / "media" / "radio"
    std::wstring message;   // user-facing, Chinese
    long hresult = 0;       // raw HRESULT / Win32 error, for the log

    bool ok() const { return message.empty(); }
    static HardwareControlError None() { return HardwareControlError{}; }
    static HardwareControlError Failed(const std::wstring& scope_,
                                       const std::wstring& message_, long hr) {
        return HardwareControlError{scope_, message_, hr};
    }
};

}  // namespace dockforge::finderbar
