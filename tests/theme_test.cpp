// Tests for theme pack loading (T-052, CONTRACTS.md C-02).
//
// The themes directory is shared with the product (%APPDATA%\DockForge\themes),
// so every case that writes a pack uses a tick-stamped name and cleans up - the
// same discipline the icon cache tests taught (PID reuse made a stale directory
// flip an assertion once).
#include <catch_amalgamated.hpp>

#include <windows.h>

#include <string>
#include <vector>

#include "core/config.h"
#include "core/paths.h"
#include "core/theme.h"

namespace core = dockforge::core;

namespace {

std::wstring ThemeDir(const std::string& name) {
    return core::ThemesDir() + L"\\" + core::Utf8ToWide(name);
}

// Writes themes/<name>/theme.json and removes it on scope exit.
struct TempTheme {
    explicit TempTheme(const std::string& name, const std::string& json) : name(name) {
        const std::wstring dir = ThemeDir(name);
        CreateDirectoryW(core::ThemesDir().c_str(), nullptr);
        CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring file = dir + L"\\theme.json";
        HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(h != INVALID_HANDLE_VALUE);
        DWORD written = 0;
        WriteFile(h, json.data(), static_cast<DWORD>(json.size()), &written, nullptr);
        CloseHandle(h);
    }
    ~TempTheme() {
        DeleteFileW((ThemeDir(name) + L"\\theme.json").c_str());
        RemoveDirectoryW(ThemeDir(name).c_str());
    }
    TempTheme(const TempTheme&) = delete;
    TempTheme& operator=(const TempTheme&) = delete;
    std::string name;
};

std::uint64_t Stamp() {
    static std::uint64_t counter = 0;
    return GetTickCount64() + (++counter);
}

}  // namespace

TEST_CASE("The themeable key table matches the contract", "[theme]") {
    CHECK(core::IsThemeableKey("dock.icon_size"));
    CHECK(core::IsThemeableKey("appearance.material"));
    CHECK(core::IsThemeableKey("launchpad.columns"));
    // User data and behaviour are never themeable (C-01/C-18).
    CHECK_FALSE(core::IsThemeableKey("launchpad.page_size"));
    CHECK_FALSE(core::IsThemeableKey("dock.tiles"));
    CHECK_FALSE(core::IsThemeableKey("behavior.autostart"));
    CHECK_FALSE(core::IsThemeableKey("launchpad.extra_paths"));
    CHECK_FALSE(core::IsThemeableKey("nonsense.key"));
}

TEST_CASE("Overrides apply only themeable keys and ignore the rest", "[theme]") {
    core::Config config;
    const std::string overrides = R"({
        "appearance": {"material": "solid", "corner_radius": 8},
        "dock": {"icon_size": 40, "magnification": 2.2},
        "launchpad": {"columns": 8, "page_size": 70},
        "behavior": {"autostart": true},
        "unknown_group": {"whatever": 1}
    })";

    std::vector<std::string> ignored;
    const int applied = core::ApplyThemeOverrides(overrides, config, &ignored);
    CHECK(applied == 5);
    CHECK(config.appearance.material == core::MaterialLevel::Solid);
    CHECK(config.appearance.corner_radius == 8);
    CHECK(config.dock.icon_size == 40);
    CHECK(config.dock.magnification == 2.2f);
    CHECK(config.launchpad.columns == 8);
    // Non-themeable and unknown keys are reported, never applied.
    CHECK(config.launchpad.page_size == core::Config{}.launchpad.page_size);
    CHECK(config.behavior.autostart == false);
    CHECK(ignored.size() == 3);
}

TEST_CASE("Overrides clamp through the same ranges boot uses", "[theme]") {
    core::Config config;
    const int applied =
        core::ApplyThemeOverrides(R"({"dock": {"icon_size": 9999}})", config, nullptr);
    CHECK(applied == 1);
    CHECK(config.dock.icon_size == core::IconSizeRange().max);
}

TEST_CASE("The built-in themes always resolve", "[theme]") {
    // default applies nothing
    core::Config config;
    config.dock.icon_size = 40;
    core::Config untouched = config;
    std::string error;
    REQUIRE(core::LoadThemeInto("default", config, &error));
    CHECK(config.dock.icon_size == untouched.dock.icon_size);

    // dark is a real builtin even with no themes directory content
    core::Config dark;
    REQUIRE(core::LoadThemeInto("dark", dark, &error));
    CHECK(dark.appearance.material == core::MaterialLevel::Solid);
}

TEST_CASE("A missing pack leaves the config untouched", "[theme]") {
    core::Config config;
    config.dock.icon_size = 40;
    const std::string ghost = "ghost-theme-" + std::to_string(Stamp());
    core::Config copy = config;
    std::string error;
    CHECK_FALSE(core::LoadThemeInto(ghost, config, &error));
    CHECK(config.dock.icon_size == copy.dock.icon_size);
    CHECK(config.appearance.material == copy.appearance.material);
}

TEST_CASE("A pack file extends the base and applies both override sets", "[theme]") {
    const std::uint64_t stamp = Stamp();
    const std::string base = "t052-base-" + std::to_string(stamp);
    const std::string pack = "t052-pack-" + std::to_string(stamp);
    TempTheme baseGuard(base, R"({
        "name": "base", "extends": "default",
        "overrides": {"appearance": {"corner_radius": 4}}
    })");
    TempTheme packGuard(pack, R"({
        "name": "pack", "extends": ")" + base + R"(",
        "overrides": {"appearance": {"material": "blur"}}
    })");

    core::Config config;
    std::string error;
    REQUIRE(core::LoadThemeInto(pack, config, &error));
    // Base overrides first, pack overrides second - both visible.
    CHECK(config.appearance.corner_radius == 4);
    CHECK(config.appearance.material == core::MaterialLevel::Blur);

    // The theme list now contains both packs plus the built-ins.
    const std::vector<std::string> names = core::ListThemes();
    CHECK(names.size() >= 4);
    CHECK(names[0] == "default");
    CHECK(names[1] == "dark");
}

TEST_CASE("A circular extends chain is rejected, not hung", "[theme]") {
    const std::uint64_t stamp = Stamp();
    const std::string a = "t052-a-" + std::to_string(stamp);
    const std::string b = "t052-b-" + std::to_string(stamp);
    TempTheme aGuard(a, R"({"name": "a", "extends": ")" + b + R"(",
        "overrides": {"appearance": {"corner_radius": 2}}})");
    TempTheme bGuard(b, R"({"name": "b", "extends": ")" + a + R"(",
        "overrides": {"appearance": {"corner_radius": 3}}})");

    core::Config config;
    config.appearance.corner_radius = 22;
    std::string error;
    CHECK_FALSE(core::LoadThemeInto(a, config, &error));
    // The config survives a rejected theme exactly as it was.
    CHECK(config.appearance.corner_radius == 22);
}

TEST_CASE("A corrupt theme file is rejected, not applied", "[theme]") {
    const std::string name = "t052-bad-" + std::to_string(Stamp());
    TempTheme guard(name, "{ this is not json");
    core::Config config;
    std::string error;
    CHECK_FALSE(core::LoadThemeInto(name, config, &error));
    CHECK(config.dock.icon_size == core::Config{}.dock.icon_size);
}
