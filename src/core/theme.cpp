#include "core/theme.h"

#include "core/log.h"
#include "core/paths.h"

#include <windows.h>

#include <algorithm>
#include <set>

#include <nlohmann/json.hpp>

namespace dockforge::core {
namespace {

using json = nlohmann::json;

constexpr char kLogModule[] = "theme";

// The themeable surface of C-01/C-18 as frozen today. Deliberately NOT the
// whole schema: tiles/hidden/extra_paths/folders are user data, page_size is
// layout math the contract marks non-themeable, behavior.* changes what the app
// does rather than how it looks.
constexpr const char* kThemeableKeys[] = {
    "dock.icon_size",      "dock.gap",             "dock.magnification",
    "dock.hover_falloff",  "dock.influence_radius", "dock.falloff_sharpness",
    "dock.position",       "appearance.material",  "appearance.corner_radius",
    "launchpad.columns",   "launchpad.icon_size",
};

// Built-in themes (T-052 DoD). Without colour keys in the frozen schema the
// dark pack can only express itself through the material tier - recorded as the
// v1.1 gap in the theme header and the roadmap.
const char* kBuiltinDarkOverrides = R"({"appearance": {"material": "solid"}})";

const json* BuiltinOverrides(const std::string& name) {
    static const json dark = json::parse(kBuiltinDarkOverrides, nullptr, false);
    if (name == "dark" && !dark.is_discarded()) return &dark;
    return nullptr;
}

bool IsBuiltin(const std::string& name) {
    return name == "default" || BuiltinOverrides(name) != nullptr;
}

std::wstring ThemePath(const std::string& name) {
    return ThemesDir() + L"\\" + core::Utf8ToWide(name) + L"\\theme.json";
}

bool ReadThemeFile(const std::string& name, std::string* text, ThemeInfo* info,
                   json* overrides) {
    const std::wstring path = ThemePath(name);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart > (1 << 20)) {
        CloseHandle(h);
        return false;
    }
    std::string data(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD got = 0;
    const BOOL ok = ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &got, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    data.resize(got);

    const json parsed = json::parse(data, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) return false;
    if (info != nullptr) {
        info->name = parsed.value("name", name);
        info->author = parsed.value("author", "");
        info->version = parsed.value("version", "");
        info->extends = parsed.value("extends", "");
    }
    if (overrides != nullptr) {
        const auto it = parsed.find("overrides");
        *overrides = (it != parsed.end() && it->is_object()) ? *it : json::object();
    }
    return true;
}

}  // namespace

bool IsThemeableKey(const std::string& key) {
    for (const char* candidate : kThemeableKeys) {
        if (key == candidate) return true;
    }
    return false;
}

int ApplyThemeOverrides(const std::string& overrides_json, core::Config& config,
                        std::vector<std::string>* ignored) {
    const json node = json::parse(overrides_json, nullptr, false);
    if (node.is_discarded() || !node.is_object()) return 0;

    int applied = 0;
    // The overrides object mirrors the C-01 grouping: {"appearance": {...}}.
    for (auto group = node.begin(); group != node.end(); ++group) {
        if (!group.value().is_object()) {
            if (ignored) ignored->push_back(group.key());
            continue;
        }
        for (auto field = group.value().begin(); field != group.value().end(); ++field) {
            const std::string key = group.key() + "." + field.key();
            if (!IsThemeableKey(key)) {
                DF_WARN(kLogModule, "theme override '%s' is not themeable; ignored",
                        key.c_str());
                if (ignored) ignored->push_back(key);
                continue;
            }
            bool applied_this = false;
            try {
                if (key == "dock.icon_size" && field.value().is_number()) {
                    config.dock.icon_size = field.value().get<int>();
                    applied_this = true;
                } else if (key == "dock.gap" && field.value().is_number()) {
                    config.dock.gap = field.value().get<int>();
                    applied_this = true;
                } else if (key == "dock.magnification" && field.value().is_number()) {
                    config.dock.magnification = field.value().get<float>();
                    applied_this = true;
                } else if (key == "dock.influence_radius" && field.value().is_number()) {
                    config.dock.influence_radius = field.value().get<float>();
                    applied_this = true;
                } else if (key == "dock.falloff_sharpness" && field.value().is_number()) {
                    config.dock.falloff_sharpness = field.value().get<float>();
                    applied_this = true;
                } else if (key == "dock.hover_falloff" && field.value().is_string()) {
                    core::FalloffCurve parsed{};
                    if (core::ParseFalloff(field.value().get<std::string>(), parsed)) {
                        config.dock.hover_falloff = parsed;
                        applied_this = true;
                    }
                } else if (key == "dock.position" && field.value().is_string()) {
                    core::DockPosition parsed{};
                    if (core::ParsePosition(field.value().get<std::string>(), parsed)) {
                        config.dock.position = parsed;
                        applied_this = true;
                    }
                } else if (key == "appearance.material" && field.value().is_string()) {
                    core::MaterialLevel parsed{};
                    if (core::ParseMaterial(field.value().get<std::string>(), parsed)) {
                        config.appearance.material = parsed;
                        applied_this = true;
                    }
                } else if (key == "appearance.corner_radius" && field.value().is_number()) {
                    config.appearance.corner_radius = field.value().get<int>();
                    applied_this = true;
                } else if (key == "launchpad.columns" && field.value().is_number()) {
                    config.launchpad.columns = field.value().get<int>();
                    applied_this = true;
                } else if (key == "launchpad.icon_size" && field.value().is_number()) {
                    config.launchpad.icon_size = field.value().get<int>();
                    applied_this = true;
                }
            } catch (const json::exception&) {
                applied_this = false;  // wrong JSON type for the key: ignored
            }
            if (applied_this) {
                ++applied;
            } else if (ignored) {
                ignored->push_back(key);
            }
        }
    }
    ClampConfig(config);
    return applied;
}

bool LoadThemeInto(const std::string& name, core::Config& config, std::string* error) {
    // Resolve the chain base-first onto a copy, so a failure anywhere leaves the
    // caller's config untouched - the dock keeps its current look instead of
    // applying half a theme. "default" is the terminal base and applies nothing.
    core::Config themed = config;
    std::vector<std::string> chain;
    std::string cursor = name.empty() ? "default" : name;

    for (int depth = 0; depth < 5; ++depth) {
        if (cursor == "default") {
            config = std::move(themed);
            if (error) *error = "resolved";
            return true;
        }
        // Cycle guard: a chain that revisits a name stops instead of hanging.
        if (std::find(chain.begin(), chain.end(), cursor) != chain.end()) {
            if (error) *error = "circular extends at '" + cursor + "'";
            DF_WARN(kLogModule, "theme '%s' not applied: %s", name.c_str(),
                    error->c_str());
            return false;
        }
        chain.push_back(cursor);

        std::string overrides_text;
        std::string base;  // empty = terminal
        if (const json* builtin = BuiltinOverrides(cursor); builtin != nullptr) {
            overrides_text = builtin->dump();
        } else {
            ThemeInfo info;
            json overrides;
            if (!ReadThemeFile(cursor, nullptr, &info, &overrides)) {
                if (error) *error = "theme '" + cursor + "' could not be read";
                DF_WARN(kLogModule, "theme '%s' not applied: %s", name.c_str(),
                        error->c_str());
                return false;
            }
            overrides_text = overrides.dump();
            base = info.extends;
            if (base == cursor) base.clear();
        }
        ApplyThemeOverrides(overrides_text, themed);
        if (base.empty()) {
            // Terminal pack: the chain is fully applied.
            config = std::move(themed);
            if (error) *error = "resolved";
            return true;
        }
        cursor = base;
    }

    if (error) *error = "extends chain deeper than 5";
    DF_WARN(kLogModule, "theme '%s' not applied: %s", name.c_str(), error->c_str());
    return false;
}

std::vector<std::string> ListThemes() {
    // Built-ins always exist, even with no themes directory at all.
    std::vector<std::string> names{"default", "dark"};

    const std::wstring dir = ThemesDir() + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW(dir.c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
            const std::wstring wide = fd.cFileName;
            if (wide == L"." || wide == L"..") continue;
            const std::string name = core::WideToUtf8(wide);
            if (IsBuiltin(name)) continue;
            ThemeInfo info;
            json overrides;
            if (ReadThemeFile(name, nullptr, &info, &overrides)) {
                names.push_back(name);
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    std::sort(names.begin() + 2, names.end());
    return names;
}

}  // namespace dockforge::core
