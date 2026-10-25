// DockForge - hardware control popup panel (T-068, C-23).
//
// The Finder Bar itself is not built yet (M2), so the panel is a standalone
// dropdown anchored to the top of the primary work area - the spot the Finder
// Bar will own later. It never occupies the bar permanently (DoD), it appears
// on demand and closes on Esc / click-outside / deactivation, exactly the
// FolderPanel semantics.
//
// GDI on purpose, same trade as the folder panel and the preview popup: small
// surface, click-triggered, must not pull the D2D device into a failure path
// the Dock has to survive.
//
// Content rows:
//   media     title / artist / [prev][play|pause][next] / clock
//   volumes   master slider, microphone slider, per-app session sliders
//   radios    WiFi + Bluetooth switches
//   stats     CPU + memory bars (0.5Hz samples pushed in by the owner)
//   error     one line, red - the C-23 "never fail silently" surface
#pragma once

#include "finderbar/audio_hardware.h"
#include "finderbar/hardware_types.h"
#include "finderbar/media_sidecar.h"

#include <string>
#include <vector>

#include "platform/win_common.h"

namespace dockforge::finderbar {

class HardwarePanel {
  public:
    // `on_stats` is called by the panel's own 2s tick when visible - the panel
    // owns the sampling cadence (C-23: <=0.5Hz, never on the render path).
    HardwarePanel(AudioHardware* audio, MediaSidecarBridge* sidecar,
                  std::function<SystemStatsUpdated()> on_stats);
    ~HardwarePanel();

    HardwarePanel(const HardwarePanel&) = delete;
    HardwarePanel& operator=(const HardwarePanel&) = delete;

    static bool RegisterWindowClass(HINSTANCE instance);

    bool Create(HINSTANCE instance);
    void Destroy();
    void Open();   // show + start ticking (spawns nothing; owner owns sidecar)
    void Close();  // hide (HWND survives for the next open)
    bool visible() const { return hwnd_ != nullptr && IsWindowVisible(hwnd_); }
    bool IsValid() const { return hwnd_ != nullptr; }

    // VolumeChanged arrived (audio callback or sidecar push): refresh the
    // affected row and repaint. Cheap when hidden.
    void OnAudioChanged();

    // Sidecar state arrived: refresh media/radio rows.
    void OnSidecarState(const SidecarState& state);

    // ---- controls (probe calls these too, same chain as the mouse) --------
    HardwareControlError SetMasterVolume(float level);        // 0..1
    HardwareControlError ToggleMasterMute();
    HardwareControlError SetMicrophoneMute(bool mute);
    HardwareControlError MediaCommand(const std::string& action);
    HardwareControlError SetRadio(int kind, bool on);
    // `index` into the visible session rows; -1 invalid.
    HardwareControlError SetSessionVolume(int index, float level);

    // Owner calls every tick; the panel self-throttles to 2s while visible.
    void Tick(double delta_ms);

    // ---- probe observability ---------------------------------------------
    // Full panel state as one JSON object (same schema as the sidecar poll
    // answer, plus volumes/stats/error) - written to the state file by the
    // main loop whenever it changes, so an unattended probe can assert on it.
    std::string SnapshotJson() const;
    int SessionRowCount() const { return static_cast<int>(sessions_.size()); }
    const std::wstring& LastErrorText() const { return last_error_.message; }

  private:
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    void Relayout();          // compute row geometry (DIP -> px at dpi)
    void Paint(HDC dc);
    void RefreshSessions();   // re-enumerate audio sessions
    void RepaintNow();

    // Hit regions (physical px), recomputed in Relayout.
    struct Region {
        int x = 0, y = 0, w = 0, h = 0;
    };
    struct SliderRow {
        Region track;
        Region label;
        Region mute;
        enum Kind { kMaster, kMic, kSession } kind = kMaster;
        int session_index = -1;      // for kSession
        std::wstring name;
        float level = 0.0f;
        bool muted = false;
    };
    struct ButtonRow {
        Region prev, play, next;
        Region title_area;
    };
    struct SwitchRow {
        Region box;
        Region label;
        int radio_kind = 0;
        std::wstring name;
        bool on = false;
        bool present = false;
    };
    struct StatsRow {
        Region bar;
        std::wstring label;
        float percent = 0.0f;
        std::wstring detail;
    };

    // Slider interaction state.
    int dragging_slider_ = -1;  // index into sliders_ while the button is down

    float SliderValueAt(const SliderRow& slider, int x_px) const;

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    float dpi_ = 96.0f;
    float scale_ = 1.0f;
    HFONT font_ = nullptr;
    HFONT font_bold_ = nullptr;
    HFONT font_small_ = nullptr;

    AudioHardware* audio_ = nullptr;
    MediaSidecarBridge* sidecar_ = nullptr;
    std::function<SystemStatsUpdated()> on_stats_;

    // Visible state (all refreshed by Tick / events).
    AudioEndpointState master_;
    AudioEndpointState mic_;
    std::vector<AudioSessionInfo> sessions_;
    SidecarState sidecar_state_;
    SystemStatsUpdated stats_;
    HardwareControlError last_error_;

    // Geometry, physical px.
    int width_px_ = 0;
    int height_px_ = 0;

    ButtonRow media_buttons_;
    std::vector<SliderRow> sliders_;
    SwitchRow wifi_switch_;
    SwitchRow bt_switch_;
    StatsRow cpu_bar_;
    StatsRow mem_bar_;
    Region error_strip_;
    Region media_title_area_;
    Region media_artist_area_;
    Region media_clock_area_;

    // 2s sampling accumulator (ms), C-23 ceiling.
    double stats_accumulator_ms_ = 0.0;
    bool was_activated_ = false;   // same dismissal rule as FolderPanel
};

}  // namespace dockforge::finderbar
