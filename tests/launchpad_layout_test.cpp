// DockForge - Launchpad grid and animation tests (CONTRACTS.md C-18).
//
// These are pure-arithmetic tests: no window is created, so they run on any desktop and
// cannot be flaked by a monitor being asleep. The numbers come from the contract, not
// from whatever the current code happens to produce.
#include "catch_amalgamated.hpp"

#include "dock/launchpad_layout.h"

using namespace dockforge;
using namespace dockforge::dock;

namespace {

LaunchpadGridInput MakeInput(float width_dip, float height_dip, int icon_size = 96,
                             int columns = 0, int page_size = kLaunchpadDefaultPageSize) {
    LaunchpadGridInput input;
    input.width_dip = width_dip;
    input.height_dip = height_dip;
    input.icon_size = icon_size;
    input.columns = columns;
    input.page_size = page_size;
    return input;
}

}  // namespace

TEST_CASE("Adaptive columns follow the C-18 formula", "[launchpad-layout]") {
    // 1440 DIP wide, 96 DIP icons: (1440 - 128) / (96 + 26) = 10.75 -> 10 columns.
    const LaunchpadGrid grid = ComputeLaunchpadGrid(MakeInput(1440.0f, 900.0f));
    CHECK(grid.columns == 10);
    // 35 items at 10 columns is 4 rows, not 3.5.
    CHECK(grid.rows == 4);
    CHECK(grid.per_page == kLaunchpadDefaultPageSize);
    CHECK(grid.icon_size_dip == Catch::Approx(96.0f));
}

TEST_CASE("An explicit column count overrides the adaptive one", "[launchpad-layout]") {
    // C-18: the 5x7 page is what the default 35 items are laid out for, and an explicit
    // `columns` must win even when the adaptive formula would pick something wider.
    const LaunchpadGrid grid = ComputeLaunchpadGrid(MakeInput(1440.0f, 900.0f, 96, 5));
    CHECK(grid.columns == 5);
    CHECK(grid.rows == 7);
    CHECK(grid.per_page == 35);
}

TEST_CASE("Column count is clamped to the contract range", "[launchpad-layout]") {
    CHECK(ComputeLaunchpadGrid(MakeInput(1440.0f, 900.0f, 96, 1)).columns == kLaunchpadMinColumns);
    CHECK(ComputeLaunchpadGrid(MakeInput(1440.0f, 900.0f, 96, 99)).columns == kLaunchpadMaxColumns);
    // A very narrow display cannot produce fewer than 3 columns either.
    CHECK(ComputeLaunchpadGrid(MakeInput(200.0f, 900.0f)).columns == kLaunchpadMinColumns);
}

TEST_CASE("A degenerate frame still yields a usable grid", "[launchpad-layout]") {
    // The first paint can run before the window has a size. Dividing by columns or rows
    // there would be a crash, so the contract is a legal grid rather than zeroes.
    const LaunchpadGrid grid = ComputeLaunchpadGrid(MakeInput(0.0f, 0.0f));
    CHECK(grid.columns >= kLaunchpadMinColumns);
    CHECK(grid.columns <= kLaunchpadMaxColumns);
    CHECK(grid.rows >= 1);
    CHECK(grid.per_page >= 1);
    CHECK(grid.cell_width_dip >= 0.0f);
}

TEST_CASE("Page arithmetic covers the boundaries", "[launchpad-layout]") {
    const LaunchpadGrid grid = ComputeLaunchpadGrid(MakeInput(1440.0f, 900.0f, 96, 5));
    REQUIRE(grid.per_page == 35);

    CHECK(LaunchpadPageCount(0, grid) == 1);   // an empty grid still has page 0
    CHECK(LaunchpadPageCount(1, grid) == 1);
    CHECK(LaunchpadPageCount(35, grid) == 1);
    CHECK(LaunchpadPageCount(36, grid) == 2);
    CHECK(LaunchpadPageCount(70, grid) == 2);
    CHECK(LaunchpadPageCount(71, grid) == 3);

    CHECK(LaunchpadPageItemCount(36, grid, 0) == 35);
    CHECK(LaunchpadPageItemCount(36, grid, 1) == 1);
    CHECK(LaunchpadPageItemCount(36, grid, 2) == 0);  // past the end
    CHECK(LaunchpadPageItemCount(0, grid, 0) == 0);
    CHECK(LaunchpadPageItemCount(36, grid, -1) == 0);
}

