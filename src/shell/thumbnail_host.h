// DockForge - DWM thumbnail host for the dock preview popup (T-015).
//
// Contract: CONTRACTS.md C-06 puts thumbnails in src/shell, so every DWM
// thumbnail call in the product lives here. This class owns the registration
// lifetime and the slot geometry; it does not own the popup HWND (that is
// platform::PreviewWindow) and it does not decide when a preview is shown (that
// is the dock hover timer).
#pragma once

#include "platform/win_common.h"

#include <cstdint>
#include <vector>

namespace dockforge::shell {

// One registered thumbnail: a source HWND plus the DWM handle for it.
struct ThumbnailSlot {
    HWND source = nullptr;
    HTHUMBNAIL thumb = nullptr;
    // Client-area rectangle of the destination popup, in PHYSICAL pixels. DWM
    // interprets DWM_THUMBNAIL_PROPERTIES::rcDestination in destination client
    // pixels, never DIP; passing DIP silently shifts every slot by half at 200%
    // (spikes/t015_thumb, comment in RegisterThumbnails).
    RECT dest{0, 0, 0, 0};
};

// Measured limits from spikes/t015_thumb:
//   * a cold DwmRegisterThumbnail costs 219..609ms per slot, so registration has
//     to be spread over frames by the caller, never done in one tick;
//   * with slot size 110x70 DIP and gap 8 DIP, eight slots on one destination
//     window render 8/8 reliably on this machine; larger slots lose slots to an
//     unexplained DWM ceiling (150x70 -> 7, 180x70 -> 6, 200x130 -> 4..6).
// These are the constants the dock side must ask for.
inline constexpr int kMaxThumbnails = 8;
inline constexpr float kSlotWidthDip = 110.0f;
inline constexpr float kSlotHeightDip = 70.0f;
inline constexpr float kSlotGapDip = 8.0f;

// How many registrations to attempt per owner tick. At the measured worst case
// of 609ms per registration, one per tick keeps a tick under ~16ms in the common
// case and degrades instead of stalling.
inline constexpr int kRegistrationsPerTick = 1;

// Gutter between the popup edge and the first slot, in DIP. Keeps the thumbnail
// from touching the rounded popup border.
inline constexpr float kSlotPaddingDip = 12.0f;

class ThumbnailHost {
public:
    ThumbnailHost() = default;
    ~ThumbnailHost();

    ThumbnailHost(const ThumbnailHost&) = delete;
    ThumbnailHost& operator=(const ThumbnailHost&) = delete;

    // Binds the host to the popup that renders the thumbnails. Any registration
    // made against a previous destination is released first.
    void SetDestination(HWND hwnd) {
        if (dest_ == hwnd) {
            return;
        }
        ReleaseAll();
        dest_ = hwnd;
    }

    HWND Destination() const { return dest_; }

    // Replaces the visible slot set. Returns the number of registrations that
    // completed immediately; the caller must keep calling PumpPending() until
    // PendingCount() reaches 0.
    //
    // The set is applied in three phases so that a popup with eight slots never
    // blocks a frame: slots that already exist and whose source is unchanged are
    // left untouched, changed sources are re-registered lazily, and new sources
    // are queued.
    int SetSources(const std::vector<HWND>& sources);

    // Performs at most kRegistrationsPerTick queued registrations. Returns the
    // number performed.
    int PumpPending();

    int PendingCount() const { return static_cast<int>(pending_.size()); }
    int LiveCount() const { return static_cast<int>(slots_.size()); }

    // Releases every registration. Called before destroying or hiding the
    // destination and by SetSources when the set shrinks.
    void ReleaseAll();

    // Recomputes the destination rectangles for the current slot set and pushes
    // them to DWM. Called when the popup moves or resizes. `scale` is the DPI
    // scale of the destination window (window.Scale(), never GetDpiForWindow,
    // per C-16d), `left_px`/`top_px` are the popup's virtual-screen origin.
    void UpdateDestinations(int left_px, int top_px, float scale);

    const std::vector<ThumbnailSlot>& Slots() const { return slots_; }

private:
    // Registers one source into `slot` and updates its destination rect.
    bool RegisterSlot(ThumbnailSlot& slot, HWND source, int index);

    HWND dest_ = nullptr;
    std::vector<ThumbnailSlot> slots_;
    std::vector<HWND> pending_;
    int origin_x_px_ = 0;
    int origin_y_px_ = 0;
    float scale_ = 1.0f;
};

}  // namespace dockforge::shell
