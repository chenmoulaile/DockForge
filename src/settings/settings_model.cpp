#include "settings/settings_model.h"

#include "core/hotkeys.h"
#include "core/log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dockforge::settings {
namespace {

constexpr char kLogModule[] = "settings";

struct FieldSpec {
    const char* name;
    // Kind discriminator: int/float ranges come from core, enums parse by name,
    // bools parse 0/1/false/true, City is a free-text identifier.
    enum class Kind { Int, Float, Bool, Position, Material, Falloff, Theme, City } kind;
};

// The field table is the single place a settings control maps onto C-01. The
// ranges are NOT duplicated here: Apply() reads them from the core accessors
// (IconSizeRange etc.), so the settings UI can never drift from the loader.
constexpr FieldSpec kFields[] = {
    {"dock.icon_size", FieldSpec::Kind::Int},
    {"dock.gap", FieldSpec::Kind::Int},
    {"dock.magnification", FieldSpec::Kind::Float},
    {"dock.influence_radius", FieldSpec::Kind::Float},
    {"dock.falloff_sharpness", FieldSpec::Kind::Float},
    {"dock.position", FieldSpec::Kind::Position},
    {"launchpad.columns", FieldSpec::Kind::Int},
    {"launchpad.icon_size", FieldSpec::Kind::Int},
    {"launchpad.page_size", FieldSpec::Kind::Int},
    {"appearance.material", FieldSpec::Kind::Material},
    {"appearance.theme", FieldSpec::Kind::Theme},
    {"appearance.corner_radius", FieldSpec::Kind::Int},
    {"behavior.autostart", FieldSpec::Kind::Bool},
    {"behavior.hide_taskbar", FieldSpec::Kind::Bool},
    // T-075: the weather tile config. SameWeather() is the owner's change
    // detector, so an edit that lands on the stored value moves nothing.
    {"weather.enabled", FieldSpec::Kind::Bool},
    {"weather.city", FieldSpec::Kind::City},
    {"weather.refresh_minutes", FieldSpec::Kind::Int},
    // T-065 (C-25): one switch per shipped adapter. The adapter id sits verbatim
    // in the field name, matching the C-01 template
    // integrations.message_counts.<adapter_id>.enabled.
    {"integrations.message_counts.weixin.exe.enabled", FieldSpec::Kind::Bool},
    {"integrations.message_counts.qq.exe.enabled", FieldSpec::Kind::Bool},
};

// Strips `prefix` and `suffix` off `field`, returning the id between them.
bool ShortcutFieldId(const std::string& field, const char* prefix, const char* suffix,
                     std::string* id) {
    const std::size_t prefix_len = std::strlen(prefix);
    const std::size_t suffix_len = std::strlen(suffix);
    if (field.size() <= prefix_len + suffix_len) return false;
    if (field.compare(0, prefix_len, prefix) != 0) return false;
    if (field.compare(field.size() - suffix_len, suffix_len, suffix) != 0) return false;
    id->assign(field, prefix_len, field.size() - prefix_len - suffix_len);
    return true;
}

const FieldSpec* FindField(const std::string& field) {
    for (const FieldSpec& spec : kFields) {
        if (field == spec.name) return &spec;
    }
    // T-069: shortcut fields are matched by shape, not by a static row, so the
    // catalog can grow without touching this table:
    //   hotkeys.<catalog id>.enabled
    //   hotspots.<catalog id>.enabled
    //   hotspots.<catalog id>.size_px
    static const FieldSpec kHotkeyEnabled{"", FieldSpec::Kind::Bool};
    static const FieldSpec kHotspotEnabled{"", FieldSpec::Kind::Bool};
    static const FieldSpec kHotspotSize{"", FieldSpec::Kind::Int};
    std::string id;
    if (ShortcutFieldId(field, "hotkeys.", ".enabled", &id)) {
        return core::FindHotkeyCatalogEntry(id) != nullptr ? &kHotkeyEnabled : nullptr;
    }
    if (ShortcutFieldId(field, "hotspots.", ".enabled", &id)) {
        return core::FindHotspotCatalogEntry(id) != nullptr ? &kHotspotEnabled : nullptr;
    }
    if (ShortcutFieldId(field, "hotspots.", ".size_px", &id)) {
        return core::FindHotspotCatalogEntry(id) != nullptr ? &kHotspotSize : nullptr;
    }
    return nullptr;
}

bool ParseBool(const std::string& text, bool& out) {
    if (text == "1" || text == "true" || text == "True") {
        out = true;
        return true;
    }
    if (text == "0" || text == "false" || text == "False") {
        out = false;
        return true;
    }
    return false;
}

int ClampInt(int value, const core::IntRange& range) {
    return std::clamp(value, range.min, range.max);
}

float ClampFloat(float value, const core::FloatRange& range) {
    return std::clamp(value, range.min, range.max);
}

}  // namespace

