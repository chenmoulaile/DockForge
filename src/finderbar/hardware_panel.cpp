// DockForge - hardware control popup panel implementation (T-068, C-23).
#include "finderbar/hardware_panel.h"

#include "core/log.h"
#include "core/paths.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <windowsx.h>

namespace dockforge::finderbar {

namespace {

const wchar_t kPanelClass[] = L"DockForgeHardwarePanel";
const char kLogModule[] = "hwpanel";

// DIP layout constants (scaled by DPI on open).
constexpr int kWidthDip = 340;
constexpr int kPadDip = 14;
constexpr int kSectionGapDip = 12;
constexpr int kMediaTitleDip = 22;
constexpr int kMediaArtistDip = 16;
constexpr int kMediaButtonsDip = 36;
constexpr int kMediaClockDip = 16;
constexpr int kSliderRowDip = 34;
constexpr int kSwitchRowDip = 32;
constexpr int kStatsRowDip = 34;
constexpr int kErrorStripDip = 26;
constexpr int kMaxSessionRows = 4;

// Palette: dark panel like the folder panel, accent-styled bars.
constexpr COLORREF kBg = RGB(32, 32, 36);
constexpr COLORREF kBorder = RGB(70, 70, 76);
constexpr COLORREF kText = RGB(232, 232, 238);
constexpr COLORREF kTextDim = RGB(150, 150, 158);
constexpr COLORREF kAccent = RGB(0, 120, 212);
constexpr COLORREF kTrack = RGB(70, 70, 76);
constexpr COLORREF kErrorText = RGB(255, 99, 99);
constexpr COLORREF kSwitchOff = RGB(90, 90, 96);

int Dip(float scale, int dip) { return static_cast<int>(dip * scale); }

std::wstring TruncateMiddle(const std::wstring& text, std::size_t max_chars) {
    if (text.size() <= max_chars) return text;
    if (max_chars < 4) return text.substr(0, max_chars);
    const std::size_t keep = max_chars - 3;
    return text.substr(0, keep / 2) + L"…" + text.substr(text.size() - (keep - keep / 2));
}

std::wstring GbText(unsigned long long bytes) {
    const double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    wchar_t buf[32];
    swprintf(buf, 32, L"%.1f GB", gb);
    return buf;
}

std::wstring PercentText(float percent) {
    wchar_t buf[16];
    swprintf(buf, 16, L"%d%%", static_cast<int>(percent + 0.5f));
    return buf;
}

std::wstring RadioName(int kind, const std::wstring& fallback) {
    if (!fallback.empty()) return fallback;
    switch (kind) {
        case kRadioWiFi: return L"WLAN";
        case kRadioBluetooth: return L"蓝牙";
        case kRadioMobileBroadband: return L"移动宽带";
        default: return L"无线电";
    }
}

}  // namespace

HardwarePanel::HardwarePanel(AudioHardware* audio, MediaSidecarBridge* sidecar,
                             std::function<SystemStatsUpdated()> on_stats)
    : audio_(audio), sidecar_(sidecar), on_stats_(std::move(on_stats)) {}

HardwarePanel::~HardwarePanel() { Destroy(); }

bool HardwarePanel::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
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
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPanelClass,
                                L"DockForgeHardwarePanel", WS_POPUP, 0, 0, 1, 1,
                                nullptr, nullptr, instance, this);
        if (hwnd_ == nullptr) {
            DF_ERROR(kLogModule, "CreateWindowExW failed %lu", GetLastError());
            return false;
        }
    }
    return true;
}

void HardwarePanel::Destroy() {
    if (hwnd_ == nullptr) return;
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    if (font_) { DeleteObject(font_); font_ = nullptr; }
    if (font_bold_) { DeleteObject(font_); font_bold_ = nullptr; }
    if (font_small_) { DeleteObject(font_small_); font_small_ = nullptr; }
}

