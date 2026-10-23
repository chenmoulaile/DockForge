#include "dock/drag_controller.h"

#include <algorithm>
#include <cmath>

namespace dockforge::dock {

const char* ToString(DragPhase phase) {
    switch (phase) {
        case DragPhase::Idle:
            return "idle";
        case DragPhase::Pending:
            return "pending";
        case DragPhase::Dragging:
            return "dragging";
        case DragPhase::Ejecting:
            return "ejecting";
    }
    return "idle";
}

namespace {

float DistanceSquared(float ax, float ay, float bx, float by) {
    const float dx = ax - bx;
    const float dy = ay - by;
    return dx * dx + dy * dy;
}

}  // namespace

void DragController::Begin(int tile_index, float x_dip, float y_dip) {
    state_ = DragState{};
    if (tile_index < 0) return;
    state_.phase = DragPhase::Pending;
    state_.source_index = tile_index;
    state_.insert_index = tile_index;
    state_.pointer_x_dip = x_dip;
    state_.pointer_y_dip = y_dip;
    state_.press_x_dip = x_dip;
    state_.press_y_dip = y_dip;
}

void DragController::Move(float x_dip, float y_dip, const std::vector<float>& centers_dip,
                          float window_width_dip, float window_height_dip) {
    if (state_.phase == DragPhase::Idle) return;

    state_.pointer_x_dip = x_dip;
    state_.pointer_y_dip = y_dip;

    if (state_.phase == DragPhase::Pending) {
        // The threshold is measured from the press point, not from the previous
        // frame: measuring per-frame would let a slow drag never start, because each
        // individual step stays under 4 DIP.
        const float moved = DistanceSquared(x_dip, y_dip, state_.press_x_dip,
                                           state_.press_y_dip);
        if (moved < kDragThresholdDip * kDragThresholdDip) return;
        state_.phase = DragPhase::Dragging;
    }

    // Eject is checked on every move, and it is reversible: coming back inside turns
    // the gesture back into a reorder. Making it one-way would trap the user in a
    // removal they did not intend.
    const bool eject = IsEjectPosition(x_dip, y_dip, window_width_dip, window_height_dip);
    const DragPhase next_phase = eject ? DragPhase::Ejecting : DragPhase::Dragging;
    if (next_phase != state_.phase) {
        // Entering or leaving the eject zone restarts the confirmation. A hold that
        // could be accumulated across a slip out and back in would let a wobble delete
        // a tile, which is the exact failure the confirmation exists to prevent.
        state_.eject_hold_ms = 0.0;
        state_.eject_progress = 0.0f;
        state_.phase = next_phase;
    }

    // The insertion slot is still computed while ejecting so the ghost keeps
    // following the pointer and the dock shows where the tile came from. It is
    // ignored on release.
    state_.insert_index = InsertionIndexFor(x_dip, centers_dip, state_.source_index);
}

bool DragController::Tick(double delta_ms) {
    if (state_.phase != DragPhase::Ejecting) return false;
    if (!(delta_ms > 0.0)) return false;

    const double before_hold = state_.eject_hold_ms;
    const float before_progress = state_.eject_progress;

    state_.eject_hold_ms += delta_ms;
    const double clamped = std::min(state_.eject_hold_ms, kEjectConfirmMs);
    state_.eject_progress = static_cast<float>(clamped / kEjectConfirmMs);

    // Reports "changed" only while the progress is actually moving, so a tile held
    // past the threshold stops asking for repaints instead of spinning the frame loop
    // for as long as the user keeps holding.
    return state_.eject_hold_ms != before_hold && state_.eject_progress != before_progress;
}

DragController::Result DragController::End() {
    Result result;
    result.source_index = state_.source_index;
    result.insert_index = state_.insert_index;

    switch (state_.phase) {
        case DragPhase::Idle:
            result.kind = Result::Kind::None;
            break;
        case DragPhase::Pending:
            // Never crossed the threshold: this was a click, and the caller must run
            // the normal click action instead of reordering.
            result.kind = Result::Kind::Click;
            break;
        case DragPhase::Dragging:
            result.kind = result.insert_index == result.source_index
                              ? Result::Kind::Click
                              : Result::Kind::Reorder;
            break;
        case DragPhase::Ejecting:
            // Removal needs the hold to have completed. Letting go early is the escape
            // hatch: the tile stays where it was and nothing is written to the config,
            // which is the whole point of confirming with a hold instead of a dialog
            // (the dock cannot show a dialog without stealing focus).
            result.kind = state_.eject_hold_ms >= kEjectConfirmMs ? Result::Kind::Remove
                                                                  : Result::Kind::Cancel;
            break;
    }

    state_ = DragState{};
    return result;
}

void DragController::Cancel() {
    // No reorder, no click: the system took the capture, so the user's intent is
    // unknown. Doing nothing is the only safe answer.
    state_ = DragState{};
}

int InsertionIndexFor(float pointer_x_dip, const std::vector<float>& centers_dip,
                      int source_index) {
    if (centers_dip.empty()) return source_index;

    // Walk the slot centres and count how many the pointer has reached. Each centre
    // the pointer is on or past pushes the insertion point one slot right.
    //
    // Inclusive (`>=`, not `>`): a pointer sitting exactly on a slot's centre is over
    // that icon, and dragging onto an icon means taking its place. Exclusive counting
    // lands the tile one slot short of the icon being aimed at - which is invisible on
    // a synthetic pointer that never stops exactly on a centre, and always wrong for a
    // user who does.
    int passed = 0;
    for (const float center : centers_dip) {
        if (pointer_x_dip >= center) ++passed;
    }

    const int count = static_cast<int>(centers_dip.size());
    // `passed` counts centres strictly left of the pointer. When the dragged tile is
    // itself left of the pointer it must not count itself, or every drag would jump
    // one extra slot.
    if (source_index >= 0 && source_index < passed) --passed;

    return std::clamp(passed, 0, count - 1);
}

bool IsEjectPosition(float x_dip, float y_dip, float window_width_dip,
                     float window_height_dip) {
    if (!(window_width_dip > 0.0f) || !(window_height_dip > 0.0f)) return false;
    // Only the top and the sides eject. Below the window is the taskbar or the screen
    // edge, and a dock pinned to the bottom edge has no space there anyway; treating
    // a downward slip as removal would delete tiles on a mis-click.
    const bool above = y_dip < -kDragEjectDip;
    const bool left = x_dip < -kDragEjectDip;
    const bool right = x_dip > window_width_dip + kDragEjectDip;
    return above || left || right;
}

}  // namespace dockforge::dock
