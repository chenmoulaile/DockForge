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

// ---------------------------------------------------------------------------
// T-307: the right region, the clock format and the config default table.
// ---------------------------------------------------------------------------

#include "core/config.h"
#include "finderbar/calendar_panel.h"
#include "finderbar/finder_bar.h"

#include <string>
#include <vector>

namespace {

using dockforge::finderbar::RightItem;
using dockforge::finderbar::RightType;

// Builds one right-region entry with a text of a known fake width.
RightItem Item(RightType type, int order, const std::wstring& text, bool icon = false) {
    RightItem item;
    item.type = type;
    item.order = order;
    item.text = text;
    item.has_icon = icon;
    return item;
}

}  // namespace

TEST_CASE("T-307 right: a single clock sits hard against the right edge at order 0") {
    // The default table is [{clock, 0}] and the spec freezes 0 = rightmost.
    std::vector<RightItem> items = {Item(RightType::Clock, 0, L"17:27")};
    dockforge::finderbar::RightGroupSummary summary;
    const auto boxes =
        dockforge::finderbar::LayoutRightGroup(items, 1200.0f, 28.0f, &FixedWidth, &summary);

    REQUIRE(boxes.size() == 1);
    // Width: 2 * 6 DIP padding + 5 chars * 13 DIP * 0.5 = 12 + 32.5 = 44.5.
    const float expected_width =
        2.0f * dockforge::finderbar::kRightContentPaddingDip + 5.0f * 13.0f * 0.5f;
    CHECK(boxes[0].rect.Width() == expected_width);
    // Right edge lands exactly on width - kTrailingInsetDip; 0 = rightmost means the
    // box's right edge is the group's right edge, never a gap further in.
    CHECK(boxes[0].rect.right == 1200.0f - dockforge::finderbar::kTrailingInsetDip);
    CHECK(boxes[0].rect.left == boxes[0].rect.right - expected_width);
    // The summary is what LayoutBar's status_width_dip consumes (DoD: no longer 0).
    CHECK(summary.total_width_dip == expected_width);
    CHECK(summary.placed == 1);
    CHECK(summary.dropped == 0);
    // Full bar height, like every left-group box.
    CHECK(boxes[0].rect.top == 0.0f);
    CHECK(boxes[0].rect.bottom == 28.0f);
}

TEST_CASE("T-307 right: order counts from the right edge, so 1 lands left of 0") {
    // The documented insertion convention: weather_brief at order 1 shows on the
    // LEFT of the clock at order 0.
    std::vector<RightItem> items = {Item(RightType::Clock, 0, L"17:27"),
                                    Item(RightType::WeatherBrief, 1, L"23\u00B0", true)};
    dockforge::finderbar::RightGroupSummary summary;
    const auto boxes =
        dockforge::finderbar::LayoutRightGroup(items, 1200.0f, 28.0f, &FixedWidth, &summary);
    REQUIRE(boxes.size() == 2);
    CHECK(boxes[1].type == RightType::Clock);  // rightmost in left-to-right order
    CHECK(boxes[0].type == RightType::WeatherBrief);
    CHECK(boxes[0].rect.right < boxes[1].rect.left);  // weather sits left of the clock
    // Exactly one gap between them.
    CHECK(boxes[1].rect.left - boxes[0].rect.right == dockforge::finderbar::kRightItemGapDip);
}

