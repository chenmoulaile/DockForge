// T-301: Finder Bar layout tests.
//
// The bar itself needs a window and a D2D device, so it cannot be exercised here.
// Everything that decides where things land is a pure function, and that is what
// these cases cover: the geometry arithmetic, the overflow fold, the hit test, and
// the auto-hide curve. A test that needed a desktop would be a probe, not a test.
#include "catch_amalgamated.hpp"

#include "finderbar/bar_layout.h"

#include <string>
#include <vector>

namespace {

using dockforge::finderbar::AutoHideState;
using dockforge::finderbar::BarBox;
using dockforge::finderbar::BarLayoutParams;
using dockforge::finderbar::BarLayoutSummary;
using dockforge::finderbar::kNoMenu;

// A fake measurer with a fixed advance per character, so every expected width in this
// file is arithmetic a reader can redo by hand. Real metrics come from D2D and vary
// with the installed font, which would make the assertions unmaintainable.
float FixedWidth(const wchar_t* text, float size_dip) {
    if (text == nullptr) return 0.0f;
    std::size_t count = 0;
    for (const wchar_t* p = text; *p != L'\0'; ++p) ++count;
    return static_cast<float>(count) * size_dip * 0.5f;
}

std::vector<std::wstring> Menus(std::initializer_list<const wchar_t*> names) {
    std::vector<std::wstring> out;
    for (const wchar_t* name : names) out.emplace_back(name);
    return out;
}

BarLayoutParams Params(float width_dip) {
    BarLayoutParams params;
    params.width_dip = width_dip;
    params.height_dip = 28.0f;
    params.max_menu_items = 6;
    params.status_width_dip = 0.0f;
    return params;
}

}  // namespace

TEST_CASE("T-301 bar: system menu, name and menus are placed left to right") {
    BarLayoutParams params = Params(1200.0f);
    params.app_name = L"Notes";

    BarLayoutSummary summary;
    const auto boxes = dockforge::finderbar::LayoutBar(
        params, Menus({L"File", L"Edit"}), &FixedWidth, &summary);

    REQUIRE(boxes.size() == 4);
    CHECK(boxes[0].is_system_menu);
    CHECK(boxes[1].is_app_name);
    CHECK(boxes[1].label == L"Notes");
    CHECK(boxes[2].label == L"File");
    CHECK(boxes[3].label == L"Edit");
    CHECK(summary.placed_menus == 2);
    CHECK_FALSE(summary.overflowed);

    // Leading inset, then the glyph box, then the system-menu gap.
    CHECK(boxes[0].rect.left == 20.0f);
    // "Notes" at 13 DIP with a 0.5 em fake advance is 5 * 6.5 = 32.5 DIP.
    CHECK(boxes[1].rect.Width() == 32.5f);
    // File/Edit are 4 * 6.5 + 2 * 8 = 42 DIP each.
    CHECK(boxes[2].rect.Width() == 42.0f);
    CHECK(boxes[3].rect.Width() == 42.0f);

    // Boxes never overlap and always advance rightwards.
    for (std::size_t i = 1; i < boxes.size(); ++i) {
        CHECK(boxes[i].rect.left >= boxes[i - 1].rect.right);
    }
}

TEST_CASE("T-301 bar: every box is the full bar height so the whole row is clickable") {
    BarLayoutParams params = Params(1200.0f);
    params.app_name = L"Notes";
    BarLayoutSummary summary;
    const auto boxes = dockforge::finderbar::LayoutBar(
        params, Menus({L"File"}), &FixedWidth, &summary);
    REQUIRE_FALSE(boxes.empty());
    for (const BarBox& box : boxes) {
        CHECK(box.rect.top == 0.0f);
        CHECK(box.rect.bottom == 28.0f);
    }
}

