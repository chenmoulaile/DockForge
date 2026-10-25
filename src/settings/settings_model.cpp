#include "settings/settings_model.h"

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
    // bools parse 0/1/false/true.
    enum class Kind { Int, Float, Bool, Position, Material, Falloff, Theme } kind;
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
    // T-065 (C-25): one switch per shipped adapter. The adapter id sits verbatim
    // in the field name, matching the C-01 template
    // integrations.message_counts.<adapter_id>.enabled.
    {"integrations.message_counts.weixin.exe.enabled", FieldSpec::Kind::Bool},
    {"integrations.message_counts.qq.exe.enabled", FieldSpec::Kind::Bool},
};

const FieldSpec* FindField(const std::string& field) {
    for (const FieldSpec& spec : kFields) {
        if (field == spec.name) return &spec;
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
            break;
        case FieldSpec::Kind::Float:
            if (field == "dock.magnification") std::snprintf(buffer, sizeof(buffer), "%.2f", config.dock.magnification);
            if (field == "dock.influence_radius") std::snprintf(buffer, sizeof(buffer), "%.2f", config.dock.influence_radius);
            if (field == "dock.falloff_sharpness") std::snprintf(buffer, sizeof(buffer), "%.2f", config.dock.falloff_sharpness);
            break;
        case FieldSpec::Kind::Bool:
            if (field == "behavior.autostart") return config.behavior.autostart ? "1" : "0";
            if (field == "behavior.hide_taskbar") return config.behavior.hide_taskbar ? "1" : "0";
            // T-065: adapter switches default to ON when the id is absent.
            if (field == "integrations.message_counts.weixin.exe.enabled") {
                const auto it = config.integrations.message_counts.find("weixin.exe");
                return it == config.integrations.message_counts.end() || it->second ? "1" : "0";
            }
            if (field == "integrations.message_counts.qq.exe.enabled") {
                const auto it = config.integrations.message_counts.find("qq.exe");
                return it == config.integrations.message_counts.end() || it->second ? "1" : "0";
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