TEST_CASE("T-307 right: the insertion convention yields the macOS reading order") {
    // Full default plan (later tasks extend the default table to this): the sequence
    // from the right edge reads clock <- control_center <- notification_center <-
    // tray_mirror <- network <- battery <- ime.
    std::vector<RightItem> items = {
        Item(RightType::Ime, 6, L"EN"),
        Item(RightType::Battery, 5, L"87%"),
        Item(RightType::Network, 4, L""),
        Item(RightType::TrayMirror, 3, L""),
        Item(RightType::NotificationCenter, 2, L""),
        Item(RightType::ControlCenter, 1, L""),
        Item(RightType::Clock, 0, L"17:27"),
    };
    dockforge::finderbar::RightGroupSummary summary;
    const auto boxes =
        dockforge::finderbar::LayoutRightGroup(items, 2400.0f, 28.0f, &FixedWidth, &summary);
    REQUIRE(boxes.size() == 7);
    // Left-to-right output: ime, battery, network, tray, notification, control, clock.
    CHECK(boxes[0].type == RightType::Ime);
    CHECK(boxes[1].type == RightType::Battery);
    CHECK(boxes[2].type == RightType::Network);
    CHECK(boxes[3].type == RightType::TrayMirror);
    CHECK(boxes[4].type == RightType::NotificationCenter);
    CHECK(boxes[5].type == RightType::ControlCenter);
    CHECK(boxes[6].type == RightType::Clock);
    // The clock is still hard against the right edge.
    CHECK(boxes[6].rect.right == 2400.0f - dockforge::finderbar::kTrailingInsetDip);
    // Every box advances strictly rightwards with one gap between neighbours.
    for (std::size_t i = 1; i < boxes.size(); ++i) {
        CHECK(boxes[i].rect.left - boxes[i - 1].rect.right ==
              dockforge::finderbar::kRightItemGapDip);
    }
}

TEST_CASE("T-307 right: same-order conflicts resolve by the C-31 declaration order") {
    // clock (declared 7th, enum 6) vs weather_brief (declared 8th, enum 7) at the same
    // order: the earlier declaration goes further RIGHT, deterministically.
    std::vector<RightItem> items = {Item(RightType::WeatherBrief, 0, L"23\u00B0", true),
                                    Item(RightType::Clock, 0, L"17:27")};
    dockforge::finderbar::RightGroupSummary summary;
    const auto boxes =
        dockforge::finderbar::LayoutRightGroup(items, 1200.0f, 28.0f, &FixedWidth, &summary);
    REQUIRE(boxes.size() == 2);
    CHECK(boxes[1].type == RightType::Clock);  // earlier in C-31's list -> right
    CHECK(boxes[0].type == RightType::WeatherBrief);
}

TEST_CASE("T-307/T-306/T-304 right: unregistered types are skipped, never drawn as placeholders") {
    // The ask's rule, updated as the tasks land: T-307 registered clock and
    // weather_brief; T-306 flipped the three status indicators (ime/battery/
    // network); T-304 flipped the control centre. The rest wait for T-303/T-305
    // and cost no box, no gap, no width.
    CHECK(dockforge::finderbar::RightTypeRegistered(RightType::Clock));
    CHECK(dockforge::finderbar::RightTypeRegistered(RightType::WeatherBrief));
    CHECK(dockforge::finderbar::RightTypeRegistered(RightType::Ime));
    CHECK(dockforge::finderbar::RightTypeRegistered(RightType::Battery));
    CHECK(dockforge::finderbar::RightTypeRegistered(RightType::Network));
    CHECK(dockforge::finderbar::RightTypeRegistered(RightType::ControlCenter));
    // ADR-0058 flipped the last two C-31 enum types: the bell and the tray chevron
    // are resident right-cluster fixtures per the acceptance reference.
    CHECK(dockforge::finderbar::RightTypeRegistered(RightType::TrayMirror));
    CHECK(dockforge::finderbar::RightTypeRegistered(RightType::NotificationCenter));
    // ADR-0058's additive enum extension ships with its renderers - no dead value.
    CHECK(dockforge::finderbar::RightTypeRegistered(RightType::Volume));
    CHECK(dockforge::finderbar::RightTypeRegistered(RightType::Spotlight));

    // An entry that no renderer produced (a caller filtered it out before layout)
    // must also be invisible here - layout sees only what it is given.
    std::vector<RightItem> items = {Item(RightType::Clock, 0, L"17:27")};
    dockforge::finderbar::RightGroupSummary summary;
    const auto boxes =
        dockforge::finderbar::LayoutRightGroup(items, 1200.0f, 28.0f, &FixedWidth, &summary);
    REQUIRE(boxes.size() == 1);
    CHECK(boxes[0].type == RightType::Clock);
}

