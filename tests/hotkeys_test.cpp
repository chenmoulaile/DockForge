// Tests for the hotkey/hotspot engine (T-069, CONTRACTS.md C-24).
//
// The Win32 registration itself is probe territory (tools/hotkeys_probe.ps1
// drives the real app with keybd_event injection); what must not rot lives in
// the pure halves asserted here: C-24 parsing/clamping/round-trip, the
// import/export merge, the catalog, the HotkeyHost bookkeeping through fake
// Register/Unregister functions, and the hotspot geometry + state machine.
#include <catch_amalgamated.hpp>

#include <windows.h>

#include <string>
#include <vector>

#include "core/config.h"
#include "core/hotkeys.h"

namespace core = dockforge::core;

// ---- Fake Win32 layer --------------------------------------------------------
namespace {

struct FakeHotkeyState {
    // id -> (modifiers passed to RegisterHotKey, vk)
    std::vector<std::pair<int, std::pair<UINT, UINT>>> registered;
    std::vector<int> unregistered;
    // Combinations the fake reports as taken (1409) when registered.
    std::vector<std::pair<UINT, UINT>> externally_held;
    // Combinations whose registration fails with a different error.
    std::vector<std::pair<UINT, UINT>> fail_with_error;

    bool Taken(const std::pair<UINT, UINT>& combo) const {
        for (const auto& held : externally_held) {
            if (held == combo) return true;
        }
        return false;
    }
};

FakeHotkeyState g_fake;

BOOL WINAPI FakeRegister(HWND, int id, UINT modifiers, UINT vk) {
    const auto combo = std::make_pair(modifiers, vk);
    for (const auto& entry : g_fake.registered) {
        if (entry.second == combo) {
            SetLastError(ERROR_HOTKEY_ALREADY_REGISTERED);
            return FALSE;
        }
    }
    if (g_fake.Taken(combo)) {
        SetLastError(ERROR_HOTKEY_ALREADY_REGISTERED);
        return FALSE;
    }
    for (const auto& failing : g_fake.fail_with_error) {
        if (failing == combo) {
            SetLastError(ERROR_INVALID_PARAMETER);  // a non-1409 failure
            return FALSE;
        }
    }
    g_fake.registered.emplace_back(id, combo);
    return TRUE;
}

BOOL WINAPI FakeUnregister(HWND, int id) {
    g_fake.unregistered.push_back(id);
    for (auto it = g_fake.registered.begin(); it != g_fake.registered.end(); ++it) {
        if (it->first == id) {
            g_fake.registered.erase(it);
            break;
        }
    }
    return TRUE;
}

void ResetFake() {
    g_fake = FakeHotkeyState{};
}

core::HotkeyConfig MakeHotkey(const char* id, UINT mods, UINT vk, const char* action,
                              bool enabled) {
    core::HotkeyConfig entry;
    entry.id = id;
    entry.modifiers = static_cast<int>(mods);
    entry.vk = static_cast<int>(vk);
    entry.action = action;
    entry.enabled = enabled;
    return entry;
}

core::HotspotConfig MakeHotspot(const char* id, core::HotspotEdge edge,
                                core::HotspotCorner corner, int size, const char* action,
                                bool enabled) {
    core::HotspotConfig entry;
    entry.id = id;
    entry.edge = edge;
    entry.corner = corner;
    entry.size_px = size;
    entry.action = action;
    entry.enabled = enabled;
    return entry;
}

}  // namespace

// ---- Action table ------------------------------------------------------------

TEST_CASE("The action table covers the four DoD categories", "[hotkeys]") {
    using Cat = core::HotkeyCategory;
    bool component = false, system = false, display = false, windows = false;
    for (int i = 0; i < core::ActionCount(); ++i) {
        const core::ActionInfo* info =
            core::FindAction(std::string(core::kActions[i].name));
        REQUIRE(info != nullptr);
        switch (info->category) {
            case Cat::ComponentToggle: component = true; break;
            case Cat::SystemControl: system = true; break;
            case Cat::DisplayMode: display = true; break;
            case Cat::WindowManagement: windows = true; break;
        }
    }
    CHECK(component);
    CHECK(system);
    CHECK(display);
    CHECK(windows);
    CHECK(core::FindAction("no_such_action") == nullptr);
}

