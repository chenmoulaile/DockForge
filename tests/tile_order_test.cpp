// DockForge - curated tile order tests (T-013).
//
// The order rules are the only part of the reorder feature that survives a restart,
// so they are tested as pure functions: no window, no shell, no config file. A test
// that needed a live AppsFolder could not run on every commit, and these rules are
// exactly the ones that break silently.
#include <catch_amalgamated.hpp>

#include <string>
#include <vector>

#include "dock/tile_order.h"

namespace dock = dockforge::dock;
namespace shell = dockforge::shell;
namespace core = dockforge::core;

namespace {

shell::AppEntry MakeApp(const std::string& name, const std::string& aumid = {},
                        const std::string& path = {}) {
    shell::AppEntry app;
    app.kind = shell::AppKind::Executable;
    app.display_name = std::wstring(name.begin(), name.end());
    if (!aumid.empty()) app.aumid = std::wstring(aumid.begin(), aumid.end());
    if (!path.empty()) app.target_path = std::wstring(path.begin(), path.end());
    return app;
}

std::vector<shell::AppEntry> MakeScan() {
    // Scan order is alphabetical by display name, which is what AppsFolder gives.
    return {MakeApp("Alpha", "alpha.app", R"(C:\Apps\alpha.exe)"),
            MakeApp("Bravo", "bravo.app", R"(C:\Apps\bravo.exe)"),
            MakeApp("Charlie", {}, R"(C:\Apps\charlie.exe)"),
            MakeApp("Delta", "delta.app", R"(C:\Apps\delta.exe)")};
}

std::vector<std::string> NamesOf(const std::vector<shell::AppEntry>& apps) {
    std::vector<std::string> names;
    names.reserve(apps.size());
    for (const auto& app : apps) names.push_back(std::string(app.display_name.begin(),
                                                             app.display_name.end()));
    return names;
}

}  // namespace

TEST_CASE("IdentityOf prefers the AUMID over the path", "[dock][order]") {
    // Every packaged app's window is hosted by ApplicationFrameHost.exe, so the path
    // is shared; only the AUMID distinguishes them (C-13b). The same rule has to hold
    // for the persisted order or two apps would trade places on restart.
    const auto app = MakeApp("Calculator", "microsoft.windowscalculator_8wekyb3d8bbwe!app",
                             R"(C:\Program Files\WindowsApps\Calculator\CalculatorApp.exe)");
    CHECK(dock::IdentityOf(app) == "microsoft.windowscalculator_8wekyb3d8bbwe!app");
}

TEST_CASE("IdentityOf falls back through namespace then path then name",
          "[dock][order]") {
    shell::AppEntry ns;
    ns.kind = shell::AppKind::NamespaceItem;
    ns.display_name = L"Command Prompt";
    ns.namespace_parsing = L"{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\cmd.exe";
    CHECK(dock::IdentityOf(ns) ==
          "{1ac14e77-02e7-4e5d-b744-2eb1ae5198b7}\\cmd.exe");

    const auto path_only = MakeApp("Charlie", {}, R"(C:\Apps\Charlie.exe)");
    CHECK(dock::IdentityOf(path_only) == "c:\\apps\\charlie.exe");

    // Last resort: a display name. Two apps could share one, which is why it is last,
    // but a tile with no identity at all could never be reordered or hidden and would
    // come back after every restart.
    shell::AppEntry name_only;
    name_only.display_name = L"Nameless";
    CHECK(dock::IdentityOf(name_only) == "nameless");
}

TEST_CASE("IdentityOf lowercases so case changes do not orphan a tile",
          "[dock][order]") {
    // Windows paths and AUMIDs are case-insensitive. A shell update that changed the
    // casing would otherwise silently drop the user's arrangement.
    const auto upper = MakeApp("App", "Anysphere.Cursor");
    const auto lower = MakeApp("App", "anysphere.cursor");
    CHECK(dock::IdentityOf(upper) == dock::IdentityOf(lower));
}

TEST_CASE("an unmanaged dock auto-populates from the scan", "[dock][order]") {
    const auto scan = MakeScan();
    const auto result = dock::ApplyOrder(scan, {}, {}, 12);
    CHECK(NamesOf(result) == std::vector<std::string>{"Alpha", "Bravo", "Charlie", "Delta"});
}

TEST_CASE("an unmanaged dock respects the tile limit", "[dock][order]") {
    const auto result = dock::ApplyOrder(MakeScan(), {}, {}, 2);
    CHECK(NamesOf(result) == std::vector<std::string>{"Alpha", "Bravo"});
}

TEST_CASE("an unmanaged dock skips hidden entries", "[dock][order]") {
    const auto result = dock::ApplyOrder(MakeScan(), {}, {"bravo.app"}, 12);
    CHECK(NamesOf(result) == std::vector<std::string>{"Alpha", "Charlie", "Delta"});
}

