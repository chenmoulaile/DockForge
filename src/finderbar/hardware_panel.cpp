// DockForge - the control centre panel implementation (see hardware_panel.h).
#include "finderbar/hardware_panel.h"

#include "core/log.h"
#include "core/paths.h"
#include "finderbar/status_icons.h"

#include <nlohmann/json.hpp>
#include <wbemcli.h>

#include <cmath>
#include <cstdio>
#include <windowsx.h>
// MinGW ships the DDC/CI declarations + libdxva2.a (the T-304 spike linked
// -ldxva2 and called the real API); the panel links it the same way.
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>

namespace dockforge::finderbar {

namespace {

const wchar_t kPanelClass[] = L"DockForgeHardwarePanel";
const char kLogModule[] = "hwpanel";

// ---- palette (Beautiful ControlCenterPanel.xaml / MacOS.xaml values) ----------
// Plate: MacOS.xaml ThemeDockGlassBackground three stops + ThemeBackdropTint.
constexpr float kPlateTopColor[4] = {0x38 / 255.0f, 0x38 / 255.0f, 0x3A / 255.0f,
                                     0xC8 / 255.0f};
constexpr float kPlateMidColor[4] = {0x24 / 255.0f, 0x24 / 255.0f, 0x26 / 255.0f,
                                     0xB8 / 255.0f};
constexpr float kPlateBotColor[4] = {0x16 / 255.0f, 0x16 / 255.0f, 0x18 / 255.0f,
                                     0xA8 / 255.0f};
constexpr float kPopupTint[4] = {0x20 / 255.0f, 0x20 / 255.0f, 0x20 / 255.0f,
                                 0x4D / 255.0f};
constexpr float kStroke[4] = {1.0f, 1.0f, 1.0f, 0x16 / 255.0f};
// Cards: AuroraTokenDictionary.cs AurCardFill / AurCardBorder (macOS fallback set).
constexpr float kCardFill[4] = {1.0f, 1.0f, 1.0f, 0x14 / 255.0f};
constexpr float kCardBorder[4] = {1.0f, 1.0f, 1.0f, 0x1F / 255.0f};
// Connection circle: CcConnToggle #3DFFFFFF off / ThemeAccent on.
constexpr float kButtonOff[4] = {1.0f, 1.0f, 1.0f, 0x3D / 255.0f};
// Apple systemBlue dark (Beautiful ThemeAccent / status_popup accent).
constexpr float kAccent[4] = {0x0A / 255.0f, 0x84 / 255.0f, 0xFF / 255.0f, 1.0f};
// Slider track: MacTokens.cs MacControlFillHover #4DFFFFFF; fill = accent.
constexpr float kTrack[4] = {1.0f, 1.0f, 1.0f, 0x4D / 255.0f};
constexpr float kText[4] = {1.0f, 1.0f, 1.0f, 1.0f};
constexpr float kTextFaint[4] = {1.0f, 1.0f, 1.0f, 0.66f};
constexpr float kTextPct[4] = {1.0f, 1.0f, 1.0f, 0.85f};
constexpr float kErrorText[4] = {1.0f, 0x63 / 255.0f, 0x63 / 255.0f, 1.0f};

constexpr int kMaxSessionRows = 4;

// Registry levers (ADR-0057). 勿扰 = the toast master switch; absent value =
// system default = toasts on = DND off. 夜览 = the CloudStore state blob.
constexpr wchar_t kToastKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\PushNotifications";
constexpr wchar_t kToastValue[] = L"ToastNotificationEnabled";
constexpr wchar_t kNightKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\CloudStore\\Store\\DefaultAccount"
    L"\\Current\\default$windows.data.bluelightreduction.bluelightreductionstate\\Current";
constexpr wchar_t kNightValue[] = L"Data";

std::wstring PercentText(float percent01) {
    wchar_t buf[16];
    swprintf(buf, 16, L"%d%%", static_cast<int>(percent01 * 100.0f + 0.5f));
    return buf;
}

render::ColorF Color(const float v[4]) {
    return render::ColorF{v[0], v[1], v[2], v[3]};
}

// ---- glyph primitives (rounded-rect only; circle = rounded rect at half width,
// the dock_view.cpp trick the whole codebase uses) --------------------------------
void FillCircle(render::IRenderBackend& backend, float cx, float cy, float radius,
                const render::ColorF& color) {
    const render::RectF rect{cx - radius, cy - radius, cx + radius, cy + radius};
    backend.FillRoundedRect(rect, radius, color);
}

// Diagonal stroke via a dot chain - the same trick status_icons uses for the
// charging bolt and the Wi-Fi arcs.
void DotLine(render::IRenderBackend& backend, float x0, float y0, float x1, float y1,
             int dots, float radius, const render::ColorF& color) {
    for (int i = 0; i < dots; ++i) {
        const float t = dots <= 1 ? 0.5f : static_cast<float>(i) / (dots - 1);
        FillCircle(backend, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, radius, color);
    }
}

// Bluetooth rune: vertical bar + the two crossing diagonals, dot-chained.
void DrawBtRune(render::IRenderBackend& backend, const render::RectF& slot,
                const render::ColorF& color) {
    const float cx = slot.CenterX();
    const float top = slot.top + 1.0f;
    const float bottom = slot.bottom - 1.0f;
    const float arm = slot.Width() * 0.30f;
    const render::RectF bar{cx - slot.Width() * 0.07f, top, cx + slot.Width() * 0.07f,
                            bottom};
    backend.FillRoundedRect(bar, slot.Width() * 0.07f, color);
    DotLine(backend, cx, top, cx + arm, top + slot.Height() * 0.25f, 4,
            slot.Width() * 0.08f, color);
    DotLine(backend, cx + arm, top + slot.Height() * 0.25f, cx - arm, bottom - 1.5f, 6,
            slot.Width() * 0.08f, color);
    DotLine(backend, cx - arm, top + 1.5f, cx, top + slot.Height() * 0.45f, 5,
            slot.Width() * 0.08f, color);
}

// 勿扰: crescent moon - a filled disc with a bite taken out by a second disc in
// the button background colour (both states of the 28x28 button are flat fills,
// so the bite colour is exact).
void DrawMoonGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                   const render::ColorF& color, const render::ColorF& bite) {
    const float r = std::min(slot.Width(), slot.Height()) * 0.5f;
    FillCircle(backend, slot.CenterX(), slot.CenterY(), r, color);
    FillCircle(backend, slot.CenterX() + r * 0.55f, slot.CenterY() - r * 0.35f, r * 0.82f,
               bite);
}

// 亮度: sun disc + eight rays (four cardinal bars + four diagonal dots).
void DrawSunGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                  const render::ColorF& color) {
    const float cx = slot.CenterX();
    const float cy = slot.CenterY();
    const float side = std::min(slot.Width(), slot.Height());
    const float r = side * 0.28f;
    FillCircle(backend, cx, cy, r, color);
    const float ray0 = r + side * 0.12f;
    const float ray1 = side * 0.48f;
    const float w = std::max(0.8f, r * 0.36f);
    // Cardinal rays.
    backend.FillRoundedRect(render::RectF{cx - w * 0.5f, cy - ray1, cx + w * 0.5f,
                                          cy - ray0},
                            w * 0.5f, color);
    backend.FillRoundedRect(render::RectF{cx - w * 0.5f, cy + ray0, cx + w * 0.5f,
                                          cy + ray1},
                            w * 0.5f, color);
    backend.FillRoundedRect(render::RectF{cx - ray1, cy - w * 0.5f, cx - ray0,
                                          cy + w * 0.5f},
                            w * 0.5f, color);
    backend.FillRoundedRect(render::RectF{cx + ray0, cy - w * 0.5f, cx + ray1,
                                          cy + w * 0.5f},
                            w * 0.5f, color);
    // Diagonal rays (single dots on the diagonals).
    for (int i = 0; i < 4; ++i) {
        const float dx = (i == 0 || i == 3) ? 0.7071f : -0.7071f;
        const float dy = (i == 0 || i == 1) ? -0.7071f : 0.7071f;
        FillCircle(backend, cx + dx * (ray0 + ray1) * 0.5f,
                   cy + dy * (ray0 + ray1) * 0.5f, w * 0.5f, color);
    }
}

// 夜览 cell: the sun with a crescent bite - night = the display dimmed. Same
// bite trick as the moon.
void DrawNightGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                    const render::ColorF& color, const render::ColorF& bite) {
    DrawSunGlyph(backend, slot, color);
    FillCircle(backend, slot.CenterX() + slot.Width() * 0.20f,
               slot.CenterY() + slot.Height() * 0.20f,
               std::min(slot.Width(), slot.Height()) * 0.34f, bite);
}

