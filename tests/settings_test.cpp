// Tests for the settings edit model (T-050, CONTRACTS.md C-20).
//
// The window itself needs a desktop; the rules that must not silently rot live
// in settings::SettingsModel and are asserted here against an isolated config
// path, the same pattern the launchpad tests use.
#include <catch_amalgamated.hpp>

#include <windows.h>

#include <cstdio>
#include <string>

#include "core/config.h"
#include "core/paths.h"
#include "settings/settings_model.h"

namespace core = dockforge::core;
namespace settings = dockforge::settings;

// The config path is isolated process-wide by tests/main_test.cpp (static init):
// DOCKFORGE_CONFIG_PATH points at a throwaway file before any test can read the
// real one. The round-trip case below uses that file directly - re-pointing the
// variable per test would fight the ConfigPath() cache and clear the invariant
// paths_test asserts on (measured: the env var vanished for later suites).

TEST_CASE("An over-range edit is clamped, not rejected", "[settings][model]") {
    core::Config config;
    config.dock.icon_size = 48;

    settings::SettingsModel model;
    model.Record("dock.icon_size", "9999");
    std::vector<std::string> changed;
    REQUIRE(model.Apply(config, &changed) == 1);

    // C-20: the value lands inside the range and the caller is told it moved,
    // so the control can echo the clamped number instead of leaving 9999.
    const core::IntRange range = core::IconSizeRange();
    CHECK(config.dock.icon_size == range.max);
    REQUIRE(changed.size() == 1);
    CHECK(changed[0] == "dock.icon_size");
}

TEST_CASE("Edits to the same field replace each other within a debounce window",
          "[settings][model]") {
    // The window restarts its 300ms timer on every keystroke; the model's
    // half of the guarantee is that the flush sees ONE value per field, not
    // the history of the typing.
    core::Config config;
    settings::SettingsModel model;
    model.Record("dock.gap", "5");
    model.Record("dock.gap", "56");
    model.Record("dock.gap", "560");
    CHECK(model.PendingCount() == 1);

    CHECK(model.Apply(config) == 1);
    CHECK(config.dock.gap == core::GapRange().max);
}

TEST_CASE("A flush applies every pending field in one pass", "[settings][model]") {
    core::Config config;
    settings::SettingsModel model;
    model.Record("dock.gap", "16");
    model.Record("dock.magnification", "2.2");
    model.Record("launchpad.page_size", "40");
    CHECK(model.PendingCount() == 3);

    std::vector<std::string> changed;
    CHECK(model.Apply(config, &changed) == 3);
    CHECK(config.dock.gap == 16);
    CHECK(config.dock.magnification == 2.2f);
    CHECK(config.launchpad.page_size == 40);
    CHECK(changed.size() == 3);
}

TEST_CASE("Enum fields parse by name and reject garbage", "[settings][model]") {
    core::Config config;
    settings::SettingsModel model;

    model.Record("dock.position", "left");
    CHECK(model.Apply(config) == 1);
    CHECK(config.dock.position == core::DockPosition::Left);

    model.Record("dock.position", "sideways");
    std::vector<std::string> rejected;
    CHECK(model.Apply(config, nullptr, &rejected) == 0);
    REQUIRE(rejected.size() == 1);
    CHECK(rejected[0] == "dock.position");
    // The stored config survives a rejected edit untouched.
    CHECK(config.dock.position == core::DockPosition::Left);

    model.Record("appearance.material", "blur");
    CHECK(model.Apply(config) == 1);
    CHECK(config.appearance.material == core::MaterialLevel::Blur);
}

TEST_CASE("Bool fields parse 0/1 and true/false", "[settings][model]") {
    core::Config config;
    settings::SettingsModel model;
    model.Record("behavior.autostart", "1");
    model.Record("behavior.hide_taskbar", "false");
    CHECK(model.Apply(config) == 2);
    CHECK(config.behavior.autostart);
    CHECK_FALSE(config.behavior.hide_taskbar);
}