TEST_CASE("Every catalog entry names a real action and a legal combo", "[hotkeys]") {
    for (const core::HotkeyCatalogEntry& entry : core::kHotkeyCatalog) {
        INFO("hotkey catalog id: " << entry.id);
        REQUIRE(core::FindAction(entry.action) != nullptr);
        REQUIRE((entry.modifiers & 0xF) != 0);
        REQUIRE(entry.vk >= 1);
        REQUIRE(entry.vk <= 254);
        // Catalog defaults must not shadow the built-ins (main.cpp).
        const bool builtin_combo =
            (entry.modifiers == (MOD_CONTROL | MOD_ALT) && entry.vk == VK_SPACE) ||
            (entry.modifiers == (MOD_CONTROL | MOD_ALT) && entry.vk == 'T');
        CHECK_FALSE(builtin_combo);
    }
    for (const core::HotspotCatalogEntry& entry : core::kHotspotCatalog) {
        INFO("hotspot catalog id: " << entry.id);
        REQUIRE(core::FindAction(entry.action) != nullptr);
        REQUIRE(entry.size_px >= core::HotspotSizeRange().min);
        REQUIRE(entry.size_px <= core::HotspotSizeRange().max);
    }
    CHECK(core::FindHotkeyCatalogEntry("toggle_dock") != nullptr);
    CHECK(core::FindHotkeyCatalogEntry("no_such") == nullptr);
}

// ---- C-24 config parse/serialize -----------------------------------------------

TEST_CASE("Hotkeys parse from C-24 JSON and round-trip", "[hotkeys][config]") {
    const char* json = R"({
        "hotkeys": [
            {"id": "toggle_dock", "modifiers": 3, "vk": 68, "action": "toggle_dock",
             "enabled": true},
            {"id": "disabled.one", "modifiers": 1, "vk": 65, "action": "toggle_settings",
             "enabled": false}
        ],
        "hotspots": [
            {"id": "corner_tl", "edge": "top", "corner": "tl", "size_px": 4,
             "action": "toggle_launchpad", "enabled": false},
            {"id": "edge_top", "edge": "top", "corner": null, "size_px": 2,
             "action": "toggle_dock", "enabled": true}
        ]
    })";
    core::Config config;
    std::string error;
    REQUIRE(core::ParseConfig(json, config, &error));
    REQUIRE(config.hotkeys.size() == 2);
    CHECK(config.hotkeys[0].id == "toggle_dock");
    CHECK(config.hotkeys[0].modifiers == 3);
    CHECK(config.hotkeys[0].vk == 68);
    CHECK(config.hotkeys[0].enabled);
    CHECK(config.hotkeys[1].enabled == false);
    REQUIRE(config.hotspots.size() == 2);
    CHECK(config.hotspots[0].corner == core::HotspotCorner::TopLeft);
    CHECK(config.hotspots[1].corner == core::HotspotCorner::None);
    CHECK(config.hotspots[1].edge == core::HotspotEdge::Top);

    // Round-trip: serialize -> parse reproduces the same lists.
    const std::string text = core::SerializeConfig(config);
    core::Config reparsed;
    REQUIRE(core::ParseConfig(text, reparsed, &error));
    REQUIRE(reparsed.hotkeys.size() == 2);
    REQUIRE(reparsed.hotspots.size() == 2);
    for (std::size_t i = 0; i < 2; ++i) {
        CHECK(reparsed.hotkeys[i].id == config.hotkeys[i].id);
        CHECK(reparsed.hotkeys[i].modifiers == config.hotkeys[i].modifiers);
        CHECK(reparsed.hotkeys[i].vk == config.hotkeys[i].vk);
        CHECK(reparsed.hotkeys[i].action == config.hotkeys[i].action);
        CHECK(reparsed.hotkeys[i].enabled == config.hotkeys[i].enabled);
        CHECK(reparsed.hotspots[i].id == config.hotspots[i].id);
        CHECK(reparsed.hotspots[i].corner == config.hotspots[i].corner);
        CHECK(reparsed.hotspots[i].size_px == config.hotspots[i].size_px);
        CHECK(reparsed.hotspots[i].enabled == config.hotspots[i].enabled);
    }
}

