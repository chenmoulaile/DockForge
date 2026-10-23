// DockForge - configuration load/save (CONTRACTS.md C-01).
#include "core/config.h"

#include "core/log.h"
#include "core/paths.h"
#include "platform/win_common.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace dockforge::core {
namespace {

using json = nlohmann::json;

// Keys owned by the schema. Anything else survives a save round-trip.
const std::unordered_set<std::string> kDockKeys = {
    "icon_size", "gap", "magnification", "hover_falloff",
    "influence_radius", "falloff_sharpness", "position", "tiles", "hidden"};
const std::unordered_set<std::string> kAppearanceKeys = {"material", "corner_radius"};
const std::unordered_set<std::string> kBehaviorKeys = {"autostart", "hide_taskbar"};

// Upper bound on the persisted tile list.
//
// The dock shows at most a dozen tiles today, but the config is user-editable and
// a hand-written file must not be able to make the dock allocate unbounded memory
// or paint for minutes. 256 is far above any plausible curated dock and low enough
// that a corrupt file is harmless.
constexpr std::size_t kMaxTileListEntries = 256;
constexpr std::size_t kMaxIdentityLength = 512;

// Reads a string array of identities, trimming over-long or non-string entries.
// Returns true when the array was present but needed cleaning (caller rewrites).
bool ReadIdentityList(const json& node, const char* key, std::vector<std::string>& out) {
    if (!node.contains(key)) return false;
    const auto& value = node.at(key);
    if (!value.is_array()) {
        DF_WARN("core", "config %s has non-array type, keeping default", key);
        return true;
    }
    bool dirty = false;
    std::vector<std::string> parsed;
    parsed.reserve(std::min(value.size(), kMaxTileListEntries));
    for (const auto& item : value) {
        if (parsed.size() >= kMaxTileListEntries) {
            DF_WARN("core", "config %s has more than %zu entries, extra entries dropped", key,
                    kMaxTileListEntries);
            dirty = true;
            break;
        }
        if (!item.is_string()) {
            DF_WARN("core", "config %s contains a non-string entry, dropped", key);
            dirty = true;
            continue;
        }
        std::string text = item.get<std::string>();
        if (text.empty()) {
            dirty = true;
            continue;
        }
        if (text.size() > kMaxIdentityLength) {
            DF_WARN("core", "config %s entry longer than %zu bytes, dropped", key,
                    kMaxIdentityLength);
            dirty = true;
            continue;
        }
        parsed.push_back(std::move(text));
    }
    out = std::move(parsed);
    return dirty;
}

void WriteIdentityList(json& node, const char* key, const std::vector<std::string>& values) {
    json array = json::array();
    for (const auto& value : values) array.push_back(value);
    node[key] = std::move(array);
}

int ClampInt(int value, IntRange range, const char* name) {
    const int clamped = std::clamp(value, range.min, range.max);
    if (clamped != value) {
        DF_WARN("core", "config %s=%d out of range [%d,%d], clamped to %d", name, value,
                range.min, range.max, clamped);
    }
    return clamped;
}

float ClampFloat(float value, FloatRange range, const char* name) {
    if (std::isnan(value)) {
        DF_WARN("core", "config %s is NaN, using default", name);
        return std::isnan(value) ? range.min : value;
    }
    const float clamped = std::clamp(value, range.min, range.max);
    if (clamped != value) {
        DF_WARN("core", "config %s=%g out of range [%g,%g], clamped to %g", name,
                static_cast<double>(value), static_cast<double>(range.min),
                static_cast<double>(range.max), static_cast<double>(clamped));
    }
    return clamped;
}

// Copies every key from `source` that the schema does not own into `target`.
void PreserveUnknown(const json& source, json& target, const std::unordered_set<std::string>& owned) {
    if (!source.is_object()) return;
    for (auto it = source.begin(); it != source.end(); ++it) {
        if (owned.count(it.key()) != 0) continue;
        target[it.key()] = it.value();
    }
}

json StripUnknownKeys(const json& source) {
    json result = json::object();
    if (!source.is_object()) return result;

    for (const char* section : {"dock", "appearance", "behavior"}) {
        if (!source.contains(section) || !source[section].is_object()) continue;
        json section_out = json::object();
        const auto& owned = std::string(section) == "dock"        ? kDockKeys
                            : std::string(section) == "appearance" ? kAppearanceKeys
                                                                   : kBehaviorKeys;
        PreserveUnknown(source[section], section_out, owned);
        if (!section_out.empty()) result[section] = section_out;
    }
    // Top-level unknown keys.
    static const std::unordered_set<std::string> kTopKeys = {"dock", "appearance", "behavior"};
    PreserveUnknown(source, result, kTopKeys);
    return result;
}

std::string ReadWholeFile(const std::wstring& path) {
    // std::ifstream does not take wchar_t paths on MinGW; open through the
    // Win32 API and hand the bytes to a stringstream.
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};
    auto close_guard = MakeGuard([handle] { CloseHandle(handle); });

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart <= 0) return {};

    std::string data(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!ReadFile(handle, data.data(), static_cast<DWORD>(data.size()), &read, nullptr)) {
        return {};
    }
    data.resize(read);
    return data;
}