TEST_CASE("Page index is clamped into range", "[launchpad-layout]") {
    const LaunchpadGrid grid = ComputeLaunchpadGrid(MakeInput(1440.0f, 900.0f, 96, 5));
    CHECK(ClampLaunchpadPage(-5, 36, grid) == 0);
    CHECK(ClampLaunchpadPage(0, 36, grid) == 0);
    CHECK(ClampLaunchpadPage(1, 36, grid) == 1);
    CHECK(ClampLaunchpadPage(9, 36, grid) == 1);  // 36 items = 2 pages
    CHECK(ClampLaunchpadPage(3, 0, grid) == 0);   // empty index collapses to page 0
}

TEST_CASE("Cell rectangles tile the grid without overlapping", "[launchpad-layout]") {
    const LaunchpadGrid grid = ComputeLaunchpadGrid(MakeInput(1440.0f, 900.0f, 96, 5));
    const render::RectF first = LaunchpadCellRect(grid, 0);
    const render::RectF second = LaunchpadCellRect(grid, 1);
    const render::RectF next_row = LaunchpadCellRect(grid, 5);

    CHECK(first.left == Catch::Approx(grid.origin_x_dip));
    CHECK(first.top == Catch::Approx(grid.origin_y_dip));
    CHECK(first.Width() == Catch::Approx(grid.cell_width_dip));
    CHECK(second.left > first.right);          // a gap between columns
    CHECK(next_row.top > first.bottom);        // a gap between rows
    CHECK(next_row.left == Catch::Approx(first.left));
    // The icon is centred in the cell and the label sits under it, inside the cell.
    const render::RectF icon = LaunchpadIconRect(grid, 0);
    const render::RectF label = LaunchpadLabelRect(grid, 0);
    CHECK(icon.CenterX() == Catch::Approx(first.CenterX()));
    CHECK(icon.top == Catch::Approx(first.top));
    CHECK(label.top >= icon.bottom);
    CHECK(label.bottom <= first.bottom + 0.01f);
}

TEST_CASE("Hit test accepts the label band and rejects empty space", "[launchpad-layout]") {
    const LaunchpadGrid grid = ComputeLaunchpadGrid(MakeInput(1440.0f, 900.0f, 96, 5));
    const render::RectF cell0 = LaunchpadCellRect(grid, 0);
    const render::RectF label0 = LaunchpadLabelRect(grid, 0);

    CHECK(LaunchpadHitTest(grid, 35, cell0.CenterX(), cell0.CenterY()) == 0);
    // The label is part of the click target; a grid where only the icon responds feels
    // broken because the label is visually inside the same highlight.
    CHECK(LaunchpadHitTest(grid, 35, label0.CenterX(), label0.CenterY()) == 0);
    CHECK(LaunchpadHitTest(grid, 35, LaunchpadCellRect(grid, 6).CenterX(),
                           LaunchpadCellRect(grid, 6).CenterY()) == 6);

    // Between two columns: the gutter is not a cell.
    const render::RectF cell1 = LaunchpadCellRect(grid, 1);
    const float gutter_x = (cell0.right + cell1.left) * 0.5f;
    CHECK(LaunchpadHitTest(grid, 35, gutter_x, cell0.CenterY()) == -1);

    // Above the grid and below it.
    CHECK(LaunchpadHitTest(grid, 35, cell0.CenterX(), grid.origin_y_dip - 4.0f) == -1);
    CHECK(LaunchpadHitTest(grid, 35, cell0.CenterX(), 899.0f) == -1);
}