TEST_CASE("a curated order wins over scan order", "[dock][order]") {
    const std::vector<std::string> order{"delta.app", "alpha.app"};
    const auto result = dock::ApplyOrder(MakeScan(), order, {}, 12);
    CHECK(NamesOf(result) == std::vector<std::string>{"Delta", "Alpha"});
}

TEST_CASE("a curated dock does not auto-fill from the scan", "[dock][order]") {
    // The rule that makes removal legible: after dragging a tile off, the slot must
    // stay empty. Auto-appending would slide the next scanned app into it in the same
    // frame, and the user would read that as "removal did not work".
    const std::vector<std::string> order{"alpha.app", "c:\\apps\\charlie.exe"};
    const auto result = dock::ApplyOrder(MakeScan(), order, {}, 12);
    CHECK(NamesOf(result) == std::vector<std::string>{"Alpha", "Charlie"});
}

TEST_CASE("a curated order drops names whose app is gone", "[dock][order]") {
    // Uninstalling an app must not stop the dock from starting.
    const std::vector<std::string> order{"alpha.app", "uninstalled.app", "delta.app"};
    const auto result = dock::ApplyOrder(MakeScan(), order, {}, 12);
    CHECK(NamesOf(result) == std::vector<std::string>{"Alpha", "Delta"});
}

TEST_CASE("a curated order with nothing resolvable yields an empty dock",
          "[dock][order]") {
    const std::vector<std::string> order{"gone.app", "also-gone.app"};
    const auto result = dock::ApplyOrder(MakeScan(), order, {}, 12);
    CHECK(result.empty());
}

TEST_CASE("hidden entries are excluded even when the order names them",
          "[dock][order]") {
    // `hidden` is exclusion, `tiles` is order. A stale order entry that the user has
    // since removed must not resurrect the tile.
    const std::vector<std::string> order{"alpha.app", "bravo.app"};
    const auto result = dock::ApplyOrder(MakeScan(), order, {"bravo.app"}, 12);
    CHECK(NamesOf(result) == std::vector<std::string>{"Alpha"});
}

TEST_CASE("hidden matching is case-insensitive", "[dock][order]") {
    const std::vector<std::string> order{"alpha.app"};
    const auto result = dock::ApplyOrder(MakeScan(), order, {"ALPHA.APP"}, 12);
    CHECK(result.empty());
}

TEST_CASE("duplicate identities in the scan both survive", "[dock][order]") {
    // The same executable can legitimately be registered twice (different arguments,
    // two Start-menu entries). Collapsing them into one would silently lose a tile.
    std::vector<shell::AppEntry> scan;
    scan.push_back(MakeApp("Terminal", {}, R"(C:\Apps\wt.exe)"));
    scan.push_back(MakeApp("Terminal (admin)", {}, R"(C:\Apps\wt.exe)"));

    const auto result = dock::ApplyOrder(scan, {}, {}, 12);
    CHECK(result.size() == 2);

    // A curated order naming that identity twice resolves both, in order.
    const std::vector<std::string> order{"c:\\apps\\wt.exe", "c:\\apps\\wt.exe"};
    const auto curated = dock::ApplyOrder(scan, order, {}, 12);
    CHECK(curated.size() == 2);
}

TEST_CASE("ApplyOrder handles a non-positive limit and an empty scan",
          "[dock][order]") {
    CHECK(dock::ApplyOrder(MakeScan(), {}, {}, 0).empty());
    CHECK(dock::ApplyOrder(MakeScan(), {}, {}, -5).empty());
    CHECK(dock::ApplyOrder({}, {}, {}, 12).empty());
    CHECK(dock::ApplyOrder({}, {"alpha.app"}, {}, 12).empty());
}

TEST_CASE("MoveTile shifts the tiles between the two slots", "[dock][order]") {
    // A drag from slot 0 to slot 2 must produce Delta? no - it must produce
    // Bravo, Charlie, Alpha: three tiles shift left, not two swapping.
    std::vector<dock::DockTile> tiles(3);
    tiles[0].app = MakeApp("Alpha");
    tiles[1].app = MakeApp("Bravo");
    tiles[2].app = MakeApp("Charlie");

    dock::MoveTile(tiles, 0, 2);
    std::vector<shell::AppEntry> as_apps;
    for (const auto& tile : tiles) as_apps.push_back(tile.app);
    CHECK(NamesOf(as_apps) == std::vector<std::string>{"Bravo", "Charlie", "Alpha"});

    dock::MoveTile(tiles, 2, 0);
    as_apps.clear();
    for (const auto& tile : tiles) as_apps.push_back(tile.app);
    CHECK(NamesOf(as_apps) == std::vector<std::string>{"Alpha", "Bravo", "Charlie"});
}

