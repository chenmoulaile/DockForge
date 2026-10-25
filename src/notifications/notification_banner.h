// DockForge - the top-right notification banner (T-076).
//
// macOS shape, not Windows toast shape: cards appear at the top-right of the
// work area, newest on top, each one counting its own timeout, and clicking a
// card takes you to the app that posted it. Windows' own toasts already exist;
// the reason this window exists is that they do not stack the way macOS does and
// they disappear before a long message can be read.
//
// Split in two on purpose:
//   * BannerModel - pure state. Which cards are up, how old each one is, what
//     the window's height should be. No HWND, no timers, so the expiry rules are
//     testable without a desktop.
//   * NotificationBanner - the GDI window in the folder-panel/weather-popup
//     family (own class, own WndProc, BitBlt double buffering).
//
// C-21: this window never reads the power state. It holds a material tier that
// main.cpp feeds from MaterialMonitor's MaterialDegraded/MaterialRestored, and
// the tier only changes how translucent the plate is - frost costs GPU time, so
// the degraded tier is the opaque one.
#pragma once

#include "core/config.h"
#include "notifications/notification_types.h"
#include "platform/win_common.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace dockforge::notifications {

// How many cards can be up at once. Four is the macOS practical ceiling: past
// that the stack covers the window underneath it, and the extras are better
// served by the history panel than by pushing the first card off screen.
inline constexpr std::size_t kMaxBannerCards = 4;

struct BannerCard {
    NotificationItem item;
    // Time this card has been on screen. Per-card rather than per-window: a card
    // that arrived 4s ago must not be kept alive by a card that arrived now.
    double age_ms = 0.0;
};

class BannerModel {
  public:
    // Timeout comes from config.notifications.banner_timeout_seconds.
    void SetTimeoutSeconds(int seconds);
    int timeout_seconds() const { return timeout_seconds_; }

    // Adds a card on top. When the stack is full the oldest card is dropped -
    // a banner the user has had the longest is the one they have already read.
    // Returns true when the visible set changed.
    bool Push(const NotificationItem& item);

    // Ages every card and drops the expired ones. Returns true when the set
    // changed (the caller then resizes the window; an unchanged set must not
    // repaint, or the banner would animate its own idle frames).
    bool Tick(double delta_ms);

    // Removes the card at `index` (a click). Returns true when it existed.
    bool Dismiss(std::size_t index);

    void Clear();

    const std::vector<BannerCard>& cards() const { return cards_; }
    bool empty() const { return cards_.empty(); }
    std::size_t size() const { return cards_.size(); }

    // Total plate height in DIP for the current stack, 0 when empty.
    double HeightDip() const;

    // Which card a click at `y_dip` (measured from the plate's top edge) landed
    // on, or kNoCard. Hit-testing lives here so the window code has no layout
    // arithmetic of its own.
    static constexpr std::size_t kNoCard = static_cast<std::size_t>(-1);
    std::size_t CardAtDip(double y_dip) const;

  private:
    std::vector<BannerCard> cards_;  // newest first
    int timeout_seconds_ = 5;
};

// DIP layout constants (scaled once per Show, same one-shot rule as every other
// popover in this codebase).
inline constexpr float kBannerWidthDip = 344.0f;
inline constexpr float kBannerCardHeightDip = 78.0f;
inline constexpr float kBannerGapDip = 8.0f;
inline constexpr float kBannerEdgeMarginDip = 12.0f;

class NotificationBanner {
  public:
    NotificationBanner() = default;
    ~NotificationBanner();

    NotificationBanner(const NotificationBanner&) = delete;
    NotificationBanner& operator=(const NotificationBanner&) = delete;

    static bool RegisterWindowClass(HINSTANCE instance);

    bool Create(HINSTANCE instance);
    void Destroy();

    bool IsValid() const { return hwnd_ != nullptr; }
    HWND Handle() const { return hwnd_; }
    bool Visible() const { return hwnd_ != nullptr && IsWindowVisible(hwnd_) != FALSE; }

    BannerModel& model() { return model_; }
    const BannerModel& model() const { return model_; }

    // C-21. Translucency is the thing that costs GPU time, so the degraded tier
    // is the opaque one; the value is applied with LWA_ALPHA rather than a
    // per-pixel blend because a GDI plate has nothing to blend against.
    void SetMaterialTier(core::MaterialLevel tier);
    core::MaterialLevel material_tier() const { return tier_; }

    // Called with the card's notification when the user clicks it.
    void SetActivateHandler(std::function<void(const NotificationItem&)> handler) {
        on_activate_ = std::move(handler);
    }

    // Positions and paints the current stack. Hides the window when the model is
    // empty. `dpi` is the monitor's DPI; the caller passes it because this window
    // does not own a DPI-aware surface.
    void Refresh(int dpi);

    void Hide();

    // Probe payload: which cards are up and how old they are.
    std::string SnapshotJson() const;

  private:
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);
    void Paint(HDC dc);

    HWND hwnd_ = nullptr;
    BannerModel model_;
    core::MaterialLevel tier_ = core::MaterialLevel::Solid;
    std::function<void(const NotificationItem&)> on_activate_;
    int width_px_ = 0;
    int height_px_ = 0;
    float scale_ = 1.0f;
};

// The alpha LWA_ALPHA gets for a tier. Exposed for the test that pins the
// direction of the ladder (degraded must be the more opaque one).
BYTE BannerAlphaForTier(core::MaterialLevel tier);

}  // namespace dockforge::notifications
