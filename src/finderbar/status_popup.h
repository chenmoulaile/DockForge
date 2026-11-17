// DockForge - the status detail popup (T-306).
//
// The card a click on the battery / network status item drops below the bar: the
// battery level and state, the connection type / online state / SSID + signal
// bars, and the current input mode. The Wi-Fi *network list* deliberately stays
// out - it belongs to T-304b's control centre; the T-306 spec pins this popup as
// the simple detail panel ("本包网络点击先开 status_popup 简面板").
//
// Window model: the same one notification_panel.h established and calendar_panel
// followed - a frameless topmost tool window whose surface is the Finder Bar's own
// DComp path (WS_EX_NOREDIRECTIONBITMAP + platform::CompositionSurface), drawn by
// the PlateRenderer-family self-tinted plate. Material is ADR-0008's ruling: a
// system backdrop ignores per-pixel alpha and the rounded radius on this system,
// so the plate is painted directly with the theme's own background token; and per
// ADR-0045 the panel stays on the Solid tier because a capture-excluding tier
// would make the panel invisible to every screenshot - including this task's own
// evidence. The one deliberate departure from notification_panel is the renderer:
// that panel is GDI (it predates the bar's surface), this one draws D2D because it
// must reuse the status glyph painters, and calendar_panel already proved the
// DComp card pattern for the bar's popups.
//
// Dismissal is calendar_panel's rule: Esc, or the loss of activation after the
// panel claimed the foreground at Open (a click outside deactivates it). There is
// no global mouse hook chasing clicks.
//
// Layout is a pure function: HeightFor decides the panel height from which
// sections exist, so a desktop without a battery gets a shorter card instead of a
// hole, and the unit tests can pin it.
#pragma once

#include "finderbar/status_sampler.h"
#include "platform/composition.h"
#include "platform/win_common.h"
#include "render/backend.h"

#include <memory>
#include <string>

namespace dockforge::finderbar {

// DIP metrics (Beautiful NotificationCenterWindow's card family: root radius 12,
// content cards radius 10, 12 DIP outer margin - the same rhythm calendar_panel
// uses, so the bar's three popups read as one product).
inline constexpr float kStatusPopupWidthDip = 320.0f;
inline constexpr float kStatusPopupMarginDip = 12.0f;
inline constexpr float kStatusPopupCardGapDip = 10.0f;
inline constexpr float kStatusPopupCardRadiusDip = 10.0f;
inline constexpr float kStatusPopupRootRadiusDip = 12.0f;
// One section card: caption row (12) + detail row (18) + second detail row (16)
// when present + paddings. A section with no second row is 62 tall; with two, 78.
inline constexpr float kStatusPopupSectionMinDip = 62.0f;
inline constexpr float kStatusPopupSectionRowDip = 16.0f;

// The panel height for a given combination of sections. Pure, so a test can pin
// the arithmetic (and so a change to it must be deliberate).
float StatusPopupHeightFor(bool battery_present, bool network_shown, bool ime_shown);

class StatusPopup {
  public:
    StatusPopup() = default;
    ~StatusPopup();

    StatusPopup(const StatusPopup&) = delete;
    StatusPopup& operator=(const StatusPopup&) = delete;

    static bool RegisterWindowClass(HINSTANCE instance);
    bool Create(HINSTANCE instance);
    void Destroy();

    bool IsValid() const { return hwnd_ != nullptr; }
    bool Visible() const { return hwnd_ != nullptr && IsWindowVisible(hwnd_) != FALSE; }

    // Opens the popup anchored under a right-region box: top edge just below the
    // bar, horizontal centre at the box's centre, clamped into the monitor. Takes
    // a copy of the sample; the panel is a snapshot, not a live view - the next
    // Open refreshes it (the popup's lifetime is one click, and the sampler
    // continues on the bar regardless).
    void Open(int dpi, const RECT& monitor, int bar_bottom_px, float anchor_center_x_dip,
              const StatusSample& sample);
    void Close();

    std::string SnapshotJson() const;

  private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    void Repaint();

    HWND hwnd_ = nullptr;
    platform::CompositionSurface surface_;
    std::unique_ptr<render::IRenderBackend> backend_;
    // No PlateRenderer, same rule as calendar_panel.h: its Solid tint is fixed
    // dark and this panel must read in both light and dark mode. The plate is the
    // ADR-0008 self-tinted rounded rect painted directly from the theme tokens.
    StatusSample sample_{};
    float scale_ = 1.0f;
    int width_px_ = 0;
    int height_px_ = 0;
};

}  // namespace dockforge::finderbar
