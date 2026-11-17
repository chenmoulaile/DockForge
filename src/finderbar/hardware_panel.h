// DockForge - the control centre panel (T-304).
//
// The T-068/C-23 hardware panel upgraded into the macOS-Sonoma control centre
// (ADR-0039 + CONTRACTS.md C-34, metrics from Beautiful ControlCenterPanel.xaml /
// MacOS.xaml, recorded in ADR-0057). The class keeps its T-068 name, window class
// and control API on purpose: tools/hw_panel_probe.ps1 locates the surface by the
// "DockForgeHardwarePanel" class name and drives the same SetMasterVolume /
// SetRadio / SnapshotJson chain, so the upgrade swaps the surface and the layout,
// never the probe contract.
//
// Window model: the calendar_panel / status_popup pattern - a frameless topmost
// tool window whose surface is the Finder Bar's own DComp path
// (WS_EX_NOREDIRECTIONBITMAP + platform::CompositionSurface), drawn by the D2D
// backend. The old GDI double-buffer is gone: the T-068 panel predates the bar's
// surface and this panel must reuse the same visual language as the bar's other
// popups.
//
// Material: ADR-0008's ruling (a system backdrop ignores per-pixel alpha and the
// rounded radius on this system - Beautiful AcrylicWindow.cs:29-30 measured the
// same on Win11 24H2, where ACCENT_ENABLE_ACRYLICBLURBEHIND stops blurring and
// tiles the tint across the window rect), so the plate is painted directly. Per
// ADR-0045 the panel stays on the Solid tier because a capture-excluding tier
// would make the panel invisible to every screenshot - including this task's own
// evidence. The plate is the MacOS.xaml glass recipe painted as-is: the three-stop
// vertical gradient #C838383A(0) -> #B8242426(0.5) -> #A8161618(1) plus the popup
// tint #4D202020 on top, root radius 18 (ThemePanelCornerRadius), 1 DIP stroke
// #16FFFFFF. Text is white on both system themes: Beautiful's macOS theme pins
// the popup base to dark glass ("macOS 主题恒为深色玻璃"), and this panel follows
// that ruling instead of inventing a light variant.
//
// Content (first session; media rows, the Wi-Fi network list, per-app volume and
// the scroll-wheel bindings land in T-304b):
//   connection card  2x2 (Wi-Fi / Bluetooth / 勿扰 / 夜览), 28x28 round buttons
//   volume           vertical capsule slider 26x118 (thumb 26x4), live via
//                    AudioHardware::SetEndpointLevel (C-23)
//   brightness       horizontal capsule row; WMI (WmiSetBrightness) is the
//                    laptop internal-panel path, DDC/CI (dxva2
//                    SetMonitorBrightness) the external-monitor fallback - the
//                    T-304 spike measured this machine as DDC
//                    ERROR_NOT_CONNECTED(1168) but WMI 0..100 levels present;
//                    unsupported machines get an in-panel hint, never a fake
//                    slider (the C-23 no-silent-failure rule)
//   error strip      one line, red - the C-23 surface
//
// Dismissal is status_popup's rule: Esc, or the loss of activation after the panel
// claimed the foreground at Open (a click outside deactivates it). No global mouse
// hook. C-34 forbids touching the system volume/brightness OSD - the panel only
// drives the same endpoints the system UI drives.
#pragma once

#include "finderbar/audio_hardware.h"
#include "finderbar/hardware_types.h"
#include "finderbar/media_sidecar.h"
#include "platform/composition.h"
#include "platform/win_common.h"
#include "render/backend.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dockforge::finderbar {

// ---- Sonoma metrics (Beautiful ControlCenterPanel.xaml; ADR-0057 pins them) ----
inline constexpr float kCcPanelWidthDip = 348.0f;       // Card Width=348
inline constexpr float kCcPanelRadiusDip = 18.0f;       // MacOS.xaml ThemePanelCornerRadius
inline constexpr float kCcCardRadiusDip = 16.0f;        // CcConnCard CornerRadius=16
inline constexpr float kCcGroupRadiusDip = 12.0f;       // CcGroupStyle CornerRadius=12
inline constexpr float kCcTileRadiusDip = 14.0f;        // CcSliderTile CornerRadius=14
inline constexpr float kCcConnButtonDip = 28.0f;        // connection circle button 28x28
inline constexpr float kCcSliderWidthDip = 26.0f;       // Slider Width=26 (hit span)
inline constexpr float kCcSliderTrackWidthDip = 20.0f;  // visible track Border Width=20
inline constexpr float kCcSliderLengthDip = 118.0f;     // Slider Height=118
inline constexpr float kCcThumbDip = 4.0f;              // Thumb 26x4 (fill-edge handle)
inline constexpr float kCcSidePadDip = 16.0f;           // StackPanel Margin 16,10,16,12
inline constexpr float kCcTopPadDip = 10.0f;
inline constexpr float kCcBottomPadDip = 12.0f;
inline constexpr float kCcSectionGapDip = 8.0f;         // the 0,0,0,8 rhythm
inline constexpr float kCcErrorStripDip = 22.0f;
inline constexpr float kCcTopAreaDip = 168.0f;          // connection card row height
inline constexpr float kCcVolumeTileWidthDip = 46.0f;   // CcSliderTile Width=46
inline constexpr float kCcBrightnessRowDip = 46.0f;     // horizontal capsule row

