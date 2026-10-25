// DockForge - Launchpad search geometry, folder strip and folder authoring
// (T-042, CONTRACTS.md C-18).
//
// These are the rules that break silently:
//
//   * the search bar must not move the grid, so its band is asserted to be the
//     same one the grid already reserves;
//   * the caret must track the typed prefix, or the field looks frozen;
//   * the chip strip must clip rather than overflow, and the hit test must agree
//     with what was actually drawn;
//   * a folder mutation must be idempotent and must never leave a zero-item
//     folder behind, because the loader discards those and the user would think
//     the app lost their data.
#include <catch_amalgamated.hpp>

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <string>
#include <vector>

#include "core/config.h"
#include "dock/launchpad_folders.h"
#include "dock/launchpad_search.h"
#include "render/backend.h"
#include "shell/app_identity.h"
#include "shell/launchpad_index.h"

namespace dock = dockforge::dock;
namespace core = dockforge::core;
namespace render = dockforge::render;
namespace shell = dockforge::shell;

using Catch::Approx;

namespace {

// Scoped COM, because NewFolderId() calls CoCreateGuid.
struct ComScope {
    bool ok = false;
    ComScope() { ok = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)); }
    ~ComScope() {
        if (ok) ::CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
};

core::Config MakeConfig() {
    core::Config config;
    config.dock.icon_size = 48;
    config.launchpad.icon_size = 96;
    config.launchpad.page_size = 35;
    return config;
}

}  // namespace

// ---------------------------------------------------------------- search bar

TEST_CASE("Search bar sits inside the band the grid already reserves",
          "[launchpad][search][geometry]") {
    // The grid's origin_y starts at kLaunchpadTopBandDip. If the field ever grew
    // past that band it would cover the first row of icons with no way to notice
    // in a unit test that only checks the field.
    const render::RectF field = dock::LaunchpadSearchRect(1440.0f, 900.0f);
    CHECK(field.top >= 0.0f);
    CHECK(field.bottom <= dock::kLaunchpadTopBandDip);
    CHECK(field.Width() <= dock::kLaunchpadSearchWidthDip);
    CHECK(field.Height() == Approx(dock::kLaunchpadSearchHeightDip));
}

TEST_CASE("Search bar is centred and clamps to a narrow viewport",
          "[launchpad][search][geometry]") {
    const render::RectF wide = dock::LaunchpadSearchRect(1440.0f, 900.0f);
    CHECK(wide.left == Approx((1440.0f - wide.Width()) * 0.5f).epsilon(0.01f));

    // A viewport narrower than the field plus margins must still produce a
    // positive, on-screen rect rather than one that starts off the left edge.
    const render::RectF narrow = dock::LaunchpadSearchRect(240.0f, 600.0f);
    CHECK(narrow.left >= 0.0f);
    CHECK(narrow.right <= 240.0f);
    CHECK(narrow.Width() > 0.0f);
}

TEST_CASE("Search hit test matches the field and nothing else", "[launchpad][search][geometry]") {
    const float width = 1440.0f;
    const float height = 900.0f;
    const render::RectF field = dock::LaunchpadSearchRect(width, height);

    CHECK(dock::LaunchpadSearchHit(field.CenterX(), field.CenterY(), width, height));
    // Just outside the bottom edge is the grid's territory, not the field's.
    CHECK_FALSE(dock::LaunchpadSearchHit(field.CenterX(), field.bottom + 1.0f, width, height));
    CHECK_FALSE(dock::LaunchpadSearchHit(field.left - 1.0f, field.CenterY(), width, height));
}

TEST_CASE("Caret tracks the typed prefix and clamps inside the field",
          "[launchpad][search][geometry]") {
    const float width = 1440.0f;
    const float height = 900.0f;
    const render::RectF field = dock::LaunchpadSearchRect(width, height);

    const render::RectF none = dock::LaunchpadSearchCaretRect(L"", 0, field);
    const render::RectF one = dock::LaunchpadSearchCaretRect(L"a", 1, field);
    const render::RectF two = dock::LaunchpadSearchCaretRect(L"ab", 2, field);

    // Each character advances the caret by the same amount: a proportional font
    // would make the step depend on which character was typed.
    CHECK(one.left > none.left);
    CHECK(two.left > one.left);
    CHECK(two.left - one.left == Approx(one.left - none.left));

    // A caret index past the end of the text must not draw outside the field.
    const render::RectF overflow = dock::LaunchpadSearchCaretRect(L"abcdefghij", 10, field);
    CHECK(overflow.right <= field.right);
    CHECK(overflow.left >= field.left);

    // Negative index is a caller bug; it must not produce a negative rect.
    const render::RectF negative = dock::LaunchpadSearchCaretRect(L"a", -5, field);
    CHECK(negative.right >= negative.left);
}

