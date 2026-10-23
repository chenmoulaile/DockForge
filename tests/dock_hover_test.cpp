// T-010: hover ramp and clickable-region geometry.
//
// The ramp and the region are the two places where a bug is invisible in a
// screenshot: a hover that snaps still looks right in a still frame, and a region
// that is too large only shows up as "clicks near the dock do nothing".
#include <windows.h>

#include "dock/dock_view.h"
#include "platform/hit_region.h"

#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstddef>

using namespace dockforge;

namespace {

// 60 FPS frames worth of milliseconds, the cadence the ramp is tuned against.
constexpr double kFrameMs = 1000.0 / 60.0;

}  // namespace

TEST_CASE("hover intensity ramps up and down instead of snapping", "[dock][hover]") {
    dock::DockHover hover;
    CHECK(hover.intensity == 0.0f);

    hover.active = true;
    // One frame must not jump to full: the whole point of the ramp.
    CHECK(dock::AdvanceHover(hover, kFrameMs));
    CHECK(hover.intensity > 0.0f);
    CHECK(hover.intensity < 0.5f);

    // Reaching full magnification takes about 125ms at 8/s.
    int frames = 1;
    while (hover.intensity < 1.0f && frames < 100) {
        dock::AdvanceHover(hover, kFrameMs);
        ++frames;
    }
    CHECK(hover.intensity == 1.0f);
    CHECK(frames >= 6);
    CHECK(frames <= 10);

    // At the target the ramp reports no change, which is what lets the frame loop
    // go back to sleep instead of repainting forever.
    CHECK_FALSE(dock::AdvanceHover(hover, kFrameMs));
    CHECK(hover.intensity == 1.0f);

    hover.active = false;
    CHECK(dock::AdvanceHover(hover, kFrameMs));
    CHECK(hover.intensity < 1.0f);
    while (hover.intensity > 0.0f) {
        dock::AdvanceHover(hover, kFrameMs);
    }
    CHECK(hover.intensity == 0.0f);
    CHECK_FALSE(dock::AdvanceHover(hover, kFrameMs));
}

TEST_CASE("hover ramp ignores a non-positive frame delta", "[dock][hover]") {
    dock::DockHover hover;
    hover.active = true;
    CHECK_FALSE(dock::AdvanceHover(hover, 0.0));
    CHECK_FALSE(dock::AdvanceHover(hover, -16.0));
    CHECK(hover.intensity == 0.0f);
}

TEST_CASE("hover ramp never overshoots its target", "[dock][hover]") {
    dock::DockHover hover;
    hover.active = true;
    // A 10 second stall must clamp to 1.0, not to 80.
    CHECK(dock::AdvanceHover(hover, 10000.0));
    CHECK(hover.intensity == 1.0f);

    hover.active = false;
    CHECK(dock::AdvanceHover(hover, 10000.0));
    CHECK(hover.intensity == 0.0f);
}

TEST_CASE("dock geometry reserves room for the tallest icon", "[dock][hover]") {
    core::Config config;
    config.dock.icon_size = 48;
    config.dock.gap = 8;
    config.dock.magnification = 1.8f;

    const auto geometry = dock::ComputeGeometry(config, 12, 1.0f);

    // Plate height is the idle icon plus padding; it does not grow on hover.
    CHECK(geometry.plate_height_dip == Catch::Approx(68.0f));
    // The window has to be tall enough for a fully magnified icon (48*1.8 = 86.4)
    // plus the same padding, otherwise the top of the magnified icon is clipped.
    CHECK(geometry.window_height_dip == Catch::Approx(106.4f));
    // Width has to hold the magnified row, so it is strictly wider than the plate.
    CHECK(geometry.window_width_dip > geometry.plate_width_dip);
}

TEST_CASE("dock geometry grows with DPI and stays proportional", "[dock][hover]") {
    core::Config config;
    config.dock.icon_size = 48;
    config.dock.gap = 8;
    config.dock.magnification = 2.0f;

    const auto at96 = dock::ComputeGeometry(config, 8, 1.0f);
    const auto at192 = dock::ComputeGeometry(config, 8, 2.0f);

    // Device-pixel content doubles, DIP content is unchanged.
    CHECK(at96.content_width_dip == Catch::Approx(at192.content_width_dip));
    CHECK(at96.window_height_dip == Catch::Approx(at192.window_height_dip));
    // A 2x magnification needs the same DIP box regardless of DPI, because the
    // window is measured in DIPs and scaled by the OS.
    CHECK(at96.window_width_dip == Catch::Approx(at192.window_width_dip));
}

