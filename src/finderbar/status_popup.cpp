// DockForge - the status detail popup implementation (see status_popup.h).
#include "finderbar/status_popup.h"

#include "core/log.h"
#include "finderbar/status_icons.h"

#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace dockforge::finderbar {
namespace {

constexpr wchar_t kPopupClass[] = L"DockForgeStatusPopup";
constexpr char kLogModule[] = "finderbar";

// Theme colours, the calendar_panel.h token set (Beautiful MacTokens.cs: dark
// WindowBackground #1E1E1E / light #F5F5F7, ControlFill 20% white / 10% black,
// TextPrimary #FFFFFF / #1D1D1F, secondaries 70%/40% on dark, 60%/30% on light;
// accent Apple systemBlue light #007AFF / dark #0A84FF). Values only - the plate
// and cards are drawn by this file's own D2D code (ASSETS.md).
struct ThemeColors {
    render::ColorF background{};
    render::ColorF border{};
    render::ColorF card{};
    render::ColorF text{};
    render::ColorF text_secondary{};
    render::ColorF text_tertiary{};
    render::ColorF accent{};
};

render::ColorF ColorFromHex(std::uint32_t rgb, float alpha) {
    return render::ColorF{static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                          static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                          static_cast<float>(rgb & 0xFF) / 255.0f, alpha};
}

ThemeColors ColorsFor(bool light) {
    ThemeColors c;
    if (light) {
        c.background = ColorFromHex(0xF5F5F7, 0.99f);
        c.border = ColorFromHex(0x000000, 0.10f);
        c.card = ColorFromHex(0x000000, 0.10f);
        c.text = ColorFromHex(0x1D1D1F, 1.0f);
        c.text_secondary = ColorFromHex(0x000000, 0.60f);
        c.text_tertiary = ColorFromHex(0x000000, 0.30f);
        c.accent = ColorFromHex(0x007AFF, 1.0f);
    } else {
        c.background = ColorFromHex(0x1E1E1E, 0.99f);
        c.border = ColorFromHex(0xFFFFFF, 0.20f);
        c.card = ColorFromHex(0xFFFFFF, 0.20f);
        c.text = ColorFromHex(0xFFFFFF, 1.0f);
        c.text_secondary = ColorFromHex(0xFFFFFF, 0.70f);
        c.text_tertiary = ColorFromHex(0xFFFFFF, 0.40f);
        c.accent = ColorFromHex(0x0A84FF, 1.0f);
    }
    return c;
}

bool SystemUsesLightTheme() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    auto guard = MakeGuard([&key] { RegCloseKey(key); });
    DWORD value = 1;
    DWORD size = sizeof(value);
    DWORD type = 0;
    if (RegQueryValueExW(key, L"SystemUsesLightTheme", nullptr, &type,
                         reinterpret_cast<LPBYTE>(&value), &size) != ERROR_SUCCESS ||
        type != REG_DWORD) {
        return false;
    }
    return value != 0;
}

std::wstring SignalText(int bars) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"\u4FE1\u53F7 %d/4 \u683C", bars);  // 信号 N/4 格
    return buffer;
}

}  // namespace

// ---- layout -------------------------------------------------------------------

float StatusPopupHeightFor(bool battery_present, bool network_shown, bool ime_shown) {
    // Root margin top + sections + gaps + root margin bottom. A network section
    // carries one extra detail row when connected (SSID line); the simple "无网络"
    // card does not, but the height keeps one slot so the card never jumps while
    // the link flaps - a stable card beats a minimal one for something that opens
    // under the pointer.
    float sections = 0.0f;
    if (battery_present) sections += kStatusPopupSectionMinDip + kStatusPopupSectionRowDip;
    if (network_shown) sections += kStatusPopupSectionMinDip + kStatusPopupSectionRowDip;
    if (ime_shown) sections += kStatusPopupSectionMinDip;
    if (sections <= 0.0f) return 0.0f;
    const int gaps = static_cast<int>(battery_present + network_shown + ime_shown) - 1;
    float height = kStatusPopupMarginDip * 2.0f + sections +
                   static_cast<float>(std::max(0, gaps)) * kStatusPopupCardGapDip;
    return height;
}