// Speaker + waves (volume tile); muted adds the slash chain and drops the waves.
void DrawSpeakerGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                      const render::ColorF& color, bool muted) {
    const float w = slot.Width();
    const float h = slot.Height();
    const float cx = slot.left + w * 0.30f;
    const float cy = slot.CenterY();
    // Cabinet: three stacked bars approximating the trapezoid, then the cone.
    const float cone_l = slot.left;
    const float cone_r = cx + w * 0.08f;
    backend.FillRoundedRect(
        render::RectF{cone_l, cy - h * 0.10f, cone_r, cy + h * 0.10f}, h * 0.10f, color);
    backend.FillRoundedRect(render::RectF{cone_l + w * 0.08f, cy - h * 0.20f,
                                          cx + w * 0.16f, cy + h * 0.20f},
                            h * 0.12f, color);
    backend.FillRoundedRect(render::RectF{cx + w * 0.16f, cy - h * 0.34f,
                                          cx + w * 0.26f, cy + h * 0.34f},
                            h * 0.14f, color);
    if (muted) {
        DotLine(backend, slot.left + w * 0.15f, slot.top + h * 0.15f, slot.right - 1.0f,
                slot.bottom - h * 0.15f, 5, std::max(0.7f, w * 0.06f), color);
        return;
    }
    // Waves: two short bars with growing gap, right of the cone.
    const float w0 = cx + w * 0.40f;
    backend.FillRoundedRect(render::RectF{w0, cy - h * 0.10f, w0 + w * 0.10f, cy + h * 0.10f},
                            h * 0.10f, color);
    backend.FillRoundedRect(render::RectF{w0 + w * 0.18f, cy - h * 0.22f,
                                          w0 + w * 0.28f, cy + h * 0.22f},
                            h * 0.14f, color);
}

}  // namespace

// ---- pure helpers --------------------------------------------------------------

float HardwarePanelHeightFor(bool /*brightness_supported*/) {
    // Top pad + top area + gap + brightness row + gap + error strip + bottom pad.
    // The brightness row exists in both states (the unsupported machine shows the
    // hint inside the row instead of the percent), so the height does not move.
    return kCcTopPadDip + kCcTopAreaDip + kCcSectionGapDip + kCcBrightnessRowDip +
           kCcSectionGapDip + kCcErrorStripDip + kCcBottomPadDip;
}

float ControlCenterSliderLevelFromY(float y_dip, float track_top_dip, float track_len_dip) {
    if (track_len_dip <= 0.0f) return 0.0f;
    // Fill grows from the bottom: level 0 at the track bottom, 1 at the top.
    const float level = 1.0f - (y_dip - track_top_dip) / track_len_dip;
    if (level < 0.0f) return 0.0f;
    if (level > 1.0f) return 1.0f;
    return level;
}

float ControlCenterSliderLevelFromX(float x_dip, float track_left_dip, float track_len_dip) {
    if (track_len_dip <= 0.0f) return 0.0f;
    const float level = (x_dip - track_left_dip) / track_len_dip;
    if (level < 0.0f) return 0.0f;
    if (level > 1.0f) return 1.0f;
    return level;
}

bool CcDndOnFromToastValue(bool present, std::uint32_t value) {
    // 0 = toasts off = DND on; missing/nonzero = toasts on = DND off.
    return present && value == 0;
}

bool CcNightBlobLooksValid(const std::uint8_t* blob, std::size_t size) {
    if (blob == nullptr || size <= kCcNightEnableOffset) return false;
    // CB header magic: CloudStore blobs start with the two chars "CB".
    if (blob[0] != 'C' || blob[1] != 'B') return false;
    const std::uint8_t b = blob[kCcNightEnableOffset];
    return b == kCcNightByteOff || b == kCcNightByteOn;
}

int CcNightStateFromBlob(const std::uint8_t* blob, std::size_t size) {
    if (!CcNightBlobLooksValid(blob, size)) return 0;
    return blob[kCcNightEnableOffset] == kCcNightByteOn ? 1 : -1;
}

std::uint8_t CcNightFlippedEnableByte(std::uint8_t byte) {
    return byte == kCcNightByteOn ? kCcNightByteOff : kCcNightByteOn;
}

const wchar_t* CcSettingsPageUri(CcSettingsPage page) {
    switch (page) {
        case CcSettingsPage::Sound: return L"ms-settings:sound";
        case CcSettingsPage::Display: return L"ms-settings:display";
        case CcSettingsPage::Bluetooth: return L"ms-settings:bluetooth";
        case CcSettingsPage::Network: return L"ms-settings:network-wifi";
        case CcSettingsPage::Notifications: return L"ms-settings:notifications";
        case CcSettingsPage::All: return L"ms-settings:";
        default: return nullptr;
    }
}

const wchar_t* CcSettingsPageLabel(CcSettingsPage page) {
    switch (page) {
        case CcSettingsPage::Sound: return L"声音设置…";
        case CcSettingsPage::Display: return L"显示设置…";
        case CcSettingsPage::Bluetooth: return L"蓝牙设置…";
        case CcSettingsPage::Network: return L"WLAN 设置…";
        case CcSettingsPage::Notifications: return L"通知设置…";
        case CcSettingsPage::All: return L"打开系统设置…";
        default: return nullptr;
    }
}

// The macOS menu-bar control centre glyph. ADR-0058 second round: the grid stays
// (round10's SymbolControlCenter form, confirmed by the first acceptance round)
// but the whole glyph now fits the reference's 20x20 physical px = 10 DIP on the
// 16 DIP box - the first round drew it 1.75x too large with too-thick strokes.
void DrawControlCenterGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                            const render::ColorF& color) {
    const float w = slot.Width();
    const float h = slot.Height();
    if (w <= 0.0f || h <= 0.0f) return;
    // The glyph box is 62.5% of the slot (10 DIP at the 16 DIP icon box).
    const float glyph = std::min(w, h) * 0.625f;
    const float stroke = std::max(0.8f, glyph * 0.09f);
    const float gutter = glyph * 0.20f;
    const float cell = (glyph - gutter) * 0.5f;
    const float left = slot.CenterX() - glyph * 0.5f;
    const float top = slot.CenterY() - glyph * 0.5f;
    const float radius = cell * 0.30f;
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 2; ++col) {
            const float x = left + static_cast<float>(col) * (cell + gutter);
            const float y = top + static_cast<float>(row) * (cell + gutter);
            backend.StrokeRoundedRect(render::RectF{x, y, x + cell, y + cell}, radius,
                                      color, stroke);
        }
    }
}

// ---- construction / window ------------------------------------------------------

HardwarePanel::HardwarePanel(AudioHardware* audio, MediaSidecarBridge* sidecar,
                             std::function<SystemStatsUpdated()> on_stats)
    : audio_(audio), sidecar_(sidecar), on_stats_(std::move(on_stats)) {}

HardwarePanel::~HardwarePanel() { Destroy(); }

bool HardwarePanel::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    // No CS_HREDRAW/CS_VREDRAW: the pixels come from DComp, exactly like the bar.
    wc.style = 0;
    wc.lpfnWndProc = &HardwarePanel::StaticWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kPanelClass;
    const ATOM atom = RegisterClassExW(&wc);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        DF_ERROR(kLogModule, "RegisterClassExW failed %lu", GetLastError());
        return false;
    }
    return true;
}

bool HardwarePanel::Create(HINSTANCE instance) {
    instance_ = instance;
    if (hwnd_ == nullptr) {
        if (!RegisterWindowClass(instance)) return false;
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP,
                                kPanelClass, L"DockForge Control Center", WS_POPUP, 0, 0, 1, 1,
                                nullptr, nullptr, instance, this);
        if (hwnd_ == nullptr) {
            DF_ERROR(kLogModule, "CreateWindowExW failed %lu", GetLastError());
            return false;
        }
        if (!surface_.Initialize(hwnd_)) {
            DF_ERROR(kLogModule, "surface initialize failed");
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
            return false;
        }
        backend_ = render::CreateD2DBackend();
        if (!backend_ || !backend_->Initialize()) {
            DF_ERROR(kLogModule, "D2D backend failed");
            backend_.reset();
            surface_.Shutdown();
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
            return false;
        }
        BrightnessInitialize();
        RefreshSwitches();
    }
    return true;
}