TEST_CASE("dock geometry survives a degenerate config", "[dock][hover]") {
    core::Config config;
    config.dock.icon_size = 0;
    config.dock.magnification = 0.0f;
    // Must not divide by zero or produce a negative window.
    const auto geometry = dock::ComputeGeometry(config, 0, 0.0f);
    CHECK(geometry.scale == 1.0f);
    CHECK(geometry.window_width_dip >= 0.0f);
    CHECK(geometry.window_height_dip >= 0.0f);
}

TEST_CASE("dock region excludes the empty band above the plate when idle",
          "[dock][region]") {
    const RECT plate{0, 76, 700, 106};
    const RECT row{0, 0, 0, 0};
    HRGN region = platform::CreateDockRegion(plate, 22, row, false);
    REQUIRE(region != nullptr);

    // Inside the plate: clickable.
    CHECK(PtInRegion(region, 350, 90));
    // Above the plate, in the reserved band the magnified icons will use: not
    // clickable, so the click reaches the window underneath.
    CHECK_FALSE(PtInRegion(region, 350, 20));
    // Outside the window entirely.
    CHECK_FALSE(PtInRegion(region, -5, 90));
    CHECK_FALSE(PtInRegion(region, 705, 90));
    DeleteObject(region);
}

TEST_CASE("dock region includes the icon row while magnified", "[dock][region]") {
    const RECT plate{0, 76, 700, 106};
    const RECT row{120, 40, 580, 106};
    HRGN region = platform::CreateDockRegion(plate, 22, row, true);
    REQUIRE(region != nullptr);

    // The overhang is clickable now.
    CHECK(PtInRegion(region, 350, 50));
    // Beside the row, still dead space.
    CHECK_FALSE(PtInRegion(region, 20, 50));
    CHECK_FALSE(PtInRegion(region, 660, 50));
    DeleteObject(region);
}

TEST_CASE("dock region clips the rounded corners", "[dock][region]") {
    const RECT plate{0, 0, 200, 100};
    HRGN region = platform::CreateDockRegion(plate, 30, RECT{0, 0, 0, 0}, false);
    REQUIRE(region != nullptr);

    // Dead centre of the corner radius is outside a 30px rounded rect.
    CHECK_FALSE(PtInRegion(region, 2, 2));
    CHECK_FALSE(PtInRegion(region, 197, 97));
    // The middle of the edge is inside.
    CHECK(PtInRegion(region, 100, 2));
    CHECK(PtInRegion(region, 2, 50));
    DeleteObject(region);
}

TEST_CASE("dock region clamps a radius larger than the plate", "[dock][region]") {
    const RECT plate{0, 0, 40, 20};
    // A 500px radius on a 40x20 plate must not produce an empty or inverted region.
    HRGN region = platform::CreateDockRegion(plate, 500, RECT{0, 0, 0, 0}, false);
    REQUIRE(region != nullptr);
    CHECK(PtInRegion(region, 20, 10));
    DeleteObject(region);
}

TEST_CASE("dock region rejects a degenerate plate", "[dock][region]") {
    CHECK(platform::CreateDockRegion(RECT{0, 0, 0, 0}, 10, RECT{0, 0, 0, 0}, false) == nullptr);
    CHECK(platform::CreateDockRegion(RECT{0, 0, -5, 20}, 10, RECT{0, 0, 0, 0}, false) == nullptr);
}