// ------------------------------------------------------------- folder chips

TEST_CASE("Folder chips lay out left to right without overlapping",
          "[launchpad][search][chips]") {
    std::vector<render::RectF> rects;
    dock::LaunchpadFolderChipRects(1440.0f, 3, rects);
    REQUIRE(rects.size() == 3);

    for (std::size_t i = 0; i < rects.size(); ++i) {
        CHECK(rects[i].Height() == Approx(dock::kLaunchpadFolderChipHeightDip));
        CHECK(rects[i].top == Approx(dock::kLaunchpadFolderStripTopDip));
        if (i > 0) {
            // The gap is exact, so a chip strip that reflows on rename cannot
            // shift its neighbours by a pixel and steal a click.
            CHECK(rects[i].left - rects[i - 1].right == Approx(dock::kLaunchpadFolderChipGapDip));
        }
    }
}

TEST_CASE("Folder chips clip instead of running off the right edge",
          "[launchpad][search][chips]") {
    const float width = 1440.0f;
    const int limit = dock::LaunchpadFolderChipLimit(width);
    REQUIRE(limit > 0);

    std::vector<render::RectF> rects;
    dock::LaunchpadFolderChipRects(width, limit + 50, rects);
    // The strip must stop at the edge rather than draw past it, and the hit test
    // must agree with what was drawn.
    CHECK(static_cast<int>(rects.size()) == limit);
    for (const render::RectF& r : rects) {
        CHECK(r.right <= width);
    }
    CHECK(dock::LaunchpadFolderChipHit(rects.back().CenterX(), rects.back().CenterY(), rects) ==
          static_cast<int>(rects.size()) - 1);
    // Past the last chip is not a chip.
    CHECK(dock::LaunchpadFolderChipHit(width - 1.0f, rects.front().CenterY(), rects) == -1);
}

TEST_CASE("Folder chip hit test walks the drawn rects only", "[launchpad][search][chips]") {
    std::vector<render::RectF> rects;
    dock::LaunchpadFolderChipRects(1440.0f, 5, rects);
    REQUIRE(rects.size() == 5);

    CHECK(dock::LaunchpadFolderChipHit(rects[2].CenterX(), rects[2].CenterY(), rects) == 2);
    // In the gap between chips: not a hit.
    const float gap_x = (rects[2].right + rects[3].left) * 0.5f;
    CHECK(dock::LaunchpadFolderChipHit(gap_x, rects[2].CenterY(), rects) == -1);
    // Above the strip is the search bar.
    CHECK(dock::LaunchpadFolderChipHit(rects[0].CenterX(), rects[0].top - 5.0f, rects) == -1);
}

TEST_CASE("A zero or negative viewport yields no chips, not a crash",
          "[launchpad][search][chips]") {
    std::vector<render::RectF> rects;
    dock::LaunchpadFolderChipRects(0.0f, 5, rects);
    CHECK(rects.empty());
    dock::LaunchpadFolderChipRects(-100.0f, 5, rects);
    CHECK(rects.empty());
    CHECK(dock::LaunchpadFolderChipLimit(0.0f) == 0);
    dock::LaunchpadFolderChipRects(1440.0f, 0, rects);
    CHECK(rects.empty());
    dock::LaunchpadFolderChipRects(1440.0f, -3, rects);
    CHECK(rects.empty());
    // An empty drawn-rect list means nothing to hit, even inside the strip band.
    std::vector<render::RectF> no_chips;
    CHECK(dock::LaunchpadFolderChipHit(100.0f, dock::kLaunchpadFolderStripTopDip + 1.0f, no_chips) ==
          -1);
}

// ---------------------------------------------------------- folder authoring

TEST_CASE("CreateLaunchpadFolder assigns a lowercase canonical id",
          "[launchpad][folders]") {
    ComScope com;
    REQUIRE(com.ok);

    core::Config config = MakeConfig();
    const std::string id = dock::CreateLaunchpadFolder(config, "Games", {"a", "b"});
    REQUIRE_FALSE(id.empty());
    REQUIRE(config.launchpad.folders.size() == 1);
    CHECK(config.launchpad.folders[0].id == id);

    // Lowercase canonical UUID: the loader reads ids back and compares them
    // literally, so an uppercase id would stop matching after a round-trip.
    CHECK(id == shell::LowerAscii(id));
    // v4 form: 8-4-4-4-12.
    CHECK(id.size() == 36);
    CHECK(id[8] == '-');
    CHECK(id[13] == '-');
    CHECK(id[18] == '-');
    CHECK(id[23] == '-');
}