// ---- window -------------------------------------------------------------------

StatusPopup::~StatusPopup() { Destroy(); }

bool StatusPopup::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    // No CS_HREDRAW/CS_VREDRAW: the pixels come from DComp, exactly like the bar.
    wc.style = 0;
    wc.lpfnWndProc = &StatusPopup::StaticWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kPopupClass;
    const ATOM atom = RegisterClassExW(&wc);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        DF_ERROR(kLogModule, "status popup RegisterClassExW failed %lu", GetLastError());
        return false;
    }
    return true;
}

bool StatusPopup::Create(HINSTANCE instance) {
    if (hwnd_ != nullptr) return true;
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP,
                            kPopupClass, L"DockForge Status", WS_POPUP, 0, 0, 1, 1, nullptr,
                            nullptr, instance, this);
    if (hwnd_ == nullptr) {
        DF_ERROR(kLogModule, "status popup CreateWindowExW failed %lu", GetLastError());
        return false;
    }
    if (!surface_.Initialize(hwnd_)) {
        DF_ERROR(kLogModule, "status popup surface initialize failed");
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }
    backend_ = render::CreateD2DBackend();
    if (!backend_ || !backend_->Initialize()) {
        DF_ERROR(kLogModule, "status popup D2D backend failed");
        backend_.reset();
        surface_.Shutdown();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }
    return true;
}