TEST_CASE("A default config carries neither shortcuts key and everything is off",
          "[hotkeys][config]") {
    core::Config config;
    const std::string text = core::SerializeConfig(config);
    CHECK(text.find("hotkeys") == std::string::npos);
    CHECK(text.find("hotspots") == std::string::npos);

    core::Config parsed;
    std::string error;
    REQUIRE(core::ParseConfig(text, parsed, &error));
    CHECK(parsed.hotkeys.empty());
    CHECK(parsed.hotspots.empty());
}

TEST_CASE("Bad shortcut entries are dropped or clamped, never fatal", "[hotkeys][config]") {
    const char* json = R"({
        "hotkeys": [
            {"id": "UPPER_CASE", "modifiers": 3, "vk": 68, "action": "toggle_dock",
             "enabled": true},
            {"id": "repeated", "modifiers": 3, "vk": 68, "action": "toggle_dock",
             "enabled": true},
            {"id": "repeated", "modifiers": 3, "vk": 68, "action": "toggle_dock",
             "enabled": true},
            {"id": "clamped", "modifiers": 16387, "vk": 999, "action": "toggle_dock",
             "enabled": true}
        ],
        "hotspots": [
            {"id": "size_clamped", "edge": "top", "corner": "tl", "size_px": 9999,
             "action": "toggle_dock", "enabled": false},
            {"id": "bad_corner", "edge": "top", "corner": "zz", "size_px": 4,
             "action": "toggle_dock", "enabled": false}
        ]
    })";
    core::Config config;
    std::string error;
    REQUIRE(core::ParseConfig(json, config, &error));
    // Invalid id dropped, duplicate id keeps the first.
    REQUIRE(config.hotkeys.size() == 2);
    CHECK(config.hotkeys[0].id == "repeated");
    CHECK(config.hotkeys[1].id == "clamped");
    // MOD_NOREPEAT bit masked off (C-24: added only at registration), vk clamped.
    CHECK(config.hotkeys[1].modifiers == 3);
    CHECK(config.hotkeys[1].vk == 254);
    REQUIRE(config.hotspots.size() == 2);
    CHECK(config.hotspots[0].size_px == core::HotspotSizeRange().max);
    CHECK(config.hotspots[1].corner == core::HotspotCorner::None);
    // Something moved, so the loader rewrites the file.
    CHECK(config.needs_rewrite);
}

// ---- Import / export --------------------------------------------------------

TEST_CASE("Export then import round-trips by id", "[hotkeys][import]") {
    core::Config config;
    config.hotkeys.push_back(MakeHotkey("toggle_dock", MOD_CONTROL | MOD_ALT, 'D',
                                        "toggle_dock", true));
    config.hotspots.push_back(
        MakeHotspot("corner_tl", core::HotspotEdge::Top, core::HotspotCorner::TopLeft, 6,
                    "toggle_launchpad", false));

    const std::string exported = core::ExportShortcutsJson(config);
    core::Config restored;
    std::string error;
    REQUIRE(core::ImportShortcutsJson(restored, exported, &error));
    REQUIRE(restored.hotkeys.size() == 1);
    REQUIRE(restored.hotspots.size() == 1);
    CHECK(restored.hotkeys[0].id == "toggle_dock");
    CHECK(restored.hotkeys[0].modifiers == (MOD_CONTROL | MOD_ALT));
    CHECK(restored.hotkeys[0].vk == 'D');
    CHECK(restored.hotkeys[0].action == "toggle_dock");
    CHECK(restored.hotkeys[0].enabled);
    CHECK(restored.hotspots[0].corner == core::HotspotCorner::TopLeft);
    CHECK(restored.hotspots[0].size_px == 6);
    CHECK(restored.hotspots[0].enabled == false);
}

