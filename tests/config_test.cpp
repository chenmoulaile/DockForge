// T-002 config tests: defaults, clamping, corrupt fallback, unknown-key survival.
#include "catch_amalgamated.hpp"

#include "core/config.h"

using namespace dockforge::core;

TEST_CASE("config: defaults match CONTRACTS.md C-01", "[config]") {
    const Config config;
    REQUIRE(config.dock.icon_size == 48);
    REQUIRE(config.dock.gap == 8);
    REQUIRE(config.dock.magnification == Catch::Approx(1.8f));
    REQUIRE(config.dock.hover_falloff == FalloffCurve::Sine);
    REQUIRE(config.dock.influence_radius == Catch::Approx(3.0f));
    REQUIRE(config.dock.falloff_sharpness == Catch::Approx(2.0f));
    REQUIRE(config.dock.position == DockPosition::Bottom);
    REQUIRE(config.appearance.material == MaterialLevel::Mica);
    REQUIRE(config.appearance.corner_radius == 22);
    REQUIRE(config.behavior.autostart == false);
    REQUIRE(config.behavior.hide_taskbar == false);
    // T-302c: the dock is on screen until the user says otherwise. A default of false
    // would mean a fresh install launches with no dock at all, which is the opposite of
    // what the product is.
    REQUIRE(config.behavior.dock_visible == true);
}

TEST_CASE("config: serialized defaults contain every schema key", "[config]") {
    const std::string text = SerializeConfig(Config{});
    for (const char* key : {"icon_size", "gap", "magnification", "hover_falloff",
                            "influence_radius", "falloff_sharpness", "position", "material",
                            "corner_radius", "autostart", "hide_taskbar", "dock_visible"}) {
        INFO("missing key: " << key);
        REQUIRE(text.find(key) != std::string::npos);
    }
}

TEST_CASE("config: out-of-range values are clamped, not rejected", "[config]") {
    const std::string text = R"({
        "dock": { "icon_size": 400, "gap": -9, "magnification": 9.9,
                  "influence_radius": 0.1, "falloff_sharpness": 99.0 },
        "appearance": { "corner_radius": 999 }
    })";

    Config config;
    std::string error;
    REQUIRE(ParseConfig(text, config, &error));
    REQUIRE(config.dock.icon_size == 96);
    REQUIRE(config.dock.gap == 0);
    REQUIRE(config.dock.magnification == Catch::Approx(2.5f));
    REQUIRE(config.dock.influence_radius == Catch::Approx(1.0f));
    REQUIRE(config.dock.falloff_sharpness == Catch::Approx(4.0f));
    REQUIRE(config.appearance.corner_radius == 40);
}

TEST_CASE("config: corrupt json is rejected without touching the target", "[config]") {
    Config config;
    config.dock.icon_size = 64;

    std::string error;
    REQUIRE_FALSE(ParseConfig("{ this is not json", config, &error));
    REQUIRE_FALSE(error.empty());
    REQUIRE(config.dock.icon_size == 64);  // unchanged

    REQUIRE_FALSE(ParseConfig("[1,2,3]", config, &error));
    REQUIRE(error == "root is not a JSON object");
}

TEST_CASE("config: unknown keys survive a load/save round trip", "[config]") {
    const std::string text = R"({
        "dock": { "icon_size": 56, "future_key": 123 },
        "appearance": { "material": "acrylic" },
        "experimental": { "nested": [1, 2, 3] },
        "top_level_scalar": "keep me"
    })";

    Config config;
    REQUIRE(ParseConfig(text, config, nullptr));
    REQUIRE(config.dock.icon_size == 56);
    REQUIRE(config.appearance.material == MaterialLevel::Acrylic);

    const std::string saved = SerializeConfig(config);
    REQUIRE(saved.find("future_key") != std::string::npos);
    REQUIRE(saved.find("experimental") != std::string::npos);
    REQUIRE(saved.find("top_level_scalar") != std::string::npos);

    // Second round trip must be stable (no duplication, no loss).
    Config again;
    REQUIRE(ParseConfig(saved, again, nullptr));
    REQUIRE(again.dock.icon_size == 56);
    REQUIRE(SerializeConfig(again) == saved);
}

TEST_CASE("config: unknown enum strings fall back to defaults", "[config]") {
    const std::string text = R"({
        "dock": { "hover_falloff": "quartic", "position": "ceiling" },
        "appearance": { "material": "obsidian" }
    })";

    Config config;
    REQUIRE(ParseConfig(text, config, nullptr));
    REQUIRE(config.dock.hover_falloff == FalloffCurve::Sine);
    REQUIRE(config.dock.position == DockPosition::Bottom);
    REQUIRE(config.appearance.material == MaterialLevel::Mica);
}