void StatusPopup::Destroy() {
    if (hwnd_ != nullptr) {
        if (backend_) backend_->Shutdown();
        backend_.reset();
        surface_.Shutdown();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void StatusPopup::Open(int dpi, const RECT& monitor, int bar_bottom_px,
                       float anchor_center_x_dip, const StatusSample& sample) {
    if (hwnd_ == nullptr) return;
    sample_ = sample;
    scale_ = dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;

    const bool network_shown = true;  // the section always renders (dim when offline)
    const bool ime_shown = sample_.ime_valid;
    const float height_dip = StatusPopupHeightFor(sample_.battery_present, network_shown,
                                                  ime_shown);
    if (height_dip <= 0.0f) return;
    width_px_ = static_cast<int>(kStatusPopupWidthDip * scale_);
    height_px_ = static_cast<int>(height_dip * scale_);

    // Anchor: top edge just below the bar, centre under the clicked box, clamped
    // into the monitor - the same clamps calendar_panel::Open applies.
    const int anchor_px =
        static_cast<int>(monitor.left) + static_cast<int>(anchor_center_x_dip * scale_);
    int left = anchor_px - width_px_ / 2;
    int top = bar_bottom_px;
    left = std::max(static_cast<int>(monitor.left),
                    std::min(left, static_cast<int>(monitor.right) - width_px_));
    top = std::max(static_cast<int>(monitor.top),
                   std::min(top, static_cast<int>(monitor.bottom) - height_px_));

    if (!surface_.Resize(width_px_, height_px_)) {
        DF_WARN(kLogModule, "status popup surface resize failed");
        return;
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, left, top, width_px_, height_px_, SWP_SHOWWINDOW);
    // Claim the foreground so Esc works and an outside click deactivates the
    // panel - the measured menu requirement (spike Q6/Q7) and calendar_panel's
    // same-open claim.
    if (!SetForegroundWindow(hwnd_)) {
        DF_DEBUG(kLogModule, "SetForegroundWindow refused on status popup open");
    }
    SetFocus(hwnd_);
    Repaint();
}

void StatusPopup::Close() {
    if (hwnd_ == nullptr || !Visible()) return;
    ShowWindow(hwnd_, SW_HIDE);
    DF_DEBUG(kLogModule, "status popup closed");
}

LRESULT CALLBACK StatusPopup::StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                            LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW* created = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(created->lpCreateParams));
    }
    StatusPopup* self = reinterpret_cast<StatusPopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) return self->WndProc(hwnd, msg, wparam, lparam);
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT StatusPopup::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_ACTIVATE:
            // Dismiss on ANY loss of activation - calendar_panel's measured rule
            // (the was_activated_ intermediate flag misread on this host, so the
            // flag does not exist here either). The panel claims the foreground at
            // Open, so a WA_INACTIVE only arrives after a real activation, except
            // when the claim was refused entirely (probe path) - then the panel
            // stays open and the next Open toggles it away.
            if (LOWORD(wparam) == WA_INACTIVE) {
                Close();
            }
            return 0;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                Close();
                return 0;
            }
            return 0;
        case WM_NCDESTROY:
            hwnd_ = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void StatusPopup::Repaint() {
    if (hwnd_ == nullptr || !surface_.IsValid() || !backend_) return;
    const bool light = SystemUsesLightTheme();
    const ThemeColors colors = ColorsFor(light);
    const float width = scale_ > 0.0f ? static_cast<float>(width_px_) / scale_ : 0.0f;
    const float height = scale_ > 0.0f ? static_cast<float>(height_px_) / scale_ : 0.0f;
    const float dpi = scale_ * 96.0f;
    if (width <= 0.0f || height <= 0.0f) return;

    // Where the battery text lands: red when below the 20% alarm band, otherwise
    // the theme text colour (the same frozen mapping the bar uses).
    const bool battery_alert =
        sample_.battery_present &&
        BatteryGradeForPercent(sample_.battery_percent) == UsageGrade::Critical;

    const bool painted = surface_.Paint(
        dpi, [&](ID2D1DeviceContext& dc, const platform::CompositionSurface::PaintInfo&) {
            backend_->BeginFrame(dc, dpi);

            // Root plate: the ADR-0008 self-tinted rounded rect + its 1 DIP border
            // (Solid tier by the ADR-0045 capture-exclusion rule, as on calendar_panel).
            const render::RectF root{0.0f, 0.0f, width, height};
            backend_->FillRoundedRect(root, kStatusPopupRootRadiusDip, colors.background);
            backend_->StrokeRoundedRect(root, kStatusPopupRootRadiusDip, colors.border, 1.0f);

            float cursor_y = kStatusPopupMarginDip;
            const float card_left = kStatusPopupMarginDip;
            const float card_right = width - kStatusPopupMarginDip;

            // Battery section: glyph + percent + state line. Drawn only when the
            // machine has one - the bar hides the item and so does the card.
            if (sample_.battery_present) {
                const float card_h = kStatusPopupSectionMinDip + kStatusPopupSectionRowDip;
                const render::RectF card{card_left, cursor_y, card_right, cursor_y + card_h};
                backend_->FillRoundedRect(card, kStatusPopupCardRadiusDip, colors.card);
                const float glyph_w = 42.0f;
                const render::RectF glyph{card.left + 16.0f, card.CenterY() - 12.0f,
                                          card.left + 16.0f + glyph_w,
                                          card.CenterY() + 12.0f};
                DrawBatteryGlyph(*backend_, glyph, sample_.battery_percent,
                                 sample_.battery_charging, battery_alert, colors.text);
                wchar_t pct[16]{};
                swprintf_s(pct, L"%d%%", sample_.battery_percent);
                backend_->DrawLabel(pct,
                                    render::RectF{glyph.right + 14.0f, card.top + 8.0f,
                                                  card.right - 16.0f, card.top + 34.0f},
                                    20.0f, battery_alert ? kWidgetCriticalColor : colors.text,
                                    render::TextAlign::Left);
                const wchar_t* state =
                    sample_.battery_charging ? L"\u5145\u7535\u4E2D"          // 充电中
                        : sample_.battery_ac_online ? L"\u5DF2\u63A5\u901A\u7535\u6E90"
                                                    // 已接通电源
                                                    : L"\u4F7F\u7528\u7535\u6C60\u7535\u91CF";
                // 使用电池电量
                backend_->DrawLabel(state,
                                    render::RectF{card.left + 16.0f, card.bottom - 26.0f,
                                                  card.right - 16.0f, card.bottom - 8.0f},
                                    12.0f, colors.text_secondary, render::TextAlign::Left);
                cursor_y += card_h + kStatusPopupCardGapDip;
            }

            // Network section: glyph + type label, then the detail line. Offline
            // states render dimmed text rather than vanishing - the card exists
            // because the user clicked the network item.
            {
                const float card_h = kStatusPopupSectionMinDip + kStatusPopupSectionRowDip;
                const render::RectF card{card_left, cursor_y, card_right, cursor_y + card_h};
                backend_->FillRoundedRect(card, kStatusPopupCardRadiusDip, colors.card);
                const render::RectF glyph{card.left + 16.0f, card.CenterY() - 11.0f,
                                          card.left + 16.0f + 22.0f, card.CenterY() + 11.0f};
                if (sample_.network_kind == ConnectionKind::Wifi) {
                    DrawWifiGlyph(*backend_, glyph, sample_.wifi_bars, colors.text);
                } else {
                    DrawEthernetGlyph(*backend_, glyph, colors.text);
                }
                backend_->DrawLabel(
                    ConnectionKindLabel(sample_.network_kind),
                    render::RectF{glyph.right + 14.0f, card.top + 8.0f, card.right - 16.0f,
                                  card.top + 30.0f},
                    14.0f, colors.text, render::TextAlign::Left);
                std::wstring detail;
                if (sample_.network_kind == ConnectionKind::Wifi && !sample_.wifi_ssid.empty()) {
                    detail = sample_.wifi_ssid + L" · " + SignalText(sample_.wifi_bars);
                } else if (sample_.network_kind == ConnectionKind::Wifi) {
                    detail = SignalText(sample_.wifi_bars);
                } else if (sample_.network_kind == ConnectionKind::Ethernet) {
                    detail = sample_.network_online ? L"\u5DF2\u8FDE\u63A5\u5230\u4E92\u8054\u7F51"
                                                    : L"\u672A\u8FDE\u63A5";
                    // 已连接到互联网 / 未连接
                } else {
                    detail = sample_.network_online ? L"\u5DF2\u8FDE\u63A5\u5230\u4E92\u8054\u7F51"
                                                    : L"\u65E0\u7F51\u7EDC\u8FDE\u63A5";
                    // 已连接到互联网 / 无网络连接
                }
                backend_->DrawLabel(detail.c_str(),
                                    render::RectF{card.left + 16.0f, card.bottom - 26.0f,
                                                  card.right - 16.0f, card.bottom - 8.0f},
                                    12.0f, colors.text_secondary, render::TextAlign::Left);
                cursor_y += card_h + kStatusPopupCardGapDip;
            }

            // Input mode section: the conversion label big, with the toggle hint.
            if (sample_.ime_valid) {
                const float card_h = kStatusPopupSectionMinDip;
                const render::RectF card{card_left, cursor_y, card_right, cursor_y + card_h};
                backend_->FillRoundedRect(card, kStatusPopupCardRadiusDip, colors.card);
                backend_->DrawLabel(
                    ImeConversionLabel(sample_.ime_conversion_mode).c_str(),
                    render::RectF{card.left + 16.0f, card.top + 8.0f, card.left + 80.0f,
                                  card.bottom - 8.0f},
                    24.0f, colors.accent, render::TextAlign::Left);
                backend_->DrawLabel(
                    L"\u8F93\u5165\u6CD5\u8F6C\u6362\u6A21\u5F0F"  // 输入法转换模式
                    L"\uFF08\u70B9\u51FB\u680F\u4E0A\u56FE\u6807\u5207\u6362\uFF09",
                    // （点击栏上图标切换）
                    render::RectF{card.left + 96.0f, card.top + 8.0f, card.right - 16.0f,
                                  card.bottom - 8.0f},
                    12.0f, colors.text_secondary, render::TextAlign::Left);
            }

            backend_->EndFrame();
        });

    if (!painted) {
        DF_WARN(kLogModule, "status popup paint failed");
        return;
    }
    surface_.Commit();
}

std::string StatusPopup::SnapshotJson() const {
    char buffer[128]{};
    std::snprintf(buffer, sizeof(buffer), "{\"visible\":%s,\"width_px\":%d,\"height_px\":%d}",
                  Visible() ? "true" : "false", width_px_, height_px_);
    return buffer;
}

}  // namespace dockforge::finderbar
