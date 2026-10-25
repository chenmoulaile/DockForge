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
const std::unordered_set<std::string> kAppearanceKeys = {"material", "corner_radius", "theme"};
const std::unordered_set<std::string> kBehaviorKeys = {"autostart", "hide_taskbar"};
const std::unordered_set<std::string> kLaunchpadKeys = {"extra_paths", "columns", "icon_size",
                                                       "page_size", "folders"};
// T-065: the whole message_counts object is parsed into the typed map (every
// adapter id, known or not, round-trips through IntegrationsConfig), so the
// section owns everything under it.
const std::unordered_set<std::string> kIntegrationsKeys = {"message_counts"};
// T-075: the weather section owns exactly these keys.
const std::unordered_set<std::string> kWeatherKeys = {"enabled", "provider", "city",
                                                      "refresh_minutes"};

// Upper bound on the persisted tile list.
//
// The dock shows at most a dozen tiles today, but the config is user-editable and
// a hand-written file must not be able to make the dock allocate unbounded memory
// or paint for minutes. 256 is far above any plausible curated dock and low enough
// that a corrupt file is harmless.
constexpr std::size_t kMaxTileListEntries = 256;
constexpr std::size_t kMaxIdentityLength = 512;

// Launchpad limits from C-18. The folders list is bounded separately from the
// entries inside it because both are user-editable and either could otherwise be
// made unbounded by a hand-written file.
constexpr std::size_t kMaxExtraPaths = 16;
constexpr std::size_t kMaxFolders = 64;
constexpr std::size_t kMaxFolderItems = 256;
constexpr std::size_t kMaxFolderNameLength = 64;
constexpr std::size_t kMaxFolderIdLength = 64;

// T-069 (C-24) bounds: the lists are hand-editable, so they need the same
// ceiling treatment as tiles/folders. 64 hotkeys is far above the catalog (6)
// and above any plausible curated set; 16 hotspots likewise (catalog: 3).
constexpr std::size_t kMaxHotkeys = 64;
constexpr std::size_t kMaxHotspots = 16;
constexpr std::size_t kMaxShortcutIdLength = 64;

// C-24 modifier bits. MOD_NOREPEAT is never stored (hotkeys.h adds it at the
// one RegisterHotKey call site).
constexpr int kShortcutModifierMask = 0xF;

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