TEST_CASE("MoveTile ignores out-of-range and no-op moves", "[dock][order]") {
    std::vector<dock::DockTile> tiles(2);
    tiles[0].app = MakeApp("Alpha");
    tiles[1].app = MakeApp("Bravo");

    dock::MoveTile(tiles, 0, 0);
    dock::MoveTile(tiles, -1, 1);
    dock::MoveTile(tiles, 0, 5);
    dock::MoveTile(tiles, 5, 0);
    CHECK(tiles.size() == 2);
    CHECK(tiles[0].app.display_name == L"Alpha");
}

TEST_CASE("RemoveTile erases and reports whether it did", "[dock][order]") {
    std::vector<dock::DockTile> tiles(3);
    tiles[0].app = MakeApp("Alpha");
    tiles[1].app = MakeApp("Bravo");
    tiles[2].app = MakeApp("Charlie");

    CHECK(dock::RemoveTile(tiles, 1));
    CHECK(tiles.size() == 2);
    CHECK(tiles[0].app.display_name == L"Alpha");
    CHECK(tiles[1].app.display_name == L"Charlie");

    CHECK_FALSE(dock::RemoveTile(tiles, -1));
    CHECK_FALSE(dock::RemoveTile(tiles, 2));
    CHECK(tiles.size() == 2);
}

TEST_CASE("CommitOrder writes the visible order and the removal together",
          "[dock][order]") {
    // Both halves must be written by the same call: a removal that recorded the
    // exclusion but not the shortened order would resurrect the tile on the next
    // scan, and an order without the exclusion would be undone the moment the dock
    // went back to unmanaged.
    core::Config config;
    std::vector<dock::DockTile> tiles(2);
    tiles[0].app = MakeApp("Alpha", "alpha.app");
    tiles[1].app = MakeApp("Charlie", {}, R"(C:\Apps\charlie.exe)");

    dock::CommitOrder(config, tiles, "bravo.app");

    CHECK(config.dock.tiles ==
          std::vector<std::string>{"alpha.app", "c:\\apps\\charlie.exe"});
    CHECK(config.dock.hidden == std::vector<std::string>{"bravo.app"});
}

TEST_CASE("CommitOrder does not duplicate a hidden identity", "[dock][order]") {
    core::Config config;
    config.dock.hidden = {"bravo.app"};
    std::vector<dock::DockTile> tiles(1);
    tiles[0].app = MakeApp("Alpha", "alpha.app");

    dock::CommitOrder(config, tiles, "BRAVO.APP");
    CHECK(config.dock.hidden.size() == 1);
}

TEST_CASE("CommitOrder without a removal leaves the hidden list alone",
          "[dock][order]") {
    core::Config config;
    config.dock.hidden = {"old.app"};
    std::vector<dock::DockTile> tiles(1);
    tiles[0].app = MakeApp("Alpha", "alpha.app");

    dock::CommitOrder(config, tiles, std::string());
    CHECK(config.dock.hidden == std::vector<std::string>{"old.app"});
}

TEST_CASE("a committed order round-trips through the config serializer",
          "[dock][order]") {
    // The only place the two halves of the feature meet: the order has to survive
    // JSON, or every restart silently reverts the dock.
    core::Config config;
    config.dock.tiles = {"alpha.app", "c:\\apps\\charlie.exe"};
    config.dock.hidden = {"bravo.app"};

    const std::string text = core::SerializeConfig(config);
    core::Config parsed;
    std::string error;
    REQUIRE(core::ParseConfig(text, parsed, &error));
    CHECK(parsed.dock.tiles == config.dock.tiles);
    CHECK(parsed.dock.hidden == config.dock.hidden);
    CHECK_FALSE(parsed.needs_rewrite);
}

TEST_CASE("an empty tile list stays out of the serialized file", "[dock][order]") {
    // A default config should not advertise a feature nobody has used.
    const std::string text = core::SerializeConfig(core::Config{});
    CHECK(text.find("\"tiles\"") == std::string::npos);
    CHECK(text.find("\"hidden\"") == std::string::npos);
}

TEST_CASE("a malformed tile list is dropped and flagged for rewrite",
          "[dock][order]") {
    // The config is user-editable. A hand-edited file must degrade to "unmanaged",
    // not to a dock that cannot start or one that trusts arbitrary input.
    core::Config parsed;
    std::string error;
    const std::string text =
        R"({"dock":{"tiles":["alpha.app",42,"",["nested"]],"hidden":"nope"}})";
    REQUIRE(core::ParseConfig(text, parsed, &error));
    CHECK(parsed.dock.tiles == std::vector<std::string>{"alpha.app"});
    CHECK(parsed.dock.hidden.empty());
    CHECK(parsed.needs_rewrite);
}