void HardwarePanel::Open() {
    if (hwnd_ == nullptr) return;
    // DPI of the primary monitor at open time (per-open, like FolderPanel).
    const HDC screen = GetDC(nullptr);
    dpi_ = static_cast<float>(GetDeviceCaps(screen, LOGPIXELSX));
    ReleaseDC(nullptr, screen);
    scale_ = dpi_ / 96.0f;

    if (font_ == nullptr) {
        const int body_px = -MulDiv(12, static_cast<int>(dpi_), 72);
        const int small_px = -MulDiv(10, static_cast<int>(dpi_), 72);
        const int title_px = -MulDiv(13, static_cast<int>(dpi_), 72);
        font_ = CreateFontW(body_px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        font_bold_ = CreateFontW(title_px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        font_small_ = CreateFontW(small_px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }

    RefreshSessions();
    master_ = audio_ ? audio_->GetEndpoint(false) : AudioEndpointState{};
    mic_ = audio_ ? audio_->GetEndpoint(true) : AudioEndpointState{};
    last_error_ = HardwareControlError::None();
    was_activated_ = false;
    Relayout();

    // Top of the primary work area, horizontally centred: where the Finder Bar
    // will hang it from (M2). Not a permanent occupant of anything.
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + ((work.right - work.left) - width_px_) / 2;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, work.top, width_px_, height_px_,
                 SWP_SHOWWINDOW);
    if (!SetForegroundWindow(hwnd_)) {
        DF_DEBUG(kLogModule, "SetForegroundWindow refused on open (probe path?)");
    }
    DF_INFO(kLogModule, "panel open dpi=%.0f sessions=%d", static_cast<double>(dpi_),
            SessionRowCount());
    RepaintNow();
}

void HardwarePanel::Close() {
    if (hwnd_ == nullptr) return;
    ShowWindow(hwnd_, SW_HIDE);
    DF_INFO(kLogModule, "panel closed");
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

void HardwarePanel::Relayout() {
    const float s = scale_;
    const int pad = Dip(s, kPadDip);
    const int w = Dip(s, kWidthDip);
    int y = 0;
    const int inner_x = pad;
    const int inner_w = w - pad * 2;
    width_px_ = w;

    // ---- media ---------------------------------------------------------------
    media_title_area_ = {inner_x, y + 2, inner_x + inner_w, y + Dip(s, kMediaTitleDip)};
    y += Dip(s, kMediaTitleDip);
    media_artist_area_ = {inner_x, y, inner_x + inner_w, y + Dip(s, kMediaArtistDip)};
    y += Dip(s, kMediaArtistDip);
    const int btn_h = Dip(s, kMediaButtonsDip);
    const int btn_w = Dip(s, 56);
    const int btn_gap = Dip(s, 10);
    const int btn_row_w = btn_w * 3 + btn_gap * 2;
    const int btn_x0 = inner_x + (inner_w - btn_row_w) / 2;
    media_buttons_.prev = {btn_x0, y, btn_x0 + btn_w, y + btn_h};
    media_buttons_.play = {btn_x0 + btn_w + btn_gap, y, btn_x0 + btn_w * 2 + btn_gap,
                           y + btn_h};
    media_buttons_.next = {btn_x0 + (btn_w + btn_gap) * 2, y,
                           btn_x0 + (btn_w + btn_gap) * 2 + btn_w, y + btn_h};
    y += btn_h;
    media_clock_area_ = {inner_x, y, inner_x + inner_w, y + Dip(s, kMediaClockDip)};
    y += Dip(s, kMediaClockDip);
    y += Dip(s, kSectionGapDip);

    // ---- volume sliders --------------------------------------------------------
    sliders_.clear();
    const int slider_h = Dip(s, kSliderRowDip);
    const int track_x = inner_x + Dip(s, 76);
    const int track_w = inner_w - Dip(s, 76) - Dip(s, 46);
    auto add_slider = [&](SliderRow::Kind kind, const std::wstring& name, float level,
                          bool muted, int session_index) {
        SliderRow row;
        row.kind = kind;
        row.session_index = session_index;
        row.name = name;
        row.level = level;
        row.muted = muted;
        row.label = {inner_x, y, track_x - Dip(s, 6), y + slider_h};
        row.track = {track_x, y + slider_h / 2 - Dip(s, 5), track_x + track_w,
                     y + slider_h / 2 + Dip(s, 5)};
        row.mute = {track_x + track_w + Dip(s, 6), y, inner_x + inner_w, y + slider_h};
        sliders_.push_back(row);
        y += slider_h;
    };
    add_slider(SliderRow::kMaster, L"主音量", master_.level, master_.muted, -1);
    add_slider(SliderRow::kMic, L"麦克风", mic_.present ? mic_.level : 0.0f,
               mic_.muted, -1);
    for (int i = 0; i < static_cast<int>(sessions_.size()); ++i) {
        add_slider(SliderRow::kSession,
                   TruncateMiddle(sessions_[i].name, 10) + L" " +
                       FormatLevelPercent(sessions_[i].level),
                   sessions_[i].level, sessions_[i].muted, i);
    }
    y += Dip(s, kSectionGapDip);

    // ---- radio switches ----------------------------------------------------------
    const int switch_h = Dip(s, kSwitchRowDip);
    auto place_switch = [&](SwitchRow& row, int kind, const SidecarRadioState* radio) {
        row.radio_kind = kind;
        row.present = radio != nullptr;
        row.on = radio && radio->state == 1;
        row.name = RadioName(kind, radio ? radio->name : L"");
        row.label = {inner_x, y, inner_x + inner_w - Dip(s, 64), y + switch_h};
        const int box_w = Dip(s, 44);
        row.box = {inner_x + inner_w - box_w, y + (switch_h - Dip(s, 22)) / 2,
                   inner_x + inner_w, y + (switch_h - Dip(s, 22)) / 2 + Dip(s, 22)};
        y += switch_h;
    };
    place_switch(wifi_switch_, kRadioWiFi, FindRadio(sidecar_state_, kRadioWiFi));
    place_switch(bt_switch_, kRadioBluetooth, FindRadio(sidecar_state_, kRadioBluetooth));
    y += Dip(s, kSectionGapDip);

    // ---- stats bars -----------------------------------------------------------------
    const int stats_h = Dip(s, kStatsRowDip);
    auto place_stats = [&](StatsRow& row, const std::wstring& label, float pct,
                           const std::wstring& detail) {
        row.label = label;
        row.percent = pct;
        row.detail = detail;
        row.bar = {inner_x + Dip(s, 60), y + stats_h / 2 - Dip(s, 5),
                   inner_x + inner_w - Dip(s, 64), y + stats_h / 2 + Dip(s, 5)};
        y += stats_h;
    };
    place_stats(cpu_bar_, L"CPU", stats_.cpu_percent, PercentText(stats_.cpu_percent));
    place_stats(mem_bar_, L"内存", stats_.memory_percent,
                GbText(stats_.memory_used_bytes) + L" / " +
                    GbText(stats_.memory_total_bytes));
    y += Dip(s, kSectionGapDip);

    // ---- error strip (always laid out; painted only with an error) ---------------------
    error_strip_ = {inner_x, y, inner_x + inner_w, y + Dip(s, kErrorStripDip)};
    y += Dip(s, kErrorStripDip) + Dip(s, 6);

    height_px_ = y;
}

void HardwarePanel::RepaintNow() {
    if (hwnd_ == nullptr) return;
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void HardwarePanel::OnAudioChanged() {
    if (!visible()) return;
    master_ = audio_ ? audio_->GetEndpoint(false) : AudioEndpointState{};
    mic_ = audio_ ? audio_->GetEndpoint(true) : AudioEndpointState{};
    Relayout();
    RepaintNow();
}

void HardwarePanel::OnSidecarState(const SidecarState& state) {
    sidecar_state_ = state;
    if (!visible()) return;
    Relayout();
    RepaintNow();
}

HardwareControlError HardwarePanel::SetMasterVolume(float level) {
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    auto err = audio_ ? audio_->SetEndpointLevel(false, level)
                      : HardwareControlError::Failed(L"主音量", L"音频不可用", E_FAIL);
    last_error_ = err;
    master_.level = err.ok() ? level : master_.level;
    if (visible()) {
        Relayout();
        RepaintNow();
    }
    return err;
}

HardwareControlError HardwarePanel::ToggleMasterMute() {
    auto err = audio_ ? audio_->SetEndpointMute(false, !master_.muted)
                      : HardwareControlError::Failed(L"主音量", L"音频不可用", E_FAIL);
    last_error_ = err;
    return err;
}

HardwareControlError HardwarePanel::SetMicrophoneMute(bool mute) {
    auto err = audio_ ? audio_->SetEndpointMute(true, mute)
                      : HardwareControlError::Failed(L"麦克风", L"音频不可用", E_FAIL);
    last_error_ = err;
    return err;
}

HardwareControlError HardwarePanel::MediaCommand(const std::string& action) {
    if (!sidecar_ || !sidecar_->running()) {
        return (last_error_ = HardwareControlError::Failed(
                    L"媒体控制", L"媒体控制服务不可用（sidecar 未运行）", E_FAIL));
    }
    if (!sidecar_->SendCommand("media " + action)) {
        return (last_error_ = HardwareControlError::Failed(
                    L"媒体控制", L"发送媒体命令失败", E_FAIL));
    }
    // The sidecar answers asynchronously; wait (bounded) for a non-push line
    // so a rejection lands in the error strip instead of racing past it. A
    // poll answer arriving meanwhile refreshes the rows for free.
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
        DF_WARN("hwpanel", "media '%s' rejected: %s", action.c_str(),
                core::WideToUtf8(error).c_str());
    } else {
        last_error_ = HardwareControlError::None();
    }
    if (visible()) RepaintNow();
    return last_error_;
}

HardwareControlError HardwarePanel::SetRadio(int kind, bool on) {
    if (!sidecar_ || !sidecar_->running()) {
        return (last_error_ = HardwareControlError::Failed(
                    L"无线电", L"无线电服务不可用（sidecar 未运行）", E_FAIL));
    }
    std::string cmd = "radio ";
    cmd += std::to_string(kind);
    cmd += on ? " 1" : " 0";
    if (!sidecar_->SendCommand(cmd)) {
        return (last_error_ = HardwareControlError::Failed(
                    L"无线电", L"发送无线电命令失败", E_FAIL));
    }
    // Radios flip in ~1s; a poll right after settles the row faster than the
    // next tick.
    SidecarState state = sidecar_->PollSync(2500);
    if (state.valid) OnSidecarState(state);
    const SidecarRadioState* radio = FindRadio(sidecar_state_, kind);
    if (!radio || (radio->state != 1) == on) {
        return (last_error_ = HardwareControlError::Failed(
                    L"无线电", L"切换无线电被系统拒绝", E_FAIL));
    }
    return (last_error_ = HardwareControlError::None());
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
        if (visible()) {
            Relayout();
            RepaintNow();
        }
    }
    return err;
}

void HardwarePanel::Tick(double delta_ms) {
    if (!visible()) return;
    stats_accumulator_ms_ += delta_ms;
    if (stats_accumulator_ms_ < 2000.0) return;  // C-23 ceiling: 0.5Hz
    stats_accumulator_ms_ = 0.0;

    if (on_stats_) stats_ = on_stats_();
    RefreshSessions();
    master_ = audio_ ? audio_->GetEndpoint(false) : AudioEndpointState{};
    mic_ = audio_ ? audio_->GetEndpoint(true) : AudioEndpointState{};

    if (sidecar_ && sidecar_->running()) {
        for (const auto& line : sidecar_->DrainLines()) {
            SidecarState parsed;
            bool is_push = false;
            std::string push_kind;
            std::wstring error;
            if (ParseSidecarLine(line, &parsed, &is_push, &push_kind, &error) &&
                parsed.valid) {
                OnSidecarState(parsed);
            }
        }
        SidecarState state = sidecar_->PollSync(600);
        if (state.valid) sidecar_state_ = state;
    }
    Relayout();
    RepaintNow();
}

float HardwarePanel::SliderValueAt(const SliderRow& slider, int x_px) const {
    const int track_w = slider.track.w - slider.track.x;
    if (track_w <= 0) return 0.0f;
    const int rel = x_px - slider.track.x;
    if (rel <= 0) return 0.0f;
    if (rel >= track_w) return 1.0f;
    return static_cast<float>(rel) / static_cast<float>(track_w);
}

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
    return j.dump();
}

// ---- window procedure --------------------------------------------------------

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
            return 1;  // WM_PAINT owns every pixel (double buffered)

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
            // FolderPanel's dismissal rule: a loss of activation AFTER the
            // panel once held activation closes it; the transient deactivate
            // during the foreground grant does not.
            if (LOWORD(wparam) == WA_INACTIVE && was_activated_ && visible()) {
                Close();
            }
            if (LOWORD(wparam) != WA_INACTIVE) {
                was_activated_ = true;
            }
            return 0;

        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                Close();
                return 0;
            }
            return 0;

        case WM_LBUTTONDOWN: {
            const int x = GET_X_LPARAM(lparam);
            const int y = GET_Y_LPARAM(lparam);
            const auto in = [x, y](const Region& r) {
                return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
            };
            // Sliders first: a press on a track starts a drag AND applies.
            dragging_slider_ = -1;
            for (int i = 0; i < static_cast<int>(sliders_.size()); ++i) {
                const SliderRow& s = sliders_[i];
                const bool on_row = x >= s.label.x && x < s.mute.x + s.mute.w &&
                                    y >= s.track.y - Dip(scale_, 16) &&
                                    y < s.track.y + s.track.h + Dip(scale_, 16);
                if (on_row) {
                    if (in(s.mute)) {
                        if (s.kind == SliderRow::kMaster) {
                            ToggleMasterMute();
                        } else if (s.kind == SliderRow::kMic) {
                            SetMicrophoneMute(!mic_.muted);
                        }
                        RepaintNow();
                        return 0;
                    }
                    if (s.kind == SliderRow::kSession && !s.track.w) {
                        // track_w == 0 can't happen (laid out above); keep the
                        // guard for robustness.
                        return 0;
                    }
                    const float level = SliderValueAt(s, x);
                    dragging_slider_ = i;
                    SetCapture(hwnd_);
                    if (s.kind == SliderRow::kSession) {
                        SetSessionVolume(s.session_index, level);
                    } else {
                        SetMasterVolume(level);
                    }
                    RepaintNow();
                    return 0;
                }
            }
            // Media buttons.
            if (in(media_buttons_.prev)) {
                MediaCommand("prev");
                return 0;
            }
            if (in(media_buttons_.play)) {
                MediaCommand(sidecar_state_.media.playing ? "pause" : "play");
                return 0;
            }
            if (in(media_buttons_.next)) {
                MediaCommand("next");
                return 0;
            }
            // Radio switches.
            if (in(wifi_switch_.box) && wifi_switch_.present) {
                SetRadio(kRadioWiFi, !wifi_switch_.on);
                return 0;
            }
            if (in(bt_switch_.box) && bt_switch_.present) {
                SetRadio(kRadioBluetooth, !bt_switch_.on);
                return 0;
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (dragging_slider_ < 0 ||
                dragging_slider_ >= static_cast<int>(sliders_.size())) {
                return 0;
            }
            const int x = GET_X_LPARAM(lparam);
            const SliderRow& s = sliders_[dragging_slider_];
            const float level = SliderValueAt(s, x);
            if (s.kind == SliderRow::kSession) {
                SetSessionVolume(s.session_index, level);
            } else {
                SetMasterVolume(level);
            }
            return 0;
        }

        case WM_LBUTTONUP:
            if (dragging_slider_ >= 0) {
                ReleaseCapture();
                dragging_slider_ = -1;
                // Session rows show their level in the name: relabel after a
                // finished drag.
                RefreshSessions();
                Relayout();
                RepaintNow();
            }
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            Paint(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY:
            hwnd_ = nullptr;
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

void HardwarePanel::Paint(HDC dc) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int w = rc.right - rc.left;

    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, rc.bottom);
    HBITMAP old_bmp = static_cast<HBITMAP>(SelectObject(mem, bmp));

    RECT full = rc;
    const HBRUSH bg = CreateSolidBrush(kBg);
    FillRect(mem, &full, bg);
    DeleteObject(bg);

    HPEN border_pen = CreatePen(PS_SOLID, 1, kBorder);
    HGDIOBJ old_pen = SelectObject(mem, border_pen);
    HGDIOBJ old_brush = SelectObject(mem, GetStockObject(NULL_BRUSH));
    Rectangle(mem, 0, 0, w, rc.bottom);
    SelectObject(mem, old_pen);
    SelectObject(mem, old_brush);
    DeleteObject(border_pen);

    SetBkMode(mem, TRANSPARENT);
    // Region is {x, y, w, h}; DrawTextW wants {left, top, right, bottom}. The
    // conversion is by VALUE on purpose: a reinterpret_cast would silently read
    // w/h as right/bottom.
    const auto rect_of = [](const Region& r) -> RECT {
        return RECT{r.x, r.y, r.x + r.w, r.y + r.h};
    };

    // ---- media --------------------------------------------------------------
    const HFONT old_font = static_cast<HFONT>(SelectObject(mem, font_bold_));
    SetTextColor(mem, kText);
    const bool has_media = sidecar_state_.media.has_session;
    const std::wstring title =
        has_media ? (sidecar_state_.media.title.empty() ? L"(无标题)"
                                                        : sidecar_state_.media.title)
                  : L"无媒体会话";
    RECT title_rc = rect_of(media_title_area_);
    DrawTextW(mem, title.c_str(), static_cast<int>(title.size()), &title_rc,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(mem, font_small_);
    SetTextColor(mem, kTextDim);
    const std::wstring artist = has_media ? sidecar_state_.media.artist : L"";
    RECT artist_rc = rect_of(media_artist_area_);
    DrawTextW(mem, artist.c_str(), static_cast<int>(artist.size()), &artist_rc,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(mem, old_font);

    // Media buttons: rounded rects with glyph text.
    auto draw_button = [&](const Region& r, const wchar_t* glyph, bool enabled) {
        const HBRUSH face = CreateSolidBrush(enabled ? kAccent : kTrack);
        const RECT rr{r.x, r.y, r.x + r.w, r.y + r.h};
        FillRect(mem, &rr, face);
        DeleteObject(face);
        SetTextColor(mem, kText);
        DrawTextW(mem, glyph, -1, const_cast<RECT*>(&rr),
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    };
    draw_button(media_buttons_.prev, L"⏮", true);
    draw_button(media_buttons_.play, sidecar_state_.media.playing ? L"⏸" : L"▶",
                true);
    draw_button(media_buttons_.next, L"⏭", true);

    SelectObject(mem, font_small_);
    SetTextColor(mem, kTextDim);
    const std::wstring clock =
        has_media ? FormatMediaClock(sidecar_state_.media.position_ticks) + L" / " +
                        FormatMediaClock(sidecar_state_.media.duration_ticks)
                  : L"--:-- / --:--";
    RECT clock_rc = rect_of(media_clock_area_);
    DrawTextW(mem, clock.c_str(), static_cast<int>(clock.size()), &clock_rc,
              DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    SelectObject(mem, old_font);

    // ---- sliders ------------------------------------------------------------
    SelectObject(mem, font_);
    const HBRUSH track_brush = CreateSolidBrush(kTrack);
    const HBRUSH fill_brush = CreateSolidBrush(kAccent);
    for (const auto& s : sliders_) {
        SetTextColor(mem, s.muted ? kTextDim : kText);
        // Label: name (+ % for endpoints; session names carry their own).
        std::wstring label = s.name;
        if (s.kind != SliderRow::kSession) label += L" " + FormatLevelPercent(s.level);
        RECT label_rc = rect_of(s.label);
        DrawTextW(mem, label.c_str(), static_cast<int>(label.size()), &label_rc,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
        RECT track{s.track.x, s.track.y, s.track.x + s.track.w,
                   s.track.y + s.track.h};
        FillRect(mem, &track, track_brush);
        const int fill_w = static_cast<int>(static_cast<float>(s.track.w) *
                                            (s.muted ? 0.0f : s.level));
        RECT filled{s.track.x, s.track.y, s.track.x + fill_w, s.track.y + s.track.h};
        FillRect(mem, &filled, fill_brush);
        // Mute toggle on the right.
        SetTextColor(mem, s.muted ? kErrorText : kTextDim);
        const wchar_t* mute_glyph = s.muted ? L"🔇" : L"🔊";
        RECT mute_rc = rect_of(s.mute);
        DrawTextW(mem, mute_glyph, -1, &mute_rc,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    }

    // ---- radio switches --------------------------------------------------------
    auto draw_switch = [&](const SwitchRow& s) {
        SetTextColor(mem, s.present ? kText : kTextDim);
        std::wstring label = s.name;
        if (!s.present) label += L"（不可用）";
        RECT switch_label_rc = rect_of(s.label);
        DrawTextW(mem, label.c_str(), static_cast<int>(label.size()),
                  &switch_label_rc,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
        RECT box{s.box.x, s.box.y, s.box.x + s.box.w, s.box.y + s.box.h};
        const HBRUSH body = CreateSolidBrush(s.on ? kAccent : kSwitchOff);
        FillRect(mem, &box, body);
        DeleteObject(body);
        // Knob square at the active side.
        const int knob = s.box.h - 4;
        RECT knob_rc = s.on ? RECT{box.right - knob - 2, box.top + 2, box.right - 2,
                                   box.bottom - 2}
                            : RECT{box.left + 2, box.top + 2, box.left + 2 + knob,
                                   box.bottom - 2};
        const HBRUSH knob_brush = CreateSolidBrush(kText);
        FillRect(mem, &knob_rc, knob_brush);
        DeleteObject(knob_brush);
    };
    draw_switch(wifi_switch_);
    draw_switch(bt_switch_);

    // ---- stats bars ---------------------------------------------------------------
    const int pad_px = Dip(scale_, kPadDip);
    auto draw_stats = [&](const StatsRow& s) {
        SetTextColor(mem, kText);
        RECT label_rc{pad_px, s.bar.y - Dip(scale_, 12), s.bar.x - Dip(scale_, 6),
                      s.bar.y + s.bar.h + Dip(scale_, 12)};
        DrawTextW(mem, s.label.c_str(), static_cast<int>(s.label.size()), &label_rc,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        RECT bar{s.bar.x, s.bar.y, s.bar.x + s.bar.w, s.bar.y + s.bar.h};
        FillRect(mem, &bar, track_brush);
        const int fill_w = static_cast<int>(static_cast<float>(s.bar.w) *
                                            s.percent / 100.0f);
        RECT filled{s.bar.x, s.bar.y, s.bar.x + fill_w, s.bar.y + s.bar.h};
        FillRect(mem, &filled, fill_brush);
        SetTextColor(mem, kTextDim);
        RECT detail_rc{bar.right + Dip(scale_, 6), label_rc.top, rc.right - pad_px,
                       label_rc.bottom};
        DrawTextW(mem, s.detail.c_str(), static_cast<int>(s.detail.size()), &detail_rc,
                  DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
    };
    draw_stats(cpu_bar_);
    draw_stats(mem_bar_);
    DeleteObject(track_brush);
    DeleteObject(fill_brush);

    // ---- error strip ----------------------------------------------------------------
    if (!last_error_.message.empty()) {
        SelectObject(mem, font_small_);
        SetTextColor(mem, kErrorText);
        RECT err_rc = rect_of(error_strip_);
        DrawTextW(mem, last_error_.message.c_str(),
                  static_cast<int>(last_error_.message.size()), &err_rc,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
        SelectObject(mem, old_font);
    }

    // THE step that makes the double buffer visible: everything above drew
    // into `bmp` through `mem`; without this BitBlt the panel shows whatever
    // the screen already had (found by the T-068 probe: shots held the window
    // rect but Sublime's pixels).
    BitBlt(dc, 0, 0, w, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

}  // namespace dockforge::finderbar
