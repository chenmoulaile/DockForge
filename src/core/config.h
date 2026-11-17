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
    // T-302c: whether the dock is on screen. Persisted rather than transient because
    // the user asked for a dock that can be hidden - a hide that a restart silently
    // undoes is not a setting, it is a bug. Default true: the dock is the product.
    //
    // Distinct from DockConfig::hidden, which lists tile identities the user removed
    // from the dock. That one is about which apps are in the dock; this one is about
    // whether the dock is on screen at all.
    bool dock_visible = true;
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

// Notification centre (T-076, ADR-0025). OFF by default, and that default is
// the whole privacy design: reading the system notification store means reading
// other applications' message text, so the app does not even start the sidecar
// until the user says yes. Nothing here is persisted notification CONTENT -
// these are only the switches.
// T-301 / C-31: the Finder Bar. Only the keys this task actually implements are
// declared here; the centre widgets (T-302) and the tray mirror (T-303..T-307)
// add their own keys when they land, so no file ever carries a switch nothing
// reads.
// C-31: one entry of the finderbar.right table. `type` keeps the C-31 enum spelling
// verbatim ("clock", "weather_brief", ...). An unknown or not-yet-registered type is
// kept in the list unchanged - the renderer skips it (bar_layout.h) - so a config
// written before the next task (T-303..T-306) registers its widget survives the
// round-trip instead of silently dropping user data (C-01).
struct FinderBarRightItem {
    std::string type;
    int order = 0;  // position FROM the right edge; 0 = rightmost (bar_layout.h)
};

bool operator==(const FinderBarRightItem& a, const FinderBarRightItem& b);
bool SameRightItems(const std::vector<FinderBarRightItem>& a,
                    const std::vector<FinderBarRightItem>& b);

// T-302: one entry of the finderbar.widgets table (C-31's centre zone). The key
// shape [{type, order}] is C-31-frozen and shared with the right table; `type`
// keeps the C-31 enum spelling verbatim ("cpu", "temperature", ...). An unknown
// or not-yet-registered type is kept in the list unchanged - the renderer skips
// it (bar_widgets.h) - so a config written before T-302d registers weather /
// media / calendar_brief survives the round-trip instead of dropping user data
// (C-01, the same rule FinderBarRightItem obeys).
//
// Order semantics for the centre row (documented in bar_widgets.h): order counts
// FROM THE LEFT, ascending = left to right (the natural reading for a centre
// row); ties resolve by C-31's declaration sequence, deterministically.
struct FinderBarWidgetItem {
    std::string type;
    int order = 0;
};

bool operator==(const FinderBarWidgetItem& a, const FinderBarWidgetItem& b);
bool SameWidgetItems(const std::vector<FinderBarWidgetItem>& a,
                     const std::vector<FinderBarWidgetItem>& b);

// The frozen default for finderbar.widgets (T-302; ADR-0054 records the new key).
// The five monitoring widgets in their natural left-to-right reading order, i.e.
// the order C-31 itself declares them in. weather / media / calendar_brief stay
// OUT of the default table until T-302d can render them.
std::vector<FinderBarWidgetItem> DefaultFinderBarWidgets();

// The frozen default for finderbar.right. C-31 (P5) freezes the key shape but not the
// default value, so T-307 pins it here (ADR-0053): the clock alone, at position 0 =
// hard against the right edge, which is where macOS puts it. Later tasks extend this
// table in place - control_center=1, notification_center=2, tray_mirror=3, network=4,
// battery=5, ime=6 (see bar_layout.h for the full convention). weather_brief stays
// out of the default table; the user opts in with [{type:"weather_brief",order:1}].
// An empty or missing finderbar.right falls back to this table.
std::vector<FinderBarRightItem> DefaultFinderBarRightItems();

// finderbar.clock.* (T-307, new keys - C-31 has no clock section, ADR-0053 records the
// addition). Format-string semantics are the Windows format-picture grammar used by
// GetTimeFormatEx/GetDateFormatEx, because that is exactly what LOCALE_SSHORTTIME and
// LOCALE_SDATE return: the user can copy what their region shows and edit it, and the
// follow-the-system default needs no parser at all.
//
// T-207 对接裁决（TASKS.md T-207「数字式格式串」，T-207 未实装，本注释即对接点）：
//   * T-207 实装前：仅 finderbar.clock.* 生效（本结构是唯一数据源）。
//   * T-207 实装后：themes/<name>/calendar.json 的同义视觉字段（字体/大小/颜色）覆盖
//     finderbar.clock.*（主题优先，随主题分发）；行为字段（格式串/显隐日期/显隐秒）
//     仍以 finderbar.clock.* 为准，主题只接管观感不接管行为。
//   * 冲突细节（哪些键算"视觉"）以 T-207 实装时的 ADR 为准，本包不预设更多规则。
struct FinderBarClockConfig {
    // Empty = follow the system region (GetLocaleInfoEx LOCALE_SSHORTTIME).
    // Non-empty = a GetTimeFormatEx format picture, e.g. "HH:mm:ss" or "h:mm tt".
    // Clamped to 64 bytes; longer strings are rejected at parse time.
    std::string format;
    // macOS shows the date left of the time on the menu bar by default, so the
    // default is true (ADR-0053). The date text uses LOCALE_SDATE as-is.
    bool show_date = true;
    // Appends ":ss" when the effective time picture lacks a seconds field, and
    // strips it when the picture has one but the user asked seconds off.
    bool show_seconds = false;
};

