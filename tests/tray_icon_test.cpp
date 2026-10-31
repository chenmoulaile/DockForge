#include "tray/tray_icon.h"

#include "catch_amalgamated.hpp"

#include <algorithm>
#include <string>

using dockforge::tray::BuildMenu;
using dockforge::tray::Command;
using dockforge::tray::IconBitmap;
using dockforge::tray::MenuEntry;
using dockforge::tray::MenuState;
using dockforge::tray::RenderIcon;

namespace {

const MenuEntry* Find(const std::vector<MenuEntry>& entries, Command command) {
    const auto it = std::find_if(entries.begin(), entries.end(),
                                 [command](const MenuEntry& entry) { return entry.command == command; });
    return it == entries.end() ? nullptr : &*it;
}

std::size_t CountSeparators(const std::vector<MenuEntry>& entries) {
    return static_cast<std::size_t>(
        std::count_if(entries.begin(), entries.end(), [](const MenuEntry& entry) { return entry.separator; }));
}

}  // namespace

TEST_CASE("The tray menu offers both visibility toggles before anything else") {
    // Order is the contract: a user opening a tray menu is usually looking for
    // "where did it go" or "make it go away", so those two are the first things
    // under the cursor, not buried under Settings.
    const std::vector<MenuEntry> entries = BuildMenu(MenuState{});
    REQUIRE(entries.size() >= 2);
    CHECK(entries[0].command == Command::ToggleDock);
    CHECK(entries[1].command == Command::ToggleFinderBar);
    CHECK_FALSE(entries[0].separator);
    CHECK_FALSE(entries[1].separator);
}

TEST_CASE("Quit is the last item and is never the default") {
    // A destructive item must sit at the bottom and must never be what Enter
    // activates: pressing Enter on a tray menu has to be harmless.
    const std::vector<MenuEntry> entries = BuildMenu(MenuState{});
    REQUIRE_FALSE(entries.empty());
    CHECK(entries.back().command == Command::Quit);
    CHECK(entries.back().enabled);
    CHECK(entries.front().command != Command::Quit);
}

TEST_CASE("The dock toggle follows the dock's current visibility") {
    // The vector has to outlive the lookup: Find() returns a pointer into it, and a
    // temporary built inline would dangle the moment the statement ended.
    MenuState visible;
    visible.dock_visible = true;
    const std::vector<MenuEntry> visible_entries = BuildMenu(visible);
    const MenuEntry* hide = Find(visible_entries, Command::ToggleDock);
    REQUIRE(hide != nullptr);
    CHECK(hide->label == L"Hide Dock");

    MenuState hidden;
    hidden.dock_visible = false;
    const std::vector<MenuEntry> hidden_entries = BuildMenu(hidden);
    const MenuEntry* show = Find(hidden_entries, Command::ToggleDock);
    REQUIRE(show != nullptr);
    CHECK(show->label == L"Show Dock");
}

TEST_CASE("The Finder Bar entry is checked when the bar is on") {
    MenuState on;
    on.finder_bar_enabled = true;
    const std::vector<MenuEntry> on_entries = BuildMenu(on);
    const MenuEntry* enabled = Find(on_entries, Command::ToggleFinderBar);
    REQUIRE(enabled != nullptr);
    CHECK(enabled->checked);

    MenuState off;
    off.finder_bar_enabled = false;
    const std::vector<MenuEntry> off_entries = BuildMenu(off);
    const MenuEntry* disabled = Find(off_entries, Command::ToggleFinderBar);
    REQUIRE(disabled != nullptr);
    CHECK_FALSE(disabled->checked);
    // Still enabled: a bar that is off must stay reachable, or the user cannot turn
    // it back on from here.
    CHECK(disabled->enabled);
}

TEST_CASE("Settings is greyed out when the settings window is unavailable") {
    // The settings window is optional (DOCKFORGE_SETTINGS can leave it out), so the
    // entry has to reflect that rather than opening nothing.
    MenuState without;
    without.settings_available = false;
    const std::vector<MenuEntry> without_entries = BuildMenu(without);
    const MenuEntry* entry = Find(without_entries, Command::OpenSettings);
    REQUIRE(entry != nullptr);
    CHECK_FALSE(entry->enabled);

    MenuState with;
    with.settings_available = true;
    const std::vector<MenuEntry> with_entries = BuildMenu(with);
    const MenuEntry* available = Find(with_entries, Command::OpenSettings);
    REQUIRE(available != nullptr);
    CHECK(available->enabled);
}