bool WriteWholeFile(const std::wstring& path, const std::string& data) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    auto close_guard = MakeGuard([handle] { CloseHandle(handle); });

    DWORD written = 0;
    if (!WriteFile(handle, data.data(), static_cast<DWORD>(data.size()), &written, nullptr)) {
        return false;
    }
    FlushFileBuffers(handle);
    return written == data.size();
}

// Reads a value only when the JSON type matches; otherwise the default stands.
// Returns true when the key was present but had an unusable type.
template <typename T>
bool ReadNumber(const json& node, const char* key, T& out) {
    if (!node.contains(key)) return false;
    const auto& value = node.at(key);
    if (value.is_number()) {
        out = static_cast<T>(value.get<double>());
        return false;
    }
    DF_WARN("core", "config %s has non-numeric type, keeping default", key);
    return true;
}

bool ReadBool(const json& node, const char* key, bool& out) {
    if (!node.contains(key)) return false;
    const auto& value = node.at(key);
    if (value.is_boolean()) {
        out = value.get<bool>();
        return false;
    }
    DF_WARN("core", "config %s has non-boolean type, keeping default", key);
    return true;
}

// Returns true when the key was present but unusable (caller rewrites the file).
template <typename Enum>
bool ReadEnum(const json& node, const char* key, bool (*parse)(const std::string&, Enum&),
              Enum& out) {
    if (!node.contains(key)) return false;
    const auto& value = node.at(key);
    if (!value.is_string()) {
        DF_WARN("core", "config %s has non-string type, keeping default", key);
        return true;
    }
    Enum parsed{};
    if (parse(value.get<std::string>(), parsed)) {
        out = parsed;
        return false;
    }
    DF_WARN("core", "config %s=\"%s\" is not a known value, keeping default", key,
            value.get<std::string>().c_str());
    return true;
}

// Floats are stored as float in memory but must not leak binary noise
// (1.8f -> 1.7999999523162842) into the user-visible file.
double RoundForJson(float value) {
    return std::round(static_cast<double>(value) * 10000.0) / 10000.0;
}

}  // namespace

IntRange IconSizeRange() { return {32, 96}; }
IntRange GapRange() { return {0, 32}; }
IntRange CornerRadiusRange() { return {0, 40}; }
FloatRange MagnificationRange() { return {1.0f, 2.5f}; }
FloatRange InfluenceRadiusRange() { return {1.0f, 5.0f}; }
FloatRange FalloffSharpnessRange() { return {0.5f, 4.0f}; }

const char* ToString(FalloffCurve value) {
    switch (value) {
        case FalloffCurve::Sine: return "sine";
        case FalloffCurve::CosineIntegral: return "cosine_integral";
        case FalloffCurve::Gaussian: return "gaussian";
    }
    return "sine";
}

const char* ToString(DockPosition value) {
    switch (value) {
        case DockPosition::Bottom: return "bottom";
        case DockPosition::Left: return "left";
        case DockPosition::Right: return "right";
    }
    return "bottom";
}

const char* ToString(MaterialLevel value) {
    switch (value) {
        case MaterialLevel::Mica: return "mica";
        case MaterialLevel::Acrylic: return "acrylic";
        case MaterialLevel::Blur: return "blur";
        case MaterialLevel::Solid: return "solid";
    }
    return "mica";
}

bool ParseFalloff(const std::string& text, FalloffCurve& out) {
    if (text == "sine") { out = FalloffCurve::Sine; return true; }
    if (text == "cosine_integral") { out = FalloffCurve::CosineIntegral; return true; }
    if (text == "gaussian") { out = FalloffCurve::Gaussian; return true; }
    return false;
}