TEST_CASE("Hit test ignores cells past the end of a short last page", "[launchpad-layout]") {
    const LaunchpadGrid grid = ComputeLaunchpadGrid(MakeInput(1440.0f, 900.0f, 96, 5));
    // 36 items means page 1 holds exactly one cell. Cell 2's rectangle exists
    // geometrically but must not report a hit, or a click on blank space would launch
    // whatever happens to follow in the vector.
    const render::RectF cell2 = LaunchpadCellRect(grid, 2);
    CHECK(LaunchpadHitTest(grid, 1, cell2.CenterX(), cell2.CenterY()) == -1);
    CHECK(LaunchpadHitTest(grid, 1, LaunchpadCellRect(grid, 0).CenterX(),
                           LaunchpadCellRect(grid, 0).CenterY()) == 0);
    CHECK(LaunchpadHitTest(grid, 0, cell2.CenterX(), cell2.CenterY()) == -1);
}

TEST_CASE("A short display shrinks the icon instead of overflowing", "[launchpad-layout]") {
    // 7 rows of 120 DIP cells need 996 DIP; this display offers 496. The contract fixes
    // `rows`, so the icon edge is the only variable that can give.
    const LaunchpadGrid grid = ComputeLaunchpadGrid(MakeInput(800.0f, 600.0f, 96, 5));
    REQUIRE(grid.rows == 7);
    CHECK(grid.icon_size_dip < 96.0f);
    CHECK(grid.icon_size_dip >= 16.0f);
    const float bottom = LaunchpadCellRect(grid, 34).bottom;
    CHECK(bottom <= 600.0f);
}

TEST_CASE("Hover magnification stays inside the cell", "[launchpad-layout]") {
    const LaunchpadGrid grid = ComputeLaunchpadGrid(MakeInput(1440.0f, 900.0f, 96, 5));
    const float idle = LaunchpadIconEdgeFor(grid, 0.0f);
    const float full = LaunchpadIconEdgeFor(grid, 1.0f);
    CHECK(idle == Catch::Approx(grid.icon_size_dip));
    CHECK(full == Catch::Approx(grid.icon_size_dip * kLaunchpadHoverScale));
    CHECK(full > idle);
    // Out-of-range amounts are clamped, not extrapolated.
    CHECK(LaunchpadIconEdgeFor(grid, 2.0f) == Catch::Approx(full));
    CHECK(LaunchpadIconEdgeFor(grid, -1.0f) == Catch::Approx(idle));
    // A 10% growth of a 96 DIP icon is 4.8 DIP per side; the 26 DIP column pitch
    // leaves room for it.
    CHECK(full <= grid.cell_width_dip);
}

TEST_CASE("Page dots appear only when there is more than one page", "[launchpad-layout]") {
    CHECK_FALSE(LaunchpadShowsPageDots(0));
    CHECK_FALSE(LaunchpadShowsPageDots(1));
    CHECK(LaunchpadShowsPageDots(2));
    CHECK(LaunchpadShowsPageDots(7));

    const LaunchpadGrid grid = ComputeLaunchpadGrid(MakeInput(1440.0f, 900.0f, 96, 5));
    // With a single page the rect is empty, which is what keeps the paint path from
    // having to branch on page count separately.
    CHECK(LaunchpadDotRect(grid, 900.0f, 0, 1).Width() == 0.0f);

    const render::RectF dot0 = LaunchpadDotRect(grid, 900.0f, 0, 3);
    const render::RectF dot1 = LaunchpadDotRect(grid, 900.0f, 1, 3);
    const render::RectF dot2 = LaunchpadDotRect(grid, 900.0f, 2, 3);
    REQUIRE(dot0.Width() > 0.0f);
    CHECK(dot0.Width() == Catch::Approx(kLaunchpadDotDip));
    CHECK(dot1.left > dot0.right);
    CHECK(dot2.left > dot1.right);
    // The row is centred on the grid and sits near the bottom of the display.
    const float row_center = (dot0.left + dot2.right) * 0.5f;
    const float grid_center = grid.origin_x_dip + grid.cell_width_dip * grid.columns * 0.5f;
    CHECK(row_center == Catch::Approx(grid_center).margin(0.01));
    CHECK(dot0.CenterY() == Catch::Approx(LaunchpadDotCenterYDip(grid, 900.0f)));
    CHECK(dot0.CenterY() > 800.0f);
}