TEST_CASE("T-307 right: the type names round-trip through the C-31 spellings") {
    using dockforge::finderbar::RightTypeFromName;
    using dockforge::finderbar::RightTypeName;
    for (int i = 0; i < dockforge::finderbar::kRightTypeCount; ++i) {
        const auto parsed = RightTypeFromName(RightTypeName(i));
        REQUIRE(parsed.has_value());
        CHECK(static_cast<int>(*parsed) == i);
    }
    CHECK_FALSE(RightTypeFromName(L"widget").has_value());
    CHECK_FALSE(RightTypeFromName(L"").has_value());
    CHECK_FALSE(RightTypeFromName(nullptr).has_value());
}

TEST_CASE("T-307 right: an entry that crosses the left edge is dropped, not squeezed") {
    // One clock on a bar too narrow to hold it: the box is refused and reported.
    std::vector<RightItem> items = {Item(RightType::Clock, 0, L"17:27")};
    dockforge::finderbar::RightGroupSummary summary;
    const auto boxes =
        dockforge::finderbar::LayoutRightGroup(items, 30.0f, 28.0f, &FixedWidth, &summary);
    CHECK(boxes.empty());
    CHECK(summary.dropped == 1);
    CHECK(summary.placed == 0);
    // Nothing placed means nothing reserved: the left group gets the whole bar back.
    CHECK(summary.total_width_dip == 0.0f);
}

TEST_CASE("T-307 right: hit testing finds the clock and nothing outside it") {
    std::vector<RightItem> items = {Item(RightType::Clock, 0, L"17:27")};
    dockforge::finderbar::RightGroupSummary summary;
    const auto boxes =
        dockforge::finderbar::LayoutRightGroup(items, 1200.0f, 28.0f, &FixedWidth, &summary);
    REQUIRE(boxes.size() == 1);
    CHECK(dockforge::finderbar::RightBoxIndexAt(boxes.data(), boxes.size(),
                                                boxes[0].rect.CenterX(), 14.0f) == 0);
    // Outside the box (in the gap, above, below, left) is nobody's.
    CHECK(dockforge::finderbar::RightBoxIndexAt(boxes.data(), boxes.size(), boxes[0].rect.right,
                                                14.0f) == kNoMenu);
    CHECK(dockforge::finderbar::RightBoxIndexAt(boxes.data(), boxes.size(),
                                                boxes[0].rect.CenterX(), 40.0f) == kNoMenu);
}

TEST_CASE("T-307 clock: the default follows the injected region pictures") {
    // Production passes the LOCALE_SSHORTTIME / LOCALE_SDATE lookups; a test pins
    // them so the host machine's region cannot leak in.
    dockforge::core::FinderBarClockConfig clock;  // format empty, show_date true, seconds off
    SYSTEMTIME now{};                             // 2026-11-18 (a Wednesday) 17:27:56
    now.wYear = 2026;
    now.wMonth = 11;
    now.wDay = 18;
    now.wHour = 17;
    now.wMinute = 27;
    now.wSecond = 56;

    const dockforge::finderbar::BarClockText parts =
        dockforge::finderbar::FormatBarClock(now, clock, L"HH:mm", L"yyyy/MM/dd");
    CHECK(parts.time == L"17:27");
    CHECK(parts.date == L"2026/11/18");
    CHECK(dockforge::finderbar::FormatBarClockText(now, clock, L"HH:mm", L"yyyy/MM/dd") ==
          L"2026/11/18 17:27");
}

TEST_CASE("T-307 clock: a custom format string wins over the region picture") {
    dockforge::core::FinderBarClockConfig clock;
    clock.format = "HH:mm:ss";  // explicit seconds
    clock.show_date = false;
    SYSTEMTIME now{};
    now.wYear = 2026;
    now.wMonth = 11;
    now.wDay = 18;
    now.wHour = 17;
    now.wMinute = 27;
    now.wSecond = 56;
    CHECK(dockforge::finderbar::FormatBarClockText(now, clock, L"HH:mm", L"yyyy/MM/dd") ==
          L"17:27:56");
}