struct FinderBarConfig {
    // C-31 freezes the default as true, and that is safe here because the bar changes
    // no system behaviour: it does not reserve the work area (Q8 proved that is
    // impossible), does not hook anything, and does not touch the taskbar. It is a
    // topmost overlay exactly like the dock, which is also on by default. Auto-hide
    // (below) means it spends its life as a 4dip hairline, so the "draws over every
    // window" objection only applies to the pinned mode a user asks for.
    //
    // Three independent escapes, in increasing severity: DOCKFORGE_FINDERBAR=0 for one
    // run, finderbar.enabled=false in config.json for good, and the system menu's
    // "Quit DockForge" for the whole app.
    bool enabled = true;
    // C-31 says 28px at 100%. Height is DIP, so the bar is 56 physical px at the
    // 192 DPI this host runs at.
    int height = 28;
    bool show_system_menu = true;
    bool show_app_name = true;
    bool show_app_menu = true;
    // C-32: at most this many top-level items, the rest fold into one "More"
    // entry that opens a popup listing them.
    int max_menu_items = 6;
    // Q8 in spikes/T-301_finderbar_menu proved SPI_SETWORKAREA is a no-op on
    // Windows 11 (explorer rewrites the work area immediately), and that finding used
    // to make auto-hide the only option. T-302b superseded it: the shell AppBar
    // protocol (SHAppBarMessage ABM_SETPOS) does reserve the strip - the work area
    // really moves and a maximized window really moves down by the reserved height
    // (spikes/T-302_tray/appbar_spike.cpp, Q10..Q13). So the bar can now be pinned
    // without covering anything, which is the macOS behaviour and what the user asked
    // for.
    //
    // Auto-hide stays available and is still the safe fallback: if the reservation
    // fails at runtime (another appbar owns the edge, the shell refuses), the bar sets
    // this true on itself rather than sitting on top of every title bar.
    bool auto_hide = false;
    // C-31: the right-region widget table. The default IS the frozen default table
    // (clock at 0) - not an empty vector, because an empty right region would blank
    // the clock and break the quiet-file invariant that SerializeConfig(Config{})
    // writes no finderbar key at all. See bar_layout.h for the order semantics; the
    // renderer skips types whose widget task has not landed yet.
    std::vector<FinderBarRightItem> right = DefaultFinderBarRightItems();
    // T-302 (ADR-0054): the centre-zone monitoring table, C-31's finderbar.widgets.
    // Same quiet-file rule as `right`: the member starts at the frozen default
    // table, not at empty. One deliberate difference from `right`: an explicit
    // empty array here STAYS empty - the centre zone is optional chrome, and a
    // user writing "widgets": [] is asking for no widgets (the clock on the right
    // has no equivalent argument, which is why `right` falls back there). See
    // bar_widgets.h for the sampling/colour rules; the renderer skips types whose
    // task has not landed yet.
    std::vector<FinderBarWidgetItem> widgets = DefaultFinderBarWidgets();
    // T-302 (ADR-0054): centre-widget refresh period in milliseconds. Default 2000
    // = C-23's 0.5Hz ceiling; the clamp floor is 2000 so the ceiling can never be
    // violated by hand-editing the file. Range 2000..60000.
    int widgets_refresh_ms = 2000;
    // T-207 对接点：见 FinderBarClockConfig 的注释（本包即写入处，ADR-0053）。
    FinderBarClockConfig clock;
};

bool SameFinderBar(const FinderBarConfig& a, const FinderBarConfig& b);

struct NotificationsConfig {
    bool enabled = false;             // master switch; false means no sidecar process
    bool banner_enabled = true;       // top-right banner for newly arrived toasts
    bool pulse_icons = true;          // C-22 pulse on the matching dock tile
    int banner_timeout_seconds = 5;   // 2..30, how long a banner stays up
    int poll_seconds = 2;             // 1..30, sidecar poll cadence while the dock runs
};

bool SameNotifications(const NotificationsConfig& a, const NotificationsConfig& b);

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
    // T-076: notification centre. Off by default; a default config.json carries
    // no "notifications" key.
    NotificationsConfig notifications;
    // T-301: Finder Bar. Off by default; a default config.json carries no
    // "finderbar" key.
    FinderBarConfig finderbar;

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
IntRange NotificationBannerTimeoutRange();
IntRange NotificationPollRange();
// T-301: the bar must stay readable (too short clips the label) and must not
// eat a third of the screen (a hand-edited 400 would).
IntRange FinderBarHeightRange();
IntRange FinderBarMaxMenuItemsRange();
// T-302: centre-widget refresh period; the floor IS C-23's 0.5Hz ceiling.
IntRange FinderBarWidgetsRefreshRange();

// Hotspot enum <-> string helpers (C-24 canonical spellings).
const char* ToString(HotspotEdge value);
const char* ToString(HotspotCorner value);
bool ParseHotspotEdge(const std::string& text, HotspotEdge& out);
bool ParseHotspotCorner(const std::string& text, HotspotCorner& out);

}  // namespace dockforge::core