// Panel height for a given combination. Pure, so a test can pin the arithmetic.
// The brightness row exists in both states (unsupported machines show the hint
// inside the row instead of the percent), so the height does not move.
float HardwarePanelHeightFor(bool brightness_supported);

// Slider level (0..1) from a track position. The vertical slider fills from the
// BOTTOM (macOS volume/brightness capsules grow upward); the horizontal row fills
// from the left. Positions outside the track clamp to 0/1. Pure.
float ControlCenterSliderLevelFromY(float y_dip, float track_top_dip, float track_len_dip);
float ControlCenterSliderLevelFromX(float x_dip, float track_left_dip, float track_len_dip);

// ---- 勿扰 / 夜览 registry semantics (pure parts; ADR-0057) --------------------
// 勿扰 is the toast master switch behind the Settings > Notifications page:
// HKCU\...\PushNotifications\ToastNotificationEnabled (0 = toasts off = DND on).
// A missing value means the system default = toasts enabled = DND off.
bool CcDndOnFromToastValue(bool present, std::uint32_t value);

// 夜览 lives in a CloudStore binary blob. The documented enable byte sits at
// offset 0x12: 0x0F = off, 0x13 = on. A blob that is too short or carries any
// other byte there is "unrecognized" - the panel then shows the hint instead of
// guessing a state.
inline constexpr std::size_t kCcNightEnableOffset = 0x12;
inline constexpr std::uint8_t kCcNightByteOff = 0x0F;
inline constexpr std::uint8_t kCcNightByteOn = 0x13;
bool CcNightBlobLooksValid(const std::uint8_t* blob, std::size_t size);
// Precondition: CcNightBlobLooksValid. -1 = off, 1 = on (the caller's tri-state).
int CcNightStateFromBlob(const std::uint8_t* blob, std::size_t size);
std::uint8_t CcNightFlippedEnableByte(std::uint8_t byte);

// ---- right-click minimal menu (finderbar-parity I2; ADR-0039) -----------------
// The bar button's context menu opens the corresponding system settings page and
// nothing else - ADR-0039 pins the control centre to an aggregation entry that
// never takes over the Settings app itself.
enum class CcSettingsPage {
    Sound = 1,          // ms-settings:sound          (volume slider)
    Display = 2,        // ms-settings:display        (brightness row / 夜览)
    Bluetooth = 3,      // ms-settings:bluetooth      (bluetooth cell)
    Network = 4,        // ms-settings:network-wifi   (wifi cell)
    Notifications = 5,  // ms-settings:notifications  (勿扰 cell)
    All = 6,            // ms-settings:               (打开系统设置)
};
const wchar_t* CcSettingsPageUri(CcSettingsPage page);
const wchar_t* CcSettingsPageLabel(CcSettingsPage page);

// ---- the bar-entry glyph (16 DIP box, self-drawn; ASSETS.md) -------------------
// The macOS control centre icon: two toggle pills side by side, one knob up the
// other down. Rounded-rect primitives only (the C-04 backend has no ellipse).
void DrawControlCenterGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                            const render::ColorF& color);

// Cell kinds beyond the C-23 radio kinds: the 2x2 card's system-switch cells.
inline constexpr int kCcCellDnd = 90;
inline constexpr int kCcCellNightLight = 91;

class HardwarePanel {
  public:
    // `on_stats` is called by the panel's own 2s tick when visible - the panel
    // owns the sampling cadence (C-23: <=0.5Hz, never on the render path). The
    // Sonoma layout has no CPU/memory row; the samples keep flowing so
    // SnapshotJson stays truthful for the T-068 probe.
    HardwarePanel(AudioHardware* audio, MediaSidecarBridge* sidecar,
                  std::function<SystemStatsUpdated()> on_stats);
    ~HardwarePanel();

    HardwarePanel(const HardwarePanel&) = delete;
    HardwarePanel& operator=(const HardwarePanel&) = delete;

    static bool RegisterWindowClass(HINSTANCE instance);

    bool Create(HINSTANCE instance);
    void Destroy();
    // Legacy probe open: top-centred of the primary work area (the spot the
    // T-068 probe expects, independent of the bar).
    void Open();
    // T-304 bar open: anchored just below the bar, centred on the clicked
    // control-centre box, clamped into the monitor. The sidecar starts with the
    // panel and stops on Close (no sidecar process outlives a closed panel -
    // media_sidecar.h's lifecycle rule).
    void Open(int dpi, const RECT& monitor, int bar_bottom_px, float anchor_center_x_dip);
    void Close();  // hide (HWND survives for the next open)
    bool visible() const { return hwnd_ != nullptr && IsWindowVisible(hwnd_); }
    bool IsValid() const { return hwnd_ != nullptr; }
    // The panel's HWND - the audio thread posts volume notifications here
    // (AudioHardware::SetEventSink). Valid after Create.
    HWND Handle() const { return hwnd_; }