TEST_CASE("T-307 clock: show_seconds can add and remove seconds over the picture") {
    dockforge::core::FinderBarClockConfig on;
    on.show_seconds = true;  // picture "HH:mm" gains ":ss"
    on.show_date = false;
    dockforge::core::FinderBarClockConfig off;  // picture "HH:mm:ss" loses its seconds
    off.show_date = false;
    SYSTEMTIME now{};
    now.wYear = 2026;
    now.wMonth = 11;
    now.wDay = 18;
    now.wHour = 17;
    now.wMinute = 27;
    now.wSecond = 56;
    CHECK(dockforge::finderbar::FormatBarClockText(now, on, L"HH:mm", L"yyyy/MM/dd") ==
          L"17:27:56");
    CHECK(dockforge::finderbar::FormatBarClockText(now, off, L"HH:mm:ss", L"yyyy/MM/dd") ==
          L"17:27");
    // The seconds insert BEFORE an AM/PM designator, never after it. The designator's
    // own text is locale-dependent ("PM" / "下午"), so the assertion pins the numeric
    // prefix only: whatever the region calls it, the seconds come first.
    dockforge::core::FinderBarClockConfig meridiem;
    meridiem.show_seconds = true;
    meridiem.show_date = false;
    const std::wstring text =
        dockforge::finderbar::FormatBarClockText(now, meridiem, L"h:mm tt", L"yyyy/MM/dd");
    CHECK(text.rfind(L"5:27:56", 0) == 0);
    CHECK(text.size() > std::wstring(L"5:27:56").size());  // the designator is still there
}

TEST_CASE("T-307 clock: show_date off removes the date part entirely") {
    dockforge::core::FinderBarClockConfig clock;
    clock.show_date = false;
    SYSTEMTIME now{};
    now.wYear = 2026;
    now.wMonth = 11;
    now.wDay = 18;
    now.wHour = 17;
    now.wMinute = 27;
    CHECK(dockforge::finderbar::FormatBarClockText(now, clock, L"HH:mm", L"yyyy/MM/dd") ==
          L"17:27");
}

TEST_CASE("T-307/T-306/T-304 config: an empty right table falls back to the frozen default") {
    // The frozen default: ADR-0053 pinned the clock at 0; ADR-0057 inserted
    // control_center=1 (T-304, per the same insertion convention); ADR-0056
    // inserted the three status indicators at the pre-agreed positions -
    // network=4, battery=5, ime=6 (order counts from the right edge, macOS
    // reading order clock <- control centre <- status icons).
    dockforge::core::Config config;
    std::string error;
    REQUIRE(dockforge::core::ParseConfig("{}", config, &error));
    CHECK(error.empty());
    REQUIRE(config.finderbar.right.size() == 9);
    CHECK(config.finderbar.right[0].type == "clock");
    CHECK(config.finderbar.right[0].order == 0);
    CHECK(config.finderbar.right[1].type == "control_center");
    CHECK(config.finderbar.right[1].order == 1);
    // ADR-0058: the pre-agreed notification/tray slots are filled (the C-31 enum
    // always had the types; the renderers landed with the fidelity fixes).
    CHECK(config.finderbar.right[2].type == "notification_center");
    CHECK(config.finderbar.right[2].order == 2);
    CHECK(config.finderbar.right[3].type == "tray_mirror");
    CHECK(config.finderbar.right[3].order == 3);
    CHECK(config.finderbar.right[4].type == "network");
    CHECK(config.finderbar.right[4].order == 4);
    CHECK(config.finderbar.right[5].type == "battery");
    CHECK(config.finderbar.right[5].order == 5);
    CHECK(config.finderbar.right[6].type == "ime");
    CHECK(config.finderbar.right[6].order == 6);
    // ADR-0058's extension: volume=7 and spotlight=8 close the right cluster.
    CHECK(config.finderbar.right[7].type == "volume");
    CHECK(config.finderbar.right[7].order == 7);
    CHECK(config.finderbar.right[8].type == "spotlight");
    CHECK(config.finderbar.right[8].order == 8);
    CHECK(dockforge::core::SameRightItems(config.finderbar.right,
                                          dockforge::core::DefaultFinderBarRightItems()));

    // An explicit empty array falls back too (the spec's "empty config" rule).
    dockforge::core::Config empty_table;
    REQUIRE(dockforge::core::ParseConfig("{\"finderbar\":{\"right\":[]}}", empty_table, &error));
    CHECK(dockforge::core::SameRightItems(empty_table.finderbar.right,
                                          dockforge::core::DefaultFinderBarRightItems()));
}