void HardwarePanel::Destroy() {
    if (hwnd_ == nullptr) return;
    if (backend_) backend_->Shutdown();
    backend_.reset();
    surface_.Shutdown();
    BrightnessShutdown();
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

const wchar_t* HardwarePanel::BrightnessPathName() const {
    switch (brightness_path_) {
        case 1: return L"WMI";
        case 2: return L"DDC/CI";
        default: return L"none";
    }
}

void HardwarePanel::Open() {
    if (hwnd_ == nullptr) return;
    // DPI of the primary monitor at open time (per-open, like FolderPanel).
    const HDC screen = GetDC(nullptr);
    const float dpi = static_cast<float>(GetDeviceCaps(screen, LOGPIXELSX));
    ReleaseDC(nullptr, screen);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    scale_ = dpi / 96.0f;
    if (sidecar_ && !sidecar_->running()) sidecar_->Start();
    // Centre under the top edge of the work area - the T-068 probe's anchor.
    const float anchor_center_x_dip =
        static_cast<float>(work.right - work.left) / 2.0f / scale_;
    Open(dpi, work, work.top, anchor_center_x_dip);
    DF_INFO(kLogModule, "panel open (probe anchor) dpi=%.0f", static_cast<double>(dpi));
}

void HardwarePanel::Open(int dpi, const RECT& monitor, int bar_bottom_px,
                         float anchor_center_x_dip) {
    if (hwnd_ == nullptr) return;
    scale_ = dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
    const float height_dip = HardwarePanelHeightFor(brightness_supported_);
    width_px_ = static_cast<int>(kCcPanelWidthDip * scale_);
    height_px_ = static_cast<int>(height_dip * scale_);

    RefreshVolumes();
    RefreshSwitches();
    if (brightness_level_ < 0.0f) BrightnessRefresh();

    // The sidecar lives exactly as long as the panel is open (media_sidecar.h).
    if (sidecar_ && !sidecar_->running()) sidecar_->Start();
    if (sidecar_ && sidecar_->running()) sidecar_->SendCommand("poll");

    // Anchor: top edge just below the bar, centre under the clicked box, clamped
    // into the monitor - the same clamps status_popup::Open applies.
    const int anchor_px =
        static_cast<int>(monitor.left) + static_cast<int>(anchor_center_x_dip * scale_);
    int left = anchor_px - width_px_ / 2;
    int top = bar_bottom_px;
    left = std::max(static_cast<int>(monitor.left),
                    std::min(left, static_cast<int>(monitor.right) - width_px_));
    top = std::max(static_cast<int>(monitor.top),
                   std::min(top, static_cast<int>(monitor.bottom) - height_px_));

    if (!surface_.Resize(width_px_, height_px_)) {
        DF_WARN(kLogModule, "surface resize failed");
        return;
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, left, top, width_px_, height_px_, SWP_SHOWWINDOW);
    // Claim the foreground so Esc works and an outside click deactivates the
    // panel - the measured menu requirement and status_popup's same-open claim.
    if (!SetForegroundWindow(hwnd_)) {
        DF_DEBUG(kLogModule, "SetForegroundWindow refused on open (probe path?)");
    }
    SetFocus(hwnd_);
    DF_INFO(kLogModule, "panel open dpi=%.0f brightness_path=%ls sessions=%d",
            static_cast<double>(scale_ * 96.0f), BrightnessPathName(), SessionRowCount());
    Repaint();
}

void HardwarePanel::Close() {
    if (hwnd_ == nullptr || !visible()) return;
    ShowWindow(hwnd_, SW_HIDE);
    if (sidecar_ && sidecar_->running()) sidecar_->Stop();
    DF_INFO(kLogModule, "panel closed");
}

// ---- state refresh --------------------------------------------------------------

void HardwarePanel::RefreshVolumes() {
    master_ = audio_ ? audio_->GetEndpoint(false) : AudioEndpointState{};
    mic_ = audio_ ? audio_->GetEndpoint(true) : AudioEndpointState{};
}

void HardwarePanel::RefreshSessions() {
    sessions_.clear();
    if (!audio_) return;
    auto all = audio_->EnumerateSessions();
    for (auto& s : all) {
        if (sessions_.size() >= kMaxSessionRows) break;
        sessions_.push_back(std::move(s));
    }
}

namespace {

// Registry readers/writers for the two system switches. HKCU only, no elevation.
bool RegReadDword(const wchar_t* path, const wchar_t* value, bool* present,
                  std::uint32_t* out) {
    *present = false;
    *out = 0;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    auto guard = MakeGuard([&key] { RegCloseKey(key); });
    DWORD v = 0, size = sizeof(v), type = 0;
    if (RegQueryValueExW(key, value, nullptr, &type, reinterpret_cast<LPBYTE>(&v),
                         &size) != ERROR_SUCCESS ||
        type != REG_DWORD) {
        return false;
    }
    *present = true;
    *out = v;
    return true;
}

// Reads the 夜览 state blob. Returns false when the key/value is absent.
bool RegReadBlob(const wchar_t* path, const wchar_t* value,
                 std::vector<std::uint8_t>* out) {
    out->clear();
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    auto guard = MakeGuard([&key] { RegCloseKey(key); });
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(key, value, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        type != REG_BINARY || size == 0) {
        return false;
    }
    out->resize(size);
    if (RegQueryValueExW(key, value, nullptr, &type, out->data(), &size) != ERROR_SUCCESS) {
        out->clear();
        return false;
    }
    return true;
}

bool RegWriteDword(const wchar_t* path, const wchar_t* value, std::uint32_t data) {
    HKEY key = nullptr;
    // Create the key when absent (a machine that never touched the Settings page
    // has no PushNotifications value yet - the write defines it).
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    auto guard = MakeGuard([&key] { RegCloseKey(key); });
    return RegSetValueExW(key, value, 0, REG_DWORD,
                          reinterpret_cast<const BYTE*>(&data), sizeof(data)) == ERROR_SUCCESS;
}

bool RegWriteBlob(const wchar_t* path, const wchar_t* value,
                  const std::vector<std::uint8_t>& data) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    auto guard = MakeGuard([&key] { RegCloseKey(key); });
    return RegSetValueExW(key, value, 0, REG_BINARY, data.data(),
                          static_cast<DWORD>(data.size())) == ERROR_SUCCESS;
}

void BroadcastSettingChange(const wchar_t* what) {
    // Best effort: the shell may or may not react; the write itself is the
    // system-defined switch and reads back truthfully either way.
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(what),
                        SMTO_ABORTIFHUNG, 1000, nullptr);
}

}  // namespace

void HardwarePanel::RefreshSwitches() {
    std::uint32_t toast = 0;
    dnd_present_ = RegReadDword(kToastKey, kToastValue, &dnd_present_, &toast);
    dnd_on_ = CcDndOnFromToastValue(dnd_present_, toast);
    std::vector<std::uint8_t> blob;
    night_state_ = RegReadBlob(kNightKey, kNightValue, &blob)
                       ? CcNightStateFromBlob(blob.data(), blob.size())
                       : 0;
}

HardwareControlError HardwarePanel::ToggleDnd() {
    const std::uint32_t target = dnd_on_ ? 1 : 0;  // flip the toast master
    if (!RegWriteDword(kToastKey, kToastValue, target)) {
        last_error_ = HardwareControlError::Failed(L"勿扰", L"写入通知开关失败（注册表拒绝）",
                                                   GetLastError());
        return last_error_;
    }
    BroadcastSettingChange(L"Notifications");
    dnd_present_ = true;
    dnd_on_ = CcDndOnFromToastValue(true, target);
    last_error_ = HardwareControlError::None();
    DF_INFO(kLogModule, "dnd -> %d (ToastNotificationEnabled=%lu)", dnd_on_ ? 1 : 0,
            static_cast<unsigned long>(target));
    if (visible()) Repaint();
    return last_error_;
}