bool ParsePosition(const std::string& text, DockPosition& out) {
    if (text == "bottom") { out = DockPosition::Bottom; return true; }
    if (text == "left") { out = DockPosition::Left; return true; }
    if (text == "right") { out = DockPosition::Right; return true; }
    return false;
}

bool ParseMaterial(const std::string& text, MaterialLevel& out) {
    if (text == "mica") { out = MaterialLevel::Mica; return true; }
    if (text == "acrylic") { out = MaterialLevel::Acrylic; return true; }
    if (text == "blur") { out = MaterialLevel::Blur; return true; }
    if (text == "solid") { out = MaterialLevel::Solid; return true; }
    return false;
}

bool ClampConfig(Config& config) {
    bool changed = false;
    auto& dock = config.dock;

    const int icon_size = ClampInt(dock.icon_size, IconSizeRange(), "dock.icon_size");
    if (icon_size != dock.icon_size) { dock.icon_size = icon_size; changed = true; }

    const int gap = ClampInt(dock.gap, GapRange(), "dock.gap");
    if (gap != dock.gap) { dock.gap = gap; changed = true; }

    const float mag = ClampFloat(dock.magnification, MagnificationRange(), "dock.magnification");
    if (mag != dock.magnification) { dock.magnification = mag; changed = true; }

    const float radius =
        ClampFloat(dock.influence_radius, InfluenceRadiusRange(), "dock.influence_radius");
    if (radius != dock.influence_radius) { dock.influence_radius = radius; changed = true; }

    const float sharp =
        ClampFloat(dock.falloff_sharpness, FalloffSharpnessRange(), "dock.falloff_sharpness");
    if (sharp != dock.falloff_sharpness) { dock.falloff_sharpness = sharp; changed = true; }

    const int corner =
        ClampInt(config.appearance.corner_radius, CornerRadiusRange(), "appearance.corner_radius");
    if (corner != config.appearance.corner_radius) {
        config.appearance.corner_radius = corner;
        changed = true;
    }
    return changed;
}

bool ParseConfig(const std::string& json_text, Config& out, std::string* error) {
    json document;
    try {
        document = json::parse(json_text);
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    }
    if (!document.is_object()) {
        if (error) *error = "root is not a JSON object";
        return false;
    }

    Config parsed;  // starts at documented defaults
    parsed.used_defaults = false;
    bool bad_value = false;

    if (document.contains("dock") && document["dock"].is_object()) {
        const auto& node = document["dock"];
        bad_value |= ReadNumber(node, "icon_size", parsed.dock.icon_size);
        bad_value |= ReadNumber(node, "gap", parsed.dock.gap);
        bad_value |= ReadNumber(node, "magnification", parsed.dock.magnification);
        bad_value |= ReadNumber(node, "influence_radius", parsed.dock.influence_radius);
        bad_value |= ReadNumber(node, "falloff_sharpness", parsed.dock.falloff_sharpness);
        bad_value |= ReadEnum(node, "hover_falloff", ParseFalloff, parsed.dock.hover_falloff);
        bad_value |= ReadEnum(node, "position", ParsePosition, parsed.dock.position);
        bad_value |= ReadIdentityList(node, "tiles", parsed.dock.tiles);
        bad_value |= ReadIdentityList(node, "hidden", parsed.dock.hidden);
    }
    if (document.contains("appearance") && document["appearance"].is_object()) {
        const auto& node = document["appearance"];
        bad_value |= ReadNumber(node, "corner_radius", parsed.appearance.corner_radius);
        bad_value |= ReadEnum(node, "material", ParseMaterial, parsed.appearance.material);
    }

    if (document.contains("behavior") && document["behavior"].is_object()) {
        const auto& node = document["behavior"];
        bad_value |= ReadBool(node, "autostart", parsed.behavior.autostart);
        bad_value |= ReadBool(node, "hide_taskbar", parsed.behavior.hide_taskbar);
    }
    parsed.unknown_keys_json = StripUnknownKeys(document).dump();

    // Snapshot the raw parsed values so we can tell whether clamping actually
    // changed anything (that drives the rewrite decision together with enums).
    const DockConfig raw_dock = parsed.dock;
    const AppearanceConfig raw_appearance = parsed.appearance;

    ClampConfig(parsed);

    parsed.needs_rewrite =
        bad_value ||
        raw_dock.icon_size != parsed.dock.icon_size ||
        raw_dock.gap != parsed.dock.gap ||
        raw_dock.magnification != parsed.dock.magnification ||
        raw_dock.influence_radius != parsed.dock.influence_radius ||
        raw_dock.falloff_sharpness != parsed.dock.falloff_sharpness ||
        raw_appearance.corner_radius != parsed.appearance.corner_radius;

    out = std::move(parsed);
    return true;
}