TEST_CASE("Import merges by id and keeps unmentioned locals", "[hotkeys][import]") {
    core::Config config;
    config.hotkeys.push_back(MakeHotkey("keep.me", MOD_ALT, 'A', "toggle_dock", false));
    config.hotkeys.push_back(MakeHotkey("replace.me", MOD_ALT, 'B', "toggle_dock", false));
    config.hotspots.push_back(
        MakeHotspot("keep.spot", core::HotspotEdge::Left, core::HotspotCorner::None, 8,
                    "minimize_all", false));

    const std::string imported = R"({
        "hotkeys": [
            {"id": "replace.me", "modifiers": 5, "vk": 66, "action": "toggle_settings",
             "enabled": true},
            {"id": "append.me", "modifiers": 2, "vk": 67, "action": "minimize_all",
             "enabled": true}
        ]
    })";
    std::string error;
    REQUIRE(core::ImportShortcutsJson(config, imported, &error));
    REQUIRE(config.hotkeys.size() == 3);
    CHECK(config.hotkeys[0].id == "keep.me");        // untouched
    CHECK(config.hotkeys[0].enabled == false);
    CHECK(config.hotkeys[1].id == "replace.me");     // replaced in place
    CHECK(config.hotkeys[1].modifiers == 5);
    CHECK(config.hotkeys[1].action == "toggle_settings");
    CHECK(config.hotkeys[1].enabled);
    CHECK(config.hotkeys[2].id == "append.me");      // appended
    REQUIRE(config.hotspots.size() == 1);            // unmentioned local kept
    CHECK(config.hotspots[0].id == "keep.spot");
}

TEST_CASE("An import naming an unknown action is rejected whole", "[hotkeys][import]") {
    core::Config config;
    const std::string imported = R"({
        "hotkeys": [
            {"id": "a", "modifiers": 3, "vk": 65, "action": "toggle_dock", "enabled": true},
            {"id": "b", "modifiers": 3, "vk": 66, "action": "not_an_action", "enabled": true}
        ]
    })";
    std::string error;
    CHECK_FALSE(core::ImportShortcutsJson(config, imported, &error));
    CHECK(error.find("not_an_action") != std::string::npos);
    // Nothing half-applied.
    CHECK(config.hotkeys.empty());

    // Malformed JSON also rejects without touching the config.
    CHECK_FALSE(core::ImportShortcutsJson(config, "{ not json", &error));
    CHECK(config.hotkeys.empty());
}

// ---- HotkeyHost (fake Win32) --------------------------------------------------

TEST_CASE("Apply registers only enabled hotkeys with MOD_NOREPEAT or'ed in",
          "[hotkeys][host]") {
    ResetFake();
    core::HotkeyHost host(nullptr, &FakeRegister, &FakeUnregister);
    std::vector<core::HotkeyConfig> list;
    list.push_back(MakeHotkey("on", MOD_CONTROL | MOD_ALT, 'D', "toggle_dock", true));
    list.push_back(MakeHotkey("off", MOD_CONTROL | MOD_ALT, 'L', "toggle_launchpad", false));

    const std::vector<core::HotkeyHost::Result> results = host.Apply(list);
    REQUIRE(results.size() == 1);  // disabled entries produce no result row
    CHECK(results[0].ok);
    REQUIRE(g_fake.registered.size() == 1);
    // C-24 rule: MOD_NOREPEAT (0x4000) is or'ed into the modifiers.
    CHECK(g_fake.registered[0].second.first == (MOD_CONTROL | MOD_ALT | 0x4000));
    CHECK(g_fake.registered[0].second.second == 'D');
    CHECK(host.ActiveCount() == 1);

    // The WM_HOTKEY id maps back to the config entry.
    const core::HotkeyConfig* found = host.FindByWParam(
        static_cast<WPARAM>(g_fake.registered[0].first));
    REQUIRE(found != nullptr);
    CHECK(found->id == "on");
    CHECK(host.FindByWParam(static_cast<WPARAM>(core::HotkeyHost::kIdBase + 99)) == nullptr);
}