TEST_CASE("Open and close animation curves are monotonic and bounded", "[launchpad-layout]") {
    CHECK(LaunchpadAnimationScale(0.0f) == Catch::Approx(kLaunchpadOpenScaleFrom));
    CHECK(LaunchpadAnimationScale(1.0f) == Catch::Approx(1.0f));
    CHECK(LaunchpadAnimationFade(0.0f) == Catch::Approx(0.0f));
    CHECK(LaunchpadAnimationFade(1.0f) == Catch::Approx(1.0f));

    float previous_scale = -1.0f;
    float previous_fade = -1.0f;
    for (int step = 0; step <= 20; ++step) {
        const float progress = static_cast<float>(step) / 20.0f;
        const float scale = LaunchpadAnimationScale(progress);
        const float fade = LaunchpadAnimationFade(progress);
        CHECK(scale >= previous_scale);
        CHECK(fade >= previous_fade);
        CHECK(scale >= kLaunchpadOpenScaleFrom - 0.0001f);
        CHECK(scale <= 1.0f + 0.0001f);
        CHECK(fade >= -0.0001f);
        CHECK(fade <= 1.0f + 0.0001f);
        previous_scale = scale;
        previous_fade = fade;
    }

    // Out-of-range progress is clamped rather than extrapolated: a frame that overshoots
    // the duration must not scale the grid past 1.0.
    CHECK(LaunchpadAnimationScale(2.0f) == Catch::Approx(1.0f));
    CHECK(LaunchpadAnimationFade(-1.0f) == Catch::Approx(0.0f));
}

TEST_CASE("Progress advances at the contract duration", "[launchpad-layout]") {
    // Half the open duration is half the progress.
    float progress = AdvanceLaunchpadProgress(0.0f, kLaunchpadOpenMs * 0.5f, kLaunchpadOpenMs);
    CHECK(progress == Catch::Approx(0.5f));

    // It clamps at 1 instead of running past it, which is what lets Tick() test
    // `progress >= 1` as the completion condition.
    progress = AdvanceLaunchpadProgress(progress, kLaunchpadOpenMs, kLaunchpadOpenMs);
    CHECK(progress == Catch::Approx(1.0f));

    // A negative delta rewinds, which is how the close animation shares the curve. The
    // delta has to be measured in the *close* duration, or the rewind lands somewhere
    // between the two pages: -90ms over 140ms is -0.643, not -0.5.
    progress = AdvanceLaunchpadProgress(progress, -kLaunchpadCloseMs * 0.5f, kLaunchpadCloseMs);
    CHECK(progress == Catch::Approx(0.5f));
    progress = AdvanceLaunchpadProgress(progress, -kLaunchpadCloseMs, kLaunchpadCloseMs);
    CHECK(progress == Catch::Approx(0.0f));

    // A zero duration settles immediately instead of dividing by zero.
    CHECK(AdvanceLaunchpadProgress(0.0f, 16.0f, 0.0f) == Catch::Approx(0.0f));
    CHECK(AdvanceLaunchpadProgress(1.0f, 16.0f, 0.0f) == Catch::Approx(1.0f));
}

TEST_CASE("Two displays can resolve different grids from one config", "[launchpad-layout]") {
    // ADR-0021 requires a separate overlay and therefore a separate grid per display.
    // A 1440x900 at 100% and a 1280x720 at 150% are the shapes a mixed-DPI pair produces
    // once each is converted to DIP.
    const LaunchpadGrid wide = ComputeLaunchpadGrid(MakeInput(1440.0f, 900.0f, 96, 0));
    const LaunchpadGrid narrow = ComputeLaunchpadGrid(MakeInput(1280.0f, 720.0f, 96, 0));
    CHECK(wide.columns != narrow.columns);
    CHECK(wide.cell_width_dip != Catch::Approx(narrow.cell_width_dip));
    // Both are still legal, and neither overflows its own display.
    CHECK(wide.columns >= kLaunchpadMinColumns);
    CHECK(narrow.columns >= kLaunchpadMinColumns);
    CHECK(LaunchpadCellRect(wide, wide.columns - 1).right <= 1440.0f);
    CHECK(LaunchpadCellRect(narrow, narrow.columns - 1).right <= 1280.0f);
}