TEST_CASE("T-301 bar: hiding a group removes it and closes the gap") {
    BarLayoutParams full = Params(1200.0f);
    full.app_name = L"Notes";
    BarLayoutSummary full_summary;
    const auto full_boxes = dockforge::finderbar::LayoutBar(
        full, Menus({L"File"}), &FixedWidth, &full_summary);
    REQUIRE(full_boxes.size() == 3);

    BarLayoutParams no_glyph = full;
    no_glyph.show_system_menu = false;
    BarLayoutSummary no_glyph_summary;
    const auto no_glyph_boxes = dockforge::finderbar::LayoutBar(
        no_glyph, Menus({L"File"}), &FixedWidth, &no_glyph_summary);
    REQUIRE(no_glyph_boxes.size() == 2);
    // The name starts at the leading inset instead of after the glyph box.
    CHECK(no_glyph_boxes[0].rect.left == 20.0f);
    CHECK(no_glyph_boxes[0].is_app_name);

    BarLayoutParams no_name = full;
    no_name.show_app_name = false;
    BarLayoutSummary no_name_summary;
    const auto no_name_boxes = dockforge::finderbar::LayoutBar(
        no_name, Menus({L"File"}), &FixedWidth, &no_name_summary);
    REQUIRE(no_name_boxes.size() == 2);
    CHECK(no_name_boxes[0].is_system_menu);
    CHECK(no_name_boxes[1].label == L"File");
    // Without the name the menu starts right after the glyph, so it is further left
    // than in the full layout.
    CHECK(no_name_boxes[1].rect.left < full_boxes[2].rect.left);

    BarLayoutParams no_menu = full;
    no_menu.show_app_menu = false;
    BarLayoutSummary no_menu_summary;
    const auto no_menu_boxes = dockforge::finderbar::LayoutBar(
        no_menu, Menus({L"File"}), &FixedWidth, &no_menu_summary);
    REQUIRE(no_menu_boxes.size() == 2);
    CHECK(no_menu_summary.placed_menus == 0);
}

TEST_CASE("T-301 bar: an empty application name produces no name box") {
    BarLayoutParams params = Params(1200.0f);
    params.app_name.clear();
    BarLayoutSummary summary;
    const auto boxes = dockforge::finderbar::LayoutBar(
        params, Menus({L"File"}), &FixedWidth, &summary);
    REQUIRE(boxes.size() == 2);
    CHECK(boxes[0].is_system_menu);
    CHECK(boxes[1].label == L"File");
}

TEST_CASE("T-301 bar: no menus at all leaves only the glyph and the name") {
    // This is the common case, not an edge case: spikes/T-301 measured that 9 of 11
    // visible windows on a real desktop expose no menu at all (explorer, chrome, VS
    // Code), so the no-menu bar is what most users will see.
    BarLayoutParams params = Params(1200.0f);
    params.app_name = L"Explorer";
    BarLayoutSummary summary;
    const auto boxes = dockforge::finderbar::LayoutBar(params, {}, &FixedWidth, &summary);
    REQUIRE(boxes.size() == 2);
    CHECK(boxes[0].is_system_menu);
    CHECK(boxes[1].is_app_name);
    CHECK(summary.placed_menus == 0);
    CHECK_FALSE(summary.overflowed);
}

TEST_CASE("T-301 bar: more titles than max_menu_items folds into an overflow box") {
    BarLayoutParams params = Params(2000.0f);
    params.max_menu_items = 3;
    // The glyph box is suppressed so the counts in this case are only about titles.
    params.show_system_menu = false;
    BarLayoutSummary summary;
    const auto boxes = dockforge::finderbar::LayoutBar(
        params, Menus({L"File", L"Edit", L"View", L"Window", L"Help"}), &FixedWidth, &summary);

    // 3 titles + the overflow box.
    REQUIRE(boxes.size() == 4);
    CHECK(summary.placed_menus == 3);
    CHECK(summary.overflowed);
    CHECK(boxes.back().is_overflow);
    CHECK(boxes.back().label == L"\x2026");
    // The placed titles are the first three, in order.
    CHECK(boxes[0].label == L"File");
    CHECK(boxes[1].label == L"Edit");
    CHECK(boxes[2].label == L"View");
}

TEST_CASE("T-301 bar: a narrow bar folds titles instead of drawing under the status area") {
    // 240 DIP is roughly a third of a 1080p screen at 100%: the bar is real but there
    // is no room for five titles.
    BarLayoutParams params = Params(240.0f);
    params.app_name = L"A rather long application name";
    BarLayoutSummary summary;
    const auto boxes = dockforge::finderbar::LayoutBar(
        params, Menus({L"File", L"Edit", L"View", L"Window", L"Help"}), &FixedWidth, &summary);

    CHECK(summary.overflowed);
    CHECK(summary.placed_menus < 5);
    // Nothing runs past the status boundary.
    CHECK_FALSE(summary.clipped);
    for (const BarBox& box : boxes) {
        CHECK(box.rect.right <= summary.status_start_dip);
    }
}

TEST_CASE("T-301 bar: the status area is reserved and never overlaps the left group") {
    BarLayoutParams params = Params(1200.0f);
    params.app_name = L"Notes";
    params.status_width_dip = 400.0f;
    BarLayoutSummary summary;
    const auto boxes = dockforge::finderbar::LayoutBar(
        params, Menus({L"File", L"Edit"}), &FixedWidth, &summary);

    CHECK(summary.status_start_dip == 800.0f);
    for (const BarBox& box : boxes) {
        CHECK(box.rect.right <= summary.status_start_dip);
    }
}