TEST_CASE("hover ramp coalesces rapid moves into one layout per frame", "[dock][hover]") {
    // Windows delivers mouse moves far faster than 60 Hz. The tick handler advances
    // the ramp once per frame no matter how many moves arrived, which is what keeps
    // the animation inside the frame budget: the alternative (one repaint per move)
    // measured p99 18.2ms against a 16.7ms budget.
    dock::DockHover hover;
    hover.active = true;

    // 20 moves inside one frame: the ramp advances by exactly one frame's worth.
    for (int i = 0; i < 20; ++i) {
        hover.mouse_x_dip = 100.0f + static_cast<float>(i);
    }
    CHECK(dock::AdvanceHover(hover, kFrameMs));
    const float after_one_frame = hover.intensity;
    CHECK(after_one_frame == Catch::Approx(dock::kHoverRampPerSecond * kFrameMs / 1000.0f));

    // A second frame advances by the same amount, not by 20x.
    CHECK(dock::AdvanceHover(hover, kFrameMs));
    CHECK(hover.intensity == Catch::Approx(after_one_frame * 2.0f));
}

TEST_CASE("hover ramp stays smooth at a variable frame rate", "[dock][hover]") {
    dock::DockHover fast;
    fast.active = true;
    dock::DockHover slow;
    slow.active = true;

    // 10 frames at 120 Hz vs 5 frames at 60 Hz cover the same wall-clock time.
    for (int i = 0; i < 10; ++i) dock::AdvanceHover(fast, 1000.0 / 120.0);
    for (int i = 0; i < 5; ++i) dock::AdvanceHover(slow, 1000.0 / 60.0);

    // Same elapsed time must produce the same intensity: the ramp is defined in
    // time, not in frames, so a slow frame cannot make it jump.
    CHECK(fast.intensity == Catch::Approx(slow.intensity));
}

TEST_CASE("an empty row does not widen the region", "[dock][region]") {
    const RECT plate{0, 76, 700, 106};
    // include_row set but the row has no area: must behave like the idle case
    // rather than OR-ing in a zero-area or inverted rectangle.
    HRGN region = platform::CreateDockRegion(plate, 22, RECT{350, 90, 350, 106}, true);
    REQUIRE(region != nullptr);
    CHECK(PtInRegion(region, 350, 90));
    CHECK_FALSE(PtInRegion(region, 350, 20));
    DeleteObject(region);
}

TEST_CASE("idle slot centres do not move when the row magnifies", "[dock][drag]") {
    // This is the regression test for the feedback loop that made drags land in the
    // wrong slot: the insertion index was derived from the magnified centres, so
    // moving the pointer right magnified the icons on the right, which pushed their
    // centres further right, which changed which slot the pointer had "passed".
    // Measured on a real drag: 0 -> 4 reported 0 -> 3, and the answer depended on
    // where the drag started.
    //
    // The idle grid is a pure function of the config, so it must be identical no
    // matter what the last layout happened to be.
    core::Config config;
    config.dock.icon_size = 48;
    config.dock.gap = 8;
    config.dock.magnification = 1.8f;

    const auto geometry = dock::ComputeGeometry(config, 5, 1.0f);
    dock::DockPainter painter;

    dock::DockHover idle;
    painter.UpdateLayout(geometry, idle, 5);
    const auto at_rest = painter.IdleCentersDip(1.0f);

    dock::DockHover hovering;
    hovering.active = true;
    hovering.intensity = 1.0f;
    hovering.mouse_x_dip = 300.0f;
    painter.UpdateLayout(geometry, hovering, 5);
    const auto while_hovering = painter.IdleCentersDip(1.0f);

    REQUIRE(at_rest.size() == 5);
    REQUIRE(while_hovering.size() == at_rest.size());
    for (std::size_t i = 0; i < at_rest.size(); ++i) {
        CHECK(while_hovering[i] == Catch::Approx(at_rest[i]));
    }

    // And they must be the idle pitch: icon 48 + gap 8 = 56 apart, first centre half
    // an icon in from the row origin.
    const float pitch = 56.0f;
    for (std::size_t i = 1; i < at_rest.size(); ++i) {
        CHECK(at_rest[i] - at_rest[i - 1] == Catch::Approx(pitch));
    }

    // The magnified centres, by contrast, must differ - otherwise this test would
    // pass on a build where magnification never ran and prove nothing.
    const auto magnified = painter.IconCentersDip(1.0f);
    REQUIRE(magnified.size() == at_rest.size());
    bool any_difference = false;
    for (std::size_t i = 0; i < at_rest.size(); ++i) {
        if (std::abs(magnified[i] - at_rest[i]) > 0.5f) any_difference = true;
    }
    CHECK(any_difference);
}
