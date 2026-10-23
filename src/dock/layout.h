// DockForge - Dock layout engine (pure math, no Win32, no allocation on the hot path).
//
// Implements CONTRACTS.md C-05. The contract states the cumulative shift as
//     x_i = base_i + sum_j (s_j - 1) * w * 0.5 * sign(i - j)
// which is O(n^2) if evaluated literally. Because sign(i-j) only depends on which
// side j sits, the sum splits into "everything to my left" minus "everything to my
// right", and both are prefix sums. The O(n) form below is algebraically identical
// to the contract, which tests/layout_test.cpp pins by comparing against a direct
// O(n^2) reference implementation.
#pragma once

#include "core/config.h"

#include <cstddef>
#include <vector>

namespace dockforge::dock {

// Layout parameters in device pixels. DPI scaling is the caller's job so this
// module stays pure arithmetic and testable without a window.
struct LayoutParams {
    int icon_size = 48;
    int gap = 8;
    float magnification = 1.8f;
    float influence_radius = 3.0f;   // multiples of icon_size
    float falloff_sharpness = 2.0f;  // exponent p, or the gaussian width
    core::FalloffCurve curve = core::FalloffCurve::Sine;
};

struct IconLayout {
    float base_center = 0.0f;  // center with no hover, dock-local x
    float scale = 1.0f;        // 1.0 .. magnification
    float center = 0.0f;       // center after the cumulative shift
    float width = 0.0f;        // icon_size * scale
};

struct LayoutSummary {
    float base_width = 0.0f;   // content width with no hover
    float hover_width = 0.0f;  // content width with the current hover
    float min_extent = 0.0f;   // leftmost drawn edge (can be negative)
    float max_extent = 0.0f;   // rightmost drawn edge
    float peak_scale = 1.0f;
    int peak_index = -1;
    bool hovering = false;
};

// Weight w(t) for a normalised distance t in [0,1]. Exposed because it is the
// per-icon cost the GPU vertex path has to match, and because the three curves
// need to be pinned independently of the layout loop.
float FalloffWeight(float t, const LayoutParams& params);

// Scale for an icon whose distance from the pointer is `distance` px.
// Returns exactly 1.0f at and beyond the influence radius, and exactly
// `magnification` at distance 0.
float ComputeScale(float distance, const LayoutParams& params);

// Base content width for `count` icons, no hover.
float BaseContentWidth(int count, const LayoutParams& params);

// Largest icon count whose base layout fits in `available_width`.
std::size_t IconCountForWidth(float available_width, const LayoutParams& params);

// Core entry point. Writes at most `capacity` icons into `out` and returns the
// number written. Allocation-free so it can run inside the frame loop.
std::size_t LayoutDock(const LayoutParams& params, int count, float mouse_x, bool hovering,
                       IconLayout* out, std::size_t capacity, LayoutSummary* summary);

// Convenience wrapper for tests and cold paths.
std::vector<IconLayout> LayoutDock(const LayoutParams& params, int count, float mouse_x,
                                   bool hovering, LayoutSummary* summary = nullptr);

// Converts the DIP-based user config into device pixels.
LayoutParams FromConfig(const core::DockConfig& config, float scale);

}  // namespace dockforge::dock
