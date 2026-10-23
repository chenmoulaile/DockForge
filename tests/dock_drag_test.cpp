// DockForge - internal reorder tests (T-013).
//
// The controller is deliberately free of window handles, so the whole gesture -
// threshold, insertion slot, eject, cancel - is tested by calling methods instead of
// driving a mouse. That is the only way these rules get exercised on every commit:
// a UI test would cover one path on one desktop.
#include <catch_amalgamated.hpp>

#include <string>
#include <vector>

#include "dock/drag_controller.h"

namespace dock = dockforge::dock;

namespace {

// Five slots, 48 DIP icons with 8 DIP gaps: centres at 24, 80, 136, 192, 248.
std::vector<float> FiveCenters() { return {24.0f, 80.0f, 136.0f, 192.0f, 248.0f}; }

constexpr float kWindowW = 296.0f;
constexpr float kWindowH = 106.4f;

}  // namespace

TEST_CASE("ToString covers every drag phase", "[dock][drag]") {
    CHECK(std::string(dock::ToString(dock::DragPhase::Idle)) == "idle");
    CHECK(std::string(dock::ToString(dock::DragPhase::Pending)) == "pending");
    CHECK(std::string(dock::ToString(dock::DragPhase::Dragging)) == "dragging");
    CHECK(std::string(dock::ToString(dock::DragPhase::Ejecting)) == "ejecting");
}

TEST_CASE("a press alone stays pending and ends as a click", "[dock][drag]") {
    dock::DragController drag;
    drag.Begin(2, 136.0f, 60.0f);
    CHECK(drag.State().phase == dock::DragPhase::Pending);
    CHECK_FALSE(drag.Active());

    const auto result = drag.End();
    // A press with no movement must be a click: the caller runs the normal
    // activate-or-minimize path, not a reorder.
    CHECK(result.kind == dock::DragController::Result::Kind::Click);
    CHECK(result.source_index == 2);
    CHECK(drag.State().phase == dock::DragPhase::Idle);
}

TEST_CASE("movement under the threshold is still a click", "[dock][drag]") {
    dock::DragController drag;
    drag.Begin(1, 80.0f, 60.0f);
    // 3 DIP of jitter - a physical button on a real mouse produces this much.
    drag.Move(83.0f, 61.0f, FiveCenters(), kWindowW, kWindowH);
    CHECK(drag.State().phase == dock::DragPhase::Pending);

    const auto result = drag.End();
    CHECK(result.kind == dock::DragController::Result::Kind::Click);
}

TEST_CASE("crossing the threshold starts a drag", "[dock][drag]") {
    dock::DragController drag;
    drag.Begin(1, 80.0f, 60.0f);
    drag.Move(85.0f, 60.0f, FiveCenters(), kWindowW, kWindowH);  // 5 DIP > 4
    CHECK(drag.State().phase == dock::DragPhase::Dragging);
    CHECK(drag.Active());
}

TEST_CASE("the threshold is measured from the press, not per frame",
          "[dock][drag]") {
    // A slow drag moves a fraction of a DIP per frame. Measuring per frame would
    // never accumulate past the threshold, so the gesture would never start.
    dock::DragController drag;
    drag.Begin(1, 80.0f, 60.0f);
    for (int i = 1; i <= 10; ++i) {
        drag.Move(80.0f + static_cast<float>(i), 60.0f, FiveCenters(), kWindowW, kWindowH);
    }
    CHECK(drag.State().phase == dock::DragPhase::Dragging);
}

TEST_CASE("a drag that returns to its own slot is a click", "[dock][drag]") {
    dock::DragController drag;
    drag.Begin(2, 136.0f, 60.0f);
    drag.Move(200.0f, 60.0f, FiveCenters(), kWindowW, kWindowH);
    drag.Move(136.0f, 60.0f, FiveCenters(), kWindowW, kWindowH);

    const auto result = drag.End();
    // Dragged out and back: the user changed their mind. Reordering here would be a
    // no-op that still rewrites the config.
    CHECK(result.kind == dock::DragController::Result::Kind::Click);
    CHECK(result.insert_index == result.source_index);
}

TEST_CASE("InsertionIndexFor uses the slot centres as swap points",
          "[dock][drag]") {
    const auto centers = FiveCenters();
    // Dragging slot 0: the swap point is the midpoint between neighbours, so the
    // insertion index only advances once the pointer passes a centre.
    CHECK(dock::InsertionIndexFor(10.0f, centers, 0) == 0);
    CHECK(dock::InsertionIndexFor(30.0f, centers, 0) == 0);   // past centre 0 only
    CHECK(dock::InsertionIndexFor(90.0f, centers, 0) == 1);   // past centres 0,1
    CHECK(dock::InsertionIndexFor(200.0f, centers, 0) == 3);
    CHECK(dock::InsertionIndexFor(999.0f, centers, 0) == 4);
}