// Reads a bounded string array. Same contract as ReadIdentityList (returns true
// when the array was present but needed cleaning) but with caller-chosen limits,
// because C-18 bounds `extra_paths` at 16 entries while `tiles` allows 256.
bool ReadStringList(const json& node, const char* key, std::vector<std::string>& out,
                    std::size_t max_entries, std::size_t max_length) {
    if (!node.contains(key)) return false;
    const auto& value = node.at(key);
    if (!value.is_array()) {
        DF_WARN("core", "config %s has non-array type, keeping default", key);
        return true;
    }
    bool dirty = false;
    std::vector<std::string> parsed;
    parsed.reserve(std::min(value.size(), max_entries));
    for (const auto& item : value) {
        if (parsed.size() >= max_entries) {
            DF_WARN("core", "config %s has more than %zu entries, extra entries dropped", key,
                    max_entries);
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
        if (text.size() > max_length) {
            DF_WARN("core", "config %s entry longer than %zu bytes, dropped", key, max_length);
            dirty = true;
            continue;
        }
        parsed.push_back(std::move(text));
    }
    out = std::move(parsed);
    return dirty;
}

// Reads `launchpad.folders`.
//
// Every rule here comes from C-18: a duplicate or empty `id` drops the folder, an
// empty `items` drops the folder (an empty folder has nothing to open), and a bad
// entry inside `items` is skipped without invalidating the folder - a single
// uninstalled app must not make a curated folder disappear.
bool ReadFolders(const json& node, const char* key, std::vector<LaunchpadFolder>& out) {
    if (!node.contains(key)) return false;
    const auto& value = node.at(key);
    if (!value.is_array()) {
        DF_WARN("core", "config %s has non-array type, keeping default", key);
        return true;
    }
    bool dirty = false;
    std::vector<LaunchpadFolder> parsed;
    std::unordered_set<std::string> seen_ids;
    parsed.reserve(std::min(value.size(), kMaxFolders));

    for (const auto& item : value) {
        if (parsed.size() >= kMaxFolders) {
            DF_WARN("core", "config %s has more than %zu folders, extra folders dropped", key,
                    kMaxFolders);
            dirty = true;
            break;
        }
        if (!item.is_object()) {
            DF_WARN("core", "config %s contains a non-object entry, dropped", key);
            dirty = true;
            continue;
        }

        LaunchpadFolder folder;
        const auto id_it = item.find("id");
        if (id_it == item.end() || !id_it->is_string()) {
            DF_WARN("core", "config %s folder has no string id, dropped", key);
            dirty = true;
            continue;
        }
        folder.id = id_it->get<std::string>();
        if (folder.id.empty() || folder.id.size() > kMaxFolderIdLength) {
            DF_WARN("core", "config %s folder id is empty or longer than %zu bytes, dropped", key,
                    kMaxFolderIdLength);
            dirty = true;
            continue;
        }
        if (!seen_ids.insert(folder.id).second) {
            DF_WARN("core", "config %s folder id \"%s\" repeated, keeping the first", key,
                    folder.id.c_str());
            dirty = true;
            continue;
        }

        const auto name_it = item.find("name");
        if (name_it != item.end() && name_it->is_string()) {
            folder.name = name_it->get<std::string>();
        } else if (name_it != item.end()) {
            DF_WARN("core", "config %s folder \"%s\" has a non-string name, using the id", key,
                    folder.id.c_str());
            dirty = true;
        }
        if (folder.name.size() > kMaxFolderNameLength) {
            DF_WARN("core", "config %s folder \"%s\" name longer than %zu bytes, truncated", key,
                    folder.id.c_str(), kMaxFolderNameLength);
            folder.name.resize(kMaxFolderNameLength);
            dirty = true;
        }

        const auto items_it = item.find("items");
        if (items_it != item.end() && items_it->is_array()) {
            for (const auto& entry : *items_it) {
                if (folder.items.size() >= kMaxFolderItems) {
                    DF_WARN("core", "config %s folder \"%s\" has more than %zu items, extras dropped",
                            key, folder.id.c_str(), kMaxFolderItems);
                    dirty = true;
                    break;
                }
                if (!entry.is_string()) {
                    dirty = true;
                    continue;
                }
                std::string identity = entry.get<std::string>();
                if (identity.empty() || identity.size() > kMaxIdentityLength) {
                    dirty = true;
                    continue;
                }
                folder.items.push_back(std::move(identity));
            }
        } else if (items_it != item.end()) {
            DF_WARN("core", "config %s folder \"%s\" has a non-array items field, dropped", key,
                    folder.id.c_str());
            dirty = true;
            continue;
        }

        if (folder.items.empty()) {
            DF_WARN("core", "config %s folder \"%s\" has no items, dropped", key, folder.id.c_str());
            dirty = true;
            continue;
        }
        parsed.push_back(std::move(folder));
    }

    out = std::move(parsed);
    return dirty;
}

void WriteFolders(json& node, const char* key, const std::vector<LaunchpadFolder>& folders) {
    json array = json::array();
    for (const auto& folder : folders) {
        json entry = json::object();
        entry["id"] = folder.id;
        entry["name"] = folder.name;
        json items = json::array();
        for (const auto& item : folder.items) items.push_back(item);
        entry["items"] = std::move(items);
        array.push_back(std::move(entry));
    }
    node[key] = std::move(array);
}

// C-24 id rule: [a-z0-9_.-]{1,64}. Shared by the hotkey and hotspot readers.
bool ValidShortcutId(const std::string& id) {
    if (id.empty() || id.size() > kMaxShortcutIdLength) return false;
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// Reads the top-level `hotkeys` array (T-069, C-24). Lenient by design - this
// is the config loader, not the import path (ImportShortcutsJson is the strict
// one): bad entries drop with a WARN, values clamp, unknown actions round-trip
// so a downgrade cannot eat the user's file. Returns true when anything needed
// cleaning (drives the rewrite).
bool ReadHotkeys(const json& document, std::vector<HotkeyConfig>& out) {
    if (!document.contains("hotkeys")) return false;
    const auto& value = document.at("hotkeys");
    if (!value.is_array()) {
        DF_WARN("core", "config hotkeys has non-array type, keeping default");
        return true;
    }
    bool dirty = false;
    std::vector<HotkeyConfig> parsed;
    std::unordered_set<std::string> seen_ids;
    parsed.reserve(std::min(value.size(), kMaxHotkeys));

    for (const auto& item : value) {
        if (parsed.size() >= kMaxHotkeys) {
            DF_WARN("core", "config hotkeys has more than %zu entries, extras dropped",
                    kMaxHotkeys);
            dirty = true;
            break;
        }
        if (!item.is_object()) {
            DF_WARN("core", "config hotkeys contains a non-object entry, dropped");
            dirty = true;
            continue;
        }
        HotkeyConfig entry;
        const auto id_it = item.find("id");
        if (id_it == item.end() || !id_it->is_string() ||
            !ValidShortcutId(id_it->get<std::string>())) {
            DF_WARN("core", "config hotkeys entry has no valid id, dropped");
            dirty = true;
            continue;
        }
        entry.id = id_it->get<std::string>();
        if (!seen_ids.insert(entry.id).second) {
            DF_WARN("core", "config hotkeys id \"%s\" repeated, keeping the first",
                    entry.id.c_str());
            dirty = true;
            continue;
        }
        if (item.contains("modifiers") && item.at("modifiers").is_number()) {
            const int raw = static_cast<int>(item.at("modifiers").get<double>());
            entry.modifiers = raw & kShortcutModifierMask;
            if (entry.modifiers != raw) {
                DF_WARN("core", "config hotkeys \"%s\" modifiers=%d carries non-C-24 bits, "
                                "masked to %d",
                        entry.id.c_str(), raw, entry.modifiers);
                dirty = true;
            }
        }
        if (item.contains("vk") && item.at("vk").is_number()) {
            const int raw = static_cast<int>(item.at("vk").get<double>());
            entry.vk = std::clamp(raw, 1, 254);
            if (entry.vk != raw) {
                DF_WARN("core", "config hotkeys \"%s\" vk=%d out of [1,254], clamped to %d",
                        entry.id.c_str(), raw, entry.vk);
                dirty = true;
            }
        }
        if (item.contains("action") && item.at("action").is_string()) {
            entry.action = item.at("action").get<std::string>();
        }
        if (item.contains("enabled") && item.at("enabled").is_boolean()) {
            entry.enabled = item.at("enabled").get<bool>();
        }
        parsed.push_back(std::move(entry));
    }
    out = std::move(parsed);
    return dirty;
}

// Reads the top-level `hotspots` array. Same leniency contract as ReadHotkeys.
// `corner` may be the JSON null literal (C-24: null = whole edge); a corner
// inconsistent with `edge` is legal - corner is authoritative (hotkeys.h).
bool ReadHotspots(const json& document, std::vector<HotspotConfig>& out) {
    if (!document.contains("hotspots")) return false;
    const auto& value = document.at("hotspots");
    if (!value.is_array()) {
        DF_WARN("core", "config hotspots has non-array type, keeping default");
        return true;
    }
    bool dirty = false;
    std::vector<HotspotConfig> parsed;
    std::unordered_set<std::string> seen_ids;
    parsed.reserve(std::min(value.size(), kMaxHotspots));

    for (const auto& item : value) {
        if (parsed.size() >= kMaxHotspots) {
            DF_WARN("core", "config hotspots has more than %zu entries, extras dropped",
                    kMaxHotspots);
            dirty = true;
            break;
        }
        if (!item.is_object()) {
            DF_WARN("core", "config hotspots contains a non-object entry, dropped");
            dirty = true;
            continue;
        }
        HotspotConfig entry;
        const auto id_it = item.find("id");
        if (id_it == item.end() || !id_it->is_string() ||
            !ValidShortcutId(id_it->get<std::string>())) {
            DF_WARN("core", "config hotspots entry has no valid id, dropped");
            dirty = true;
            continue;
        }
        entry.id = id_it->get<std::string>();
        if (!seen_ids.insert(entry.id).second) {
            DF_WARN("core", "config hotspots id \"%s\" repeated, keeping the first",
                    entry.id.c_str());
            dirty = true;
            continue;
        }
        const auto edge_it = item.find("edge");
        if (edge_it != item.end() && edge_it->is_string()) {
            if (!ParseHotspotEdge(edge_it->get<std::string>(), entry.edge)) {
                DF_WARN("core", "config hotspots \"%s\" edge unknown, keeping default",
                        entry.id.c_str());
                dirty = true;
            }
        }
        const auto corner_it = item.find("corner");
        if (corner_it != item.end() && !corner_it->is_null()) {
            if (corner_it->is_string()) {
                if (!ParseHotspotCorner(corner_it->get<std::string>(), entry.corner)) {
                    DF_WARN("core", "config hotspots \"%s\" corner unknown, treating as null",
                            entry.id.c_str());
                    entry.corner = HotspotCorner::None;
                    dirty = true;
                }
            } else {
                DF_WARN("core", "config hotspots \"%s\" corner has non-string type, treating "
                                "as null",
                        entry.id.c_str());
                dirty = true;
            }
        }
        if (item.contains("size_px") && item.at("size_px").is_number()) {
            const int raw = static_cast<int>(item.at("size_px").get<double>());
            entry.size_px = std::clamp(raw, HotspotSizeRange().min, HotspotSizeRange().max);
            if (entry.size_px != raw) {
                DF_WARN("core", "config hotspots \"%s\" size_px=%d out of [%d,%d], clamped",
                        entry.id.c_str(), raw, HotspotSizeRange().min, HotspotSizeRange().max);
                dirty = true;
            }
        }
        if (item.contains("action") && item.at("action").is_string()) {
            entry.action = item.at("action").get<std::string>();
        }
        if (item.contains("enabled") && item.at("enabled").is_boolean()) {
            entry.enabled = item.at("enabled").get<bool>();
        }
        parsed.push_back(std::move(entry));
    }
    out = std::move(parsed);
    return dirty;
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

        for (const char* section :
             {"dock", "appearance", "behavior", "launchpad", "integrations", "weather"}) {
        if (!source.contains(section) || !source[section].is_object()) continue;
        json section_out = json::object();
        const std::string name = section;
        const auto& owned = name == "dock"           ? kDockKeys
                            : name == "appearance"   ? kAppearanceKeys
                            : name == "behavior"     ? kBehaviorKeys
                            : name == "integrations" ? kIntegrationsKeys
                            : name == "weather"      ? kWeatherKeys
                                                     : kLaunchpadKeys;
        PreserveUnknown(source[section], section_out, owned);
        if (!section_out.empty()) result[section] = section_out;
    }
    // Top-level unknown keys. "hotkeys"/"hotspots" (T-069) and "weather"
    // (T-075) are owned by the schema now, so they must NOT be
    // double-preserved here.
    static const std::unordered_set<std::string> kTopKeys = {
        "dock", "appearance", "behavior", "launchpad", "integrations", "hotkeys", "hotspots",
        "weather"};
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
IntRange LaunchpadColumnsRange() { return {0, 12}; }
IntRange LaunchpadIconSizeRange() { return {48, 192}; }
IntRange LaunchpadPageSizeRange() { return {10, 70}; }
IntRange HotspotSizeRange() { return {1, 256}; }
IntRange WeatherRefreshMinutesRange() { return {5, 360}; }

bool SameWeather(const WeatherConfig& a, const WeatherConfig& b) {
    return a.enabled == b.enabled && a.provider == b.provider && a.city == b.city &&
           a.refresh_minutes == b.refresh_minutes;
}

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

const char* ToString(HotspotEdge value) {
    switch (value) {
        case HotspotEdge::Top: return "top";
        case HotspotEdge::Bottom: return "bottom";
        case HotspotEdge::Left: return "left";
        case HotspotEdge::Right: return "right";
    }
    return "top";
}

const char* ToString(HotspotCorner value) {
    switch (value) {
        case HotspotCorner::None: return "null";
        case HotspotCorner::TopLeft: return "tl";
        case HotspotCorner::TopRight: return "tr";
        case HotspotCorner::BottomLeft: return "bl";
        case HotspotCorner::BottomRight: return "br";
    }
    return "null";
}

bool ParseHotspotEdge(const std::string& text, HotspotEdge& out) {
    if (text == "top") { out = HotspotEdge::Top; return true; }
    if (text == "bottom") { out = HotspotEdge::Bottom; return true; }
    if (text == "left") { out = HotspotEdge::Left; return true; }
    if (text == "right") { out = HotspotEdge::Right; return true; }
    return false;
}

bool ParseHotspotCorner(const std::string& text, HotspotCorner& out) {
    if (text == "null") { out = HotspotCorner::None; return true; }
    if (text == "tl") { out = HotspotCorner::TopLeft; return true; }
    if (text == "tr") { out = HotspotCorner::TopRight; return true; }
    if (text == "bl") { out = HotspotCorner::BottomLeft; return true; }
    if (text == "br") { out = HotspotCorner::BottomRight; return true; }
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

    auto& launchpad = config.launchpad;
    const int columns = ClampInt(launchpad.columns, LaunchpadColumnsRange(), "launchpad.columns");
    if (columns != launchpad.columns) { launchpad.columns = columns; changed = true; }

    const int lp_icon =
        ClampInt(launchpad.icon_size, LaunchpadIconSizeRange(), "launchpad.icon_size");
    if (lp_icon != launchpad.icon_size) { launchpad.icon_size = lp_icon; changed = true; }

    const int page = ClampInt(launchpad.page_size, LaunchpadPageSizeRange(), "launchpad.page_size");
    if (page != launchpad.page_size) { launchpad.page_size = page; changed = true; }

    if (launchpad.extra_paths.size() > kMaxExtraPaths) {
        DF_WARN("core", "config launchpad.extra_paths has %zu entries, keeping the first %zu",
                launchpad.extra_paths.size(), kMaxExtraPaths);
        launchpad.extra_paths.resize(kMaxExtraPaths);
        changed = true;
    }
    for (auto& path : launchpad.extra_paths) {
        if (path.size() > kMaxIdentityLength) {
            DF_WARN("core", "config launchpad.extra_paths entry longer than %zu bytes, truncated",
                    kMaxIdentityLength);
            path.resize(kMaxIdentityLength);
            changed = true;
        }
    }

    if (launchpad.folders.size() > kMaxFolders) {
        DF_WARN("core", "config launchpad.folders has %zu entries, keeping the first %zu",
                launchpad.folders.size(), kMaxFolders);
        launchpad.folders.resize(kMaxFolders);
        changed = true;
    }

    // T-069: same ceiling treatment for the shortcut lists (the readers also
    // bound them; this covers in-memory edits like an import merge).
    if (config.hotkeys.size() > kMaxHotkeys) {
        DF_WARN("core", "config hotkeys has %zu entries, keeping the first %zu",
                config.hotkeys.size(), kMaxHotkeys);
        config.hotkeys.resize(kMaxHotkeys);
        changed = true;
    }
    if (config.hotspots.size() > kMaxHotspots) {
        DF_WARN("core", "config hotspots has %zu entries, keeping the first %zu",
                config.hotspots.size(), kMaxHotspots);
        config.hotspots.resize(kMaxHotspots);
        changed = true;
    }

    // T-075: weather bounds. The city bound keeps a hand-edited file from
    // turning the geocoding query into a request-line payload.
    {
        const int minutes = ClampInt(config.weather.refresh_minutes,
                                     WeatherRefreshMinutesRange(), "weather.refresh_minutes");
        if (minutes != config.weather.refresh_minutes) {
            config.weather.refresh_minutes = minutes;
            changed = true;
        }
        if (config.weather.city.size() > kMaxShortcutIdLength + 32) {
            DF_WARN("core", "config weather.city longer than %zu bytes, truncated",
                    kMaxShortcutIdLength + 32);
            config.weather.city.resize(kMaxShortcutIdLength + 32);
            changed = true;
        }
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
        if (node.contains("theme")) {
            const auto& value = node.at("theme");
            if (value.is_string()) {
                parsed.appearance.theme = value.get<std::string>();
                if (parsed.appearance.theme.empty()) parsed.appearance.theme = "default";
            } else {
                bad_value = true;
            }
        }
    }

    if (document.contains("behavior") && document["behavior"].is_object()) {
        const auto& node = document["behavior"];
        bad_value |= ReadBool(node, "autostart", parsed.behavior.autostart);
        bad_value |= ReadBool(node, "hide_taskbar", parsed.behavior.hide_taskbar);
    }
    if (document.contains("launchpad") && document["launchpad"].is_object()) {
        const auto& node = document["launchpad"];
        bad_value |= ReadStringList(node, "extra_paths", parsed.launchpad.extra_paths,
                                    kMaxExtraPaths, kMaxIdentityLength);
        bad_value |= ReadNumber(node, "columns", parsed.launchpad.columns);
        bad_value |= ReadNumber(node, "icon_size", parsed.launchpad.icon_size);
        bad_value |= ReadNumber(node, "page_size", parsed.launchpad.page_size);
        bad_value |= ReadFolders(node, "folders", parsed.launchpad.folders);
    }
    // T-065: integrations.message_counts.<adapter_id>.enabled. Every id is kept
    // verbatim - the config must survive a downgrade without losing data, and a
    // future adapter version must find its switch still where the user left it.
    if (document.contains("integrations") && document["integrations"].is_object()) {
        const auto& section = document["integrations"];
        if (section.contains("message_counts")) {
            const auto& mc = section["message_counts"];
            if (mc.is_object()) {
                for (auto it = mc.begin(); it != mc.end(); ++it) {
                    if (it.value().is_boolean()) {
                        parsed.integrations.message_counts[it.key()] = it.value().get<bool>();
                    } else {
                        DF_WARN("core", "config integrations.message_counts.%s is not a boolean, "
                                        "keeping default",
                                it.key().c_str());
                        bad_value = true;
                    }
                }
            } else if (!mc.is_null()) {
                DF_WARN("core", "config integrations.message_counts is not an object, keeping default");
                bad_value = true;
            }
        }
    }
    // T-069 (C-24): the two shortcut arrays. Lenient parse - see ReadHotkeys.
    const std::size_t hotkeys_raw = parsed.hotkeys.size();
    bad_value |= ReadHotkeys(document, parsed.hotkeys);
    const std::size_t hotspots_raw = parsed.hotspots.size();
    bad_value |= ReadHotspots(document, parsed.hotspots);
    // T-075: the weather section. Same lenient shape as the rest of the
    // schema - wrong types warn and keep defaults, never fail the file.
    if (document.contains("weather")) {
        const auto& node = document["weather"];
        if (node.is_object()) {
            bad_value |= ReadBool(node, "enabled", parsed.weather.enabled);
            bad_value |= ReadNumber(node, "refresh_minutes", parsed.weather.refresh_minutes);
            if (node.contains("provider")) {
                const auto& value = node.at("provider");
                if (value.is_string()) {
                    const std::string provider = value.get<std::string>();
                    if (provider == "open_meteo" || provider == "amap") {
                        parsed.weather.provider = provider;
                    } else {
                        DF_WARN("core", "config weather.provider=\"%s\" is not known, keeping "
                                        "default", provider.c_str());
                        bad_value = true;
                    }
                } else {
                    bad_value = true;
                }
            }
            if (node.contains("city")) {
                const auto& value = node.at("city");
                if (value.is_string()) {
                    parsed.weather.city = value.get<std::string>();
                } else {
                    DF_WARN("core", "config weather.city is not a string, keeping default");
                    bad_value = true;
                }
            }
        } else if (!node.is_null()) {
            DF_WARN("core", "config weather is not an object, keeping default");
            bad_value = true;
        }
    }
    parsed.unknown_keys_json = StripUnknownKeys(document).dump();

    // Snapshot the raw parsed values so we can tell whether clamping actually
    // changed anything (that drives the rewrite decision together with enums).
    const DockConfig raw_dock = parsed.dock;
    const AppearanceConfig raw_appearance = parsed.appearance;
    const LaunchpadConfig raw_launchpad = parsed.launchpad;
    const WeatherConfig raw_weather = parsed.weather;

    ClampConfig(parsed);

    parsed.needs_rewrite =
        bad_value ||
        raw_dock.icon_size != parsed.dock.icon_size ||
        raw_dock.gap != parsed.dock.gap ||
        raw_dock.magnification != parsed.dock.magnification ||
        raw_dock.influence_radius != parsed.dock.influence_radius ||
        raw_dock.falloff_sharpness != parsed.dock.falloff_sharpness ||
        raw_appearance.corner_radius != parsed.appearance.corner_radius ||
        raw_launchpad.columns != parsed.launchpad.columns ||
        raw_launchpad.icon_size != parsed.launchpad.icon_size ||
        raw_launchpad.page_size != parsed.launchpad.page_size ||
        raw_launchpad.extra_paths != parsed.launchpad.extra_paths ||
        raw_launchpad.folders.size() != parsed.launchpad.folders.size() ||
        hotkeys_raw != parsed.hotkeys.size() ||
        hotspots_raw != parsed.hotspots.size() ||
        !SameWeather(raw_weather, parsed.weather);

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
        {"theme", config.appearance.theme},
    };
    document["behavior"] = {
        {"autostart", config.behavior.autostart},
        {"hide_taskbar", config.behavior.hide_taskbar},
    };
    document["launchpad"] = {
        {"columns", config.launchpad.columns},
        {"icon_size", config.launchpad.icon_size},
        {"page_size", config.launchpad.page_size},
    };
    // Same reasoning as dock.tiles: an empty array in every default file would
    // advertise a feature nobody used. Folders are written only when present.
    if (!config.launchpad.extra_paths.empty()) {
        WriteIdentityList(document["launchpad"], "extra_paths", config.launchpad.extra_paths);
    }
    if (!config.launchpad.folders.empty()) {
        WriteFolders(document["launchpad"], "folders", config.launchpad.folders);
    }
    // T-065: per-adapter switches. Written only when the user (or a probe) has
    // actually flipped one - a default file stays quiet about features that are
    // still at their defaults.
    if (!config.integrations.message_counts.empty()) {
        json mc = json::object();
        for (const auto& [id, enabled] : config.integrations.message_counts) {
            mc[id] = enabled;
        }
        document["integrations"] = {{"message_counts", mc}};
    }
    // T-069 (C-24): written only when present, same quiet-default rule. The
    // corner field serialises as JSON null for whole-edge hotspots, matching
    // the C-24 shape. `modifiers` is masked to the C-24 bits so a value that
    // leaked in from an import can never persist stray MOD_NOREPEAT bits.
    if (!config.hotkeys.empty()) {
        json array = json::array();
        for (const HotkeyConfig& entry : config.hotkeys) {
            array.push_back(json{
                {"id", entry.id},
                {"modifiers", entry.modifiers & 0xF},
                {"vk", entry.vk},
                {"action", entry.action},
                {"enabled", entry.enabled},
            });
        }
        document["hotkeys"] = std::move(array);
    }
    if (!config.hotspots.empty()) {
        json array = json::array();
        for (const HotspotConfig& entry : config.hotspots) {
            array.push_back(json{
                {"id", entry.id},
                {"edge", ToString(entry.edge)},
                {"corner", entry.corner == HotspotCorner::None ? json() : json(ToString(entry.corner))},
                {"size_px", entry.size_px},
                {"action", entry.action},
                {"enabled", entry.enabled},
            });
        }
        document["hotspots"] = std::move(array);
    }
    // T-075: written only when the widget is not at its defaults, same
    // quiet-file rule as integrations. A city alone (enabled=false) is still
    // written: the user typed it, losing it on the next save would be a
    // silent config reset.
    if (config.weather.enabled || !config.weather.city.empty() ||
        config.weather.refresh_minutes != 30 || config.weather.provider != "open_meteo") {
        document["weather"] = {
            {"enabled", config.weather.enabled},
            {"provider", config.weather.provider},
            {"city", config.weather.city},
            {"refresh_minutes", config.weather.refresh_minutes},
        };
    }

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
    const std::wstring path = ConfigPath();
    // The config path can be overridden (tests, portable setups) to point
    // anywhere, so the directory that actually needs to exist is the file's own
    // parent, not ConfigDir. Ensuring ConfigDir only was measured to break the
    // test-isolation path (%TEMP%\dockforge-tests-<pid>\config.json): the parent
    // was never created and every save silently failed.
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash == 0) return false;
    if (!EnsureDirectory(path.substr(0, slash))) return false;
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

