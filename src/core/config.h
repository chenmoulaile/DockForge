// DockForge - typed configuration model (CONTRACTS.md C-01).
#pragma once

#include <map>
#include <string>
#include <vector>

namespace dockforge::core {

enum class FalloffCurve { Sine, CosineIntegral, Gaussian };
enum class DockPosition { Bottom, Left, Right };
enum class MaterialLevel { Mica, Acrylic, Blur, Solid };

struct DockConfig {
    int icon_size = 48;             // 32..96 px @100% DPI
    int gap = 8;                    // 0..32  px @100% DPI
    float magnification = 1.8f;     // 1.0..2.5
    FalloffCurve hover_falloff = FalloffCurve::Sine;
    float influence_radius = 3.0f;  // 1.0..5.0 icon widths
    float falloff_sharpness = 2.0f; // 0.5..4.0 exponent
    DockPosition position = DockPosition::Bottom;

    // Curated tile order, as lowercased identity strings (C-01 tiles/hidden).
    //
    // Empty means "unmanaged": the dock shows the first N apps in scan order and
    // the user has never reordered anything. The first reorder or removal writes
    // the visible identities here, which is what makes the order survive a
    // restart. Only the visible tiles are listed - the tail of the AppsFolder scan
    // is re-appended in scan order on every load, so a list of 12 stays a list of
    // 12 no matter how many apps are installed.
    std::vector<std::string> tiles;

    // Identities the user removed by dragging them off the dock.
    //
    // A separate list from `tiles` because the two answer different questions:
    // `tiles` is order, `hidden` is exclusion. Without it, "append whatever the
    // order does not mention" would resurrect every removed tile on the next
    // launch - the app is still in AppsFolder, so the scan finds it again.
    std::vector<std::string> hidden;
};

struct AppearanceConfig {
    MaterialLevel material = MaterialLevel::Mica;
    int corner_radius = 22;  // 0..40 px @100% DPI
    // Active theme pack id (T-052, C-02). "default" applies nothing. This is
    // an additive v1 schema extension: old binaries round-trip it as an unknown
    // key, and the theme lens in main applies the pack's overrides at runtime
    // only - config.json always stores the user's base values.
    std::string theme = "default";
};

struct BehaviorConfig {
    bool autostart = false;
    bool hide_taskbar = false;
};

// One Launchpad folder (CONTRACTS.md C-18).
//
// `items` holds the same lowercased identity strings the Dock uses (C-15a), so a
// folder entry and a tile entry name the same app the same way. `id` is a v4 UUID
// generated on creation; it exists so a rename does not change which folder is
// which while the grid is being rearranged.
struct LaunchpadFolder {
    std::string id;
    std::string name;
    std::vector<std::string> items;
};

struct LaunchpadConfig {
    // Extra directories to aggregate besides AppsFolder. Each is walked with
    // shell::FindShortcuts, so a folder of .lnk files is a first-class source.
    std::vector<std::string> extra_paths;

    // 0 means "derive from the available width" (C-18 分页).
    int columns = 0;
    int icon_size = 96;
    int page_size = 35;