TEST_CASE("T-301 bar: one pathological title cannot push the others into the overflow") {
    // A title wider than the bar is capped at 30% of the bar, so the rest of the row
    // still lays out. Without the cap the first title would consume everything and the
    // user would see one truncated word.
    BarLayoutParams params = Params(1000.0f);
    std::wstring huge(400, L'W');
    BarLayoutSummary summary;
    const auto boxes = dockforge::finderbar::LayoutBar(
        params, {huge, L"File", L"Edit"}, &FixedWidth, &summary);

    CHECK(summary.placed_menus >= 2);
    CHECK(boxes[0].rect.Width() <= 1000.0f * 0.30f + 2 * 8.0f + 0.01f);
}

TEST_CASE("T-301 bar: a bar narrower than the leading inset still returns the glyph box") {
    // Degenerate but reachable during a display change: the caller gets one box it can
    // draw, and `clipped` tells it the layout does not fit.
    BarLayoutParams params = Params(10.0f);
    BarLayoutSummary summary;
    const auto boxes = dockforge::finderbar::LayoutBar(
        params, Menus({L"File"}), &FixedWidth, &summary);
    REQUIRE(boxes.size() == 1);
    CHECK(boxes[0].is_system_menu);
    CHECK(summary.clipped);
}

TEST_CASE("T-301 bar: hit testing maps a point to the menu it opens") {
    BarLayoutParams params = Params(1200.0f);
    params.app_name = L"Notes";
    BarLayoutSummary summary;
    const auto boxes = dockforge::finderbar::LayoutBar(
        params, Menus({L"File", L"Edit"}), &FixedWidth, &summary);
    REQUIRE(boxes.size() == 4);

    // Centre of each box, at half the bar height.
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        const float x = boxes[i].rect.CenterX();
        const std::size_t index =
            dockforge::finderbar::BoxIndexAt(boxes.data(), boxes.size(), x, 14.0f);
        CHECK(index == i);
    }

    // The two menu boxes report their own menu index; the glyph and the name report
    // kNoMenu so the caller can tell "opens the system menu" from "opens File".
    CHECK(dockforge::finderbar::HitTestBar(boxes.data(), boxes.size(),
                                           boxes[2].rect.CenterX(), 14.0f) == 0);
    CHECK(dockforge::finderbar::HitTestBar(boxes.data(), boxes.size(),
                                           boxes[3].rect.CenterX(), 14.0f) == 1);
    CHECK(dockforge::finderbar::HitTestBar(boxes.data(), boxes.size(),
                                           boxes[0].rect.CenterX(), 14.0f) == kNoMenu);

    // Empty bar to the right of the last box.
    CHECK(dockforge::finderbar::HitTestBar(boxes.data(), boxes.size(), 900.0f, 14.0f) == kNoMenu);
    // Below the bar.
    CHECK(dockforge::finderbar::HitTestBar(boxes.data(), boxes.size(),
                                           boxes[2].rect.CenterX(), 40.0f) == kNoMenu);
    // Left of the leading inset.
    CHECK(dockforge::finderbar::HitTestBar(boxes.data(), boxes.size(), 2.0f, 14.0f) == kNoMenu);
}

TEST_CASE("T-301 bar: box edges are half-open so a point never matches two boxes") {
    BarLayoutParams params = Params(1200.0f);
    BarLayoutSummary summary;
    const auto boxes = dockforge::finderbar::LayoutBar(
        params, Menus({L"File", L"Edit"}), &FixedWidth, &summary);
    REQUIRE(boxes.size() == 3);

    // Boxes are separated by gaps, so the shared-edge case cannot arise from the
    // layout; the half-open rule is what makes each edge unambiguous. The pixel just
    // inside the right edge belongs to this box, the edge itself belongs to nothing.
    const dockforge::render::RectF& file = boxes[1].rect;
    CHECK(dockforge::finderbar::BoxIndexAt(boxes.data(), boxes.size(), file.right - 0.01f,
                                           14.0f) == 1);
    CHECK(dockforge::finderbar::BoxIndexAt(boxes.data(), boxes.size(), file.right, 14.0f) ==
          kNoMenu);
    // The left edge is inclusive.
    CHECK(dockforge::finderbar::BoxIndexAt(boxes.data(), boxes.size(), file.left, 14.0f) == 1);
}

