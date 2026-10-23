// DockForge - Dock layout engine implementation.
//
// See layout.h for why the cumulative shift is evaluated with prefix sums rather
// than the literal double loop in CONTRACTS.md C-05.
#include "dock/layout.h"

#include <algorithm>
#include <cmath>

namespace dockforge::dock {
namespace {

// Normalised gaussian so the weight is exactly zero at the influence radius.
// exp(-(t*p)^2) alone leaves exp(-p^2) at t=1 (0.0183 at p=2), which is a ~1.5%
// scale step where the curve is supposed to join the resting row. Subtracting and
// renormalising costs one exp and removes the discontinuity.
float GaussianWeight(float t, float sharpness) {
    const float k = sharpness * t;
    const float raw = std::exp(-k * k);
    const float edge = std::exp(-sharpness * sharpness);
    const float span = 1.0f - edge;
    if (span <= 1e-6f) return 0.0f;
    return std::clamp((raw - edge) / span, 0.0f, 1.0f);
}

}  // namespace

float FalloffWeight(float t, const LayoutParams& params) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float p = std::max(params.falloff_sharpness, 0.01f);
    switch (params.curve) {
        case core::FalloffCurve::Gaussian:
            return GaussianWeight(t, p);
        case core::FalloffCurve::CosineIntegral:
            // The cosine-integral curve is visually a slightly steeper power
            // decay than the plain sine variant; ADR-0004 maps both onto the same
            // closed form with different sharpness presets.
            return std::pow(1.0f - t, p * 1.35f);
        case core::FalloffCurve::Sine:
        default:
            return std::pow(1.0f - t, p);
    }
}

float ComputeScale(float distance, const LayoutParams& params) {
    const float w = static_cast<float>(std::max(params.icon_size, 1));
    const float radius = std::max(params.influence_radius, 0.01f) * w;
    const float d = std::abs(distance);
    if (d >= radius) return 1.0f;
    if (d <= 0.0f) return std::max(params.magnification, 1.0f);
    const float t = d / radius;
    const float gain = (std::max(params.magnification, 1.0f) - 1.0f) * FalloffWeight(t, params);
    return 1.0f + gain;
}

float BaseContentWidth(int count, const LayoutParams& params) {
    if (count <= 0) return 0.0f;
    const float w = static_cast<float>(std::max(params.icon_size, 1));
    const float gap = static_cast<float>(std::max(params.gap, 0));
    return static_cast<float>(count) * w + static_cast<float>(count - 1) * gap;
}

std::size_t IconCountForWidth(float available_width, const LayoutParams& params) {
    const float w = static_cast<float>(std::max(params.icon_size, 1));
    const float gap = static_cast<float>(std::max(params.gap, 0));
    if (available_width < w) return 0;
    // n*w + (n-1)*gap <= available  =>  n <= (available + gap) / (w + gap)
    const double n = std::floor((static_cast<double>(available_width) + gap) / (w + gap));
    if (n < 1.0) return 0;
    return static_cast<std::size_t>(n);
}

std::size_t LayoutDock(const LayoutParams& params, int count, float mouse_x, bool hovering,
                       IconLayout* out, std::size_t capacity, LayoutSummary* summary) {
    if (count < 0) count = 0;
    const std::size_t n = static_cast<std::size_t>(count);
    const std::size_t writable = std::min(n, capacity);

    const float w = static_cast<float>(std::max(params.icon_size, 1));
    const float gap = static_cast<float>(std::max(params.gap, 0));
    const float stride = w + gap;
    const float base_width = BaseContentWidth(count, params);

    // Clamping the pointer to the span of icon *centres* is what makes the edge
    // icon reach full magnification instead of decaying as the cursor leaves the
    // dock, and it keeps the peak index monotone in mouse_x, which T-008's DoD
    // requires. Clamping to [0, base_width] instead would pin the pointer to the
    // first icon's outer edge, one half-width away from its centre, so the edge
    // icon would top out below `magnification`.
    float mouse = mouse_x;
    if (hovering && base_width > 0.0f) {
        const float half = w * 0.5f;
        mouse = std::clamp(mouse, half, std::max(half, base_width - half));
    }

    float peak_scale = 1.0f;
    int peak_index = -1;

    // Pass 1: scales from the *base* centres. Feeding shifted centres back in
    // would make the layout self-referential and oscillate.
    //
    // Icons outside the influence radius contribute exactly zero gain, so the
    // pow() is skipped for them. On a 20-icon dock only about six icons are ever
    // in range, and skipping the rest is most of the difference between this
    // function being cheap enough for the frame loop and not.
    const float radius = std::max(params.influence_radius, 0.01f) * w;
    float total_gain = 0.0f;
    for (std::size_t i = 0; i < writable; ++i) {
        const float base_center = static_cast<float>(i) * stride + w * 0.5f;
        float scale = 1.0f;
        if (hovering && std::abs(mouse - base_center) < radius) {
            scale = ComputeScale(mouse - base_center, params);
        }
        out[i].base_center = base_center;
        out[i].scale = scale;
        out[i].width = w * scale;
        total_gain += scale - 1.0f;
        if (scale > peak_scale) {
            peak_scale = scale;
            peak_index = static_cast<int>(i);
        }
    }

    // Pass 2: x_i = base_i + 0.5*w*(sum_{j<i} g_j - sum_{j>i} g_j).
    // The gains are read back out of `out` instead of a scratch array, which is
    // what keeps this allocation-free.
    float prefix = 0.0f;
    for (std::size_t i = 0; i < writable; ++i) {
        const float gain = out[i].scale - 1.0f;
        const float suffix = total_gain - prefix - gain;
        out[i].center = out[i].base_center + 0.5f * w * (prefix - suffix);
        prefix += gain;
    }

    if (summary) {
        summary->base_width = base_width;
        summary->peak_scale = peak_scale;
        summary->peak_index = peak_index;
        summary->hovering = hovering && writable > 0;
        if (writable == 0) {
            summary->hover_width = base_width;
            summary->min_extent = 0.0f;
            summary->max_extent = 0.0f;
        } else {
            float lo = out[0].center - out[0].width * 0.5f;
            float hi = out[0].center + out[0].width * 0.5f;
            for (std::size_t i = 1; i < writable; ++i) {
                lo = std::min(lo, out[i].center - out[i].width * 0.5f);
                hi = std::max(hi, out[i].center + out[i].width * 0.5f);
            }
            summary->min_extent = lo;
            summary->max_extent = hi;
            summary->hover_width = hi - lo;
        }
    }

    return writable;
}

std::vector<IconLayout> LayoutDock(const LayoutParams& params, int count, float mouse_x,
                                   bool hovering, LayoutSummary* summary) {
    std::vector<IconLayout> icons(count > 0 ? static_cast<std::size_t>(count) : 0u);
    const std::size_t written = LayoutDock(params, count, mouse_x, hovering, icons.data(),
                                           icons.size(), summary);
    icons.resize(written);
    return icons;
}

LayoutParams FromConfig(const core::DockConfig& config, float scale) {
    if (!(scale > 0.0f)) scale = 1.0f;
    const auto px = [scale](int dip) {
        return std::max(1, static_cast<int>(std::lround(static_cast<float>(dip) * scale)));
    };
    LayoutParams params;
    params.icon_size = px(config.icon_size);
    params.gap = std::max(0, static_cast<int>(std::lround(static_cast<float>(config.gap) * scale)));
    params.magnification = config.magnification;
    params.influence_radius = config.influence_radius;
    params.falloff_sharpness = config.falloff_sharpness;
    params.curve = config.hover_falloff;
    return params;
}

}  // namespace dockforge::dock
