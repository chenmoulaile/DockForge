// DockForge - Dock layout engine tests.
//
// Two things are being pinned here. First, the contract from CONTRACTS.md C-05,
// evaluated the slow literal way, must agree with the O(n) prefix-sum version in
// src/dock/layout.cpp to within float noise; that is what makes the optimisation
// safe. Second, the behavioural properties a dock must have: the peak follows the
// pointer, nothing jumps, and the row stays inside its own bounds.
#include "dock/layout.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using dockforge::core::DockConfig;
using dockforge::core::FalloffCurve;
using dockforge::dock::ComputeScale;
using dockforge::dock::FalloffWeight;
using dockforge::dock::IconLayout;
using dockforge::dock::LayoutDock;
using dockforge::dock::LayoutParams;
using dockforge::dock::LayoutSummary;

namespace {

LayoutParams Defaults() {
    LayoutParams params;
    params.icon_size = 48;
    params.gap = 8;
    params.magnification = 1.8f;
    params.influence_radius = 3.0f;
    params.falloff_sharpness = 2.0f;
    params.curve = FalloffCurve::Sine;
    return params;
}

// Literal transcription of CONTRACTS.md C-05, including the O(n^2) shift sum.
// Deliberately slow and deliberately dumb: it is the oracle.
std::vector<IconLayout> ReferenceLayout(const LayoutParams& params, int count, float mouse_x,
                                        bool hovering) {
    std::vector<IconLayout> icons(static_cast<std::size_t>(count));
    const float w = static_cast<float>(params.icon_size);
    const float stride = w + static_cast<float>(params.gap);
    const float base_width = dockforge::dock::BaseContentWidth(count, params);
    // The pointer is clamped to the span of icon centres, matching layout.cpp.
    float mouse = mouse_x;
    if (hovering && base_width > 0.0f) {
        const float half = w * 0.5f;
        mouse = std::clamp(mouse, half, std::max(half, base_width - half));
    }

    for (int i = 0; i < count; ++i) {
        const float base_center = static_cast<float>(i) * stride + w * 0.5f;
        float scale = 1.0f;
        if (hovering) {
            const float radius = params.influence_radius * w;
            const float d = std::abs(mouse - base_center);
            const float t = std::clamp(d / radius, 0.0f, 1.0f);
            scale = 1.0f + (params.magnification - 1.0f) * std::pow(1.0f - t, params.falloff_sharpness);
        }
        icons[static_cast<std::size_t>(i)].base_center = base_center;
        icons[static_cast<std::size_t>(i)].scale = scale;
        icons[static_cast<std::size_t>(i)].width = w * scale;
    }
    for (int i = 0; i < count; ++i) {
        float shift = 0.0f;
        for (int j = 0; j < count; ++j) {
            if (i == j) continue;
            const float sj = icons[static_cast<std::size_t>(j)].scale;
            shift += (sj - 1.0f) * w * 0.5f * (i > j ? 1.0f : -1.0f);
        }
        icons[static_cast<std::size_t>(i)].center =
            icons[static_cast<std::size_t>(i)].base_center + shift;
    }
    return icons;
}

}  // namespace

TEST_CASE("scale is exactly magnification at the pointer and exactly 1 at the radius",
          "[layout]") {
    const LayoutParams params = Defaults();
    CHECK(ComputeScale(0.0f, params) == Catch::Approx(params.magnification));
    const float radius = params.influence_radius * static_cast<float>(params.icon_size);
    CHECK(ComputeScale(radius, params) == Catch::Approx(1.0f));
    CHECK(ComputeScale(radius + 500.0f, params) == Catch::Approx(1.0f));
    // Symmetric in the sign of the distance: the dock does not care which side
    // the pointer approaches from.
    CHECK(ComputeScale(-17.0f, params) == Catch::Approx(ComputeScale(17.0f, params)));
}

TEST_CASE("scale is monotonically non-increasing in distance", "[layout]") {
    const LayoutParams params = Defaults();
    float previous = ComputeScale(0.0f, params);
    for (float d = 1.0f; d <= 200.0f; d += 1.0f) {
        const float s = ComputeScale(d, params);
        CHECK(s <= previous + 1e-6f);
        previous = s;
    }
}

TEST_CASE("falloff weight is continuous at the influence radius for every curve", "[layout]") {
    // A curve that does not reach zero at t=1 leaves a visible step where the
    // magnified icons meet the resting row, so this is a real bug, not pedantry.
    for (auto curve : {FalloffCurve::Sine, FalloffCurve::CosineIntegral, FalloffCurve::Gaussian}) {
        LayoutParams params = Defaults();
        params.curve = curve;
        CHECK(FalloffWeight(0.0f, params) == Catch::Approx(1.0f));
        CHECK(FalloffWeight(1.0f, params) == Catch::Approx(0.0f).margin(1e-6));
        CHECK(FalloffWeight(1.5f, params) == Catch::Approx(0.0f));
        // And it must actually decay in between, not sit at a constant.
        CHECK(FalloffWeight(0.5f, params) < FalloffWeight(0.1f, params));
    }
}