std::string SerializeConfig(const Config& config) {
    json document = json::object();
    document["dock"] = {
        {"icon_size", config.dock.icon_size},
        {"gap", config.dock.gap},
        {"magnification", RoundForJson(config.dock.magnification)},
        {"hover_falloff", ToString(config.dock.hover_falloff)},
        {"influence_radius", RoundForJson(config.dock.influence_radius)},
        {"falloff_sharpness", RoundForJson(config.dock.falloff_sharpness)},
        {"position", ToString(config.dock.position)},
    };
    // Written only when the user has actually curated the dock. An empty array in
    // every default file would suggest a feature nobody used and add two lines of
    // noise to the first thing a new user opens.
    if (!config.dock.tiles.empty()) {
        WriteIdentityList(document["dock"], "tiles", config.dock.tiles);
    }
    if (!config.dock.hidden.empty()) {
        WriteIdentityList(document["dock"], "hidden", config.dock.hidden);
    }
    document["appearance"] = {
        {"material", ToString(config.appearance.material)},
        {"corner_radius", config.appearance.corner_radius},
    };
    document["behavior"] = {
        {"autostart", config.behavior.autostart},
        {"hide_taskbar", config.behavior.hide_taskbar},
    };

    // Re-attach unknown keys captured at load time.
    if (!config.unknown_keys_json.empty()) {
        try {
            const json extras = json::parse(config.unknown_keys_json);
            if (extras.is_object()) {
                for (auto it = extras.begin(); it != extras.end(); ++it) {
                    if (!document.contains(it.key())) {
                        document[it.key()] = it.value();
                    } else if (document[it.key()].is_object() && it.value().is_object()) {
                        for (auto inner = it.value().begin(); inner != it.value().end(); ++inner) {
                            document[it.key()][inner.key()] = inner.value();
                        }
                    }
                }
            }
        } catch (const std::exception&) {
            DF_WARN("core", "unknown-key blob was not valid JSON, dropped");
        }
    }
    return document.dump(2);
}

Config LoadConfig() {
    const std::wstring path = ConfigPath();
    Config config;

    if (!EnsureDirectory(ConfigDir())) {
        DF_WARN("core", "config directory unavailable, running on defaults");
        return config;
    }

    const std::string text = ReadWholeFile(path);
    if (text.empty()) {
        DF_INFO("core", "config missing, writing defaults to %s",
                WideToUtf8(path).c_str());
        config.used_defaults = true;
        SaveConfig(config);
        return config;
    }

    std::string error;
    if (ParseConfig(text, config, &error)) {
        DF_INFO("core", "config loaded from %s", WideToUtf8(path).c_str());
        // Values that were out of range (or enums that fell back) are now
        // corrected in memory; write the corrected file back so the user sees
        // exactly what the app is using, unknown keys included.
        if (config.needs_rewrite) {
            DF_INFO("core", "config normalized, rewriting %s", WideToUtf8(path).c_str());
            SaveConfig(config);
        }
        return config;
    }

    DF_ERROR("core", "config parse failed (%s), falling back to defaults", error.c_str());
    const std::wstring bad_path = path + L".bad";
    WriteWholeFile(bad_path, text);
    DF_WARN("core", "corrupt config preserved at %s", WideToUtf8(bad_path).c_str());

    Config fallback;
    fallback.used_defaults = true;
    SaveConfig(fallback);
    return fallback;
}

bool SaveConfig(const Config& config) {
    if (!EnsureDirectory(ConfigDir())) return false;

    const std::wstring path = ConfigPath();
    const std::wstring temp = path + L".tmp";
    const std::string text = SerializeConfig(config);

    if (!WriteWholeFile(temp, text)) {
        DF_ERROR("core", "failed to write %s", WideToUtf8(temp).c_str());
        return false;
    }
    // MoveFileEx with REPLACE_EXISTING is the atomic swap on Windows.
    if (!MoveFileExW(temp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DF_ERROR("core", "failed to replace %s (err=%lu)", WideToUtf8(path).c_str(),
                 GetLastError());
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

}  // namespace dockforge::core