HardwareControlError HardwarePanel::ToggleNightLight() {
    std::vector<std::uint8_t> blob;
    if (!RegReadBlob(kNightKey, kNightValue, &blob) ||
        !CcNightBlobLooksValid(blob.data(), blob.size())) {
        // Honest failure: this machine has no CloudStore state blob (the T-304
        // spike measured exactly this), so the panel shows the hint instead of
        // fabricating a state.
        last_error_ = HardwareControlError::Failed(
            L"夜览",
            L"本机没有夜览的注册表状态（CloudStore 键不存在），无法切换；可在系统设置打开夜览",
            HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
        DF_WARN(kLogModule, "night light toggle unsupported (no state blob)");
        return last_error_;
    }
    blob[kCcNightEnableOffset] = CcNightFlippedEnableByte(blob[kCcNightEnableOffset]);
    if (!RegWriteBlob(kNightKey, kNightValue, blob)) {
        last_error_ = HardwareControlError::Failed(L"夜览", L"写入夜览状态失败（注册表拒绝）",
                                                   GetLastError());
        return last_error_;
    }
    BroadcastSettingChange(L"settings");
    night_state_ = CcNightStateFromBlob(blob.data(), blob.size());
    last_error_ = HardwareControlError::None();
    DF_INFO(kLogModule, "night light -> %d", night_state_ > 0 ? 1 : 0);
    if (visible()) Repaint();
    return last_error_;
}

// ---- brightness (WMI first, DDC/CI fallback) ------------------------------------

void HardwarePanel::BrightnessInitialize() {
    brightness_supported_ = false;
    brightness_path_ = 0;
    brightness_level_ = -1.0f;
    // WMI: works for laptop internal panels (the T-304 spike measured 0..100 live).
    IWbemLocator* locator = nullptr;
    const HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_IWbemLocator,
                                        reinterpret_cast<void**>(&locator));
    if (SUCCEEDED(hr) && locator != nullptr) {
        IWbemServices* services = nullptr;
        BSTR ns = SysAllocString(L"ROOT\\WMI");
        const HRESULT connect =
            locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr,
                                   &services);
        SysFreeString(ns);
        locator->Release();
        if (SUCCEEDED(connect) && services != nullptr) {
            CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                              RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
                              EOAC_NONE);
            brightness_wmi_services_ = services;
            brightness_path_ = 1;
        }
    }
    if (brightness_path_ == 1) {
        const HardwareControlError probe = BrightnessRefresh();
        if (probe.ok() && brightness_level_ >= 0.0f) {
            brightness_supported_ = true;
            DF_INFO(kLogModule, "brightness via WMI, level=%.2f",
                    static_cast<double>(brightness_level_));
            return;
        }
        BrightnessShutdown();
    }
    // DDC/CI: external monitors. The spike measured ERROR_NOT_CONNECTED(1168) on
    // this machine's internal panel; keep the path for external hardware.
    HMONITOR hmon = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    DWORD count = 0;
    if (GetNumberOfPhysicalMonitorsFromHMONITOR(hmon, &count) && count > 0) {
        PHYSICAL_MONITOR* mons = new PHYSICAL_MONITOR[count]{};
        if (GetPhysicalMonitorsFromHMONITOR(hmon, count, mons)) {
            DWORD lo = 0, cur = 0, hi = 0;
            if (GetMonitorBrightness(mons[0].hPhysicalMonitor, &lo, &cur, &hi) && hi > lo) {
                brightness_ddc_handle_ = mons[0].hPhysicalMonitor;
                brightness_path_ = 2;
                brightness_level_ = (cur - lo) / static_cast<float>(hi - lo);
                brightness_supported_ = true;
                DF_INFO(kLogModule, "brightness via DDC/CI, level=%.2f",
                        static_cast<double>(brightness_level_));
                return;  // keep the monitor array alive: the handle is in use
            }
            DF_INFO(kLogModule, "DDC brightness refused gle=%lu (internal panel?)",
                    GetLastError());
        }
        DestroyPhysicalMonitors(count, mons);
        delete[] mons;
    } else {
        DF_INFO(kLogModule, "no physical monitors for DDC (gle=%lu)", GetLastError());
    }
    brightness_path_ = 0;
    brightness_supported_ = false;
}

void HardwarePanel::BrightnessShutdown() {
    if (brightness_wmi_services_ != nullptr) {
        static_cast<IWbemServices*>(brightness_wmi_services_)->Release();
        brightness_wmi_services_ = nullptr;
    }
    if (brightness_ddc_handle_ != nullptr) {
        PHYSICAL_MONITOR mon{};
        mon.hPhysicalMonitor = static_cast<HANDLE>(brightness_ddc_handle_);
        DestroyPhysicalMonitors(1, &mon);
        brightness_ddc_handle_ = nullptr;
    }
    brightness_path_ = 0;
    brightness_supported_ = false;
}