bool SettingsModel::KnowsField(const std::string& field) {
    return FindField(field) != nullptr;
}

void SettingsModel::Record(const std::string& field, const std::string& text) {
    // Same-field replaces: a debounce window that saw "5" then "56" must flush
    // one edit, not two - the second would clamp 5 first and echo 32 back at the
    // user while they are still typing.
    for (PendingEdit& edit : pending_) {
        if (edit.field == field) {
            edit.text = text;
            return;
        }
    }
    pending_.push_back(PendingEdit{field, text});
}

void SettingsModel::Clear() { pending_.clear(); }

std::string SettingsModel::CurrentValueText(const core::Config& config,
                                            const std::string& field) {
    const FieldSpec* spec = FindField(field);
    if (spec == nullptr) return {};
    char buffer[64]{};
    switch (spec->kind) {
        case FieldSpec::Kind::Int:
            if (field == "dock.icon_size") std::snprintf(buffer, sizeof(buffer), "%d", config.dock.icon_size);
            if (field == "dock.gap") std::snprintf(buffer, sizeof(buffer), "%d", config.dock.gap);
            if (field == "launchpad.columns") std::snprintf(buffer, sizeof(buffer), "%d", config.launchpad.columns);
            if (field == "launchpad.icon_size") std::snprintf(buffer, sizeof(buffer), "%d", config.launchpad.icon_size);
            if (field == "launchpad.page_size") std::snprintf(buffer, sizeof(buffer), "%d", config.launchpad.page_size);
            if (field == "appearance.corner_radius") std::snprintf(buffer, sizeof(buffer), "%d", config.appearance.corner_radius);
            if (field == "weather.refresh_minutes") std::snprintf(buffer, sizeof(buffer), "%d", config.weather.refresh_minutes);
            // T-069: hotspot size reads the entry, or the catalog default when
            // the entry does not exist yet.
            {
                std::string id;
                if (ShortcutFieldId(field, "hotspots.", ".size_px", &id)) {
                    const core::HotspotCatalogEntry* catalog =
                        core::FindHotspotCatalogEntry(id);
                    int value = catalog != nullptr ? catalog->size_px : 4;
                    for (const core::HotspotConfig& entry : config.hotspots) {
                        if (entry.id == id) {
                            value = entry.size_px;
                            break;
                        }
                    }
                    std::snprintf(buffer, sizeof(buffer), "%d", value);
                }
            }
            break;
        case FieldSpec::Kind::Float:
            if (field == "dock.magnification") std::snprintf(buffer, sizeof(buffer), "%.2f", config.dock.magnification);
            if (field == "dock.influence_radius") std::snprintf(buffer, sizeof(buffer), "%.2f", config.dock.influence_radius);
            if (field == "dock.falloff_sharpness") std::snprintf(buffer, sizeof(buffer), "%.2f", config.dock.falloff_sharpness);
            break;
        case FieldSpec::Kind::Bool:
            if (field == "behavior.autostart") return config.behavior.autostart ? "1" : "0";
            if (field == "behavior.hide_taskbar") return config.behavior.hide_taskbar ? "1" : "0";
            if (field == "weather.enabled") return config.weather.enabled ? "1" : "0";
            // T-065: adapter switches default to ON when the id is absent.
            if (field == "integrations.message_counts.weixin.exe.enabled") {
                const auto it = config.integrations.message_counts.find("weixin.exe");
                return it == config.integrations.message_counts.end() || it->second ? "1" : "0";
            }
            if (field == "integrations.message_counts.qq.exe.enabled") {
                const auto it = config.integrations.message_counts.find("qq.exe");
                return it == config.integrations.message_counts.end() || it->second ? "1" : "0";
            }
            // T-069: an absent catalog entry reads as disabled (C-24 default).
            {
                std::string id;
                if (ShortcutFieldId(field, "hotkeys.", ".enabled", &id)) {
                    for (const core::HotkeyConfig& entry : config.hotkeys) {
                        if (entry.id == id) return entry.enabled ? "1" : "0";
                    }
                    return "0";
                }
                if (ShortcutFieldId(field, "hotspots.", ".enabled", &id)) {
                    for (const core::HotspotConfig& entry : config.hotspots) {
                        if (entry.id == id) return entry.enabled ? "1" : "0";
                    }
                    return "0";
                }
            }
            break;
        case FieldSpec::Kind::Position:
            return core::ToString(config.dock.position);
        case FieldSpec::Kind::Material:
            return core::ToString(config.appearance.material);
        case FieldSpec::Kind::Falloff:
            return core::ToString(config.dock.hover_falloff);
        case FieldSpec::Kind::Theme:
            return config.appearance.theme;
        case FieldSpec::Kind::City:
            return config.weather.city;
    }
    return std::string(buffer);
}