TEST_CASE("T-307 config: finderbar.right and finderbar.clock round-trip") {
    // A table referencing a not-yet-registered type must survive untouched (C-01:
    // never drop user data) - T-303..T-306 will find it already in the file.
    const std::string json =
        "{\"finderbar\":{\"right\":[{\"type\":\"clock\",\"order\":0},"
        "{\"type\":\"weather_brief\",\"order\":1},{\"type\":\"control_center\",\"order\":2}],"
        "\"clock\":{\"format\":\"HH:mm:ss\",\"show_date\":false,\"show_seconds\":true}}}";
    dockforge::core::Config config;
    std::string error;
    REQUIRE(dockforge::core::ParseConfig(json, config, &error));
    REQUIRE(config.finderbar.right.size() == 3);
    CHECK(config.finderbar.right[0].type == "clock");
    CHECK(config.finderbar.right[0].order == 0);
    CHECK(config.finderbar.right[1].type == "weather_brief");
    CHECK(config.finderbar.right[1].order == 1);
    CHECK(config.finderbar.right[2].type == "control_center");
    CHECK(config.finderbar.right[2].order == 2);
    CHECK(config.finderbar.clock.format == "HH:mm:ss");
    CHECK_FALSE(config.finderbar.clock.show_date);
    CHECK(config.finderbar.clock.show_seconds);

    // Serialize -> parse again: the exact table comes back, including the unknown
    // type string.
    dockforge::core::Config round_trip;
    REQUIRE(dockforge::core::ParseConfig(dockforge::core::SerializeConfig(config), round_trip,
                                         &error));
    CHECK(dockforge::core::SameRightItems(round_trip.finderbar.right, config.finderbar.right));
    CHECK(round_trip.finderbar.clock.format == config.finderbar.clock.format);
    CHECK(round_trip.finderbar.clock.show_date == config.finderbar.clock.show_date);
    CHECK(round_trip.finderbar.clock.show_seconds == config.finderbar.clock.show_seconds);
}

TEST_CASE("T-307 config: default finderbar values write no finderbar key at all") {
    // The C-01 quiet-file rule the T-301 section established; the T-307 keys obey it.
    dockforge::core::Config config;
    std::string error;
    REQUIRE(dockforge::core::ParseConfig("{}", config, &error));
    const std::string serialized = dockforge::core::SerializeConfig(config);
    CHECK(serialized.find("\"finderbar\"") == std::string::npos);
    // ...but a single clock tweak brings exactly the changed fields back.
    config.finderbar.clock.show_seconds = true;
    const std::string tweaked = dockforge::core::SerializeConfig(config);
    CHECK(tweaked.find("\"finderbar\"") != std::string::npos);
    CHECK(tweaked.find("\"show_seconds\": true") != std::string::npos);
    CHECK(tweaked.find("\"right\"") == std::string::npos);  // still the default table
}

