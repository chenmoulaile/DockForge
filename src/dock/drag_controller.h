// DockForge - internal tile reordering (T-013).
//
// Dragging a tile inside the dock is a gesture, not a drag-and-drop operation: no
// OLE, no data object, no cross-process transfer. That distinction matters because
// the obvious tool - IDropTarget - cannot express "move this tile three slots left",
// and RegisterDragDrop would route the pointer through the shell's drop manager,
// which is slower than a capture and adds nothing.
//
// What the spike established (spikes/t013_drag/README.md):
//   * SetCapture keeps WM_MOUSEMOVE coming after the pointer leaves the window, so
//     the gesture survives the icon being lifted above the dock.
//   * A window region does not clip captured messages - only composition content.
//     The region still has to cover the whole window during a drag, or the lifted
//     ghost gets cut off.
//   * WM_MOUSELEAVE does not arrive while a button is held, so the hover handler
//     cannot cancel the drag by accident. WM_CANCELMODE can still arrive and means
//     "the capture was taken away" - that aborts the drag.
#pragma once

#include <windows.h>

#include <cstddef>
#include <vector>

namespace dockforge::dock {

// Threshold before a press becomes a drag, in DIP.
//
// Without it every click is a zero-length drag: the tile would reorder under a
// stationary finger and a click on a tile that has drifted one pixel would register
// as a reorder. 4 DIP is small enough to feel immediate and large enough to absorb
// the jitter of a physical mouse button.
inline constexpr float kDragThresholdDip = 4.0f;

// How far outside the plate the pointer may go before the drag becomes a removal
// gesture rather than a reorder, in DIP. Measured from the window edge.
inline constexpr float kDragEjectDip = 48.0f;

// How long the tile must be held outside the plate before releasing removes it, in
// milliseconds.
//
// This is the confirmation the removal needs, and it is a HOLD rather than a dialog on
// purpose: the dock is WS_EX_NOACTIVATE and never takes focus, so a message box would
// have to steal it - and stealing focus from whatever the user is doing to ask "are you
// sure?" is a worse outcome than the mistake it prevents. A hold is pointer-only, needs
// no focus, and gives the user a way out: let go early and the tile snaps back.
//
// 550ms is long enough that a slip off the bottom edge and an immediate release does
// nothing (a real slip lasts well under 200ms), and short enough that a deliberate
// removal does not feel like waiting.
inline constexpr double kEjectConfirmMs = 550.0;

enum class DragPhase {
    Idle,      // no button, nothing happening
    Pending,   // button down, still inside the threshold
    Dragging,  // past the threshold, tile follows the pointer
    Ejecting,  // past the eject distance, release removes the tile
};

const char* ToString(DragPhase phase);

struct DragState {
    DragPhase phase = DragPhase::Idle;
    // Index of the tile being dragged, or -1. Kept separate from the insertion
    // index so the ghost can be drawn at the pointer while the insertion line
    // stays at the slot the tile would land in.
    int source_index = -1;
    // Slot the tile would be inserted at if released now. Equals source_index when
    // the pointer has not crossed a neighbour's midpoint.
    int insert_index = -1;
    // Pointer position in window DIP. Paint uses this for the ghost; the reorder
    // logic uses it to pick the insert slot.
    float pointer_x_dip = 0.0f;
    float pointer_y_dip = 0.0f;
    // Where the press started, in DIP. The threshold is measured from here rather
    // than from the previous frame, so slow movement still eventually crosses it.
    float press_x_dip = 0.0f;
    float press_y_dip = 0.0f;
    // How long the tile has been held in the eject zone, in milliseconds. Reset the
    // moment the pointer comes back inside, so the confirmation always starts over.
    double eject_hold_ms = 0.0;
    // eject_hold_ms / kEjectConfirmMs, clamped to 0..1. Paint fades the ghost with it
    // so the hold is visible instead of a silent wait.
    float eject_progress = 0.0f;
};

// Tracks one gesture. The caller feeds it pointer events and reads the phase back;
// this type never touches the window, the tiles, or the config, which is what makes
// the reorder rules testable without a desktop.
class DragController {
  public:
    // Button pressed on `tile_index` at `(x, y)` in window DIP.
    void Begin(int tile_index, float x_dip, float y_dip);

    // Pointer moved to `(x, y)`. `centers_dip` are the IDLE slot centres, used to
    // pick the insertion slot.
    //
    // Idle, not the live magnified row: the magnified layout is a function of the
    // pointer, so deriving the slot from it closes a feedback loop - moving right
    // magnifies the icons on the right, which pushes their centres further right,
    // which changes which slot the pointer is past. Measured: the same gesture
    // reported slot 3 or slot 4 depending on the layout it was fed. The idle grid is
    // fixed, so a drag lands in the same place every time.
    void Move(float x_dip, float y_dip, const std::vector<float>& centers_dip,
              float window_width_dip, float window_height_dip);

    // Advances the eject confirmation by one frame. Called from the tick, not from
    // Move: the hold has to keep counting while the pointer is still, and Move only
    // fires on motion. Returns true when the visible state changed (progress moved, or
    // the phase settled), so the caller knows whether to repaint.
    bool Tick(double delta_ms);

    // Button released. Returns the resulting action; the state resets to Idle.
    struct Result {
        enum class Kind { None, Click, Reorder, Remove, Cancel } kind = Kind::None;
        int source_index = -1;
        int insert_index = -1;
    };
    Result End();

    // Capture taken away by the system. Aborts without reordering.
    void Cancel();

    const DragState& State() const { return state_; }
    bool Active() const { return state_.phase == DragPhase::Dragging ||
                                 state_.phase == DragPhase::Ejecting; }

  private:
    DragState state_{};
};

// Computes the insertion slot for a pointer position.
//
// The rule: count the slot centres the pointer has passed, minus the dragged tile
// itself, and the result is the index the tile occupies after release. Expressed as a
// function so the tests can state it directly instead of driving a mouse.
//
// The swap point is therefore a slot's CENTRE, not the midpoint between two slots.
// That is what drag-reorder UIs converge on (a tab strip, a file list): the item
// changes places only once the pointer is more over its neighbour's slot than over its
// own, and using midpoints instead makes every drag overshoot by half a slot.
//
// Insertion is "before" the slot under the pointer: dragging the first tile onto the
// fifth reports 4 - 1 = 3, so it lands immediately left of that icon rather than
// displacing it to the right. Consistent with removing the tile from its old slot and
// re-inserting it at the reported index.
int InsertionIndexFor(float pointer_x_dip, const std::vector<float>& centers_dip,
                      int source_index);

// True when the pointer is far enough outside the window to mean "remove".
bool IsEjectPosition(float x_dip, float y_dip, float window_width_dip,
                     float window_height_dip);

}  // namespace dockforge::dock