    std::vector<LaunchpadFolder> folders;
};

// Per-adapter switches for the message-count integration (T-065, C-25).
//
// Keyed by adapter id (lowercase AUMID or process name, e.g. "weixin.exe").
// Default is ON for every known adapter: polling window titles is as invasive
// as the running indicator the dock already pays for, and a badge that is off
// until the user finds a hidden checkbox would never get its first real-world
// validation. Unknown ids round-trip untouched so a downgrade cannot corrupt
// the user's file (C-01 实现约束).
struct IntegrationsConfig {
    std::map<std::string, bool> message_counts;
};

// One global hotkey (T-069, CONTRACTS.md C-24). `id` is the stable string the
// import/export merge matches on - never a list index. `modifiers` is the
// C-24 bitmask (MOD_ALT=1, MOD_CONTROL=2, MOD_SHIFT=4, MOD_WIN=8); MOD_NOREPEAT
// (0x4000) is NOT stored here, it is OR'ed in at the single RegisterHotKey call
// site in HotkeyHost. `vk` is a virtual-key code, 1..254. Everything defaults
// to disabled: a fresh install registers nothing (C-24 所有热键默认 enabled=false).
struct HotkeyConfig {
    std::string id;
    int modifiers = 0;
    int vk = 0;
    std::string action;
    bool enabled = false;
};

enum class HotspotEdge { Top, Bottom, Left, Right };
// Corner authority rule: when `corner` is not None, the trigger rectangle is
// the corner square named by it and `edge` is ignored; when it is None, the
// whole `edge` strip fires. C-24 allows both shapes on the same list.
enum class HotspotCorner { None, TopLeft, TopRight, BottomLeft, BottomRight };

// One screen hotspot (T-069, C-24). Hotspots live on the PRIMARY display's
// monitor rectangle in virtual-screen coordinates. `size_px` is the trigger
// band/corner size, 1..256.
struct HotspotConfig {
    std::string id;
    HotspotEdge edge = HotspotEdge::Top;
    HotspotCorner corner = HotspotCorner::None;
    int size_px = 4;
    std::string action;
    bool enabled = false;
};

// Weather widget (T-075). Defaults are quiet on purpose: fetching weather is
// the first outbound network request this app makes, so nothing happens until
// the user opts in with a city. `provider` selects the source; only the
// no-key Open-Meteo fallback is implemented - "amap" keeps the configuration
// entry point for the keyed provider (its key lives in secrets.json, never
// here). `city` is a free-text geocoding query (Chinese or English); it is
// resolved to coordinates once per change and cached by the weather host.
struct WeatherConfig {
    bool enabled = false;
    std::string provider = "open_meteo";  // open_meteo | amap
    std::string city;                     // <=64 bytes, geocoding query
    int refresh_minutes = 30;             // 5..360
};

bool SameWeather(const WeatherConfig& a, const WeatherConfig& b);

struct Config {
    DockConfig dock;
    AppearanceConfig appearance;
    BehaviorConfig behavior;
    LaunchpadConfig launchpad;
    IntegrationsConfig integrations;
    // T-069 (C-24): top-level arrays, both empty by default. A default
    // config.json therefore carries neither key - the same "a quiet default
    // file advertises nothing" rule as dock.tiles.
    std::vector<HotkeyConfig> hotkeys;
    std::vector<HotspotConfig> hotspots;
    // T-075: weather widget. Off by default (outbound network), so a default
    // config.json carries no "weather" key either.
    WeatherConfig weather;

    // Raw JSON as loaded from disk. Unknown keys live here so that a
    // save round-trip never silently drops user data (C-01 实现约束).
    std::string unknown_keys_json;

    // True when the on-disk file was missing or unusable and defaults won.
    bool used_defaults = true;

    // True when clamping or enum fallback changed a value during parse; the
    // loader rewrites the file so disk matches what the app actually uses.
    bool needs_rewrite = false;
};

// Clamps every value into its documented range. Returns true if anything moved.
bool ClampConfig(Config& config);

// Parses JSON text. Never throws: malformed input returns false and leaves
// `out` untouched.
bool ParseConfig(const std::string& json_text, Config& out, std::string* error);

// Serializes the config, preserving unknown keys from `config.unknown_keys_json`.
std::string SerializeConfig(const Config& config);

// Loads %APPDATA%\DockForge\config.json, creating it with defaults on first run.
// A corrupt file is preserved as config.json.bad and defaults are used.
Config LoadConfig();

// Writes the config atomically (temp file + rename).
bool SaveConfig(const Config& config);

// Enum <-> string helpers (used by both config and theme overrides).
const char* ToString(FalloffCurve value);
const char* ToString(DockPosition value);
const char* ToString(MaterialLevel value);
bool ParseFalloff(const std::string& text, FalloffCurve& out);
bool ParsePosition(const std::string& text, DockPosition& out);
bool ParseMaterial(const std::string& text, MaterialLevel& out);

// Range metadata, exposed so tests and the future settings UI share one truth.
struct IntRange { int min; int max; };
struct FloatRange { float min; float max; };
IntRange IconSizeRange();
IntRange GapRange();
IntRange CornerRadiusRange();
FloatRange MagnificationRange();
FloatRange InfluenceRadiusRange();
FloatRange FalloffSharpnessRange();
IntRange LaunchpadColumnsRange();
IntRange LaunchpadIconSizeRange();
IntRange LaunchpadPageSizeRange();
IntRange HotspotSizeRange();
IntRange WeatherRefreshMinutesRange();

// Hotspot enum <-> string helpers (C-24 canonical spellings).
const char* ToString(HotspotEdge value);
const char* ToString(HotspotCorner value);
bool ParseHotspotEdge(const std::string& text, HotspotEdge& out);
bool ParseHotspotCorner(const std::string& text, HotspotCorner& out);

}  // namespace dockforge::core