TEST_CASE("prefix-sum layout matches the literal O(n^2) contract", "[layout]") {
    const LayoutParams params = Defaults();
    for (int count : {1, 2, 4, 7, 20}) {
        for (float mouse : {0.0f, 37.5f, 150.0f, 999.0f}) {
            const auto fast = LayoutDock(params, count, mouse, true);
            const auto slow = ReferenceLayout(params, count, mouse, true);
            REQUIRE(fast.size() == slow.size());
            for (std::size_t i = 0; i < fast.size(); ++i) {
                CHECK(fast[i].scale == Catch::Approx(slow[i].scale).margin(1e-5));
                CHECK(fast[i].center == Catch::Approx(slow[i].center).margin(1e-3));
                CHECK(fast[i].width == Catch::Approx(slow[i].width).margin(1e-4));
            }
        }
    }
}

TEST_CASE("no hover means resting geometry: scale 1, even spacing, no shift", "[layout]") {
    const LayoutParams params = Defaults();
    LayoutSummary summary;
    const auto icons = LayoutDock(params, 5, 0.0f, false, &summary);
    REQUIRE(icons.size() == 5);
    const float stride = static_cast<float>(params.icon_size + params.gap);
    for (std::size_t i = 0; i < icons.size(); ++i) {
        CHECK(icons[i].scale == Catch::Approx(1.0f));
        CHECK(icons[i].center == Catch::Approx(static_cast<float>(i) * stride +
                                               static_cast<float>(params.icon_size) * 0.5f));
        CHECK(icons[i].width == Catch::Approx(static_cast<float>(params.icon_size)));
    }
    CHECK(summary.hovering == false);
    CHECK(summary.peak_scale == Catch::Approx(1.0f));
    CHECK(summary.hover_width == Catch::Approx(summary.base_width));
}

TEST_CASE("empty dock is not a special case that crashes", "[layout]") {
    const LayoutParams params = Defaults();
    LayoutSummary summary;
    const auto icons = LayoutDock(params, 0, 123.0f, true, &summary);
    CHECK(icons.empty());
    CHECK(summary.base_width == Catch::Approx(0.0f));
    CHECK(summary.hover_width == Catch::Approx(0.0f));
    CHECK(summary.peak_index == -1);
    CHECK(summary.hovering == false);

    // Negative counts are treated as empty rather than allocating nonsense.
    const auto negative = LayoutDock(params, -3, 0.0f, true);
    CHECK(negative.empty());
}

TEST_CASE("a single icon magnifies and does not move", "[layout]") {
    const LayoutParams params = Defaults();
    LayoutSummary summary;
    const auto icons = LayoutDock(params, 1, 24.0f, true, &summary);
    REQUIRE(icons.size() == 1);
    CHECK(icons[0].scale == Catch::Approx(params.magnification).margin(1e-4));
    // Nothing on either side to push it, so the centre stays at base.
    CHECK(icons[0].center == Catch::Approx(icons[0].base_center).margin(1e-4));
    CHECK(summary.peak_index == 0);
}

TEST_CASE("the peak follows the pointer and never goes backwards", "[layout]") {
    const LayoutParams params = Defaults();
    int previous = -1;
    for (float mouse = 0.0f; mouse <= 600.0f; mouse += 5.0f) {
        LayoutSummary summary;
        LayoutDock(params, 10, mouse, true, &summary);
        CHECK(summary.peak_index >= previous);
        previous = summary.peak_index;
    }
}

TEST_CASE("pointer far outside the dock still magnifies the edge icon", "[layout]") {
    // Without clamping the pointer into the content row, moving past the last
    // icon would decay every scale back to 1 and the dock would visually go dead
    // while the cursor is still on top of it.
    const LayoutParams params = Defaults();
    LayoutSummary left;
    LayoutDock(params, 6, -4000.0f, true, &left);
    CHECK(left.peak_index == 0);
    CHECK(left.peak_scale == Catch::Approx(params.magnification).margin(1e-4));

    LayoutSummary right;
    LayoutDock(params, 6, 4000.0f, true, &right);
    CHECK(right.peak_index == 5);
    CHECK(right.peak_scale == Catch::Approx(params.magnification).margin(1e-4));
}