TEST_CASE("T-307 calendar grid: month arithmetic is exact and locale-free") {
    using dockforge::finderbar::CalendarGrid;
    // November 2026: the 1st is a Sunday, so with a Sunday-first grid the first day
    // lands in column 0 and 30 days fill rows one to five.
    CalendarGrid grid = CalendarGrid::Compute(2026, 11, 0, 2026, 11, 18);
    REQUIRE(grid.cells.size() == 42);
    CHECK(grid.first_day_col == 0);
    CHECK(grid.cells[0].day == 1);
    CHECK(grid.cells[0].in_month);
    CHECK(grid.cells[17].day == 18);  // index == day - 1 only because col == 0
    CHECK(grid.cells[17].is_today);
    CHECK(grid.cells[29].day == 30);
    CHECK(grid.cells[30].day == 1);  // December 1st trails
    CHECK_FALSE(grid.cells[30].in_month);

    // February 2026 (28 days) with Monday first: the 1st is a Sunday, so it lands in
    // column 6 after a six-cell leading run of January days.
    CalendarGrid feb = CalendarGrid::Compute(2026, 2, 1, 2026, 11, 18);
    CHECK(feb.first_day_col == 6);
    CHECK(feb.cells[0].day == 26);  // January 26th leads
    CHECK_FALSE(feb.cells[0].in_month);
    CHECK(feb.cells[6].day == 1);
    CHECK(feb.cells[33].day == 28);
    CHECK(feb.cells[34].day == 1);  // March trails

    // Leap year: February 2024 has 29 days. The 1st is a Thursday, so it sits in
    // column 4 and the 29th lands at index 4 + 28 = 32.
    CalendarGrid leap = CalendarGrid::Compute(2024, 2, 0, 2024, 2, 29);
    CHECK(leap.first_day_col == 4);  // 2024-02-01 is a Thursday
    CHECK(leap.cells[32].day == 29);
    CHECK(leap.cells[32].is_today);
    CHECK(leap.cells[33].day == 1);  // March trails
    CHECK_FALSE(leap.cells[33].in_month);

    // Weekday spot checks through Sakamoto's method.
    CHECK(CalendarGrid::WeekdayOf(2026, 11, 18) == 3);  // Wednesday
    CHECK(CalendarGrid::WeekdayOf(2026, 11, 1) == 0);   // Sunday
    CHECK(CalendarGrid::WeekdayOf(2000, 1, 1) == 6);    // Saturday
    CHECK(CalendarGrid::WeekdayOf(2024, 2, 29) == 4);   // Thursday
    CHECK(CalendarGrid::DaysInMonth(2024, 2) == 29);
    CHECK(CalendarGrid::DaysInMonth(2026, 2) == 28);
    CHECK(CalendarGrid::DaysInMonth(2026, 11) == 30);
}

TEST_CASE("T-307 calendar layout: hit test maps arrows and day cells") {
    using dockforge::finderbar::CalendarPanelHit;
    using dockforge::finderbar::CalendarPanelLayout;
    const CalendarPanelLayout layout = CalendarPanelLayout::Compute();

    // Arrow hot zones are 32 DIP squares at the card's inner edges.
    const float arrow_y = layout.nav_top_dip + 16.0f;
    CalendarPanelHit prev = layout.HitTest(layout.nav_left_dip + 8.0f, arrow_y, 42);
    CHECK(prev.kind == CalendarPanelHit::Kind::PrevMonth);
    CalendarPanelHit next = layout.HitTest(layout.nav_right_dip + 8.0f, arrow_y, 42);
    CHECK(next.kind == CalendarPanelHit::Kind::NextMonth);

    // The title strip between the arrows is inert.
    CalendarPanelHit title = layout.HitTest(
        layout.nav_left_dip + layout.nav_arrow_size_dip + 20.0f, arrow_y, 42);
    CHECK(title.kind == CalendarPanelHit::Kind::None);

    // The cell at the grid's exact centre is index 3*7+3 = 24.
    CalendarPanelHit centre = layout.HitTest(
        layout.grid_origin_x_dip + 3.5f * layout.cell_dip,
        layout.grid_origin_y_dip + 3.5f * layout.cell_dip, 42);
    REQUIRE(centre.kind == CalendarPanelHit::Kind::Day);
    CHECK(centre.cell == 24);

    // Below the grid and left of the grid are both nobody's.
    CHECK(layout.HitTest(layout.grid_origin_x_dip - 1.0f,
                         layout.grid_origin_y_dip + 2.0f * layout.cell_dip, 42)
              .kind == CalendarPanelHit::Kind::None);
    CHECK(layout.HitTest(layout.grid_origin_x_dip + 2.0f * layout.cell_dip,
                         layout.grid_origin_y_dip + 6.5f * layout.cell_dip, 42)
              .kind == CalendarPanelHit::Kind::None);
}

// T-302 widget-zone tests live in t302_widget_tests.inc (kept out of the CMake
// test GLOB so it compiles exactly once, inside this translation unit).
#include "t302_widget_tests.inc"

// T-306 right-region status tests, same include-once rule.
#include "t306_status_tests.inc"