TEST_CASE("CreateLaunchpadFolder refuses an empty item list", "[launchpad][folders]") {
    ComScope com;
    REQUIRE(com.ok);

    core::Config config = MakeConfig();
    // C-18 discards empty folders on load, so creating one would write an entry
    // that immediately vanishes - the user would think the app lost it.
    CHECK(dock::CreateLaunchpadFolder(config, "Nothing", {}).empty());
    CHECK(config.launchpad.folders.empty());
}

TEST_CASE("CreateLaunchpadFolder lowercases identities and drops duplicates",
          "[launchpad][folders]") {
    ComScope com;
    REQUIRE(com.ok);

    core::Config config = MakeConfig();
    const std::string id =
        dock::CreateLaunchpadFolder(config, "Mixed", {"C:\\APP\\A.EXE", "c:\\app\\a.exe", "b"});
    REQUIRE_FALSE(id.empty());
    const core::LaunchpadFolder* folder = dock::FindLaunchpadFolder(config, id);
    REQUIRE(folder != nullptr);
    REQUIRE(folder->items.size() == 2);
    CHECK(folder->items[0] == shell::LowerAscii("C:\\APP\\A.EXE"));
    CHECK(folder->items[1] == "b");
}

TEST_CASE("AppendToLaunchpadFolder is idempotent", "[launchpad][folders]") {
    ComScope com;
    REQUIRE(com.ok);

    core::Config config = MakeConfig();
    const std::string id = dock::CreateLaunchpadFolder(config, "Games", {"a"});
    REQUIRE_FALSE(id.empty());

    // A double drop, or a drop that arrives twice because the pointer moved
    // while the button was held, must not produce a duplicate entry.
    CHECK(dock::AppendToLaunchpadFolder(config, id, "A"));
    CHECK(dock::AppendToLaunchpadFolder(config, id, "a"));
    const core::LaunchpadFolder* folder = dock::FindLaunchpadFolder(config, id);
    REQUIRE(folder != nullptr);
    CHECK(folder->items.size() == 1);

    // Appending to an unknown id is refused rather than silently ignored, so the
    // caller re-reads the config instead of believing it wrote.
    CHECK_FALSE(dock::AppendToLaunchpadFolder(config, "no-such-id", "z"));
}

TEST_CASE("LaunchpadFolderContains folds case", "[launchpad][folders]") {
    ComScope com;
    REQUIRE(com.ok);

    core::Config config = MakeConfig();
    const std::string id = dock::CreateLaunchpadFolder(config, "Games", {"c:\\apps\\a.exe"});
    REQUIRE_FALSE(id.empty());
    CHECK(dock::LaunchpadFolderContains(config, id, "C:\\APPS\\A.EXE"));
    CHECK_FALSE(dock::LaunchpadFolderContains(config, id, "c:\\apps\\b.exe"));
    CHECK_FALSE(dock::LaunchpadFolderContains(config, "no-such-id", "a"));
}

TEST_CASE("RenameLaunchpadFolder keeps the id stable", "[launchpad][folders]") {
    ComScope com;
    REQUIRE(com.ok);

    core::Config config = MakeConfig();
    const std::string id = dock::CreateLaunchpadFolder(config, "Games", {"a"});
    REQUIRE_FALSE(id.empty());

    CHECK(dock::RenameLaunchpadFolder(config, id, "Play"));
    const core::LaunchpadFolder* folder = dock::FindLaunchpadFolder(config, id);
    REQUIRE(folder != nullptr);
    // The id is what the grid rearranges by. A rename that changed it would drop
    // the folder the user is still dragging.
    CHECK(folder->id == id);
    CHECK(folder->name == "Play");

    CHECK_FALSE(dock::RenameLaunchpadFolder(config, "no-such-id", "x"));
}

TEST_CASE("Removing the last item removes the folder entirely", "[launchpad][folders]") {
    ComScope com;
    REQUIRE(com.ok);

    core::Config config = MakeConfig();
    const std::string id = dock::CreateLaunchpadFolder(config, "Games", {"only"});
    REQUIRE_FALSE(id.empty());

    CHECK(dock::RemoveFromLaunchpadFolder(config, id, "ONLY"));
    // C-18 discards empty folders on load, so leaving one behind writes an entry
    // that disappears on the next start.
    CHECK(config.launchpad.folders.empty());
    CHECK(dock::FindLaunchpadFolder(config, id) == nullptr);

    // Removing something that is not there is reported, not silently ignored.
    const std::string other = dock::CreateLaunchpadFolder(config, "Other", {"keep"});
    REQUIRE_FALSE(other.empty());
    CHECK_FALSE(dock::RemoveFromLaunchpadFolder(config, other, "absent"));
    CHECK(dock::RemoveFromLaunchpadFolder(config, other, "keep"));
    CHECK(config.launchpad.folders.empty());
}