TEST_CASE("adjacent icons never overlap and never jump", "[layout]") {
    const LayoutParams params = Defaults();
    const auto icons = LayoutDock(params, 12, 300.0f, true);
    REQUIRE(icons.size() == 12);
    for (std::size_t i = 1; i < icons.size(); ++i) {
        // The visual gap is what the user sees; it must stay positive or icons
        // would render on top of each other.
        const float gap = (icons[i].center - icons[i].width * 0.5f) -
                          (icons[i - 1].center + icons[i - 1].width * 0.5f);
        CHECK(gap >= static_cast<float>(params.gap) - 1e-3f);
    }
    // Moving the pointer by 1 px may not move an icon by more than the scale
    // change can justify; a big step would read as a snap.
    const auto before = LayoutDock(params, 12, 300.0f, true);
    const auto after = LayoutDock(params, 12, 301.0f, true);
    for (std::size_t i = 0; i < before.size(); ++i) {
        CHECK(std::abs(after[i].center - before[i].center) < 2.0f);
    }
}

TEST_CASE("extreme magnification stays finite and ordered", "[layout]") {
    LayoutParams params = Defaults();
    params.magnification = 2.5f;
    params.falloff_sharpness = 4.0f;
    params.influence_radius = 5.0f;
    const auto icons = LayoutDock(params, 8, 200.0f, true);
    REQUIRE(icons.size() == 8);
    for (std::size_t i = 1; i < icons.size(); ++i) {
        CHECK(icons[i].center > icons[i - 1].center);
    }
    for (const auto& icon : icons) {
        CHECK(std::isfinite(icon.center));
        CHECK(icon.scale >= 1.0f);
        CHECK(icon.scale <= params.magnification + 1e-4f);
    }
}

TEST_CASE("magnification below 1 is clamped to resting size", "[layout]") {
    LayoutParams params = Defaults();
    params.magnification = 0.5f;  // nonsense value; must not shrink icons
    const auto icons = LayoutDock(params, 4, 100.0f, true);
    for (const auto& icon : icons) CHECK(icon.scale == Catch::Approx(1.0f));
}

TEST_CASE("base width and icon count round-trip", "[layout]") {
    const LayoutParams params = Defaults();
    CHECK(dockforge::dock::BaseContentWidth(0, params) == Catch::Approx(0.0f));
    CHECK(dockforge::dock::BaseContentWidth(1, params) == Catch::Approx(48.0f));
    CHECK(dockforge::dock::BaseContentWidth(3, params) == Catch::Approx(48.0f * 3 + 16.0f));
    for (int n = 1; n <= 30; ++n) {
        const float width = dockforge::dock::BaseContentWidth(n, params);
        CHECK(dockforge::dock::IconCountForWidth(width, params) == static_cast<std::size_t>(n));
        // One pixel narrower than the row cannot fit that many icons.
        CHECK(dockforge::dock::IconCountForWidth(width - 1.0f, params) <
              static_cast<std::size_t>(n));
    }
    CHECK(dockforge::dock::IconCountForWidth(0.0f, params) == 0u);
    CHECK(dockforge::dock::IconCountForWidth(10.0f, params) == 0u);
}

TEST_CASE("FromConfig scales DIP to device pixels", "[layout]") {
    DockConfig config;
    config.icon_size = 48;
    config.gap = 8;

    const auto at100 = dockforge::dock::FromConfig(config, 1.0f);
    CHECK(at100.icon_size == 48);
    CHECK(at100.gap == 8);

    const auto at150 = dockforge::dock::FromConfig(config, 1.5f);
    CHECK(at150.icon_size == 72);
    CHECK(at150.gap == 12);

    const auto at200 = dockforge::dock::FromConfig(config, 2.0f);
    CHECK(at200.icon_size == 96);
    CHECK(at200.gap == 16);

    // A zero or negative scale must not produce a zero-size icon (divide by zero
    // downstream in the renderer).
    const auto degenerate = dockforge::dock::FromConfig(config, 0.0f);
    CHECK(degenerate.icon_size >= 1);
    CHECK(degenerate.gap >= 0);
}

TEST_CASE("hover width covers the drawn extent, not just the resting row", "[layout]") {
    const LayoutParams params = Defaults();
    LayoutSummary summary;
    const auto icons = LayoutDock(params, 9, 250.0f, true, &summary);
    REQUIRE(icons.size() == 9);

    // The summary must describe the icons actually written, not a recomputation
    // that could drift out of sync with them.
    float lo = icons[0].center - icons[0].width * 0.5f;
    float hi = icons[0].center + icons[0].width * 0.5f;
    for (const auto& icon : icons) {
        lo = std::min(lo, icon.center - icon.width * 0.5f);
        hi = std::max(hi, icon.center + icon.width * 0.5f);
    }
    CHECK(summary.min_extent == Catch::Approx(lo).margin(1e-3));
    CHECK(summary.max_extent == Catch::Approx(hi).margin(1e-3));
    CHECK(summary.hover_width == Catch::Approx(hi - lo).margin(1e-3));

    // Magnifying in the middle pushes the outer icons outward, so the hovered
    // content is genuinely wider than the resting row.
    CHECK(summary.hover_width > summary.base_width);
}