TEST_CASE("InsertionIndexFor never counts the dragged tile as passed",
          "[dock][drag]") {
    const auto centers = FiveCenters();
    // Dragging slot 3 to the far left: the pointer is past centre 0 only, so the tile
    // lands at index 1. Counting slot 3 itself would push it to index 2.
    CHECK(dock::InsertionIndexFor(-50.0f, centers, 3) == 0);
    CHECK(dock::InsertionIndexFor(30.0f, centers, 3) == 1);
    CHECK(dock::InsertionIndexFor(140.0f, centers, 3) == 3);
}

TEST_CASE("InsertionIndexFor is clamped to the tile range", "[dock][drag]") {
    const auto centers = FiveCenters();
    CHECK(dock::InsertionIndexFor(-9999.0f, centers, 2) == 0);
    CHECK(dock::InsertionIndexFor(9999.0f, centers, 2) == 4);
}

TEST_CASE("InsertionIndexFor handles an empty and a single-slot dock",
          "[dock][drag]") {
    const std::vector<float> empty;
    CHECK(dock::InsertionIndexFor(100.0f, empty, 0) == 0);

    const std::vector<float> single{50.0f};
    CHECK(dock::InsertionIndexFor(0.0f, single, 0) == 0);
    CHECK(dock::InsertionIndexFor(9999.0f, single, 0) == 0);
}

TEST_CASE("reordering reports the source and destination slots", "[dock][drag]") {
    dock::DragController drag;
    drag.Begin(1, 80.0f, 60.0f);
    drag.Move(200.0f, 60.0f, FiveCenters(), kWindowW, kWindowH);

    const auto result = drag.End();
    CHECK(result.kind == dock::DragController::Result::Kind::Reorder);
    CHECK(result.source_index == 1);
    CHECK(result.insert_index == 3);
}

TEST_CASE("dragging above the window starts a removal confirmation", "[dock][drag]") {
    dock::DragController drag;
    drag.Begin(2, 136.0f, 60.0f);
    drag.Move(136.0f, -100.0f, FiveCenters(), kWindowW, kWindowH);
    CHECK(drag.State().phase == dock::DragPhase::Ejecting);
    CHECK(drag.State().eject_progress == 0.0f);

    // Letting go immediately must NOT remove. This is the whole reason removal is a
    // hold: the dock cannot show a confirmation dialog without stealing focus, so the
    // escape hatch has to be "release early".
    const auto result = drag.End();
    CHECK(result.kind == dock::DragController::Result::Kind::Cancel);
}

TEST_CASE("holding in the eject zone completes the removal", "[dock][drag]") {
    dock::DragController drag;
    drag.Begin(2, 136.0f, 60.0f);
    drag.Move(136.0f, -100.0f, FiveCenters(), kWindowW, kWindowH);
    REQUIRE(drag.State().phase == dock::DragPhase::Ejecting);

    // Progress must advance monotonically and reach exactly 1.0 at the threshold, so
    // the ghost can fade out over a known interval.
    CHECK(drag.Tick(dock::kEjectConfirmMs * 0.5));
    CHECK(drag.State().eject_progress == Catch::Approx(0.5f));
    CHECK(drag.Tick(dock::kEjectConfirmMs * 0.5));
    CHECK(drag.State().eject_progress == Catch::Approx(1.0f));

    // Past the threshold the tick stops reporting changes, otherwise holding a tile
    // outside the dock would spin the frame loop forever.
    CHECK_FALSE(drag.Tick(16.0));

    const auto result = drag.End();
    CHECK(result.kind == dock::DragController::Result::Kind::Remove);
}

TEST_CASE("tick outside the eject zone does nothing", "[dock][drag]") {
    dock::DragController drag;
    CHECK_FALSE(drag.Tick(16.0));

    drag.Begin(2, 136.0f, 60.0f);
    drag.Move(200.0f, 60.0f, FiveCenters(), kWindowW, kWindowH);
    REQUIRE(drag.State().phase == dock::DragPhase::Dragging);
    // A reorder must never accumulate eject progress: the two are different gestures
    // and only one of them can delete data.
    CHECK_FALSE(drag.Tick(1000.0));
    CHECK(drag.State().eject_progress == 0.0f);
}

TEST_CASE("leaving the eject zone restarts the confirmation", "[dock][drag]") {
    // Accumulating across a slip out and back in would let a wobble delete a tile,
    // which is the exact failure the confirmation exists to prevent.
    dock::DragController drag;
    drag.Begin(2, 136.0f, 60.0f);
    drag.Move(136.0f, -100.0f, FiveCenters(), kWindowW, kWindowH);
    REQUIRE(drag.State().phase == dock::DragPhase::Ejecting);
    drag.Tick(dock::kEjectConfirmMs * 0.9);
    CHECK(drag.State().eject_progress > 0.8f);

    drag.Move(136.0f, 60.0f, FiveCenters(), kWindowW, kWindowH);
    CHECK(drag.State().phase == dock::DragPhase::Dragging);
    CHECK(drag.State().eject_hold_ms == 0.0);
    CHECK(drag.State().eject_progress == 0.0f);

    drag.Move(136.0f, -100.0f, FiveCenters(), kWindowW, kWindowH);
    CHECK(drag.State().phase == dock::DragPhase::Ejecting);
    CHECK(drag.State().eject_progress == 0.0f);

    // And the gesture now resolves as a click-equivalent cancel, not a removal.
    const auto result = drag.End();
    CHECK(result.kind == dock::DragController::Result::Kind::Cancel);
}