TEST_CASE("DeleteLaunchpadFolder only reports success when it deleted",
          "[launchpad][folders]") {
    ComScope com;
    REQUIRE(com.ok);

    core::Config config = MakeConfig();
    const std::string id = dock::CreateLaunchpadFolder(config, "Games", {"a"});
    REQUIRE_FALSE(id.empty());

    CHECK(dock::DeleteLaunchpadFolder(config, id));
    CHECK_FALSE(dock::DeleteLaunchpadFolder(config, id));
    CHECK_FALSE(dock::DeleteLaunchpadFolder(config, "never-existed"));
}

TEST_CASE("NewFolderId does not repeat within a process", "[launchpad][folders]") {
    ComScope com;
    REQUIRE(com.ok);

    // Two folders created in the same run must not share an id: the grid would
    // treat them as one and a rename would hit both.
    std::vector<std::string> ids;
    core::Config config = MakeConfig();
    for (int i = 0; i < 16; ++i) {
        const std::string id = dock::CreateLaunchpadFolder(config, "f" + std::to_string(i), {"a"});
        REQUIRE_FALSE(id.empty());
        ids.push_back(id);
    }
    std::vector<std::string> sorted = ids;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
    CHECK(config.launchpad.folders.size() == ids.size());
}

// ------------------------------------------------------------- query matching

TEST_CASE("MatchesQuery folds case and only prefixes by default", "[launchpad][search][match]") {
    using Mode = shell::LaunchpadIndex::MatchMode;
    // The folding rule is CompareStringEx NORM_IGNORECASE (C-18 / T-042 DoD 1), so
    // "VIS" finds "Visual Studio Code" the way the Start menu's own box does.
    CHECK(shell::LaunchpadIndex::MatchesQuery(L"Visual Studio Code", L"vis", Mode::Prefix));
    CHECK(shell::LaunchpadIndex::MatchesQuery(L"VISUAL STUDIO CODE", L"visual", Mode::Prefix));
    // A prefix filter must not match the middle of a name.
    CHECK_FALSE(shell::LaunchpadIndex::MatchesQuery(L"Visual Studio Code", L"studio",
                                                    Mode::Prefix));
    // The same query in substring mode does.
    CHECK(shell::LaunchpadIndex::MatchesQuery(L"Visual Studio Code", L"studio",
                                              Mode::Substring));
    // CJK passes through unchanged - there is no case to fold, and the comparison
    // must not break one.
    CHECK(shell::LaunchpadIndex::MatchesQuery(L"记事本", L"记", Mode::Prefix));
    CHECK(shell::LaunchpadIndex::MatchesQuery(L"记事本", L"事本", Mode::Substring));
    // An empty query is "match everything", which is what the grid shows before the
    // user types.
    CHECK(shell::LaunchpadIndex::MatchesQuery(L"Anything", L"", Mode::Prefix));
    // A query longer than the name can never be a prefix of it.
    CHECK_FALSE(shell::LaunchpadIndex::MatchesQuery(L"ab", L"abc", Mode::Prefix));
}

TEST_CASE("Filter returns index positions and agrees with the empty-query identity",
          "[launchpad][search][match]") {
    ComScope com;
    if (!com.ok) {
        WARN("COM unavailable; skipping");
        return;
    }
    shell::LaunchpadIndex index;
    index.Rebuild(core::LaunchpadConfig{});
    if (index.Empty()) {
        WARN("AppsFolder scan produced no items; skipping");
        return;
    }

    // An empty query is the identity projection: every position, in order.
    const std::vector<int> all = index.Filter(L"", shell::LaunchpadIndex::MatchMode::Prefix);
    REQUIRE(all.size() == index.Items().size());
    for (std::size_t i = 0; i < all.size(); ++i) {
        CHECK(all[i] == static_cast<int>(i));
    }

    // A prefix that cannot match anything returns nothing rather than everything,
    // which is the failure mode of a broken filter.
    CHECK(index.Filter(L"zzzz-no-such-app-zzzz",
                       shell::LaunchpadIndex::MatchMode::Prefix)
              .empty());

    // Every prefix hit must really be a prefix of the item it points at, checked with
    // the same folding rule the filter uses - the two cannot disagree silently.
    const std::vector<int> hits =
        index.Filter(index.Items().front().app.display_name.substr(0, 1),
                     shell::LaunchpadIndex::MatchMode::Prefix);
    for (const int position : hits) {
        REQUIRE(position >= 0);
        REQUIRE(position < static_cast<int>(index.Items().size()));
        const std::wstring& name = index.Items()[static_cast<std::size_t>(position)]
                                       .app.display_name;
        CHECK(shell::LaunchpadIndex::MatchesQuery(
            name, index.Items().front().app.display_name.substr(0, 1),
            shell::LaunchpadIndex::MatchMode::Prefix));
    }
}