TEST_CASE("T-301 auto-hide: the bar reveals and retracts over its duration") {
    // A 45ms frame is below the 64ms clamp, so the arithmetic is exact: 45/180 = 0.25.
    AutoHideState state;
    state.progress = 0.0f;
    state.shown = false;

    dockforge::finderbar::AdvanceAutoHide(state, true, false, 45.0);
    CHECK(state.progress == 0.25f);
    CHECK(state.shown);

    dockforge::finderbar::AdvanceAutoHide(state, true, false, 45.0);
    CHECK(state.progress == 0.5f);

    // A pointer away from the trigger retracts at the same rate.
    dockforge::finderbar::AdvanceAutoHide(state, false, false, 45.0);
    CHECK(state.progress == 0.25f);
    CHECK_FALSE(state.shown);
}

TEST_CASE("T-301 auto-hide: the bar stays open while the pointer is over it") {
    // This is what makes a menu usable. Retracting while the pointer is inside the bar
    // would move the click target under the cursor.
    AutoHideState state;
    state.progress = 1.0f;
    dockforge::finderbar::AdvanceAutoHide(state, false, true, 64.0);
    CHECK(state.progress == 1.0f);
    CHECK(state.shown);
}

TEST_CASE("T-301 auto-hide: progress is clamped and a stalled frame cannot teleport it") {
    AutoHideState state;
    state.progress = 0.0f;
    // A 5 second stall must behave like a 64ms frame, not like a jump to fully open.
    dockforge::finderbar::AdvanceAutoHide(state, true, false, 5000.0);
    CHECK(state.progress < 0.5f);
    CHECK(state.progress > 0.0f);

    // Negative deltas (clock skew) are treated as zero, not as a rewind.
    const float before = state.progress;
    dockforge::finderbar::AdvanceAutoHide(state, true, false, -1000.0);
    CHECK(state.progress == before);

    // Overshoot is clamped at 1.
    for (int i = 0; i < 100; ++i) {
        dockforge::finderbar::AdvanceAutoHide(state, true, false, 64.0);
    }
    CHECK(state.progress == 1.0f);
}

TEST_CASE("T-301 auto-hide: the retracted bar leaves a hairline on screen") {
    AutoHideState hidden;
    hidden.progress = 0.0f;
    const float offset = dockforge::finderbar::AutoHideOffsetDip(hidden, 28.0f);
    // The bar is pushed up by (height - hairline), so 4 DIP stay visible.
    CHECK(offset == -(28.0f - dockforge::finderbar::kAutoHideHairlineDip));

    AutoHideState shown;
    shown.progress = 1.0f;
    CHECK(dockforge::finderbar::AutoHideOffsetDip(shown, 28.0f) == 0.0f);

    // Halfway the offset is between the two extremes.
    AutoHideState mid;
    mid.progress = 0.5f;
    const float mid_offset = dockforge::finderbar::AutoHideOffsetDip(mid, 28.0f);
    CHECK(mid_offset < 0.0f);
    CHECK(mid_offset > offset);
}

TEST_CASE("T-301 auto-hide: a retracted bar does not eat clicks") {
    AutoHideState hidden;
    hidden.progress = 0.0f;
    CHECK_FALSE(dockforge::finderbar::AutoHideInteractive(hidden));

    AutoHideState mid;
    mid.progress = 0.49f;
    CHECK_FALSE(dockforge::finderbar::AutoHideInteractive(mid));

    AutoHideState half;
    half.progress = 0.5f;
    CHECK(dockforge::finderbar::AutoHideInteractive(half));

    AutoHideState shown;
    shown.progress = 1.0f;
    CHECK(dockforge::finderbar::AutoHideInteractive(shown));
}

TEST_CASE("T-301 bar: ApproximateTextWidth scales with length and size") {
    using dockforge::finderbar::ApproximateTextWidth;
    CHECK(ApproximateTextWidth(L"", 13.0f) == 0.0f);
    CHECK(ApproximateTextWidth(nullptr, 13.0f) == 0.0f);
    CHECK(ApproximateTextWidth(L"File", 13.0f) > 0.0f);
    // Twice the characters is twice the width.
    const float four = ApproximateTextWidth(L"File", 13.0f);
    const float eight = ApproximateTextWidth(L"FileFile", 13.0f);
    CHECK(eight == four * 2.0f);
    // Twice the size is twice the width.
    CHECK(ApproximateTextWidth(L"File", 26.0f) == four * 2.0f);
    // A non-positive size produces no width rather than a negative one.
    CHECK(ApproximateTextWidth(L"File", 0.0f) == 0.0f);
}

TEST_CASE("T-301 bar: a null measurer falls back to the approximation") {
    BarLayoutParams params = Params(1200.0f);
    params.app_name = L"Notes";
    BarLayoutSummary summary;
    const auto boxes =
        dockforge::finderbar::LayoutBar(params, Menus({L"File"}), nullptr, &summary);
    REQUIRE(boxes.size() == 3);
    CHECK(boxes[2].rect.Width() > 0.0f);
}