HardwareControlError HardwarePanel::BrightnessRefresh() {
    if (brightness_path_ == 1 && brightness_wmi_services_ != nullptr) {
        auto* services = static_cast<IWbemServices*>(brightness_wmi_services_);
        IEnumWbemClassObject* rows = nullptr;
        BSTR wql = SysAllocString(L"WQL");
        BSTR query = SysAllocString(L"SELECT CurrentBrightness FROM WmiMonitorBrightness");
        const auto free_strings = MakeGuard([&] {
            SysFreeString(wql);
            SysFreeString(query);
        });
        if (wql == nullptr || query == nullptr) {
            return HardwareControlError::Failed(L"亮度", L"WMI 内存不足", E_OUTOFMEMORY);
        }
        const HRESULT hr =
            services->ExecQuery(wql, query, WBEM_FLAG_FORWARD_ONLY, nullptr, &rows);
        if (FAILED(hr) || rows == nullptr) {
            return HardwareControlError::Failed(L"亮度", L"读取亮度失败（WMI 查询拒绝）", hr);
        }
        HardwareControlError result = HardwareControlError::None();
        IWbemClassObject* row = nullptr;
        ULONG returned = 0;
        if (SUCCEEDED(rows->Next(2000, 1, &row, &returned)) && returned == 1 &&
            row != nullptr) {
            VARIANT value;
            VariantInit(&value);
            if (SUCCEEDED(row->Get(L"CurrentBrightness", 0, &value, nullptr, nullptr))) {
                float pct = -1.0f;
                if (value.vt == VT_I4) pct = static_cast<float>(value.lVal);
                else if (value.vt == VT_UI4) pct = static_cast<float>(value.ulVal);
                else if (value.vt == VT_UI1) pct = static_cast<float>(value.bVal);
                else if (value.vt == VT_BSTR)
                    pct = static_cast<float>(wcstoul(value.bstrVal, nullptr, 10));
                if (pct >= 0.0f && pct <= 100.0f) {
                    brightness_level_ = pct / 100.0f;
                } else {
                    result = HardwareControlError::Failed(L"亮度", L"亮度读数超出范围",
                                                          E_BOUNDS);
                }
            }
            VariantClear(&value);
            row->Release();
        } else {
            result = HardwareControlError::Failed(L"亮度", L"读取亮度失败（无数据行）",
                                                  WBEM_E_NOT_FOUND);
        }
        rows->Release();
        return result;
    }
    if (brightness_path_ == 2 && brightness_ddc_handle_ != nullptr) {
        const HANDLE handle = static_cast<HANDLE>(brightness_ddc_handle_);
        DWORD lo = 0, cur = 0, hi = 0;
        if (!GetMonitorBrightness(handle, &lo, &cur, &hi) || hi <= lo) {
            return HardwareControlError::Failed(L"亮度", L"读取亮度失败（DDC 拒绝）",
                                                HRESULT_FROM_WIN32(GetLastError()));
        }
        brightness_level_ = (cur - lo) / static_cast<float>(hi - lo);
        return HardwareControlError::None();
    }
    return HardwareControlError::Failed(
        L"亮度", L"本机显示器不支持亮度调节（DDC/WMI 均不可用）",
        HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
}

HardwareControlError HardwarePanel::SetBrightness(float level) {
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    if (!brightness_supported_) {
        last_error_ = HardwareControlError::Failed(
            L"亮度", L"本机显示器不支持亮度调节（DDC/WMI 均不可用），滑杆已禁用",
            HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
        return last_error_;
    }
    if (brightness_path_ == 1 && brightness_wmi_services_ != nullptr) {
        auto* services = static_cast<IWbemServices*>(brightness_wmi_services_);
        IWbemClassObject* method_def = nullptr;
        IWbemClassObject* in_params = nullptr;
        const auto release_defs = MakeGuard([&] {
            if (method_def) method_def->Release();
            if (in_params) in_params->Release();
        });
        BSTR cls = SysAllocString(L"WmiMonitorBrightnessMethods");
        BSTR method = SysAllocString(L"WmiSetBrightness");
        const auto free_strings = MakeGuard([&] {
            SysFreeString(cls);
            SysFreeString(method);
        });
        HRESULT hr = services->GetObject(cls, 0, nullptr, &method_def, nullptr);
        if (SUCCEEDED(hr) && method_def != nullptr) {
            hr = method_def->GetMethod(method, 0, &in_params, nullptr);
        }
        if (SUCCEEDED(hr) && in_params != nullptr) {
            IWbemClassObject* instance = nullptr;
            hr = in_params->SpawnInstance(0, &instance);
            if (SUCCEEDED(hr) && instance != nullptr) {
                VARIANT timeout;
                VariantInit(&timeout);
                timeout.vt = VT_I4;
                timeout.lVal = 500;  // the provider's own transition timeout
                VARIANT target;
                VariantInit(&target);
                target.vt = VT_UI1;
                target.bVal = static_cast<BYTE>(level * 100.0f + 0.5f);
                instance->Put(L"Timeout", 0, &timeout, 0);
                instance->Put(L"Brightness", 0, &target, 0);
                // ExecMethod wants the instance path, not the class name: pull the
                // single WmiMonitorBrightnessMethods instance's __PATH.
                IEnumWbemClassObject* rows = nullptr;
                BSTR wql = SysAllocString(L"WQL");
                BSTR q =
                    SysAllocString(L"SELECT __PATH FROM WmiMonitorBrightnessMethods");
                const HRESULT query_hr =
                    services->ExecQuery(wql, q, WBEM_FLAG_FORWARD_ONLY, nullptr, &rows);
                SysFreeString(wql);
                SysFreeString(q);
                if (SUCCEEDED(query_hr) && rows != nullptr) {
                    IWbemClassObject* row = nullptr;
                    ULONG returned = 0;
                    if (SUCCEEDED(rows->Next(2000, 1, &row, &returned)) && returned == 1 &&
                        row != nullptr) {
                        VARIANT path_v;
                        VariantInit(&path_v);
                        if (SUCCEEDED(row->Get(L"__PATH", 0, &path_v, nullptr, nullptr)) &&
                            path_v.vt == VT_BSTR) {
                            hr = services->ExecMethod(path_v.bstrVal, method, 0, nullptr,
                                                      instance, nullptr, nullptr);
                        }
                        VariantClear(&path_v);
                        row->Release();
                    }
                    rows->Release();
                } else {
                    hr = FAILED(query_hr) ? static_cast<HRESULT>(query_hr)
                                          : static_cast<HRESULT>(WBEM_E_NOT_FOUND);
                }
                instance->Release();
            }
        }
        if (FAILED(hr)) {
            last_error_ = HardwareControlError::Failed(L"亮度", L"设置亮度失败（WMI 拒绝）",
                                                       hr);
            return last_error_;
        }
        brightness_level_ = level;
        last_error_ = HardwareControlError::None();
        if (visible()) Repaint();
        return last_error_;
    }
    if (brightness_path_ == 2 && brightness_ddc_handle_ != nullptr) {
        const HANDLE handle = static_cast<HANDLE>(brightness_ddc_handle_);
        DWORD lo = 0, cur = 0, hi = 0;
        if (!GetMonitorBrightness(handle, &lo, &cur, &hi) || hi <= lo) {
            last_error_ = HardwareControlError::Failed(L"亮度", L"设置亮度失败（DDC 拒绝）",
                                                       HRESULT_FROM_WIN32(GetLastError()));
            return last_error_;
        }
        const DWORD target =
            lo + static_cast<DWORD>(level * static_cast<float>(hi - lo) + 0.5f);
        if (!SetMonitorBrightness(handle, target)) {
            last_error_ = HardwareControlError::Failed(L"亮度", L"设置亮度失败（DDC 拒绝）",
                                                       HRESULT_FROM_WIN32(GetLastError()));
            return last_error_;
        }
        brightness_level_ = level;
        last_error_ = HardwareControlError::None();
        if (visible()) Repaint();
        return last_error_;
    }
    last_error_ = HardwareControlError::Failed(L"亮度", L"本机显示器不支持亮度调节",
                                               HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
    return last_error_;
}

// ---- C-23 controls ---------------------------------------------------------------

void HardwarePanel::OnAudioChanged() {
    if (!visible()) return;
    RefreshVolumes();
    Repaint();
}

void HardwarePanel::OnSidecarState(const SidecarState& state) {
    sidecar_state_ = state;
    if (!visible()) return;
    Repaint();
}

HardwareControlError HardwarePanel::SetMasterVolume(float level) {
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    auto err = audio_ ? audio_->SetEndpointLevel(false, level)
                      : HardwareControlError::Failed(L"主音量", L"音频不可用", E_FAIL);
    last_error_ = err;
    master_.level = err.ok() ? level : master_.level;
    if (visible()) Repaint();
    return err;
}

HardwareControlError HardwarePanel::ToggleMasterMute() {
    auto err = audio_ ? audio_->SetEndpointMute(false, !master_.muted)
                      : HardwareControlError::Failed(L"主音量", L"音频不可用", E_FAIL);
    last_error_ = err;
    if (err.ok()) master_.muted = !master_.muted;
    if (visible()) Repaint();
    return err;
}

HardwareControlError HardwarePanel::SetMicrophoneMute(bool mute) {
    auto err = audio_ ? audio_->SetEndpointMute(true, mute)
                      : HardwareControlError::Failed(L"麦克风", L"音频不可用", E_FAIL);
    last_error_ = err;
    return err;
}

HardwareControlError HardwarePanel::SetRadioInternal(int kind, bool on) {
    if (!sidecar_ || !sidecar_->running()) {
        return (last_error_ = HardwareControlError::Failed(
                    L"无线电", L"无线电服务不可用（sidecar 未运行）", E_FAIL));
    }
    std::string cmd = "radio ";
    cmd += std::to_string(kind);
    cmd += on ? " 1" : " 0";
    if (!sidecar_->SendCommand(cmd)) {
        return (last_error_ =
                    HardwareControlError::Failed(L"无线电", L"发送无线电命令失败", E_FAIL));
    }
    // The flip confirms asynchronously: the next tick's poll answer settles the
    // row. The old T-068 path waited synchronously (PollSync 2.5s) - that blocked
    // the main thread and this panel now shares the bar's frame loop, so the
    // confirm is the tick check below instead of a sleep.
    radio_confirm_deadline_ms_ = GetTickCount() + 3000;
    radio_confirm_kind_ = kind;
    radio_confirm_target_ = on;
    return (last_error_ = HardwareControlError::None());
}

HardwareControlError HardwarePanel::SetRadio(int kind, bool on) {
    const HardwareControlError err = SetRadioInternal(kind, on);
    if (!err.ok()) return err;
    // Probe path keeps the old bounded confirmation so a script gets a settled
    // answer in one command (the panel is hidden for these runs - the frame loop
    // stall does not touch the bar).
    if (!visible() && sidecar_ && sidecar_->running()) {
        SidecarState state = sidecar_->PollSync(2500);
        if (state.valid) OnSidecarState(state);
        const SidecarRadioState* radio = FindRadio(sidecar_state_, kind);
        if (!radio || (radio->state != 1) == on) {
            return (last_error_ = HardwareControlError::Failed(
                        L"无线电", L"切换无线电被系统拒绝", E_FAIL));
        }
        radio_confirm_kind_ = -1;
    }
    if (visible()) Repaint();
    return last_error_;
}

HardwareControlError HardwarePanel::ToggleRadio(int kind) {
    const SidecarRadioState* radio = FindRadio(sidecar_state_, kind);
    if (radio == nullptr) {
        last_error_ = HardwareControlError::Failed(
            L"无线电", L"无线电状态不可用（sidecar 未运行或无此设备）", E_FAIL);
        return last_error_;
    }
    return SetRadioInternal(kind, radio->state != 1);
}

HardwareControlError HardwarePanel::MediaCommand(const std::string& action) {
    if (!sidecar_ || !sidecar_->running()) {
        return (last_error_ = HardwareControlError::Failed(
                    L"媒体控制", L"媒体控制服务不可用（sidecar 未运行）", E_FAIL));
    }
    if (!sidecar_->SendCommand("media " + action)) {
        return (last_error_ =
                    HardwareControlError::Failed(L"媒体控制", L"发送媒体命令失败", E_FAIL));
    }
    // The sidecar answers asynchronously; wait (bounded) for a non-push line
    // so a rejection lands in the error strip instead of racing past it. Probe
    // path only - the Sonoma panel has no media row until T-304b.
    const DWORD start = GetTickCount();
    bool answered = false;
    std::wstring error;
    while (GetTickCount() - start < 3000 && !answered) {
        for (const auto& line : sidecar_->DrainLines()) {
            SidecarState parsed;
            bool is_push = false;
            std::string push_kind;
            std::wstring err;
            if (!ParseSidecarLine(line, &parsed, &is_push, &push_kind, &err)) continue;
            if (is_push) continue;
            answered = true;
            error = err;
            if (parsed.valid) OnSidecarState(parsed);
        }
        if (!answered) Sleep(30);
    }
    if (!error.empty()) {
        last_error_ = HardwareControlError::Failed(L"媒体控制", error, E_FAIL);
        DF_WARN(kLogModule, "media '%s' rejected: %s", action.c_str(),
                core::WideToUtf8(error).c_str());
    } else {
        last_error_ = HardwareControlError::None();
    }
    if (visible()) Repaint();
    return last_error_;
}

HardwareControlError HardwarePanel::SetSessionVolume(int index, float level) {
    if (index < 0 || index >= SessionRowCount() || !audio_) {
        return (last_error_ = HardwareControlError::Failed(L"程序音量", L"无效会话",
                                                           E_INVALIDARG));
    }
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    auto err = audio_->SetSessionLevel(sessions_[index].session_id, level);
    last_error_ = err;
    if (err.ok()) {
        sessions_[index].level = level;
        if (visible()) Repaint();
    }
    return err;
}

// ---- cell dispatch ---------------------------------------------------------------

void HardwarePanel::HandleCellClick(int cell_index) {
    if (cell_index < 0 || cell_index >= 4) return;
    const ConnCell& cell = cells_[cell_index];
    if (!cell.present) {
        // A dimmed cell answers with the reason instead of silence (C-23).
        last_error_ = HardwareControlError::Failed(
            L"控制中心", L"该开关在此机器上不可用：" + cell.label,
            HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
        Repaint();
        return;
    }
    switch (cell.kind) {
        case kRadioWiFi:
        case kRadioBluetooth:
            ToggleRadio(cell.kind);
            break;
        case kCcCellDnd:
            ToggleDnd();
            break;
        case kCcCellNightLight:
            ToggleNightLight();
            break;
        default:
            break;
    }
    Repaint();
}

// ---- tick ------------------------------------------------------------------------

void HardwarePanel::Tick(double delta_ms) {
    if (!visible()) return;
    tick_accumulator_ms_ += delta_ms;
    if (tick_accumulator_ms_ < 1000.0) return;  // 1Hz while open
    tick_accumulator_ms_ = 0.0;

    RefreshVolumes();

    // Sidecar drain + non-blocking poll: the answer arrives as a line, the next
    // tick reads it. The T-068 PollSync(600) here blocked the main thread every
    // tick - this panel shares the bar's frame loop and must not.
    if (sidecar_ && sidecar_->running()) {
        for (const auto& line : sidecar_->DrainLines()) {
            SidecarState parsed;
            bool is_push = false;
            std::string push_kind;
            std::wstring error;
            if (ParseSidecarLine(line, &parsed, &is_push, &push_kind, &error) &&
                parsed.valid) {
                sidecar_state_ = parsed;
            }
        }
        sidecar_->SendCommand("poll");

        // Radio confirm: the optimistic cell state gets corrected here when the
        // system refused the flip (C-23: never a silent failure).
        if (radio_confirm_kind_ >= 0 && GetTickCount() > radio_confirm_deadline_ms_) {
            const SidecarRadioState* radio = FindRadio(sidecar_state_, radio_confirm_kind_);
            if (radio != nullptr && (radio->state != 1) != radio_confirm_target_) {
                last_error_ = HardwareControlError::Failed(L"无线电", L"切换无线电被系统拒绝",
                                                           E_FAIL);
            }
            radio_confirm_kind_ = -1;
        }
    }

    // 2s cadence: brightness + stats + sessions (C-23 <=0.5Hz for the stats chain).
    stats_accumulator_ms_ += 1000.0;
    if (stats_accumulator_ms_ >= 2000.0) {
        stats_accumulator_ms_ = 0.0;
        if (brightness_supported_) BrightnessRefresh();
        if (on_stats_) stats_ = on_stats_();
        RefreshSessions();
    }
    RefreshSwitches();
    Repaint();
}

// ---- probe snapshot ---------------------------------------------------------------

std::string HardwarePanel::SnapshotJson() const {
    nlohmann::json j;
    j["master_level"] = master_.level;
    j["master_muted"] = master_.muted;
    j["mic_present"] = mic_.present;
    j["mic_level"] = mic_.level;
    j["mic_muted"] = mic_.muted;
    j["cpu_percent"] = stats_.cpu_percent;
    j["memory_percent"] = stats_.memory_percent;
    j["memory_used_bytes"] = stats_.memory_used_bytes;
    j["memory_total_bytes"] = stats_.memory_total_bytes;
    j["error"] = core::WideToUtf8(last_error_.message);
    nlohmann::json media;
    media["has_session"] = sidecar_state_.media.has_session;
    media["title"] = core::WideToUtf8(sidecar_state_.media.title);
    media["artist"] = core::WideToUtf8(sidecar_state_.media.artist);
    media["playing"] = sidecar_state_.media.playing;
    media["aumid"] = core::WideToUtf8(sidecar_state_.media.aumid);
    j["media"] = media;
    nlohmann::json radios = nlohmann::json::array();
    for (const auto& r : sidecar_state_.radios) {
        radios.push_back({{"name", core::WideToUtf8(r.name)},
                          {"kind", r.kind},
                          {"state", r.state}});
    }
    j["radios"] = radios;
    nlohmann::json sessions = nlohmann::json::array();
    for (const auto& s : sessions_) {
        sessions.push_back({{"name", core::WideToUtf8(s.name)},
                            {"level", s.level},
                            {"muted", s.muted},
                            {"pid", static_cast<std::uint64_t>(s.pid)}});
    }
    j["sessions"] = sessions;
    // T-304 additions (additive; the T-068 keys above keep their names/shapes).
    nlohmann::json brightness;
    brightness["supported"] = brightness_supported_;
    brightness["path"] =
        BrightnessPathName() != nullptr ? core::WideToUtf8(BrightnessPathName()) : "";
    brightness["level"] = brightness_level_;
    j["brightness"] = brightness;
    nlohmann::json dnd;
    dnd["on"] = dnd_on_;
    dnd["value_present"] = dnd_present_;
    j["dnd"] = dnd;
    nlohmann::json night;
    night["state"] = night_state_;  // -1 off / 1 on / 0 unknown
    j["night"] = night;
    nlohmann::json vis;
    vis["visible"] = visible();
    vis["width_px"] = width_px_;
    vis["height_px"] = height_px_;
    j["panel"] = vis;
    return j.dump();
}

// ---- window procedure --------------------------------------------------------------

LRESULT CALLBACK HardwarePanel::StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                              LPARAM lparam) {
    HardwarePanel* self = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<HardwarePanel*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<HardwarePanel*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->WndProc(hwnd, msg, wparam, lparam);
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT HardwarePanel::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;  // DComp owns every pixel

        case WM_APP + 0x31: {
            // Volume notification posted by the audio thread (see
            // AudioHardware::SetEventSink - the callback itself must not
            // touch COM, the bus or GDI). Main thread from here on.
            if (!audio_) return 0;
            const bool capture = (wparam & 1u) != 0;
            const bool muted = (wparam & 2u) != 0;
            const float level = static_cast<float>(lparam) / 10000.0f;
            audio_->DispatchQueuedVolume(capture, level, muted);
            return 0;
        }

        case WM_ACTIVATE:
            // status_popup's dismissal rule: ANY loss of activation closes the
            // panel - the foreground claim at Open means WA_INACTIVE only arrives
            // after a real outside click. When the claim was refused (probe path)
            // the panel never activates and stays open for the command chain.
            if (LOWORD(wparam) == WA_INACTIVE && visible()) {
                Close();
            }
            return 0;

        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                Close();
                return 0;
            }
            return 0;

        case WM_LBUTTONDOWN: {
            const float scale = scale_ > 0.0f ? scale_ : 1.0f;
            const float x = GET_X_LPARAM(lparam) / scale;
            const float y = GET_Y_LPARAM(lparam) / scale;
            dragging_ = 0;
            const auto in = [x, y](const Rect& r) {
                return x >= r.x && x < r.right() && y >= r.y && y < r.bottom();
            };
            // Volume slider: press = jump + drag (macOS's click-anywhere capsule).
            if (in(volume_hit_) && audio_) {
                dragging_ = 1;
                SetCapture(hwnd);
                SetMasterVolume(
                    ControlCenterSliderLevelFromY(y, volume_track_.y, volume_track_.h));
                return 0;
            }
            // Brightness slider (live only when a path exists).
            if (in(brightness_hit_) && brightness_supported_) {
                dragging_ = 2;
                SetCapture(hwnd);
                SetBrightness(ControlCenterSliderLevelFromX(x, brightness_track_.x,
                                                            brightness_track_.w));
                return 0;
            }
            // Speaker glyph = the mute toggle.
            if (in(volume_button_)) {
                ToggleMasterMute();
                return 0;
            }
            // The 2x2 connection card.
            for (int i = 0; i < 4; ++i) {
                if (in(cells_[i].hit)) {
                    HandleCellClick(i);
                    return 0;
                }
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (dragging_ == 0) return 0;
            const float scale = scale_ > 0.0f ? scale_ : 1.0f;
            const float x = GET_X_LPARAM(lparam) / scale;
            const float y = GET_Y_LPARAM(lparam) / scale;
            if (dragging_ == 1) {
                SetMasterVolume(
                    ControlCenterSliderLevelFromY(y, volume_track_.y, volume_track_.h));
            } else if (dragging_ == 2 && brightness_supported_) {
                SetBrightness(ControlCenterSliderLevelFromX(x, brightness_track_.x,
                                                            brightness_track_.w));
            }
            return 0;
        }

        case WM_LBUTTONUP:
            if (dragging_ != 0) {
                ReleaseCapture();
                dragging_ = 0;
            }
            return 0;

        case WM_DESTROY:
            hwnd_ = nullptr;
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

// ---- paint -----------------------------------------------------------------------

void HardwarePanel::Repaint() {
    if (hwnd_ == nullptr || !surface_.IsValid() || !backend_) return;
    const float width = static_cast<float>(width_px_) / scale_;
    const float height = static_cast<float>(height_px_) / scale_;
    if (width <= 0.0f || height <= 0.0f) return;

    // ---- layout (DIP; the Sonoma numbers) --------------------------------------
    const float pad = kCcSidePadDip;
    const float conn_left = pad;
    const float conn_right = width - pad - kCcVolumeTileWidthDip - 6.0f;
    const float conn_top = kCcTopPadDip;
    const float conn_bottom = conn_top + kCcTopAreaDip;
    const float conn_w = conn_right - conn_left;

    // Connection card cells: 2 columns x 2 rows, 4 DIP row gap (CcTileStyle
    // Margin 3,4,3,0), 4 DIP card padding (CcConnCard Padding=4).
    const float card_pad = 4.0f;
    const float row_gap = 4.0f;
    const float col_gap = 2.0f;
    const float cell_w = (conn_w - card_pad * 2.0f - col_gap) * 0.5f;
    const float cell_h = (kCcTopAreaDip - card_pad * 2.0f - row_gap) * 0.5f;
    // Cell order = macOS Sonoma: Wi-Fi | Bluetooth / 勿扰 | 夜览.
    const wchar_t* cell_labels[4] = {L"Wi-Fi", L"蓝牙", L"勿扰", L"夜览"};
    const int cell_kinds[4] = {kRadioWiFi, kRadioBluetooth, kCcCellDnd, kCcCellNightLight};

    // Radio presence: the sidecar answers for wifi/bt, the registry for the
    // system switches.
    const SidecarRadioState* wifi_radio = FindRadio(sidecar_state_, kRadioWiFi);
    const SidecarRadioState* bt_radio = FindRadio(sidecar_state_, kRadioBluetooth);
    const bool sidecar_ok = sidecar_ != nullptr && sidecar_->running();

    for (int i = 0; i < 4; ++i) {
        ConnCell& cell = cells_[i];
        const int col = i % 2;
        const int row = i / 2;
        cell.hit = Rect{conn_left + card_pad + col * (cell_w + col_gap),
                        conn_top + card_pad + row * (cell_h + row_gap), cell_w, cell_h};
        cell.kind = cell_kinds[i];
        cell.label = cell_labels[i];
        cell.button = Rect{cell.hit.x + 7.0f, cell.hit.CenterY() - kCcConnButtonDip * 0.5f,
                           kCcConnButtonDip, kCcConnButtonDip};
        // Name + state lines, centred on the button's vertical span (the macOS
        // cell reads [button][name/state] as one block). Rect is {x, y, w, h}:
        // widths are computed, never absolute edges.
        const float text_left = cell.button.right() + 6.0f;
        const float text_w = cell.hit.right() - 6.0f - text_left;
        cell.name = Rect{text_left, cell.hit.CenterY() - 15.5f, text_w, 16.0f};
        cell.state = Rect{text_left, cell.name.bottom() + 1.0f, text_w, 14.0f};
        switch (cell.kind) {
            case kRadioWiFi:
                cell.present = sidecar_ok && wifi_radio != nullptr;
                cell.on = wifi_radio != nullptr && wifi_radio->state == 1;
                cell.state_text = cell.present ? (cell.on ? L"开" : L"关") : L"不可用";
                break;
            case kRadioBluetooth:
                cell.present = sidecar_ok && bt_radio != nullptr;
                cell.on = bt_radio != nullptr && bt_radio->state == 1;
                cell.state_text = cell.present ? (cell.on ? L"开" : L"关") : L"不可用";
                break;
            case kCcCellDnd:
                cell.present = true;  // the registry write is always available
                cell.on = dnd_on_;
                cell.state_text = cell.on ? L"开" : L"关";
                break;
            case kCcCellNightLight:
                cell.present = night_state_ != 0;
                cell.on = night_state_ > 0;
                cell.state_text = cell.present ? (cell.on ? L"开" : L"关") : L"不可用";
                break;
            default:
                break;
        }
    }

    // Volume tile (right of the connection card): CcSliderTile Width=46,
    // Padding=4,10; slider 26x118 with the 20-wide visible capsule.
    const float tile_x = conn_right + 6.0f;
    const float tile_w = kCcVolumeTileWidthDip;
    const float track_cx = tile_x + tile_w * 0.5f;
    volume_track_ = Rect{track_cx - kCcSliderTrackWidthDip * 0.5f, conn_top + 10.0f,
                         kCcSliderTrackWidthDip, kCcSliderLengthDip};
    volume_hit_ = Rect{track_cx - kCcSliderWidthDip * 0.5f, volume_track_.y,
                       kCcSliderWidthDip, kCcSliderLengthDip};
    volume_button_ = Rect{track_cx - 8.0f, volume_track_.bottom() + 6.0f, 16.0f, 15.0f};
    volume_pct_ = Rect{tile_x, volume_button_.bottom() + 2.0f, tile_w, 13.0f};

    // Brightness row: full-width group card with a horizontal capsule track.
    const float row_top = conn_bottom + kCcSectionGapDip;
    brightness_glyph_ = Rect{pad + 6.0f, row_top + (kCcBrightnessRowDip - 14.0f) * 0.5f,
                             14.0f, 14.0f};
    brightness_track_ =
        Rect{brightness_glyph_.right() + 12.0f,
             row_top + (kCcBrightnessRowDip - kCcSliderTrackWidthDip) * 0.5f,
             width - pad * 2.0f - (brightness_glyph_.right() + 12.0f - pad) - 52.0f,
             kCcSliderTrackWidthDip};
    brightness_hit_ = Rect{brightness_track_.x - 3.0f, row_top, brightness_track_.w + 6.0f,
                           kCcBrightnessRowDip};
    brightness_pct_ = Rect{brightness_track_.right() + 8.0f, row_top,
                           width - pad - (brightness_track_.right() + 8.0f),
                           kCcBrightnessRowDip};

    // Error strip.
    error_strip_ =
        Rect{pad, row_top + kCcBrightnessRowDip + kCcSectionGapDip, width - pad * 2.0f,
             kCcErrorStripDip};

    // ---- draw -------------------------------------------------------------------
    const bool painted = surface_.Paint(
        scale_ * 96.0f,
        [&](ID2D1DeviceContext& dc, const platform::CompositionSurface::PaintInfo&) {
            render::IRenderBackend& be = *backend_;
            be.BeginFrame(dc, scale_ * 96.0f);

            // Root plate: MacOS.xaml glass gradient + popup tint + hairline.
            D2D1_GRADIENT_STOP stops[3];
            stops[0].position = 0.0f;
            stops[0].color = {kPlateTopColor[0], kPlateTopColor[1], kPlateTopColor[2],
                              kPlateTopColor[3]};
            stops[1].position = 0.5f;
            stops[1].color = {kPlateMidColor[0], kPlateMidColor[1], kPlateMidColor[2],
                              kPlateMidColor[3]};
            stops[2].position = 1.0f;
            stops[2].color = {kPlateBotColor[0], kPlateBotColor[1], kPlateBotColor[2],
                              kPlateBotColor[3]};
            ID2D1GradientStopCollection* collection = nullptr;
            if (SUCCEEDED(dc.CreateGradientStopCollection(stops, 3, D2D1_GAMMA_2_2,
                                                          D2D1_EXTEND_MODE_CLAMP,
                                                          &collection)) &&
                collection != nullptr) {
                const D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES props{
                    D2D1_POINT_2F{0.0f, 0.0f}, D2D1_POINT_2F{0.0f, height}};
                ID2D1LinearGradientBrush* brush = nullptr;
                if (SUCCEEDED(dc.CreateLinearGradientBrush(props, collection, &brush)) &&
                    brush != nullptr) {
                    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
                        D2D1::RectF(0.0f, 0.0f, width, height), kCcPanelRadiusDip,
                        kCcPanelRadiusDip);
                    dc.FillRoundedRectangle(rr, brush);
                    brush->Release();
                }
                collection->Release();
            }
            const render::RectF root{0.0f, 0.0f, width, height};
            be.FillRoundedRect(root, kCcPanelRadiusDip, Color(kPopupTint));
            be.StrokeRoundedRect(root, kCcPanelRadiusDip, Color(kStroke), 1.0f);

            const render::ColorF card_fill = Color(kCardFill);
            const render::ColorF card_border = Color(kCardBorder);
            const render::ColorF text = Color(kText);
            const render::ColorF text_faint = Color(kTextFaint);
            const render::ColorF accent = Color(kAccent);
            const render::ColorF button_off = Color(kButtonOff);
            const render::ColorF track = Color(kTrack);
            const render::ColorF text_pct = Color(kTextPct);
            const render::ColorF glyph_dim{1.0f, 1.0f, 1.0f, 0.45f};

            // ---- connection card + 2x2 cells -----------------------------------
            be.FillRoundedRect(
                render::RectF{conn_left, conn_top, conn_right, conn_bottom},
                kCcCardRadiusDip, card_fill);
            be.StrokeRoundedRect(
                render::RectF{conn_left, conn_top, conn_right, conn_bottom},
                kCcCardRadiusDip, card_border, 1.0f);
            for (int i = 0; i < 4; ++i) {
                const ConnCell& cell = cells_[i];
                const render::ColorF& dim = cell.present ? text : glyph_dim;
                // Round button: accent when on, #3DFFFFFF when off (CcConnToggle).
                const render::RectF button{cell.button.x, cell.button.y,
                                           cell.button.right(), cell.button.bottom()};
                const render::ColorF& button_bg =
                    cell.on && cell.present ? accent : button_off;
                be.FillRoundedRect(button, kCcConnButtonDip * 0.5f, button_bg);
                const render::RectF glyph_slot{cell.button.x + 6.5f, cell.button.y + 6.5f,
                                               cell.button.right() - 6.5f,
                                               cell.button.bottom() - 6.5f};
                switch (cell.kind) {
                    case kRadioWiFi:
                        DrawWifiGlyph(be, glyph_slot, cell.on ? 4 : 0, dim);
                        break;
                    case kRadioBluetooth:
                        DrawBtRune(be, glyph_slot, dim);
                        break;
                    case kCcCellDnd:
                        DrawMoonGlyph(be, glyph_slot, dim, button_bg);
                        break;
                    case kCcCellNightLight:
                        DrawNightGlyph(be, glyph_slot, dim, button_bg);
                        break;
                    default:
                        break;
                }
                be.DrawLabel(cell.label.c_str(),
                             render::RectF{cell.name.x, cell.name.y, cell.name.right(),
                                           cell.name.bottom()},
                             11.0f, dim, render::TextAlign::Left);
                be.DrawLabel(cell.state_text.c_str(),
                             render::RectF{cell.state.x, cell.state.y, cell.state.right(),
                                           cell.state.bottom()},
                             9.5f, cell.present ? text_faint : glyph_dim,
                             render::TextAlign::Left);
            }

            // ---- volume tile -----------------------------------------------------
            const render::RectF tile{tile_x, conn_top, tile_x + tile_w, conn_bottom};
            be.FillRoundedRect(tile, kCcTileRadiusDip, card_fill);
            be.StrokeRoundedRect(tile, kCcTileRadiusDip, card_border, 1.0f);
            // Track capsule (MacControlFillHover) + accent fill from the bottom.
            be.FillRoundedRect(render::RectF{volume_track_.x, volume_track_.y,
                                             volume_track_.right(),
                                             volume_track_.bottom()},
                               kCcSliderTrackWidthDip * 0.5f, track);
            const float vol_level = master_.muted ? 0.0f : master_.level;
            const float fill_h = volume_track_.h * (vol_level < 0.0f ? 0.0f : vol_level);
            if (fill_h > 0.5f) {
                be.FillRoundedRect(render::RectF{volume_track_.x,
                                                 volume_track_.bottom() - fill_h,
                                                 volume_track_.right(),
                                                 volume_track_.bottom()},
                                   kCcSliderTrackWidthDip * 0.5f, accent);
            }
            // The fill edge IS the handle (Beautiful CcMacThumb: 26x4, visually
            // transparent - macOS's no-handle capsule). Draw the 4 DIP edge band
            // slightly brighter so the grip reads without a separate knob.
            be.FillRoundedRect(
                render::RectF{volume_hit_.x,
                              volume_track_.bottom() - fill_h - kCcThumbDip * 0.5f,
                              volume_hit_.right(),
                              volume_track_.bottom() - fill_h + kCcThumbDip * 0.5f},
                kCcThumbDip * 0.5f, text_pct);
            DrawSpeakerGlyph(be,
                             render::RectF{volume_button_.x, volume_button_.y,
                                           volume_button_.right(), volume_button_.bottom()},
                             master_.muted ? glyph_dim : text, master_.muted);
            be.DrawLabel(PercentText(master_.level < 0.0f ? 0.0f : master_.level).c_str(),
                         render::RectF{volume_pct_.x, volume_pct_.y, volume_pct_.right(),
                                       volume_pct_.bottom()},
                         10.5f, text_pct, render::TextAlign::Center);

            // ---- brightness row ---------------------------------------------------
            const render::RectF row{pad, row_top, width - pad,
                                    row_top + kCcBrightnessRowDip};
            be.FillRoundedRect(row, kCcGroupRadiusDip, card_fill);
            be.StrokeRoundedRect(row, kCcGroupRadiusDip, card_border, 1.0f);
            DrawSunGlyph(be,
                         render::RectF{brightness_glyph_.x, brightness_glyph_.y,
                                       brightness_glyph_.right(),
                                       brightness_glyph_.bottom()},
                         brightness_supported_ ? text : glyph_dim);
            be.FillRoundedRect(render::RectF{brightness_track_.x, brightness_track_.y,
                                             brightness_track_.right(),
                                             brightness_track_.bottom()},
                               kCcSliderTrackWidthDip * 0.5f, track);
            if (brightness_supported_) {
                const float bright = brightness_level_ < 0.0f ? 0.0f : brightness_level_;
                const float fill_w = brightness_track_.w * bright;
                if (fill_w > 0.5f) {
                    be.FillRoundedRect(render::RectF{brightness_track_.x,
                                                     brightness_track_.y,
                                                     brightness_track_.x + fill_w,
                                                     brightness_track_.bottom()},
                                       kCcSliderTrackWidthDip * 0.5f, accent);
                }
                // Thumb 4x26 at the fill edge (the horizontal CcMacThumbH).
                be.FillRoundedRect(
                    render::RectF{brightness_track_.x + fill_w - kCcThumbDip * 0.5f,
                                  row_top + (kCcBrightnessRowDip - kCcSliderWidthDip) * 0.5f,
                                  brightness_track_.x + fill_w + kCcThumbDip * 0.5f,
                                  row_top + (kCcBrightnessRowDip + kCcSliderWidthDip) * 0.5f},
                    kCcThumbDip * 0.5f, text_pct);
                be.DrawLabel(PercentText(bright).c_str(),
                             render::RectF{brightness_pct_.x, brightness_pct_.y,
                                           brightness_pct_.right(),
                                           brightness_pct_.bottom()},
                             10.5f, text_pct, render::TextAlign::Right);
            } else {
                be.DrawLabel(L"亮度不可用（不支持 DDC/WMI）",
                             render::RectF{brightness_track_.x, brightness_track_.y,
                                           brightness_track_.right(),
                                           brightness_track_.bottom()},
                             10.5f, glyph_dim, render::TextAlign::Center);
            }

            // ---- error strip -------------------------------------------------------
            if (!last_error_.message.empty()) {
                be.DrawLabel(last_error_.message.c_str(),
                             render::RectF{error_strip_.x, error_strip_.y,
                                           error_strip_.right(), error_strip_.bottom()},
                             10.5f, Color(kErrorText), render::TextAlign::Left);
            }

            be.EndFrame();
        });

    if (!painted) {
        DF_WARN(kLogModule, "panel paint failed");
        return;
    }
    surface_.Commit();
}

}  // namespace dockforge::finderbar
