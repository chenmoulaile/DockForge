// DockForge - typed configuration model (CONTRACTS.md C-01).
#pragma once

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
};

struct BehaviorConfig {
    bool autostart = false;
    bool hide_taskbar = false;
};

struct Config {
    DockConfig dock;
    AppearanceConfig appearance;
    BehaviorConfig behavior;

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

}  // namespace dockforge::core