TEST_CASE("Unknown fields are reported, never applied", "[settings][model]") {
    core::Config config;
    const int before = config.dock.icon_size;
    settings::SettingsModel model;
    model.Record("dock.icon", "48");
    std::vector<std::string> rejected;
    CHECK(model.Apply(config, nullptr, &rejected) == 0);
    CHECK(config.dock.icon_size == before);
    REQUIRE(rejected.size() == 1);
    // And the field table itself says so, which is what the window's RecordEdit
    // guard consults before arming the debounce timer.
    CHECK_FALSE(settings::SettingsModel::KnowsField("dock.icon"));
    CHECK(settings::SettingsModel::KnowsField("dock.icon_size"));
}

TEST_CASE("CurrentValueText mirrors the stored value for the control echo",
          "[settings][model]") {
    core::Config config;
    config.dock.icon_size = 64;
    config.dock.position = core::DockPosition::Right;
    CHECK(settings::SettingsModel::CurrentValueText(config, "dock.icon_size") == "64");
    CHECK(settings::SettingsModel::CurrentValueText(config, "dock.position") == "right");
    CHECK(settings::SettingsModel::CurrentValueText(config, "behavior.autostart") == "0");
}

// T-075: the weather fields ride the same C-20 path as every other control, so
// they get the same three guarantees - a clamped echo, a persisted round-trip,
// and no silent drop of an unparsable edit.
TEST_CASE("Weather fields clamp, echo and persist through the model",
          "[settings][model][weather]") {
    core::Config config;
    settings::SettingsModel model;
    model.Record("weather.enabled", "1");
    model.Record("weather.city", "  Beijing  ");
    model.Record("weather.refresh_minutes", "900");  // above the 360 cap
    REQUIRE(model.Apply(config) == 3);
    CHECK(config.weather.enabled);
    CHECK(config.weather.city == "Beijing");  // trimmed, exactly what was typed
    CHECK(config.weather.refresh_minutes == 360);  // clamped, not rejected

    // The echo must show disk truth, not what was typed.
    CHECK(settings::SettingsModel::CurrentValueText(config, "weather.enabled") == "1");
    CHECK(settings::SettingsModel::CurrentValueText(config, "weather.city") ==
          "Beijing");
    CHECK(settings::SettingsModel::CurrentValueText(config,
                                                    "weather.refresh_minutes") == "360");

    REQUIRE(core::SaveConfig(config));
    const core::Config reloaded = core::LoadConfig();
    CHECK(reloaded.weather.enabled);
    CHECK(reloaded.weather.city == "Beijing");
    CHECK(reloaded.weather.refresh_minutes == 360);
    // The same-value guard the dock owner uses must not report a change here,
    // or the settings flush would rebuild the weather host for nothing.
    CHECK(core::SameWeather(reloaded.weather, config.weather));
}

TEST_CASE("An empty weather city is a legal edit that disables the tile",
          "[settings][model][weather]") {
    core::Config config;
    config.weather.enabled = true;
    config.weather.city = "Beijing";
    settings::SettingsModel model;
    model.Record("weather.city", "   ");  // user cleared the box
    std::vector<std::string> rejected;
    REQUIRE(model.Apply(config, nullptr, &rejected) == 1);
    CHECK(rejected.empty());
    CHECK(config.weather.city.empty());
    CHECK_FALSE(core::SameWeather(config.weather, core::WeatherConfig{}));
}

TEST_CASE("A flushed model round-trips through the config file", "[settings][model]") {
    core::Config config;
    config.dock.icon_size = 56;

    settings::SettingsModel model;
    model.Record("dock.gap", "20");
    model.Record("launchpad.columns", "8");
    REQUIRE(model.Apply(config) == 2);
    REQUIRE(core::SaveConfig(config));

    // A fresh load must see exactly what the flush wrote - this is the part of
    // C-20 that makes "UI shows the value = disk has the value" true.
    const core::Config reloaded = core::LoadConfig();
    CHECK(reloaded.dock.gap == 20);
    CHECK(reloaded.launchpad.columns == 8);
    CHECK(reloaded.dock.icon_size == 56);
}
