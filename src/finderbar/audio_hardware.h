// DockForge - Core Audio hardware control (T-068 DoD #1/#2, C-23).
//
// Direct COM against mmdeviceapi/audiopolicy/endpointvolume - the MinGW-viable
// half of the hardware panel (spikes/T-068_hw_probe/audio_spike.cpp measured:
// session enumeration, per-session + endpoint read/write, and change callbacks
// all work in an unpackaged process).
//
// Threading: created and used from the app's OLE-STA main thread. MEASURED
// (T-068 probe): IAudioEndpointVolumeCallback::OnNotify fires on the AUDIO
// SERVICE's thread, not ours - doing COM/bus/paint work there dead-locks the
// audio thread on some runs. The callback therefore only PostMessages the
// panel window (SetEventSink); the panel's WndProc answers on the main thread
// through DispatchQueuedVolume, which runs the full C-23 chain safely.
//
// Error policy (C-23): every mutating call returns its HRESULT through a
// HardwareControlError so the panel can show it instead of dropping it.
#pragma once

#include "finderbar/hardware_types.h"
#include "platform/win_common.h"

#include <functional>
#include <string>
#include <vector>

namespace dockforge::finderbar {

// Snapshot of one audio endpoint (master render or microphone capture).
struct AudioEndpointState {
    float level = 0.0f;      // 0..1 scalar
    bool muted = false;
    std::wstring name;       // friendly name, may be empty if the store refuses
    bool present = false;    // false when no default endpoint exists
};

// One per-process audio session (ISimpleAudioVolume view).
struct AudioSessionInfo {
    DWORD pid = 0;
    std::wstring name;         // process exe base name; "系统声音" for pid 0
    std::wstring session_id;   // IAudioSessionControl2::GetSessionIdentifier
    float level = 0.0f;        // 0..1
    bool muted = false;
};

class AudioHardware {
  public:
    // `on_volume_changed` fires for endpoint and session changes alike
    // (C-23 VolumeChanged; session_id empty = endpoint-level).
    explicit AudioHardware(std::function<void(const VolumeChanged&)> on_volume_changed);
    ~AudioHardware();

    AudioHardware(const AudioHardware&) = delete;
    AudioHardware& operator=(const AudioHardware&) = delete;

    // Initializes the enumerator and caches the default endpoints + session
    // manager. Idempotent; returns false when COM refuses (the panel then
    // reports audio as unavailable - it does not half-work).
    bool Initialize();

    // Drops every registration and interface. Safe to call twice.
    void Shutdown();

    // ---- endpoints (DoD #2: master + microphone) --------------------------
    AudioEndpointState GetEndpoint(bool capture) const;
    HardwareControlError SetEndpointLevel(bool capture, float level);
    HardwareControlError SetEndpointMute(bool capture, bool mute);

    // ---- per-app sessions (DoD #1) ----------------------------------------
    std::vector<AudioSessionInfo> EnumerateSessions() const;
    // `session_id` as returned by EnumerateSessions. Empty id = invalid.
    HardwareControlError SetSessionLevel(const std::wstring& session_id, float level);
    HardwareControlError SetSessionMute(const std::wstring& session_id, bool mute);

    // Total sessions last enumerated (diagnostics).
    int SessionCount() const { return static_cast<int>(sessions_.size()); }

    // Callback bridge (defined in the .cpp): the COM notify objects, which
    // are file-locals there, hand events in through these. Not for any other
    // caller.
    void PublishVolumeCallback(const VolumeChanged& event);

    // ---- audio-thread -> main-thread bridge --------------------------------
    // The audio thread's OnNotify posts VolumeNotifyMessage() to `hwnd`; the
    // panel's WndProc answers with DispatchQueuedVolume ON THE MAIN THREAD.
    // Null detaches.
    void SetEventSink(HWND hwnd);
    // Turns one posted notification into the full C-23 chain (bus publish,
    // log, repaint) - main thread only, called from the panel's WndProc.
    void DispatchQueuedVolume(bool capture_endpoint, float level, bool muted);

  private:

    void RefreshSessions() const;
    void PublishVolume(bool capture_endpoint, const std::wstring& session_id,
                       float level, bool muted);

    std::function<void(const VolumeChanged&)> on_volume_changed_;

    // COM pointers, main-thread only. mmdeviceapi.h types kept out of the
    // header: the members live behind an opaque block so including this header
    // from a test does not drag the whole audio API surface in.
    void* com_ = nullptr;  // struct AudioCom members (see .cpp)

    // Session list cache, rebuilt by RefreshSessions (mutable: enumeration is
    // logically const - it refreshes a cache).
    mutable std::vector<AudioSessionInfo> sessions_;
    bool initialized_ = false;
};

// Display name for a pid: base name of the process image; "系统声音" for pid 0
// (the audiosrv system session). Exposed for the unit test (no HWND needed).
std::wstring SessionDisplayName(DWORD pid);

// The registered message the audio thread posts (see AudioHardware::
// SetEventSink). wparam bits: 1 = capture endpoint, 2 = muted; lparam =
// level * 10000.
UINT VolumeNotifyMessage();

// Formats a level as the panel shows it ("76%"). Exposed for the test.
std::wstring FormatLevelPercent(float level);

}  // namespace dockforge::finderbar