TEST_CASE("config: wrong json types keep defaults instead of throwing", "[config]") {
    const std::string text = R"({
        "dock": { "icon_size": "big", "magnification": null },
        "appearance": { "corner_radius": [1,2] },
        "behavior": { "autostart": "yes" }
    })";

    Config config;
    REQUIRE(ParseConfig(text, config, nullptr));
    REQUIRE(config.dock.icon_size == 48);
    REQUIRE(config.dock.magnification == Catch::Approx(1.8f));
    REQUIRE(config.appearance.corner_radius == 22);
    REQUIRE(config.behavior.autostart == false);
}

TEST_CASE("config: enum string round trip", "[config]") {
    for (auto curve : {FalloffCurve::Sine, FalloffCurve::CosineIntegral, FalloffCurve::Gaussian}) {
        FalloffCurve parsed{};
        REQUIRE(ParseFalloff(ToString(curve), parsed));
        REQUIRE(parsed == curve);
    }
    for (auto pos : {DockPosition::Bottom, DockPosition::Left, DockPosition::Right}) {
        DockPosition parsed{};
        REQUIRE(ParsePosition(ToString(pos), parsed));
        REQUIRE(parsed == pos);
    }
    for (auto mat : {MaterialLevel::Mica, MaterialLevel::Acrylic, MaterialLevel::Blur,
                     MaterialLevel::Solid}) {
        MaterialLevel parsed{};
        REQUIRE(ParseMaterial(ToString(mat), parsed));
        REQUIRE(parsed == mat);
    }
}

TEST_CASE("config: ClampConfig reports whether it moved anything", "[config]") {
    Config clean;
    REQUIRE_FALSE(ClampConfig(clean));

    Config dirty;
    dirty.dock.icon_size = 1000;
    REQUIRE(ClampConfig(dirty));
    REQUIRE(dirty.dock.icon_size == 96);
}

TEST_CASE("config: needs_rewrite flags exactly the repaired files", "[config]") {
    Config clean;
    REQUIRE(ParseConfig(R"({"dock":{"icon_size":64}})", clean, nullptr));
    REQUIRE_FALSE(clean.needs_rewrite);

    Config clamped;
    REQUIRE(ParseConfig(R"({"dock":{"icon_size":500}})", clamped, nullptr));
    REQUIRE(clamped.needs_rewrite);

    Config bad_enum;
    REQUIRE(ParseConfig(R"({"appearance":{"material":"obsidian"}})", bad_enum, nullptr));
    REQUIRE(bad_enum.needs_rewrite);

    Config bad_type;
    REQUIRE(ParseConfig(R"({"behavior":{"autostart":"yes"}})", bad_type, nullptr));
    REQUIRE(bad_type.needs_rewrite);
    REQUIRE(bad_type.behavior.autostart == false);
}

TEST_CASE("config: the dock can be hidden and the choice survives a round trip", "[config]") {
    // T-302c. The whole point of the flag is that a hide survives a restart, so the
    // assertion has to be a save/load round trip, not just a field assignment.
    Config hidden;
    hidden.behavior.dock_visible = false;
    const std::string text = SerializeConfig(hidden);
    REQUIRE(text.find("dock_visible") != std::string::npos);

    Config reloaded;
    REQUIRE(ParseConfig(text, reloaded, nullptr));
    REQUIRE(reloaded.behavior.dock_visible == false);
    // Not a repair: the file said exactly what the user meant.
    REQUIRE_FALSE(reloaded.needs_rewrite);
}

TEST_CASE("config: the behavior section always carries dock_visible", "[config]") {
    // Unlike the finderbar section, `behavior` writes all of its keys unconditionally -
    // autostart and hide_taskbar have always been there, and a section that sometimes
    // omits a key is harder to read than one that states the value. The assertion is
    // therefore the opposite of the finderbar one below: presence, not absence.
    const std::string text = SerializeConfig(Config{});
    REQUIRE(text.find("dock_visible") != std::string::npos);
}

TEST_CASE("config: the finder bar pins itself by default", "[config]") {
    // T-302b: the bar used to auto-hide because the work area could not be reserved.
    // The AppBar protocol changed that, so the default is now "pinned" and auto-hide is
    // the fallback. This test exists to make a silent flip back to auto-hide loud.
    Config config;
    REQUIRE(config.finderbar.auto_hide == false);
    REQUIRE(config.finderbar.enabled == true);
    const std::string text = SerializeConfig(config);
    REQUIRE(text.find("auto_hide") == std::string::npos);
}
