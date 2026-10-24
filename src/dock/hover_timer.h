// DockForge - hover dwell detector for the dock preview popup (T-015).
//
// Contract: the dock owns hover state and geometry (C-12), so the 400ms dwell
// that gates a preview lives in src/dock. This class only answers "has the
// pointer been inside one tile continuously for long enough"; what to show for
// that tile is the caller's business.
#pragma once

#include <cstdint>

namespace dockforge::dock {

// DoD: a preview pops after 400ms of hover. Measured with the spike's own
// 300ms/700ms sampling grid, this is the smallest delay that survives a mouse
// crossing the dock on its way somewhere else.
inline constexpr float kPreviewDwellMs = 400.0f;

class HoverTimer {
public:
    HoverTimer() = default;

    // Feeds the current hover state. `tile_index` is the hovered tile, or -1.
    // `delta_ms` is the frame delta. Returns true exactly on the frame where the
    // dwell completes for a NEW tile, so the caller can show a preview once.
    bool Update(int tile_index, float delta_ms) {
        if (tile_index < 0) {
            Reset();
            return false;
        }
        // A tile change, or the first armed frame, starts the dwell over. The
        // pointer must be resting, not passing through.
        if (tile_index != index_ || !armed_) {
            index_ = tile_index;
            elapsed_ms_ = 0.0f;
            armed_ = true;
            fired_ = false;
            return false;
        }
        if (fired_) {
            // Already fired for this dwell. The caller dismisses on any change,
            // so without this latch a stationary pointer would re-fire every
            // frame after the threshold.
            return false;
        }
        elapsed_ms_ += delta_ms;
        if (elapsed_ms_ < kPreviewDwellMs) {
            return false;
        }
        // Crossed the threshold.
        fired_ = true;
        return true;
    }

    int Index() const { return index_; }
    float ElapsedMs() const { return elapsed_ms_; }
    bool Armed() const { return armed_; }

    // Forgets the dwell. The pointer leaving the dock, a click, a drag or a
    // change of tile all reset it, so a preview never appears mid-gesture.
    void Reset() {
        index_ = -1;
        elapsed_ms_ = 0.0f;
        armed_ = false;
        fired_ = false;
    }

    bool Fired() const { return fired_; }

private:
    int index_ = -1;
    float elapsed_ms_ = 0.0f;
    bool armed_ = false;
    bool fired_ = false;
};

}  // namespace dockforge::dock