TEST_CASE("A conflicting registration reports 1409 as already_registered",
          "[hotkeys][host]") {
    ResetFake();
    // Another program (or a built-in) holds Ctrl+Alt+K.
    g_fake.externally_held.emplace_back(MOD_CONTROL | MOD_ALT | 0x4000, 'K');

    core::HotkeyHost host(nullptr, &FakeRegister, &FakeUnregister);
    std::vector<core::HotkeyConfig> list;
    list.push_back(MakeHotkey("clash", MOD_CONTROL | MOD_ALT, 'K', "toggle_dock", true));
    const std::vector<core::HotkeyHost::Result> results = host.Apply(list);
    REQUIRE(results.size() == 1);
    CHECK_FALSE(results[0].ok);
    CHECK(results[0].error == 1409);
    CHECK(results[0].already_registered);
    CHECK(host.ActiveCount() == 0);

    // A different failure keeps its raw error code and the 1409 flag off.
    ResetFake();
    g_fake.fail_with_error.emplace_back(MOD_CONTROL | MOD_ALT | 0x4000, 'K');
    core::HotkeyHost host2(nullptr, &FakeRegister, &FakeUnregister);
    const std::vector<core::HotkeyHost::Result> results2 = host2.Apply(list);
    REQUIRE(results2.size() == 1);
    CHECK_FALSE(results2[0].ok);
    CHECK(results2[0].error == ERROR_INVALID_PARAMETER);
    CHECK_FALSE(results2[0].already_registered);
}

TEST_CASE("In-config duplicate combinations are classified without hitting Win32",
          "[hotkeys][host]") {
    ResetFake();
    core::HotkeyHost host(nullptr, &FakeRegister, &FakeUnregister);
    std::vector<core::HotkeyConfig> list;
    list.push_back(MakeHotkey("first", MOD_CONTROL | MOD_ALT, 'D', "toggle_dock", true));
    list.push_back(MakeHotkey("second", MOD_ALT | MOD_CONTROL, 'D', "toggle_launchpad", true));
    const std::vector<core::HotkeyHost::Result> results = host.Apply(list);
    REQUIRE(results.size() == 2);
    CHECK(results[0].ok);
    CHECK_FALSE(results[1].ok);
    CHECK(results[1].already_registered);
    CHECK(g_fake.registered.size() == 1);
}

TEST_CASE("Re-apply unregisters the previous set; destructor unregisters the rest",
          "[hotkeys][host]") {
    ResetFake();
    {
        core::HotkeyHost host(nullptr, &FakeRegister, &FakeUnregister);
        std::vector<core::HotkeyConfig> first;
        first.push_back(MakeHotkey("a", MOD_CONTROL | MOD_ALT, 'D', "toggle_dock", true));
        host.Apply(first);
        REQUIRE(g_fake.registered.size() == 1);
        const int first_id = g_fake.registered[0].first;

        // A different config swaps the whole set.
        std::vector<core::HotkeyConfig> second;
        second.push_back(MakeHotkey("b", MOD_CONTROL | MOD_ALT, 'L', "toggle_launchpad", true));
        host.Apply(second);
        REQUIRE(g_fake.registered.size() == 1);
        CHECK(g_fake.registered[0].second.second == 'L');
        REQUIRE(g_fake.unregistered.size() == 1);
        CHECK(g_fake.unregistered[0] == first_id);
    }
    // Destructor ran: the swap-in registration is gone too (no residue).
    REQUIRE(g_fake.unregistered.size() == 2);
    CHECK(g_fake.registered.empty());
}

// ---- Hotspot geometry --------------------------------------------------------

TEST_CASE("HotspotRect computes corner squares and edge strips", "[hotspots]") {
    const RECT monitor{0, 0, 1920, 1080};

    SECTION("corners") {
        const RECT tl = core::HotspotRect(
            MakeHotspot("tl", core::HotspotEdge::Top, core::HotspotCorner::TopLeft, 4, "a", true),
            monitor);
        CHECK(tl.left == 0);
        CHECK(tl.top == 0);
        CHECK(tl.right == 4);
        CHECK(tl.bottom == 4);

        const RECT br = core::HotspotRect(
            MakeHotspot("br", core::HotspotEdge::Bottom, core::HotspotCorner::BottomRight, 8,
                        "a", true),
            monitor);
        CHECK(br.left == 1912);
        CHECK(br.top == 1072);
        CHECK(br.right == 1920);
        CHECK(br.bottom == 1080);
    }

    SECTION("whole edges") {
        const RECT top = core::HotspotRect(
            MakeHotspot("et", core::HotspotEdge::Top, core::HotspotCorner::None, 2, "a", true),
            monitor);
        CHECK(top.left == 0);
        CHECK(top.top == 0);
        CHECK(top.right == 1920);
        CHECK(top.bottom == 2);

        const RECT left = core::HotspotRect(
            MakeHotspot("el", core::HotspotEdge::Left, core::HotspotCorner::None, 3, "a", true),
            monitor);
        CHECK(left.left == 0);
        CHECK(left.right == 3);
        CHECK(left.bottom == 1080);
    }

    SECTION("corner wins over an inconsistent edge") {
        const RECT rect = core::HotspotRect(
            MakeHotspot("weird", core::HotspotEdge::Left, core::HotspotCorner::TopRight, 5, "a",
                        true),
            monitor);
        CHECK(rect.left == 1915);  // top-RIGHT corner, edge field ignored
        CHECK(rect.top == 0);
    }

    SECTION("negative-origin monitors work") {
        const RECT secondary{-1920, 0, 0, 1080};
        const RECT tl = core::HotspotRect(
            MakeHotspot("tl", core::HotspotEdge::Top, core::HotspotCorner::TopLeft, 4, "a", true),
            secondary);
        CHECK(tl.left == -1920);
        CHECK(tl.right == -1916);
    }
}