TEST_CASE("The menu groups its entries with separators") {
    const std::vector<MenuEntry> entries = BuildMenu(MenuState{});
    CHECK(CountSeparators(entries) == 2);
    // Every command the module can dispatch has to be reachable from the menu, or
    // the enum has a value nothing can produce.
    CHECK(Find(entries, Command::ToggleDock) != nullptr);
    CHECK(Find(entries, Command::ToggleFinderBar) != nullptr);
    CHECK(Find(entries, Command::OpenSettings) != nullptr);
    CHECK(Find(entries, Command::OpenConfigFolder) != nullptr);
    CHECK(Find(entries, Command::OpenLogs) != nullptr);
    CHECK(Find(entries, Command::Quit) != nullptr);
}

TEST_CASE("The icon glyph has real pixels at the size the shell asks for") {
    // The shell asks for SM_CXSMICON, which is 32 on this 192dpi host. A glyph that
    // is all alpha or all black shows up as a blank or black square in the tray, so
    // both counts have to be non-zero.
    const IconBitmap icon = RenderIcon(32);
    CHECK(icon.valid());
    CHECK(icon.width == 32);
    CHECK(icon.height == 32);
    int opaque = 0;
    int coloured = 0;
    icon.CountCoverage(&opaque, &coloured);
    CHECK(opaque > 100);
    CHECK(coloured > 0);
}

TEST_CASE("The icon glyph survives the small sizes the shell may pick") {
    // 16 is the classic small-icon size and 64 covers a scaled taskbar. Both have to
    // produce something visible rather than an empty buffer.
    for (const int size : {16, 24, 32, 48, 64}) {
        const IconBitmap icon = RenderIcon(size);
        INFO("size " << size);
        CHECK(icon.valid());
        int opaque = 0;
        int coloured = 0;
        icon.CountCoverage(&opaque, &coloured);
        CHECK(opaque > 0);
        CHECK(coloured > 0);
    }
}

TEST_CASE("A non-positive icon size falls back instead of producing an empty bitmap") {
    // GetSystemMetrics can return 0 in a session that has no shell yet; falling back
    // keeps the rest of the tray path from having to special-case it.
    const IconBitmap zero = RenderIcon(0);
    CHECK(zero.valid());
    CHECK(zero.width == 16);

    const IconBitmap negative = RenderIcon(-8);
    CHECK(negative.valid());
    CHECK(negative.width == 16);
}

TEST_CASE("The icon buffer is premultiplied") {
    // The buffer is handed straight to CreateDIBSection with an alpha mask, and the
    // shell composites it as premultiplied. A colour channel above its alpha would
    // be rejected as invalid premultiplied data, which is the exact bug the dock
    // icons had.
    const IconBitmap icon = RenderIcon(32);
    REQUIRE(icon.valid());
    int violations = 0;
    for (std::size_t i = 0; i + 3 < icon.pixels.size(); i += 4) {
        const int a = icon.pixels[i + 3];
        if (icon.pixels[i] > a || icon.pixels[i + 1] > a || icon.pixels[i + 2] > a) ++violations;
    }
    CHECK(violations == 0);
}

TEST_CASE("Every tray command has a distinct name") {
    // The names go into the log and the probe snapshot, so a duplicated name would
    // make two different commands indistinguishable after the fact.
    const Command all[] = {Command::None,     Command::ToggleDock,     Command::ShowDock,
                           Command::HideDock, Command::ToggleFinderBar, Command::OpenSettings,
                           Command::OpenConfigFolder, Command::OpenLogs, Command::Quit};
    std::vector<std::string> names;
    for (const Command command : all) names.emplace_back(dockforge::tray::ToString(command));
    std::sort(names.begin(), names.end());
    CHECK(std::adjacent_find(names.begin(), names.end()) == names.end());
    for (const std::string& name : names) CHECK(name != "unknown");
}

TEST_CASE("The menu command base leaves room for the shell's own ids") {
    // Menu ids are offset from a base so a stray id from another menu can never be
    // mistaken for one of ours. The base has to be above the ids the shell itself
    // hands out for default items.
    CHECK(dockforge::tray::TrayIcon::kMenuCommandBase > 100);
}