    // VolumeChanged arrived (audio callback or sidecar push): refresh the
    // affected row and repaint. Cheap when hidden.
    void OnAudioChanged();

    // Sidecar state arrived: refresh the radio rows.
    void OnSidecarState(const SidecarState& state);

    // ---- controls (probe calls these too, same chain as the mouse) --------
    HardwareControlError SetMasterVolume(float level);        // 0..1
    HardwareControlError ToggleMasterMute();
    HardwareControlError SetMicrophoneMute(bool mute);
    HardwareControlError MediaCommand(const std::string& action);
    HardwareControlError SetRadio(int kind, bool on);
    HardwareControlError ToggleRadio(int kind);               // the 2x2 cells
    // `index` into the visible session rows; -1 invalid.
    HardwareControlError SetSessionVolume(int index, float level);
    // Brightness: WMI first, DDC/CI fallback, per the T-304 spike. On an
    // unsupported machine this returns the error whose message is the in-panel
    // hint (the slider never fakes a write).
    HardwareControlError SetBrightness(float level);          // 0..1
    HardwareControlError ToggleDnd();
    HardwareControlError ToggleNightLight();
    bool BrightnessSupported() const { return brightness_supported_; }
    const wchar_t* BrightnessPathName() const;
    bool DndOn() const { return dnd_on_; }
    int NightLightState() const { return night_state_; }  // -1 off / 1 on / 0 unknown

    // Owner calls every tick; the panel self-throttles while visible.
    void Tick(double delta_ms);

    // ---- probe observability ---------------------------------------------
    // Full panel state as one JSON object (same schema as the sidecar poll
    // answer, plus volumes/stats/error) - written to the state file by the
    // main loop whenever it changes, so an unattended probe can assert on it.
    // The T-304 fields (brightness/dnd/night) are additive; every T-068 key
    // keeps its name and shape.
    std::string SnapshotJson() const;
    int SessionRowCount() const { return static_cast<int>(sessions_.size()); }
    const std::wstring& LastErrorText() const { return last_error_.message; }

  private:
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    void Repaint();
    void RefreshVolumes();   // endpoint + mic read-back
    void RefreshSessions();  // audio session re-enumeration (SnapshotJson chain)
    void RefreshSwitches();  // dnd / night light registry read
    HardwareControlError SetRadioInternal(int kind, bool on);

    // Hit regions (DIP), recomputed before every paint.
    struct Rect {
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
        float right() const { return x + w; }
        float bottom() const { return y + h; }
        float CenterX() const { return x + w * 0.5f; }
        float CenterY() const { return y + h * 0.5f; }
    };
    struct ConnCell {
        Rect hit{};       // whole cell
        Rect button{};    // 28x28 circle
        Rect name{};      // label text rect
        Rect state{};     // 开/关 line
        int kind = 0;     // kRadioWiFi / kRadioBluetooth / kCcCellDnd / kCcCellNightLight
        std::wstring label;
        std::wstring state_text;
        bool on = false;
        bool present = true;  // false = dimmed + hint in the state line
    };

    Rect volume_track_{};   // visible 20-wide capsule, DIP
    Rect volume_hit_{};     // 26-wide hit span
    Rect volume_button_{};  // speaker glyph (also the mute toggle)
    Rect volume_pct_{};
    Rect brightness_track_{};
    Rect brightness_hit_{};
    Rect brightness_glyph_{};
    Rect brightness_pct_{};
    Rect error_strip_{};
    ConnCell cells_[4]{};

    // Slider interaction state (0 = none, 1 = volume, 2 = brightness).
    int dragging_ = 0;

    // One cell toggle, shared by the mouse path and the probe path.
    void HandleCellClick(int cell_index);

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    float scale_ = 1.0f;
    platform::CompositionSurface surface_;
    std::unique_ptr<render::IRenderBackend> backend_;

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

    // Brightness (WMI/DDC) - the connection lives for the panel's lifetime.
    bool brightness_supported_ = false;
    int brightness_path_ = 0;  // 0 none / 1 WMI / 2 DDC
    float brightness_level_ = -1.0f;  // -1 = unknown
    void* brightness_wmi_services_ = nullptr;  // IWbemServices*, type-erased
    void* brightness_ddc_handle_ = nullptr;    // HPMONITOR, type-erased
    void BrightnessInitialize();
    void BrightnessShutdown();
    HardwareControlError BrightnessRefresh();

    // 勿扰 / 夜览 cached state (registry read on open + toggle).
    bool dnd_on_ = false;
    bool dnd_present_ = false;
    int night_state_ = 0;  // -1 off / 1 on / 0 unknown (key absent)

    // Tick cadence + the async radio-flip confirmation (the T-068 PollSync
    // blocked the main thread; this panel shares the bar's frame loop).
    double tick_accumulator_ms_ = 0.0;
    double stats_accumulator_ms_ = 0.0;
    DWORD radio_confirm_deadline_ms_ = 0;
    int radio_confirm_kind_ = -1;
    bool radio_confirm_target_ = false;

    // Geometry, physical px.
    int width_px_ = 0;
    int height_px_ = 0;
};

}  // namespace dockforge::finderbar