// ---- Hotspot state machine ---------------------------------------------------

TEST_CASE("A hotspot fires once on entry and re-arms only after leaving",
          "[hotspots]") {
    using core::HotspotCorner;
    using core::HotspotEdge;
    std::vector<core::HotspotConfig> list;
    list.push_back(
        MakeHotspot("tl", HotspotEdge::Top, HotspotCorner::TopLeft, 4, "toggle_launchpad", true));
    list.push_back(MakeHotspot("off", HotspotEdge::Top, HotspotCorner::TopRight, 4, "x", false));
    const RECT monitor{0, 0, 1920, 1080};

    core::HotspotTracker tracker;
    // Outside.
    CHECK(tracker.Poll({100, 100}, list, monitor) == -1);
    // Enter: fires index 0.
    CHECK(tracker.Poll({2, 2}, list, monitor) == 0);
    CHECK(tracker.ActiveIndex() == 0);
    // Still inside: no refire at any later poll.
    CHECK(tracker.Poll({3, 3}, list, monitor) == -1);
    CHECK(tracker.Poll({1, 1}, list, monitor) == -1);
    // Leave.
    CHECK(tracker.Poll({500, 500}, list, monitor) == -1);
    CHECK(tracker.ActiveIndex() == -1);
    // Re-enter: fires again.
    CHECK(tracker.Poll({2, 2}, list, monitor) == 0);

    // The disabled hotspot never fires.
    CHECK(tracker.Poll({1918, 1}, list, monitor) == -1);
    // A disabled entries-only config reports nothing at all.
    list[0].enabled = false;
    CHECK(tracker.Poll({2, 2}, list, monitor) == -1);
}

TEST_CASE("ShouldPoll throttles to the 10Hz ceiling", "[hotspots]") {
    core::HotspotTracker tracker;
    // 9 x 10ms frames: under the interval, no poll.
    for (int i = 0; i < 9; ++i) CHECK_FALSE(tracker.ShouldPoll(10.0));
    // The 10th frame crosses 100ms exactly.
    CHECK(tracker.ShouldPoll(10.0));
    // And the budget restarts.
    CHECK_FALSE(tracker.ShouldPoll(10.0));
    // A long stall (window dragged, machine asleep) yields ONE poll, not a burst.
    CHECK(tracker.ShouldPoll(5000.0));
    CHECK_FALSE(tracker.ShouldPoll(1.0));
}

TEST_CASE("SameHotkeys/SameHotspots compare every C-24 field", "[hotkeys]") {
    std::vector<core::HotkeyConfig> a, b;
    a.push_back(MakeHotkey("x", MOD_ALT, 'A', "toggle_dock", true));
    b = a;
    CHECK(core::SameHotkeys(a, b));
    b[0].enabled = false;
    CHECK_FALSE(core::SameHotkeys(a, b));

    std::vector<core::HotspotConfig> ha, hb;
    ha.push_back(MakeHotspot("s", core::HotspotEdge::Top, core::HotspotCorner::None, 2,
                             "toggle_dock", true));
    hb = ha;
    CHECK(core::SameHotspots(ha, hb));
    hb[0].size_px = 3;
    CHECK_FALSE(core::SameHotspots(ha, hb));
    hb[0].size_px = 2;
    hb.push_back(ha[0]);
    CHECK_FALSE(core::SameHotspots(ha, hb));
}