TEST_CASE("dragging sideways past the eject margin ejects", "[dock][drag]") {
    dock::DragController drag;
    drag.Begin(2, 136.0f, 60.0f);
    drag.Move(kWindowW + dock::kDragEjectDip + 1.0f, 60.0f, FiveCenters(), kWindowW,
              kWindowH);
    CHECK(drag.State().phase == dock::DragPhase::Ejecting);

    drag.Begin(2, 136.0f, 60.0f);
    drag.Move(-dock::kDragEjectDip - 1.0f, 60.0f, FiveCenters(), kWindowW, kWindowH);
    CHECK(drag.State().phase == dock::DragPhase::Ejecting);
}

TEST_CASE("eject is reversible", "[dock][drag]") {
    // Making removal one-way would trap the user in a deletion they did not intend:
    // overshooting upward is easy when the icons are only 106 DIP tall.
    dock::DragController drag;
    drag.Begin(2, 136.0f, 60.0f);
    drag.Move(136.0f, -200.0f, FiveCenters(), kWindowW, kWindowH);
    CHECK(drag.State().phase == dock::DragPhase::Ejecting);

    drag.Move(136.0f, 60.0f, FiveCenters(), kWindowW, kWindowH);
    CHECK(drag.State().phase == dock::DragPhase::Dragging);

    const auto result = drag.End();
    CHECK(result.kind == dock::DragController::Result::Kind::Click);
}

TEST_CASE("dropping below the window does not eject", "[dock][drag]") {
    // The dock sits on the bottom edge of the work area. A downward slip is the
    // easiest accidental movement there is, and treating it as removal would delete
    // tiles on a mis-click.
    CHECK_FALSE(dock::IsEjectPosition(136.0f, kWindowH + 200.0f, kWindowW, kWindowH));
}

TEST_CASE("IsEjectPosition needs a real window size", "[dock][drag]") {
    // A zero-sized geometry means the window has not been placed yet; every position
    // would look "outside". Reporting eject there would delete a tile during startup.
    CHECK_FALSE(dock::IsEjectPosition(-1000.0f, -1000.0f, 0.0f, 0.0f));
    CHECK_FALSE(dock::IsEjectPosition(-1000.0f, -1000.0f, 0.0f, kWindowH));
    CHECK_FALSE(dock::IsEjectPosition(-1000.0f, -1000.0f, kWindowW, 0.0f));
}

TEST_CASE("Cancel aborts without reordering or clicking", "[dock][drag]") {
    dock::DragController drag;
    drag.Begin(2, 136.0f, 60.0f);
    drag.Move(200.0f, 60.0f, FiveCenters(), kWindowW, kWindowH);
    REQUIRE(drag.Active());

    drag.Cancel();
    CHECK(drag.State().phase == dock::DragPhase::Idle);
    CHECK(drag.State().source_index == -1);
    // End after Cancel must be a no-op, not a reorder to the remembered slot.
    const auto result = drag.End();
    CHECK(result.kind == dock::DragController::Result::Kind::None);
}

TEST_CASE("Begin with a negative index is a no-op", "[dock][drag]") {
    dock::DragController drag;
    drag.Begin(-1, 10.0f, 10.0f);
    CHECK(drag.State().phase == dock::DragPhase::Idle);
    CHECK(drag.State().source_index == -1);
}

TEST_CASE("Move while idle changes nothing", "[dock][drag]") {
    dock::DragController drag;
    drag.Move(500.0f, 500.0f, FiveCenters(), kWindowW, kWindowH);
    CHECK(drag.State().phase == dock::DragPhase::Idle);
    CHECK(drag.State().source_index == -1);
}

TEST_CASE("a second Begin resets the previous gesture", "[dock][drag]") {
    dock::DragController drag;
    drag.Begin(0, 24.0f, 60.0f);
    drag.Move(200.0f, 60.0f, FiveCenters(), kWindowW, kWindowH);
    REQUIRE(drag.Active());

    drag.Begin(4, 248.0f, 60.0f);
    CHECK(drag.State().phase == dock::DragPhase::Pending);
    CHECK(drag.State().source_index == 4);
    CHECK(drag.State().insert_index == 4);
}

TEST_CASE("the ghost keeps following the pointer while ejecting",
          "[dock][drag]") {
    // The ghost position is what the user sees while deciding whether to let go.
    // Freezing it at the last in-window position would make the dock look stuck at
    // exactly the moment the gesture is most uncertain.
    dock::DragController drag;
    drag.Begin(2, 136.0f, 60.0f);
    drag.Move(136.0f, -300.0f, FiveCenters(), kWindowW, kWindowH);
    CHECK(drag.State().phase == dock::DragPhase::Ejecting);
    CHECK(drag.State().pointer_y_dip == Catch::Approx(-300.0f));
}