int SettingsModel::Apply(core::Config& config, std::vector<std::string>* changed,
                         std::vector<std::string>* rejected) const {
    int applied = 0;
    for (const PendingEdit& edit : pending_) {
        const FieldSpec* spec = FindField(edit.field);
        if (spec == nullptr) {
            if (rejected != nullptr) rejected->push_back(edit.field);
            DF_WARN(kLogModule, "unknown settings field '%s'", edit.field.c_str());
            continue;
        }

        // Every branch sets `moved` when the parsed value differs from the stored
        // one after clamping - that is what decides the echo, not whether the
        // text parsed "nicely".
        bool parsed = false;
        bool moved = false;
        switch (spec->kind) {
            case FieldSpec::Kind::Int: {
                const long long value = std::atoll(edit.text.c_str());
                parsed = edit.text.find_first_not_of(" \t-0123456789") == std::string::npos &&
                         !edit.text.empty();
                if (!parsed) break;
                int target = 0;
                if (edit.field == "dock.icon_size") {
                    target = ClampInt(static_cast<int>(value), core::IconSizeRange());
                    moved = target != config.dock.icon_size;
                    config.dock.icon_size = target;
                } else if (edit.field == "dock.gap") {
                    target = ClampInt(static_cast<int>(value), core::GapRange());
                    moved = target != config.dock.gap;
                    config.dock.gap = target;
                } else if (edit.field == "launchpad.columns") {
                    target = ClampInt(static_cast<int>(value), core::LaunchpadColumnsRange());
                    moved = target != config.launchpad.columns;
                    config.launchpad.columns = target;
                } else if (edit.field == "launchpad.icon_size") {
                    target = ClampInt(static_cast<int>(value), core::LaunchpadIconSizeRange());
                    moved = target != config.launchpad.icon_size;
                    config.launchpad.icon_size = target;
                } else if (edit.field == "launchpad.page_size") {
                    target = ClampInt(static_cast<int>(value), core::LaunchpadPageSizeRange());
                    moved = target != config.launchpad.page_size;
                    config.launchpad.page_size = target;
                } else if (edit.field == "appearance.corner_radius") {
                    target = ClampInt(static_cast<int>(value), core::CornerRadiusRange());
                    moved = target != config.appearance.corner_radius;
                    config.appearance.corner_radius = target;
                } else if (edit.field == "weather.refresh_minutes") {
                    // The range lives in core next to the clamp the loader uses,
                    // so the UI can never accept an interval the dock rejects.
                    target = ClampInt(static_cast<int>(value),
                                      core::WeatherRefreshMinutesRange());
                    moved = target != config.weather.refresh_minutes;
                    config.weather.refresh_minutes = target;
                }
                // T-069: hotspot size. The entry is created on first edit with
                // the catalog's defaults (edge/corner/action) so a size edit
                // alone is enough to configure a hotspot.
                {
                    std::string id;
                    if (ShortcutFieldId(edit.field, "hotspots.", ".size_px", &id)) {
                        const core::HotspotCatalogEntry* catalog =
                            core::FindHotspotCatalogEntry(id);
                        if (catalog != nullptr) {
                            target = ClampInt(static_cast<int>(value),
                                              core::HotspotSizeRange());
                            auto it = std::find_if(
                                config.hotspots.begin(), config.hotspots.end(),
                                [&](const core::HotspotConfig& entry) {
                                    return entry.id == id;
                                });
                            if (it == config.hotspots.end()) {
                                config.hotspots.push_back(core::HotspotConfig{
                                    id, catalog->edge, catalog->corner, target,
                                    catalog->action, false});
                                moved = true;
                            } else {
                                moved = target != it->size_px;
                                it->size_px = target;
                            }
                        }
                    }
                }
                break;
            }
            case FieldSpec::Kind::Float: {
                const float value = std::strtof(edit.text.c_str(), nullptr);
                parsed = !edit.text.empty() &&
                         edit.text.find_first_not_of(" \t-+.0123456789eE") == std::string::npos;
                if (!parsed) break;
                if (edit.field == "dock.magnification") {
                    const float target = ClampFloat(value, core::MagnificationRange());
                    moved = target != config.dock.magnification;
                    config.dock.magnification = target;
                } else if (edit.field == "dock.influence_radius") {
                    const float target = ClampFloat(value, core::InfluenceRadiusRange());
                    moved = target != config.dock.influence_radius;
                    config.dock.influence_radius = target;
                } else if (edit.field == "dock.falloff_sharpness") {
                    const float target = ClampFloat(value, core::FalloffSharpnessRange());
                    moved = target != config.dock.falloff_sharpness;
                    config.dock.falloff_sharpness = target;
                }
                break;
            }
            case FieldSpec::Kind::Bool: {
                bool value = false;
                parsed = ParseBool(edit.text, value);
                if (!parsed) break;
                if (edit.field == "behavior.autostart") {
                    moved = value != config.behavior.autostart;
                    config.behavior.autostart = value;
                } else if (edit.field == "behavior.hide_taskbar") {
                    moved = value != config.behavior.hide_taskbar;
                    config.behavior.hide_taskbar = value;
                } else if (edit.field == "weather.enabled") {
                    moved = value != config.weather.enabled;
                    config.weather.enabled = value;
                } else if (edit.field.rfind("integrations.message_counts.", 0) == 0 &&
                           edit.field.size() > strlen("integrations.message_counts.") &&
                           edit.field.substr(edit.field.size() - strlen(".enabled")) ==
                               ".enabled") {
                    // Adapter id = the middle of the dotted name. Written even when
                    // unchanged: an explicit user choice must not vanish from the
                    // file just because it equals the default.
                    const std::size_t prefix = strlen("integrations.message_counts.");
                    const std::size_t suffix = strlen(".enabled");
                    const std::string id =
                        edit.field.substr(prefix, edit.field.size() - prefix - suffix);
                    const auto it = config.integrations.message_counts.find(id);
                    const bool current =
                        it == config.integrations.message_counts.end() ? true : it->second;
                    moved = value != current;
                    config.integrations.message_counts[id] = value;
                }
                // T-069: hotkey enable. On = find-or-create with the catalog
                // defaults (the checkbox is the first thing a user touches);
                // off = only an EXISTING entry flips, so unchecking a box the
                // user never turned on writes nothing (C-24 quiet default).
                {
                    std::string id;
                    if (ShortcutFieldId(edit.field, "hotkeys.", ".enabled", &id)) {
                        const core::HotkeyCatalogEntry* catalog =
                            core::FindHotkeyCatalogEntry(id);
                        if (catalog != nullptr) {
                            auto it = std::find_if(
                                config.hotkeys.begin(), config.hotkeys.end(),
                                [&](const core::HotkeyConfig& entry) {
                                    return entry.id == id;
                                });
                            if (it == config.hotkeys.end()) {
                                if (value) {
                                    config.hotkeys.push_back(core::HotkeyConfig{
                                        id, catalog->modifiers, catalog->vk,
                                        catalog->action, true});
                                    moved = true;
                                }
                            } else {
                                moved = value != it->enabled;
                                it->enabled = value;
                            }
                        }
                    } else if (ShortcutFieldId(edit.field, "hotspots.", ".enabled", &id)) {
                        const core::HotspotCatalogEntry* catalog =
                            core::FindHotspotCatalogEntry(id);
                        if (catalog != nullptr) {
                            auto it = std::find_if(
                                config.hotspots.begin(), config.hotspots.end(),
                                [&](const core::HotspotConfig& entry) {
                                    return entry.id == id;
                                });
                            if (it == config.hotspots.end()) {
                                if (value) {
                                    config.hotspots.push_back(core::HotspotConfig{
                                        id, catalog->edge, catalog->corner,
                                        catalog->size_px, catalog->action, true});
                                    moved = true;
                                }
                            } else {
                                moved = value != it->enabled;
                                it->enabled = value;
                            }
                        }
                    }
                }
                break;
            }
            case FieldSpec::Kind::Position: {
                core::DockPosition value{};
                parsed = core::ParsePosition(edit.text, value);
                if (!parsed) break;
                moved = value != config.dock.position;
                config.dock.position = value;
                break;
            }
            case FieldSpec::Kind::Material: {
                core::MaterialLevel value{};
                parsed = core::ParseMaterial(edit.text, value);
                if (!parsed) break;
                moved = value != config.appearance.material;
                config.appearance.material = value;
                break;
            }
            case FieldSpec::Kind::Falloff: {
                core::FalloffCurve value{};
                parsed = core::ParseFalloff(edit.text, value);
                if (!parsed) break;
                moved = value != config.dock.hover_falloff;
                config.dock.hover_falloff = value;
                break;
            }
            case FieldSpec::Kind::Theme: {
                // Any non-empty token is a legal theme id; an unknown one is
                // rejected at lens time (LoadThemeInto falls back) rather than
                // here, so the combo can list packs the loader will validate.
                const std::string value = edit.text;
                parsed = !value.empty() && value.find_first_of(" 	") == std::string::npos;
                if (!parsed) break;
                moved = value != config.appearance.theme;
                config.appearance.theme = value;
                break;
            }
            case FieldSpec::Kind::City: {
                // T-075: the city is free text that goes into a URL query, so it
                // is trimmed and the same byte cap the loader applies is enforced
                // here - an edit the UI accepted would otherwise be clamped at
                // the next reload and echo a different value back.
                std::string value = edit.text;
                const std::size_t first = value.find_first_not_of(" \t\r\n");
                if (first == std::string::npos) {
                    value.clear();
                } else {
                    const std::size_t last = value.find_last_not_of(" \t\r\n");
                    value = value.substr(first, last - first + 1);
                }
                constexpr std::size_t kMaxCityBytes = 40;  // config.cpp clamp width
                if (value.size() > kMaxCityBytes) value.resize(kMaxCityBytes);
                parsed = true;  // an empty city is legal: it just disables the tile
                moved = value != config.weather.city;
                config.weather.city = value;
                break;
            }
        }

        if (!parsed) {
            if (rejected != nullptr) rejected->push_back(edit.field);
            DF_WARN(kLogModule, "unparsable value '%s' for '%s'", edit.text.c_str(),
                    edit.field.c_str());
            continue;
        }
        if (moved && changed != nullptr) {
            changed->push_back(edit.field);
        }
        ++applied;
    }
    return applied;
}

}  // namespace dockforge::settings
