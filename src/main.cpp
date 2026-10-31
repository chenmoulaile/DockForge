// DockForge entry point.
//
// T-001 skeleton window, T-002 config wiring, T-003 frame-driven app loop,
// T-004 DirectComposition surface, T-005 render backend + material tiers,
// T-006 shell app resolution + icon cache, T-007 display enumeration + placement,
// T-009 real dock tile row (dock/dock_view.*).
#include "core/app_loop.h"
#include "core/autostart.h"
#include "core/crash_dump.h"
#include "core/config.h"
#include "core/config_reload.h"
#include "core/hotkeys.h"
#include "core/theme.h"
#include "core/event_bus.h"
#include "core/frame_clock.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/probe_file.h"
#include "core/secrets.h"
#include "core/taskbar.h"
#include "dock/dock_view.h"
#include "dock/drop_target.h"
#include "dock/drag_controller.h"
#include "dock/folder_panel.h"
#include "dock/hover_timer.h"
#include "dock/launchpad.h"
#include "dock/running_tracker.h"
#include "dock/tile_order.h"
#include "dock/weather.h"
#include "dock/weather_popup.h"
#include "finderbar/audio_hardware.h"
#include "finderbar/hardware_panel.h"
#include "finderbar/media_sidecar.h"
#include "finderbar/system_stats.h"
#include "notifications/notification_banner.h"
#include "notifications/notification_panel.h"
#include "notifications/notification_sidecar.h"
#include "notifications/notification_store.h"
#include "notifications/notification_targets.h"
#include "settings/settings_window.h"
#include "platform/composition.h"
#include "platform/display.h"
#include "platform/hit_region.h"
#include "platform/preview_window.h"
#include "platform/window.h"
#include "render/backend.h"
#include "render/backdrop_capture.h"
#include "render/material.h"
#include "render/material_events.h"
#include "render/material_monitor.h"
#include "shell/app_launcher.h"
#include "shell/app_resolver.h"
#include "shell/drop_resolver.h"
#include "shell/icon_cache.h"
#include "shell/message_counts.h"
#include "shell/start_menu.h"
#include "shell/taskbar_progress.h"
#include "shell/thumbnail_host.h"
#include "shell/window_actions.h"
#include "shell/window_list.h"
#include "shell/window_registry.h"

#include <d2d1helper.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <cmath>
#include <functional>
#include <optional>
#include <map>
#include <vector>

namespace {

using namespace dockforge;

void EnablePerMonitorDpi() {
    // Windows 10 1703+ understands DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2.
    // The call fails harmlessly on older builds where the manifest already
    // declared system DPI awareness.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

// DOCKFORGE_DPI_OVERRIDE=<96|120|144|192> forces the layout DPI. The DPI matrix
// in COMPATIBILITY.md has to be provable on a single-DPI machine, and Windows'
// per-monitor DPI cannot be faked per-window otherwise. Test hook only.
void ApplyDpiOverrideFromEnvironment() {
    wchar_t buffer[16]{};
    const DWORD n = GetEnvironmentVariableW(L"DOCKFORGE_DPI_OVERRIDE", buffer,
                                            ARRAYSIZE(buffer));
    if (n == 0 || n >= ARRAYSIZE(buffer)) return;
    const int value = _wtoi(buffer);
    if (value >= 72 && value <= 480) {
        platform::Window::SetDpiOverride(static_cast<UINT>(value));
    }
}

// DOCKFORGE_TARGET_DISPLAY=<index> pins the dock to one enumerated display.
// Index follows platform::EnumerateDisplays() order (primary first), so it is
// stable across runs. Without it the dock follows the primary display.
std::optional<int> TargetDisplayFromEnvironment() {
    wchar_t buffer[16]{};
    const DWORD n = GetEnvironmentVariableW(L"DOCKFORGE_TARGET_DISPLAY", buffer,
                                            ARRAYSIZE(buffer));
    if (n == 0 || n >= ARRAYSIZE(buffer)) return std::nullopt;
    const int value = _wtoi(buffer);
    if (value < 0) return std::nullopt;
    return value;
}

// DOCKFORGE_SIMULATE_DPI=<dpi> posts a synthetic WM_DPICHANGED after startup.
// The OS drops cross-process WM_DPICHANGED (WM_CLOSE crosses, 0x02E0 does not),
// so a single-monitor host can only exercise the handler from inside the process.
std::optional<UINT> SimulatedDpiFromEnvironment() {
    wchar_t buffer[16]{};
    const DWORD n = GetEnvironmentVariableW(L"DOCKFORGE_SIMULATE_DPI", buffer,
                                            ARRAYSIZE(buffer));
    if (n == 0 || n >= ARRAYSIZE(buffer)) return std::nullopt;
    const int value = _wtoi(buffer);
    if (value >= 72 && value <= 480) return static_cast<UINT>(value);
    return std::nullopt;
}

// Poll interval override for the running-indicator tracker.
//
// Verification hook, not a user setting: proving the tracker's frame cost requires
// running the same benchmark with the poll disabled, and 5000ms is long enough that
// at most one poll lands in a 15s window. Without this the A/B is a guess.
std::optional<int> RunningPollFromEnvironment() {
    wchar_t buffer[16]{};
    const DWORD n = GetEnvironmentVariableW(L"DOCKFORGE_RUNNING_POLL_MS", buffer,
                                            ARRAYSIZE(buffer));
    if (n == 0 || n >= ARRAYSIZE(buffer)) return std::nullopt;
    const int value = _wtoi(buffer);
    if (value >= 50 && value <= 5000) return value;
    return std::nullopt;
}

// Poll interval override for the message-count adapters (T-065).
//
// Verification hook, same pattern as DOCKFORGE_RUNNING_POLL_MS: the probe drives
// an adapter's first poll inside a short test window instead of waiting out the
// production 1000ms.
std::optional<int> MessageCountPollFromEnvironment() {
    wchar_t buffer[16]{};
    const DWORD n = GetEnvironmentVariableW(L"DOCKFORGE_MESSAGE_POLL_MS", buffer,
                                            ARRAYSIZE(buffer));
    if (n == 0 || n >= ARRAYSIZE(buffer)) return std::nullopt;
    const int value = _wtoi(buffer);
    if (value >= 200 && value <= 10000) return value;
    return std::nullopt;
}

// Poll interval override for the taskbar-progress white-list scan (T-067).
//
// Verification hook, same pattern as DOCKFORGE_MESSAGE_POLL_MS: a probe has to
// catch a copy that only lasts seconds, so it tightens the 500ms default.
// ProgressTracker clamps to 250..5000 - an MSAA sweep costs real milliseconds,
// so the floor cannot go as low as the message adapters' 200.
std::optional<int> ProgressPollFromEnvironment() {
    wchar_t buffer[16]{};
    const DWORD n = GetEnvironmentVariableW(L"DOCKFORGE_PROGRESS_POLL_MS", buffer,
                                            ARRAYSIZE(buffer));
    if (n == 0 || n >= ARRAYSIZE(buffer)) return std::nullopt;
    return _wtoi(buffer);
}

// DOCKFORGE_BADGE_PROBE_TARGET=<lowercase process name> registers one extra
// TitleBadgeAdapter for that process (T-065 verification hook, not a config
// key). The probe launches a controllable window with a "(4) ..." title and
// watches the badge appear on the matching tile and vanish when the process
// exits - a full live pass of the C-25 chain without touching WeChat/QQ.
std::optional<std::wstring> BadgeProbeTargetFromEnvironment() {
    wchar_t buffer[128]{};
    const DWORD n = GetEnvironmentVariableW(L"DOCKFORGE_BADGE_PROBE_TARGET", buffer,
                                            ARRAYSIZE(buffer));
    if (n == 0 || n >= ARRAYSIZE(buffer)) return std::nullopt;
    return std::wstring(buffer);
}

// Copies the settings gate (C-01 integrations.message_counts.<id>.enabled) onto
// the host. Called at startup and from every ConfigChanged, so the settings
// window and a hot reload both take effect through one path.
void ApplyMessageCountConfig(const core::Config& source, shell::MessageCounterHost& host) {
    for (const auto& [id, enabled] : source.integrations.message_counts) {
        host.SetEnabled(core::Utf8ToWide(id), enabled);
    }
}

platform::DockEdge ToDockEdge(core::DockPosition position) {    switch (position) {
        case core::DockPosition::Left: return platform::DockEdge::Left;
        case core::DockPosition::Right: return platform::DockEdge::Right;
        case core::DockPosition::Bottom:
        default: return platform::DockEdge::Bottom;
    }
}

// Reads a boolean environment flag. `default_on` is what the flag reports when
// the variable is unset, which is what makes a feature opt-OUT rather than
// opt-IN without the caller having to spell out both cases at every call site.
bool ReadEnvFlag(const wchar_t* name, bool default_on) {
    wchar_t buffer[8]{};
    const DWORD n = GetEnvironmentVariableW(name, buffer, ARRAYSIZE(buffer));
    if (n == 0 || n >= ARRAYSIZE(buffer)) return default_on;
    return buffer[0] != L'0' && buffer[0] != L'\0';
}

bool ReadEnvFlag(const wchar_t* name) {
    return ReadEnvFlag(name, false);
}


// DOCKFORGE_FORCE_MATERIAL=<mica|acrylic|blur|solid> overrides the configured
// tier so every degradation step can be exercised on a machine whose OS would
// normally pick a higher one. Verification hook, not a user setting.
std::optional<core::MaterialLevel> ForcedMaterialFromEnvironment() {
    wchar_t buffer[32]{};
    const DWORD n = GetEnvironmentVariableW(L"DOCKFORGE_FORCE_MATERIAL", buffer,
                                            ARRAYSIZE(buffer));
    if (n == 0 || n >= ARRAYSIZE(buffer)) return std::nullopt;
    core::MaterialLevel level{};
    if (!core::ParseMaterial(core::WideToUtf8(buffer), level)) {
        DF_WARN("app", "DOCKFORGE_FORCE_MATERIAL=%s is not a material name",
                core::WideToUtf8(buffer).c_str());
        return std::nullopt;
    }
    return level;
}

bool EnableDarkTitleBar(HWND hwnd) {
    constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
    BOOL dark = TRUE;
    return SUCCEEDED(
        DwmSetWindowAttribute(hwnd, kDwmwaUseImmersiveDarkMode, &dark, sizeof(dark)));
}

// Number of icons warmed during startup. The Dock will eventually show ~20 tiles,
// but warming all 216 Start-menu entries would add minutes to a cold start; the
// cap keeps the measured number honest instead of hiding a slow path.
constexpr int kIconWarmupLimit = 24;

struct ShellScanReport {
    // Kept by value because the dock tile list is built from it after the scan
    // counters have been logged. 215 entries of a few hundred bytes each is far
    // cheaper than a second AppsFolder enumeration.
    std::vector<shell::AppEntry> entries;
    std::size_t app_count = 0;
    int shortcut_count = 0;
    int uwp_count = 0;
    int desktop_id_count = 0;
    int namespace_count = 0;
    int skipped = 0;
    int icons_ready = 0;
    int icons_failed = 0;
    double scan_ms = 0.0;
    double icons_ms = 0.0;
};

// Resolves the Start-menu app list and warms the icon cache. Runs before the window
// is shown so the first frame never blocks on the shell: a cold GetImage call costs
// 60-190 ms, which would blow the 800 ms cold-start budget on its own.
ShellScanReport ScanAndWarmIcons(shell::IconCache& icons) {
    ShellScanReport report;

    std::int64_t start = core::QueryPerformanceTicks();
    shell::StartMenuScan scan = shell::ScanAppsFolder();
    report.scan_ms = core::QpcToMilliseconds(core::QueryPerformanceTicks() - start);
    report.app_count = scan.entries.size();
    report.shortcut_count = scan.shortcut_count;
    report.uwp_count = scan.uwp_count;
    report.desktop_id_count = scan.desktop_id_count;
    report.namespace_count = scan.namespace_count;
    report.skipped = scan.skipped;

    start = core::QueryPerformanceTicks();
    for (std::size_t i = 0; i < scan.entries.size() &&
                            static_cast<int>(i) < kIconWarmupLimit;
         ++i) {
        shell::IconBitmap bitmap;
        if (icons.Get(shell::IconSourceFor(scan.entries[i]), 256, bitmap)) {
            ++report.icons_ready;
        } else {
            ++report.icons_failed;
        }
    }
    report.icons_ms = core::QpcToMilliseconds(core::QueryPerformanceTicks() - start);
    report.entries = std::move(scan.entries);
    return report;
}

// Excludes (or re-includes) this window in screen captures. The frost reads the
// desktop behind the plate; without exclusion the readback contains our own last
// frame, so the blur feeds on its own output and the plate darkens frame by frame
// (spike conclusion 2, measured as magenta bleeding into the capture).
bool SetWindowCaptureExclusion(HWND hwnd, bool exclude) {
    const BOOL ok =
        SetWindowDisplayAffinity(hwnd, exclude ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
    if (!ok) {
        DF_WARN("render", "SetWindowDisplayAffinity(%d) failed: %lu", exclude ? 1 : 0,
                GetLastError());
    }
    return ok != FALSE;
}

// System backdrop for the Mica and Acrylic tiers. Only reachable when
// appearance.corner_radius == 0: the spike measured that DWMWA_SYSTEMBACKDROP_TYPE
// fills the whole window rectangle and ignores both the DComp surface's per-pixel
// alpha and SetWindowRgn, so it cannot coexist with a custom radius.
bool ApplySystemBackdrop(HWND hwnd, core::MaterialLevel tier) {
    constexpr DWORD kDwmwaSystemBackdropType = 38;  // DWMWA_SYSTEMBACKDROP_TYPE
    // DWMSBT_MAINWINDOW = Mica, DWMSBT_TRANSIENTWINDOW = Acrylic.
    const int backdrop = tier == core::MaterialLevel::Mica ? 2 : 3;
    const HRESULT hr = DwmSetWindowAttribute(hwnd, kDwmwaSystemBackdropType, &backdrop,
                                             sizeof(backdrop));
    if (FAILED(hr)) {
        DF_WARN("render", "DWMWA_SYSTEMBACKDROP_TYPE failed: %s",
                core::WideToUtf8(HResultText(hr)).c_str());
        return false;
    }
    const MARGINS margins{-1, -1, -1, -1};
    if (FAILED(DwmExtendFrameIntoClientArea(hwnd, &margins))) {
        DF_WARN("render", "DwmExtendFrameIntoClientArea failed");
        return false;
    }
    return true;
}

// How many tiles the dock shows. Not a config key yet: C-01 has no field for it and
// adding one would need a migration for a value the user cannot meaningfully pick
// until drag-reorder exists (T-012). Twelve is a plausible dock and, at the default
// 48px icon with an 8px gap, 684 DIP wide - it fits a 1440x960 DIP work area with
// room to spare, which is what makes the DPI matrix screenshots usable.
constexpr int kDockTileLimit = 12;

// Icons are extracted once at 256px and downscaled at paint time. Extracting at
// the live DPI instead would re-run the shell for every scale change (60-190ms
// each, evidence/T-006) and still be softer than a 256px source.
constexpr int kDockIconPx = 256;

// DOCKFORGE_PIN_APPS=<substring>[,<substring>...] restricts the dock to entries whose
// display name or identity contains one of the substrings, ignoring case.
//
// A verification hook, not a product feature: the dock shows the first 12 apps
// alphabetically, which on this machine contains no packaged app, so the UWP click
// path could not be exercised end to end without it. Keeping it environment-only
// means it cannot leak into a shipped configuration file.
std::vector<std::wstring> PinnedAppsFromEnvironment() {
    wchar_t buffer[512]{};
    const DWORD n = GetEnvironmentVariableW(L"DOCKFORGE_PIN_APPS", buffer, ARRAYSIZE(buffer));
    if (n == 0 || n >= ARRAYSIZE(buffer)) return {};

    std::vector<std::wstring> pins;
    std::wstring current;
    for (DWORD i = 0; i < n; ++i) {
        if (buffer[i] == L',') {
            if (!current.empty()) pins.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(static_cast<wchar_t>(std::towlower(buffer[i])));
    }
    if (!current.empty()) pins.push_back(current);
    return pins;
}

bool MatchesAnyPin(const shell::AppEntry& app, const std::vector<std::wstring>& pins) {
    if (pins.empty()) return true;
    std::wstring haystack = app.display_name + L"\n" + app.aumid + L"\n" + app.namespace_parsing +
                            L"\n" + app.target_path;
    std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    for (const auto& pin : pins) {
        if (haystack.find(pin) != std::wstring::npos) return true;
    }
    return false;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    EnablePerMonitorDpi();

    // OLE, not a bare CoInitializeEx. RegisterDragDrop refuses to register on an
    // apartment initialised with CoInitializeEx and reports E_OUTOFMEMORY, which reads
    // like a memory failure and is actually "OLE was not initialised" (measured, see
    // evidence/T-014/run_coinit_target.txt vs run_oleinit_target.txt). OleSession is
    // reference counted and owns the matching OleUninitialize, so the ordering against
    // the drop target's lifetime is handled by scope rather than by remembering.
    dock::OleSession ole;
    if (!ole.ok()) {
        return 1;
    }

    core::Logger::Instance().Init(L"app");
    DF_INFO("app", "DockForge %s starting", "0.1.0");
    DF_INFO("app", "config path = %s", core::WideToUtf8(core::ConfigPath()).c_str());
    // Crash recovery (T-061) installs before anything can hide the taskbar and
    // before any code that could crash in earnest. On a machine that never
    // hides the taskbar the filter's restore is a harmless no-op show.
    core::InstallTaskbarCrashRecovery();
    // Crash dumps (T-062) install LAST on the SetUnhandledExceptionFilter chain
    // so the dump filter runs FIRST at crash time (write the dump while the
    // process state is freshest), then forwards to the taskbar filter it
    // replaced. Order is load-bearing: the chain cannot be read back, only
    // captured at install time (spikes/t062_dump Q4).
    core::InstallCrashDumper();
    ApplyDpiOverrideFromEnvironment();
    // Remembered separately from Window::DpiOverride() because the WM_DPICHANGED
    // handler writes a synthetic override of its own; without this flag an
    // operator-supplied DOCKFORGE_DPI_OVERRIDE would be silently replaced the
    // first time the dock crossed a monitor boundary.
    const bool dpi_forced_by_env = platform::Window::DpiOverride() != 0;

    core::Config config = core::LoadConfig();
    // Theme lens (T-052): config.json stores base values; the pack's overrides
    // apply at runtime only. A pack that fails to resolve leaves the config as
    // loaded and the dock keeps its current look (never black).
    if (!core::LoadThemeInto(config.appearance.theme, config, nullptr)) {
        DF_WARN("core", "theme '%s' unavailable; running with base values",
                config.appearance.theme.c_str());
    }
    DF_INFO("app", "config: icon_size=%d gap=%d mag=%.2f falloff=%s material=%s corner=%d",
            config.dock.icon_size, config.dock.gap,
            static_cast<double>(config.dock.magnification),
            core::ToString(config.dock.hover_falloff),
            core::ToString(config.appearance.material), config.appearance.corner_radius);

    // Autostart (T-060): the scheduler task is the app's own responsibility -
    // the config flag alone does not survive a user deleting the task through
    // Task Scheduler's GUI. When enabled, re-ensure on every start (a no-op
    // while the task matches, a re-register after a move or manual deletion).
    // The default is off, and off touches nothing - a fresh profile must not
    // create any scheduled task (T-060 DoD).
    bool last_autostart = config.behavior.autostart;
    if (last_autostart) {
        std::string detail;
        if (core::ApplyAutostartSystem(true, &detail)) {
            DF_INFO("core", "autostart ensured: %s", detail.c_str());
        } else {
            DF_WARN("core", "autostart self-heal failed: %s", detail.c_str());
        }
    }

    // ---- shell app list + icon cache (T-006) ----------------------------
    // Resolved before the window exists so the first painted frame already has its
    // icons and the cold-start timer measures real work rather than a queue.
    shell::IconCache icons;
    if (!icons.Initialize(core::IconCacheDir())) {
        DF_WARN("shell", "icon cache disabled; every icon will be extracted per use");
    }
    const ShellScanReport scan = ScanAndWarmIcons(icons);
    DF_INFO("app",
            "apps: %zu (shortcut=%d uwp=%d desktop-id=%d namespace=%d skipped=%d) scan=%.1fms "
            "icons=%d/%d in %.1fms",
            scan.app_count, scan.shortcut_count, scan.uwp_count, scan.desktop_id_count,
            scan.namespace_count, scan.skipped, scan.scan_ms, scan.icons_ready,
            scan.icons_ready + scan.icons_failed, scan.icons_ms);

    // ---- dock tiles (T-009) ---------------------------------------------
    // The tile list is built once, before the window exists, so painting never
    // calls the shell: a cold GetImage is 60-190ms (evidence/T-006) and would
    // stall the frame loop. The warm-up above already populated the cache, so this
    // is a map lookup per tile.
    const std::vector<std::wstring> pins = PinnedAppsFromEnvironment();
    std::vector<shell::AppEntry> dock_apps;
    if (pins.empty()) {
        dock_apps = scan.entries;
    } else {
        for (const auto& entry : scan.entries) {
            if (MatchesAnyPin(entry, pins)) {
                DF_INFO("app", "pin kept: '%s' kind=%d target='%s' aumid='%s' ns='%s'",
                        core::WideToUtf8(entry.display_name).c_str(),
                        static_cast<int>(entry.kind), core::WideToUtf8(entry.target_path).c_str(),
                        core::WideToUtf8(entry.aumid).c_str(),
                        core::WideToUtf8(entry.namespace_parsing).c_str());
                dock_apps.push_back(entry);
            }
        }
        DF_INFO("app", "DOCKFORGE_PIN_APPS kept %zu of %zu apps", dock_apps.size(),
                scan.entries.size());
    }

    // Non-const: RunningTracker writes each tile's run_state in place. Keeping the
    // state on the tile rather than in a parallel array means the two cannot fall
    // out of step if the tile list is ever rebuilt.
    //
    // The curated order from config is applied here rather than inside BuildTiles:
    // ordering is a user decision, icon extraction is a shell detail, and keeping
    // them in one function would make the order untestable without a live shell.
    // DOCKFORGE_PIN_APPS deliberately bypasses the curated order - it exists to hold
    // a fixed subset for measurement, and a saved arrangement would fight it.
    std::vector<dock::DockTile> tiles;
    if (pins.empty()) {
        const std::vector<shell::AppEntry> ordered = dock::ApplyOrder(
            dock_apps, config.dock.tiles, config.dock.hidden, kDockTileLimit);
        tiles = dock::BuildTiles(ordered, icons, kDockTileLimit, kDockIconPx);
        if (!config.dock.tiles.empty()) {
            DF_INFO("dock", "curated order: %zu names -> %zu visible of %zu scanned",
                    config.dock.tiles.size(), tiles.size(), dock_apps.size());
        }
    } else {
        tiles = dock::BuildTiles(dock_apps, icons, kDockTileLimit, kDockIconPx);
    }
    DF_INFO("app", "dock tiles: %zu of %zu apps (limit=%d icon=%dpx)", tiles.size(),
            dock_apps.size(), kDockTileLimit, kDockIconPx);
    // Logged per tile because a tile with no icon still occupies a slot: the plate
    // draws a placeholder ring, and without this line that ring looks like a
    // rendering bug rather than a shell item the shell could not produce an icon
    // for.
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        DF_DEBUG("dock", "tile[%zu] %s kind=%s icon=%s", i,
                 core::WideToUtf8(tiles[i].app.display_name).c_str(),
                 shell::ToString(tiles[i].app.kind),
                 tiles[i].icon.valid() ? "ok" : "missing");
    }

    // ---- weather tile (T-075) -------------------------------------------
    // Appended after the app row, never part of the curated order: the widget
    // is a fixture, not a launchable, and writing it into dock.tiles would
    // make the identity list reference something no shell scan can resolve.
    // The count change flows into the geometry on the next apply_placement.
    const auto weather_tile_index = [&tiles]() -> int {
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            if (tiles[i].weather) return static_cast<int>(i);
        }
        return -1;
    };
    auto SetWeatherTile = [&](bool on) {
        const int current = weather_tile_index();
        if (on && current < 0) {
            dock::DockTile tile;
            tile.weather = true;
            tile.app.display_name = L"Weather";
            tiles.push_back(std::move(tile));
        } else if (!on && current >= 0) {
            tiles.erase(tiles.begin() + current);
        } else {
            return false;
        }
        return true;
    };
    if (config.weather.enabled && !config.weather.city.empty()) {
        SetWeatherTile(true);
        DF_INFO("weather", "tile on: city='%s' provider=%s refresh=%dmin",
                config.weather.city.c_str(), config.weather.provider.c_str(),
                config.weather.refresh_minutes);
    } else if (config.weather.enabled) {
        DF_WARN("weather", "enabled but weather.city is empty; no tile this run");
    }

    // ---- material tier resolution (T-005) -------------------------------
    // The configured tier is a request, not a promise: Mica needs Win11 22000+
    // and a rectangular plate, and the self-drawn frost needs a desktop capture.
    // ResolveMaterialTier walks down to the first tier this machine can render.
    core::MaterialLevel requested = config.appearance.material;
    if (const auto forced = ForcedMaterialFromEnvironment()) {
        DF_INFO("app", "material forced to %s by DOCKFORGE_FORCE_MATERIAL",
                core::ToString(*forced));
        requested = *forced;
    }
    render::MaterialCaps caps = render::ProbeMaterialCaps(config.appearance.corner_radius);
    const core::MaterialLevel tier = render::ResolveMaterialTier(requested, caps);
    const render::PlateStyle plate_style =
        render::StyleForTier(tier, config.appearance.corner_radius);
    DF_INFO("app", "material: requested=%s resolved=%s os_build=%d backdrop=%d capture=%d",
            core::ToString(requested), core::ToString(tier), caps.os_build,
            caps.system_backdrop_supported ? 1 : 0, caps.capture_available ? 1 : 0);
    if (const char* reason = render::DegradeReason(requested, caps); *reason != '\0') {
        DF_WARN("render", "material degraded %s -> %s: %s", core::ToString(requested),
                core::ToString(tier), reason);
    }

    if (!platform::Window::RegisterWindowClass(instance)) {
        DF_FATAL("app", "window class registration failed");
        return 2;
    }

    // The preview popup has its own class, registered here rather than lazily in
    // PreviewWindow::Create: a failure at registration time is a startup problem
    // worth reporting once, not a per-preview failure the user only sees as "the
    // preview never appeared".
    const bool preview_class_ready = platform::PreviewWindow::RegisterWindowClass(instance);

    // The folder drill-through panel (T-066) has its own class for the same
    // reason. A failure is logged once; the click on a folder tile then falls
    // back to the launch path instead of silently doing nothing.
    const bool folder_panel_class_ready = dock::FolderPanel::RegisterWindowClass(instance);

    // Geometry needs the tile count, which needs the scan, which happens above.
    // The scale is unknown until the HWND exists, so start at 1.0 and recompute
    // inside apply_placement once Window::Dpi() is readable.
    dock::DockGeometry geometry = dock::ComputeGeometry(config, static_cast<int>(tiles.size()), 1.0f);
    dock::DockPainter dock_painter;
    dock::DockHover dock_hover;
    // Polls window existence on an interval and the foreground window every frame.
    // Existence costs a full EnumWindows sweep (0.4-0.7ms); foreground is one
    // syscall. Running both on the same clock would either lag the highlight by up
    // to 300ms or sweep 60 times a second for nothing.
    dock::RunningTracker running_tracker;
    if (const auto interval = RunningPollFromEnvironment()) {
        running_tracker.SetIntervalMs(*interval);
        DF_INFO("app", "running indicator poll overridden to %dms (verification hook)",
                *interval);
    }
    // Seed the indicators before the first paint so the dock never shows an idle row
    // for one frame while the first poll is still pending.
    running_tracker.Update(0.0);
    running_tracker.ApplyToTiles(tiles);

    // Message counters (T-065, C-25): one adapter per target application, first
    // registration wins, every failure degrades to count 0. The spike
    // (spikes/T-065_notification_probe/) measured that UserNotificationListener
    // holds no unread state for chat apps, so the first two targets read the
    // apps' own window titles - no hooks, no chat content (ADR-0022).
    shell::MessageCounterHost message_counters;
    // Wall clock for overlay animations (T-067's indeterminate-progress pulse),
    // accumulated by the tick handler and read by the paint lambda - declared
    // at function scope so both lambdas capture the same accumulator.
    double dock_clock_ms = 0.0;
    if (const auto interval = MessageCountPollFromEnvironment()) {
        message_counters.SetPollIntervalMs(*interval);
        DF_INFO("app", "message-count poll overridden to %dms (verification hook)", *interval);
    }
    message_counters.Register(std::make_unique<shell::TitleBadgeAdapter>(
        L"weixin.exe", std::vector<std::wstring>{L"weixin.exe", L"wechat.exe"}));
    message_counters.Register(std::make_unique<shell::TitleBadgeAdapter>(
        L"qq.exe", std::vector<std::wstring>{L"qq.exe"}));
    if (const auto probe_target = BadgeProbeTargetFromEnvironment()) {
        message_counters.Register(std::make_unique<shell::TitleBadgeAdapter>(
            *probe_target, std::vector<std::wstring>{*probe_target}));
        DF_INFO("app", "badge probe adapter registered for '%s'",
                core::WideToUtf8(*probe_target).c_str());
    }
    ApplyMessageCountConfig(config, message_counters);
    message_counters.InitializeAll();
    // Every change lands in the log: a probe that watches the badge has to be
    // able to tell "adapter polled and answered 0" from "adapter never polled".
    message_counters.set_count_changed(
        [](const std::wstring& target_app_id, int count) {
            DF_INFO("dock", "message count '%s' -> %d",
                    core::WideToUtf8(target_app_id).c_str(), count);
        });
    DF_INFO("app", "message-count adapters: %zu registered, poll %dms",
            message_counters.AdapterCount(), message_counters.poll_interval_ms());
    // Copies the last known counts onto the tiles. Cheap (a map lookup per tile);
    // called after every change and whenever tiles change length.
    auto apply_message_counts = [&]() {
        for (dock::DockTile& tile : tiles) {
            tile.message_count = message_counters.CountForEntry(tile.app);
        }
    };
    message_counters.Update();
    apply_message_counts();

    // Taskbar progress (T-067, ADR-0024): the spike measured the preferred
    // ITaskbarList3 proxy-state read path as dead (S_OK calls, zero window
    // properties) and vtable hooking as forbidden (ADR-0022 class), so the
    // shipped reader is the white-list poll - explorer copy dialogs via MSAA,
    // read-only. Progress appears as the C-22 bottom bar on the matching tile.
    shell::ProgressTracker progress_tracker;
    if (const auto interval = ProgressPollFromEnvironment()) {
        progress_tracker.SetPollIntervalMs(*interval);
        DF_INFO("app", "taskbar-progress poll overridden to %dms (clamped) (verification hook)",
                progress_tracker.poll_interval_ms());
    }
    // Every state change lands in the log: a probe must be able to tell
    // "scanned and found nothing" from "never scanned".
    progress_tracker.set_changed([](const std::wstring& target_app_id,
                                    shell::ProgressState state, float fraction) {
        DF_INFO("dock", "taskbar progress '%s' -> %s %.0f%%",
                core::WideToUtf8(target_app_id).c_str(), shell::ToString(state),
                fraction * 100.0f);
    });
    // Copies the last snapshot onto the tiles (AUMID-then-basename matching,
    // the same two shapes CountForEntry uses).
    auto apply_progress = [&]() {
        for (dock::DockTile& tile : tiles) {
            const shell::ProgressTracker::Snapshot snap = progress_tracker.StateForEntry(tile.app);
            tile.progress_state = snap.state;
            tile.progress_fraction = snap.fraction;
        }
    };
    progress_tracker.Update(0.0);
    apply_progress();
    DF_INFO("app", "taskbar-progress white list: %zu entr%s, poll %dms",
            std::size(shell::kProgressWhiteList),
            std::size(shell::kProgressWhiteList) == 1 ? "y" : "ies",
            progress_tracker.poll_interval_ms());

    platform::WindowDesc desc;
    desc.title = L"DockForge";
    // Window size is physical pixels; content is laid out in DIPs (ADR-0007).
    // The DPI is not known until the HWND exists, so start at 96 DPI and correct
    // after creation with an explicit resize.
    desc.width = static_cast<int>(std::lround(geometry.window_width_dip));
    desc.height = static_cast<int>(std::lround(geometry.window_height_dip));
    desc.topmost = true;
    // Content comes from DirectComposition only (ADR-0007).
    desc.composition_surface = true;
    // A dock is click-driven and must never steal focus from the app the user is
    // typing in. WS_EX_NOACTIVATE also keeps clicking a tile from raising the dock
    // above the window it just launched.
    desc.no_activate = true;

    platform::Window window;
    if (!window.Create(instance, desc)) {
        DF_FATAL("app", "window creation failed");
        return 3;
    }
    EnableDarkTitleBar(window.Handle());
    if (plate_style.use_system_backdrop) {
        if (!ApplySystemBackdrop(window.Handle(), tier)) {
            DF_WARN("render", "system backdrop failed at runtime; plate still draws");
        }
    }

    platform::CompositionSurface surface;
    if (!surface.Initialize(window.Handle())) {
        DF_FATAL("app", "composition surface initialization failed");
        return 4;
    }

    auto render_backend = render::CreateD2DBackend();
    if (!render_backend->Initialize()) {
        DF_FATAL("render", "D2D backend initialization failed");
        return 5;
    }
    render::PlateRenderer plate;

    // ---- backdrop capture (Blur tier only) ------------------------------
    // The frost needs the pixels physically behind the plate. Two modes:
    //   live   - worker thread re-captures every 200ms. Requires
    //            WDA_EXCLUDEFROMCAPTURE for the window's lifetime, because without
    //            it the readback contains our own last frame and the blur eats its
    //            own output (spike conclusion 2).
    //   frozen - DOCKFORGE_FREEZE_BACKDROP=1 captures once, then releases the
    //            exclusion and stops the worker. The frost stays visually correct
    //            for a static desktop and the window becomes screenshot-visible
    //            again, which is what makes the evidence screenshots possible.
    render::BackdropCapture capture;
    const bool freeze_backdrop = ReadEnvFlag(L"DOCKFORGE_FREEZE_BACKDROP");
    bool capture_active = false;
    std::vector<std::uint8_t> backdrop_pixels;
    int backdrop_w = 0;
    int backdrop_h = 0;
    std::uint64_t backdrop_generation = 0;

    // ---- dock placement (T-007) -----------------------------------------
    // Placement is computed from the display's WORK AREA in physical pixels, not
    // from the monitor rect: a dock that slides under the taskbar looks like a
    // clipping bug. ComputeDockPlacement also clamps, so a dock bigger than the
    // screen shrinks instead of running off the edge. This single lambda is the
    // only place that sizes and positions the window, so WM_DPICHANGED and
    // startup can never disagree about the geometry.
    const auto displays = platform::EnumerateDisplays();
    for (std::size_t i = 0; i < displays.size(); ++i) {
        const auto& d = displays[i];
        DF_INFO("platform",
                "display[%zu] %s monitor=%ld,%ld,%ld,%ld work=%ld,%ld,%ld,%ld dpi=%u raw=%u "
                "primary=%d",
                i, core::WideToUtf8(d.device).c_str(), d.monitor.left, d.monitor.top,
                d.monitor.right, d.monitor.bottom, d.work.left, d.work.top, d.work.right,
                d.work.bottom, d.effective_dpi, d.raw_dpi, d.primary ? 1 : 0);
    }

    platform::DisplayInfo target_display;
    bool have_target = false;
    if (const auto wanted = TargetDisplayFromEnvironment()) {
        if (*wanted < static_cast<int>(displays.size())) {
            target_display = displays[static_cast<std::size_t>(*wanted)];
            have_target = true;
            DF_INFO("app", "dock pinned to display[%d] by DOCKFORGE_TARGET_DISPLAY", *wanted);
        } else {
            DF_WARN("app", "DOCKFORGE_TARGET_DISPLAY=%d out of range (have %zu)", *wanted,
                    displays.size());
        }
    }
    if (!have_target) have_target = platform::PrimaryDisplay(target_display);

    // The DPI override exists so the DPI matrix is provable on a single-DPI
    // machine. It must drive placement as well as content, otherwise a 96 DPI
    // override would lay out DIPs at 96 while sizing the window at the monitor's
    // real scale, and the panel would not fill the window. Re-applied after a
    // WM_DPICHANGED re-resolves the display, or the override would silently
    // disappear the first time the dock crosses a monitor boundary.
    auto apply_dpi_override = [](platform::DisplayInfo& display) {
        const UINT forced = platform::Window::DpiOverride();
        if (forced != 0) display.effective_dpi = forced;
    };
    if (const UINT forced = platform::Window::DpiOverride(); forced != 0 && have_target) {
        DF_INFO("app", "dpi override %u replaces display dpi %u for placement", forced,
                target_display.effective_dpi);
    }
    apply_dpi_override(target_display);

    // Mutable since T-069: the cycle_dock_side action (and a settings edit to
    // dock.position) moves the dock to another edge at runtime, and
    // apply_placement re-reads the config here on every placement.
    platform::DockEdge edge = ToDockEdge(config.dock.position);
    // Set once the drop target exists. Placement is where the geometry is recomputed,
    // and the drop target resolves insertion slots against that geometry, so the two
    // have to be updated together - but the target is created after this lambda
    // because it needs the HWND. A null check here is cheaper than reordering the
    // whole startup so that one consumer comes first.
    std::function<void()> geometry_changed;

    // Dock screen rectangle, refreshed by apply_placement. The tick needs it to
    // place the preview popup on top of the dock, and the preview needs the same
    // "the scale comes from geometry, never from a raw DPI query" rule the drop
    // target already follows (C-16d).
    RECT dock_rect{};
    float safe_scale = 1.0f;

    auto apply_placement = [&]() {
        if (!have_target) return;
        // Re-read the edge from the live config (T-069): the dock can move to
        // another edge at runtime through a hotkey/hotspot action or settings.
        edge = ToDockEdge(config.dock.position);
        // Recompute geometry here, not once at startup: the window DPI is only
        // readable after Create(), and WM_DPICHANGED lands here too. Sizing the
        // window from a stale scale would clip the row at 150%/200%.
        geometry = dock::ComputeGeometry(config, static_cast<int>(tiles.size()), window.Scale());
        const auto placement = platform::ComputeDockPlacement(
            target_display, edge, geometry.window_width_dip, geometry.window_height_dip, 8.0f);
        if (placement.clamped) {
            DF_WARN("platform", "dock clamped to %dx%d px on %s (work area %dx%d)",
                    placement.width_px, placement.height_px,
                    core::WideToUtf8(target_display.device).c_str(),
                    target_display.WorkWidth(), target_display.WorkHeight());
        }
        SetWindowPos(window.Handle(), HWND_TOPMOST, placement.rect.left, placement.rect.top,
                     placement.width_px, placement.height_px,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        surface.Resize(placement.width_px, placement.height_px);
        if (capture_active) {
            capture.SetRegion(placement.rect.left, placement.rect.top, placement.width_px,
                              placement.height_px);
        }
        RECT actual{};
        GetWindowRect(window.Handle(), &actual);
        // The preview popup is positioned relative to the dock, so it needs the
        // dock's real screen rectangle. Read here, next to the placement that
        // produces it, rather than re-querying per frame.
        dock_rect = actual;
        safe_scale = window.Scale() > 0.0f ? window.Scale() : 1.0f;
        DF_INFO("app",
                "dock placed edge=%d work=%ld,%ld,%ld,%ld target=%ld,%ld %dx%d "
                "rect=%ld,%ld,%ld,%ld dpi=%u",
                static_cast<int>(edge), target_display.work.left, target_display.work.top,
                target_display.work.right, target_display.work.bottom, placement.rect.left,
                placement.rect.top, placement.width_px, placement.height_px, actual.left,
                actual.top, actual.right, actual.bottom, window.Dpi());
        if (geometry_changed) geometry_changed();
    };
    apply_placement();

    if (plate_style.use_capture && caps.capture_available) {
        if (freeze_backdrop) {
            // One capture, with exclusion on, taken before the window is shown.
            // Then exclusion is dropped so the window is visible to screen capture.
            SetWindowCaptureExclusion(window.Handle(), true);
            render::BackdropCapture one_shot;
            RECT rc{};
            GetWindowRect(window.Handle(), &rc);
            if (one_shot.Start(window.Handle(), rc.left, rc.top, rc.right - rc.left,
                               rc.bottom - rc.top, 1000)) {
                for (int i = 0; i < 20 && !one_shot.HasFrame(); ++i) {
                    Sleep(50);
                }
                one_shot.AcquireIfNew(0, backdrop_pixels, backdrop_w, backdrop_h,
                                      backdrop_generation);
                one_shot.Stop();
            }
            SetWindowCaptureExclusion(window.Handle(), false);
            DF_INFO("render", "frozen backdrop %dx%d (frame=%d)", backdrop_w, backdrop_h,
                    backdrop_generation > 0 ? 1 : 0);
        } else {
            RECT rc{};
            GetWindowRect(window.Handle(), &rc);
            SetWindowCaptureExclusion(window.Handle(), true);
            capture_active = capture.Start(window.Handle(), rc.left, rc.top, rc.right - rc.left,
                                           rc.bottom - rc.top, 200);
            if (!capture_active) {
                DF_WARN("render", "backdrop capture failed to start; plate will be flat");
            }
        }
    }

    // Set by WM_MOUSEMOVE, cleared by the tick. Mouse messages arrive faster than
    // the frame rate, so the tick is the only place a repaint may be requested.
    bool hover_moved = false;

    // Reorder gesture (T-013). Lives next to the hover state because both are driven
    // by the same pointer stream and must agree about where the row is: the drag
    // reads the centres the painter last laid out, so the insertion slot always
    // matches the icons on screen.
    dock::DragController drag;
    bool drag_moved = false;  // set by WM_MOUSEMOVE, consumed by the tick

    // In-flight OLE drag from another application (T-014). Written by the drop target
    // and read by the painter and the region, never the other way round: OLE runs the
    // drag loop inside the source process, so every callback here is a synchronous
    // interruption of our own message loop and must not call back into it.
    dock::DropPointer drop_pointer;
    bool drop_moved = false;  // set by the OLE callbacks, consumed by the tick

    // Paint cost, measured separately from the frame interval: the frame clock only
    // says a deadline was missed, this says whether the miss was drawing or present.
    core::FrameClock paint_clock;

    // Window preview popup (T-015). Three pieces, in the modules that own them:
    //   * platform::PreviewWindow is the popup HWND and its window class;
    //   * shell::ThumbnailHost owns every DwmRegisterThumbnail lifetime and the
    //     DWM slot rectangles;
    //   * dock::HoverTimer is the 400ms dwell that decides when to show it.
    // The popup is deliberately NOT part of the dock's CompositionSurface: DWM
    // composites thumbnails into the destination window's own redirection
    // surface, and the dock's surface is a DirectComposition visual that would
    // swallow them.
    platform::PreviewWindow preview;
    shell::ThumbnailHost thumbnails;
    dock::HoverTimer hover_timer;
    bool preview_visible = false;
    int preview_tile = -1;
    // Set when a registration is queued and pumped over later ticks. A cold
    // DwmRegisterThumbnail costs 219..609ms per slot (spikes/t015_thumb), so the
    // whole set can never be registered inside one frame.
    bool thumbnails_pending = false;

    // Switches the preview off without touching the config: the dock is a system
    // tray-level feature, and "changed system behaviour must have a switch that
    // defaults to off" (PROMPT.md).
    const bool preview_enabled = ReadEnvFlag(L"DOCKFORGE_PREVIEW", true) && preview_class_ready;
    // Weather forecast popup (T-075): same switch shape, defaults on because it
    // paints nothing until the weather tile itself is enabled in config.
    const bool weather_preview_enabled = ReadEnvFlag(L"DOCKFORGE_WEATHER_PREVIEW", true);

    // Folder drill-through panel (T-066, ADR-0023). Owns its popup HWND and
    // navigation stack; the Dock only decides when to open it (a folder tile
    // click) and feeds its watcher/command channel from the tick. Disabled
    // silently never: the registration result is logged and a failed click
    // falls back to the launch path.
    dock::FolderPanel folder_panel;
    if (!folder_panel_class_ready) {
        DF_WARN("dock", "folder panel class registration failed; folder tiles launch");
    }

    // ---- weather host + popup (T-075) ------------------------------------
    // The host owns the worker thread; every fetch happens there and results
    // cross back as snapshot copies (Tick adopts at most one per frame). The
    // popup is the hover-preview for the weather tile: a GDI popover in the
    // folder-panel family, shown after the same 400ms dwell that gates
    // thumbnails.
    const bool weather_popup_class_ready = dock::WeatherPopup::RegisterWindowClass(instance);
    if (!weather_popup_class_ready) {
        DF_WARN("weather", "popup class registration failed; no forecast preview this run");
    }
    dock::WeatherPopup weather_popup;
    dock::weather::Host weather_host;
    dock::weather::Snapshot weather_snapshot;  // failed/invalid until the first fetch lands
    bool weather_host_started = false;
    if (weather_tile_index() >= 0) {
        // Keyed-provider entry point (DoD 1): the AMap key is read from
        // secrets.json only - never from config.json, never logged, never
        // stored anywhere else. With a key present the provider stays
        // "amap" (whose API chain is not implemented this session); without
        // one the no-key Open-Meteo fallback serves the tile.
        const std::map<std::string, std::string> secrets = core::ReadSecrets();
        if (secrets.count("amap_key") != 0) {
            DF_INFO("weather", "amap_key present in secrets.json (%zu bytes, value not logged)",
                    secrets.at("amap_key").size());
        }
        weather_host.SetFetcher(dock::weather::DefaultFetcher());
        weather_host.Start(config.weather);
        weather_host_started = true;
        DF_INFO("weather", "host started (worker thread fetching)");
    }

    // Launchpad (T-041). One overlay per display, one shared index. The index scan is
    // 360-1045ms on this machine (evidence/T-040), so it is warmed once at startup
    // rather than on the first open, and the file watcher only marks it stale; the
    // rebuild happens on the next open, never inside a frame.
    //
    // Switchable off for the same reason as the preview: it changes system-visible
    // behaviour (a full-screen topmost window and a global hotkey) and PROMPT.md
    // requires such features to have a switch.
    const bool launchpad_enabled = ReadEnvFlag(L"DOCKFORGE_LAUNCHPAD", true);
    dock::Launchpad launchpad;
    bool launchpad_ready = false;
    if (launchpad_enabled) {
        if (launchpad.Initialize(instance, config, icons, dock::LaunchpadOptions{})) {
            launchpad_ready = true;
            const std::uint64_t warm_start = GetTickCount64();
            launchpad.RebuildIndex(config);
            // Icons cost 1797ms for 70 at 192px (measured this run). Extracting them
            // here rather than inside Open() is the difference between a launcher that
            // appears on the gesture and one that stalls for two seconds on the first
            // press of its hotkey.
            launchpad.WarmTiles(config);
            DF_INFO("launchpad", "index warmed: items=%d in %.1fms", launchpad.ItemCount(),
                    static_cast<double>(GetTickCount64() - warm_start));
        } else {
            DF_WARN("launchpad", "initialize failed; Launchpad unavailable this run");
        }
    } else {
        DF_INFO("launchpad", "disabled by DOCKFORGE_LAUNCHPAD");
    }

    // Settings window (T-050). Off by default in the steady state - there is no
    // dock affordance to open it yet - so the env trigger and the probe file are
    // the two entry points, exactly like the Launchpad's channel. The bus
    // subscription here is the C-20 proof for the probe: a `set` command has to
    // produce one "config published" line per flush, after the save.
    core::EventBus bus;  // declared before every subscriber it serves

    // Config hot reload (T-051). The watcher is the proven LaunchpadWatcher
    // pattern pointed at the config directory with the C-20 debounce; the
    // decision logic lives in core::ReloadConfigFromDisk (unit-tested).
    shell::LaunchpadWatcher config_watcher;
    const bool config_watcher_running = [&] {
        // Watch the config FILE's parent, not ConfigDir(): the path override
        // (tests, evidence probes) can point anywhere, and watching ConfigDir
        // would silently watch the wrong directory (measured - the probe's
        // temp config never fired).
        const std::wstring path = core::ConfigPath();
        const std::size_t slash = path.find_last_of(L"\\/");
        std::vector<std::wstring> roots;
        if (slash != std::wstring::npos && slash > 0) roots.push_back(path.substr(0, slash));
        const bool ok = config_watcher.Start(roots, 300.0);
        if (!ok) DF_WARN("core", "config watcher could not start; no hot reload this run");
        return ok;
    }();

    // ---- notification centre (T-076, ADR-0025) ---------------------------
    // Three owners, one job each:
    //   * NotificationSidecarBridge - the .NET child that can actually complete
    //     UserNotificationListener's async calls. T-065 measured that this
    //     process never completes them; T-076 re-measured it with RoInitialize
    //     (STA) and a real PeekMessage/DispatchMessage pump for 8s and the
    //     status stayed Started (evidence/T-076/spike_async_pump.log).
    //   * NotificationStore - the in-memory history and the "what is new" rule
    //     (the first poll of a session is a baseline, not a burst of arrivals).
    //   * NotificationBanner / NotificationPanel - two GDI windows in the
    //     folder-panel family.
    // Nothing here touches the disk except the probe channel, and the probe
    // payloads carry counts only: ADR-0025 says notification content is not
    // written anywhere, including our own evidence files.
    notifications::NotificationSidecarBridge notif_sidecar;
    notifications::NotificationStore notif_store;
    notifications::NotificationBanner notif_banner;
    notifications::NotificationPanel notif_panel;
    // One clear implementation for the 清除 button and the probe command. Two
    // copies would drift, and the probe would then be certifying a path the user
    // never touches. Local clear comes first: the panel must empty even when the
    // sidecar is gone, and the ids stay in the store's seen set so the next poll
    // does not re-announce what the user just dismissed.
    const auto notif_clear_history = [&](int dpi) {
        notif_store.ClearLocal();
        notif_panel.SetItems(&notif_store.items());
        notif_panel.Refresh(dpi);
        if (notif_sidecar.running()) notif_sidecar.RequestClear();
        DF_INFO("notifications", "history cleared");
    };
    bool notif_ready = false;
    if (notifications::NotificationBanner::RegisterWindowClass(instance) &&
        notifications::NotificationPanel::RegisterWindowClass(instance) &&
        notif_banner.Create(instance) && notif_panel.Create(instance)) {
        notif_ready = true;
        // C-21: both windows start at the tier the dock resolved at startup and
        // follow MaterialMonitor from then on. Neither reads the power state.
        notif_banner.SetMaterialTier(tier);
        notif_panel.SetMaterialTier(tier);
        notif_panel.SetItems(&notif_store.items());
        notif_panel.SetAccessStatus(notif_store.access());
        notif_panel.SetClearHandler([&] { notif_clear_history(window.Dpi()); });
    } else {
        DF_WARN("notifications", "banner/panel window creation failed; centre disabled");
    }

    // A banner click and a panel row click do the same thing: take the user to
    // the app that posted the notification. A pinned app goes through the dock's
    // own click path (which knows about minimize-on-second-click); an unpinned
    // one is started from its AUMID, because "click the notification" must not be
    // a no-op for an app the user never pinned.
    const auto notif_activate = [&](const notifications::NotificationItem& item) {
        std::vector<notifications::TileIdentity> identities;
        identities.reserve(tiles.size());
        for (const dock::DockTile& tile : tiles) {
            identities.push_back(notifications::TileIdentity{tile.app.aumid,
                                                             tile.app.display_name});
        }
        const int index = notifications::FindTileForNotification(item, identities);
        if (index >= 0) {
            const dock::DockTile& tile = tiles[static_cast<std::size_t>(index)];
            const shell::ActionResult result = shell::ActivateOrMinimizeTimed(tile.app);
            DF_INFO("notifications", "click -> tile[%d] %s (%s, pid=%lu)", index,
                    core::WideToUtf8(tile.app.display_name).c_str(),
                    shell::ToString(result.action),
                    static_cast<unsigned long>(result.process_id));
            return;
        }
        if (!item.aumid.empty()) {
            shell::AppEntry entry;
            entry.kind = shell::AppKind::Uwp;
            entry.aumid = item.aumid;
            entry.display_name = item.DisplayApp();
            const shell::LaunchResult result = shell::Launch(entry);
            DF_INFO("notifications", "click -> launch aumid '%s' (%s, ok=%d)",
                    core::WideToUtf8(item.aumid).c_str(), result.route,
                    result.ok() ? 1 : 0);
            return;
        }
        DF_INFO("notifications", "click -> no target for '%s'",
                core::WideToUtf8(item.DisplayApp()).c_str());
    };
    notif_banner.SetActivateHandler(notif_activate);
    notif_panel.SetActivateHandler(notif_activate);

    // The feature defaults off in config; the env flag can turn it on for a
    // probe without editing the user's file (same rule as every other
    // system-visible feature in this app). Not const: the settings window can
    // flip it at runtime, and the tick block reads this same flag.
    bool notif_enabled =
        notif_ready && ReadEnvFlag(L"DOCKFORGE_NOTIFICATIONS", config.notifications.enabled);
    std::wstring notif_sidecar_path;  // probe hook: point at a stub sidecar
    {
        wchar_t buffer[MAX_PATH]{};
        const DWORD n = GetEnvironmentVariableW(L"DOCKFORGE_NOTIFICATION_SIDECAR", buffer,
                                                ARRAYSIZE(buffer));
        if (n > 0 && n < ARRAYSIZE(buffer)) notif_sidecar_path.assign(buffer, n);
    }
    notif_banner.model().SetTimeoutSeconds(config.notifications.banner_timeout_seconds);
    // Starts at one full interval so the first poll happens on the first frame
    // rather than after a delay: the panel would otherwise say "waiting" for
    // `poll_seconds` with a sidecar already answering.
    double notif_since_poll_ms = 1e9;
    if (notif_enabled) {
        if (notif_sidecar.Start(notif_sidecar_path)) {
            DF_INFO("notifications", "centre ready pid=%lu poll=%ds timeout=%ds banner=%d pulse=%d",
                    notif_sidecar.pid(), config.notifications.poll_seconds,
                    config.notifications.banner_timeout_seconds,
                    config.notifications.banner_enabled ? 1 : 0,
                    config.notifications.pulse_icons ? 1 : 0);
        } else {
            DF_WARN("notifications", "sidecar did not start; centre shows no history this run");
        }
    } else {
        DF_INFO("notifications", "disabled (notifications.enabled=%d ready=%d)",
                config.notifications.enabled ? 1 : 0, notif_ready ? 1 : 0);
    }

    // C-21: one monitor for the whole process. Every window that uses a material
    // subscribes to its events; none of them polls the power state (that is the
    // contract, and the reason the monitor exists rather than each window reading
    // GetSystemPowerStatus itself).
    render::MaterialMonitor material_monitor(bus, [] { return render::ReadSystemPower(); });
    material_monitor.SetBaseline(render::MaterialBaseline{requested, tier});
    bus.Subscribe<core::MaterialDegraded>([&](const core::MaterialDegraded& event) {
        DF_INFO("render", "material degraded %s -> %s: %s", core::ToString(event.from),
                core::ToString(event.to), event.reason);
        notif_banner.SetMaterialTier(event.to);
        notif_panel.SetMaterialTier(event.to);
        // The dock's own plate keeps its startup style (D2D resources are built
        // around it), so the honest response is a repaint plus the log line the
        // evidence probe reads - not a silent no-op.
        InvalidateRect(window.Handle(), nullptr, FALSE);
    });
    bus.Subscribe<core::MaterialRestored>([&](const core::MaterialRestored& event) {
        DF_INFO("render", "material restored to %s", core::ToString(event.to));
        notif_banner.SetMaterialTier(event.to);
        notif_panel.SetMaterialTier(event.to);
        InvalidateRect(window.Handle(), nullptr, FALSE);
    });

    // Probe channel (T-076, same shape as the hardware panel's). Commands:
    //   show | hide | toggle   banner + panel visibility is what these drive;
    //                          `show` also opens the history panel
    //   clear                  clears the local history and asks the sidecar
    //   state                  writes DOCKFORGE_NOTIFICATIONS_STATE_FILE
    //   push <app>|<title>|<body>
    //                          injects a synthetic arrival through the exact
    //                          banner + pulse path a real toast takes, so the
    //                          visual behaviour is probeable without waiting
    //                          for a real notification to arrive
    // The state file carries counts and tiers only - never notification text
    // (ADR-0025).
    std::wstring notif_cmd_file;
    std::wstring notif_state_file;
    {
        wchar_t buffer[MAX_PATH]{};
        DWORD n = GetEnvironmentVariableW(L"DOCKFORGE_NOTIFICATIONS_CMD_FILE", buffer,
                                          ARRAYSIZE(buffer));
        if (n > 0 && n < ARRAYSIZE(buffer)) notif_cmd_file.assign(buffer, n);
        n = GetEnvironmentVariableW(L"DOCKFORGE_NOTIFICATIONS_STATE_FILE", buffer,
                                    ARRAYSIZE(buffer));
        if (n > 0 && n < ARRAYSIZE(buffer)) notif_state_file.assign(buffer, n);
        if (!notif_cmd_file.empty() || !notif_state_file.empty()) {
            DF_INFO("notifications", "probe cmd=%s state=%s",
                    notif_cmd_file.empty() ? "<none>"
                                           : core::WideToUtf8(notif_cmd_file).c_str(),
                    notif_state_file.empty() ? "<none>"
                                             : core::WideToUtf8(notif_state_file).c_str());
        }
    }
    auto write_notif_state_file = [&]() {
        if (notif_state_file.empty()) return;
        std::string json = "{\"enabled\":";
        json += notif_enabled ? "true" : "false";
        json += ",\"ready\":";
        json += notif_ready ? "true" : "false";
        json += ",\"sidecar\":";
        json += notif_sidecar.running() ? "true" : "false";
        json += ",\"pid\":";
        json += std::to_string(notif_sidecar.pid());
        json += ",\"count\":";
        json += std::to_string(notif_store.size());
        json += ",\"primed\":";
        json += notif_store.primed() ? "true" : "false";
        json += ",\"access\":\"";
        json += notifications::ToString(notif_store.access());
        json += "\",\"banner\":";
        json += notif_banner.SnapshotJson();
        json += ",\"panel\":";
        json += notif_panel.SnapshotJson();
        json += "}\n";
        HANDLE h = CreateFileW(notif_state_file.c_str(), GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(h, json.data(), static_cast<DWORD>(json.size()), &written, nullptr);
        CloseHandle(h);
    };
    core::ProbeCommandGate notif_gate;
    // A stale command left by an earlier run must not fire on this one (measured:
    // it hid the operator's taskbar). Priming makes the file's existing content
    // count as already seen.
    notif_gate.Prime(core::ReadProbeFileContent(notif_cmd_file));
    double notif_cmd_since_read_ms = 1e9;
    // Synthetic ids start above anything the sidecar reports so a probe push
    // can never collide with a real notification id.
    std::uint32_t notif_probe_id = 0xF0000000u;
    // One delivery path for every notification, real toast or probe push. Two
    // copies would drift, and the probe would then be certifying behaviour the
    // shipped path does not have. The history feed is NOT part of this: a real
    // arrival reaches the store through ApplyState, a probe push has to build a
    // synthetic state because there may be no sidecar at all.
    const auto notif_deliver = [&](const notifications::NotificationItem& item, bool* dirty) {
        if (config.notifications.banner_enabled && notif_banner.model().Push(item)) {
            notif_banner.Refresh(window.Dpi());
        }
        if (!config.notifications.pulse_icons) return;
        // C-22 pulse: 320ms x2, driven by the shared clock. Only the tile this
        // notification belongs to pulses; an unmatched toast pulses nothing,
        // because a wrong pulse points the user at an app that did nothing.
        std::vector<notifications::TileIdentity> identities;
        identities.reserve(tiles.size());
        for (const dock::DockTile& tile : tiles) {
            identities.push_back(
                notifications::TileIdentity{tile.app.aumid, tile.app.display_name});
        }
        const int index = notifications::FindTileForNotification(item, identities);
        if (index < 0) return;
        tiles[static_cast<std::size_t>(index)].pulse_start_ms = dock_clock_ms > 0.0 ? dock_clock_ms : 1.0;
        if (dirty != nullptr) *dirty = true;
        // The probe greps this line, so it must be emitted from the shared path
        // rather than from the probe branch - otherwise the probe would be
        // proving its own copy of the logic.
        DF_DEBUG("notifications", "pulse tile[%d]", index);
    };
    auto read_notif_command = [&](double delta_ms) {
        notif_cmd_since_read_ms += delta_ms;
        if (notif_cmd_since_read_ms < 50.0) return;
        notif_cmd_since_read_ms = 0.0;
        if (notif_cmd_file.empty()) return;
        const std::string content = core::ReadProbeFileContent(notif_cmd_file);
        // Full-content dedup, same as the hardware probe: the script appends a
        // unique token line so repeated identical commands all arrive.
        if (!notif_gate.Accept(content)) return;
        const std::string command = core::ProbeCommandOf(content);
        if (command.empty()) return;
        // Log the verb only. `push` carries app|title|body, and ADR-0025 says
        // notification text never reaches the log file - a probe command must
        // obey the same rule as a real toast, or the evidence run itself would
        // prove the leak.
        const std::size_t verb_end = command.find(' ');
        const std::string verb =
            verb_end == std::string::npos ? command : command.substr(0, verb_end);
        DF_INFO("notifications", "probe command=%s", verb.c_str());
        if (command == "show") {
            notif_panel.Open(window.Dpi());
        } else if (command == "hide") {
            notif_panel.Close();
            notif_banner.model().Clear();
            notif_banner.Hide();
        } else if (command == "toggle") {
            if (notif_panel.Visible()) {
                notif_panel.Close();
            } else {
                notif_panel.Open(window.Dpi());
            }
        } else if (command == "clear") {
            notif_clear_history(window.Dpi());
        } else if (command == "material degrade" || command == "material restore") {
            // C-21 has no UI of its own: the trigger is a real render device loss
            // or the OS battery-saver flag, neither of which a probe can cause on
            // demand. Driving MaterialMonitor::SetDeviceLost is the closest honest
            // substitute - it runs the shipped evaluation, publishes the real
            // events and moves the real windows, so the state file's banner.tier /
            // panel.tier afterwards is evidence about the shipped chain, not about
            // a probe-only branch.
            material_monitor.SetDeviceLost(command == "material degrade");
            notif_banner.Refresh(window.Dpi());
            notif_panel.Refresh(window.Dpi());
        } else if (command.rfind("push ", 0) == 0) {
            // app|title|body - the pipe separator keeps the probe script to one
            // line while still exercising the full DisplayApp/Headline/Detail
            // fallback chain.
            std::string payload = command.substr(5);
            std::string app = payload;
            std::string title;
            std::string body;
            const std::size_t first = payload.find('|');
            if (first != std::string::npos) {
                app = payload.substr(0, first);
                const std::size_t second = payload.find('|', first + 1);
                if (second == std::string::npos) {
                    title = payload.substr(first + 1);
                } else {
                    title = payload.substr(first + 1, second - first - 1);
                    body = payload.substr(second + 1);
                }
            }
            notifications::NotificationItem item;
            item.id = notif_probe_id++;
            item.app = core::Utf8ToWide(app);
            item.title = core::Utf8ToWide(title);
            item.body = core::Utf8ToWide(body);
            notif_deliver(item, nullptr);
            // Also put it in the history so the panel screenshot shows a row:
            // the store is normally fed by the sidecar, and a probe run may have
            // no sidecar at all. Rebuilding a state from the current list keeps
            // the two views consistent instead of letting them drift.
            {
                notifications::NotificationState synthetic;
                synthetic.valid = true;
                synthetic.access = notifications::AccessStatus::Allowed;
                synthetic.items.push_back(item);
                for (const notifications::NotificationItem& existing : notif_store.items()) {
                    synthetic.items.push_back(existing);
                }
                synthetic.total = static_cast<int>(synthetic.items.size());
                notif_store.ApplyState(synthetic, nullptr);
                notif_panel.SetItems(&notif_store.items());
                notif_panel.SetAccessStatus(notif_store.access());
                notif_panel.Refresh(window.Dpi());
            }
        }
        write_notif_state_file();
    };

    std::string last_reload_reason;
    settings::SettingsWindow settings;
    bool settings_ready = false;
    const bool settings_enabled = ReadEnvFlag(L"DOCKFORGE_SETTINGS", false);
    if (settings_enabled) {
        if (settings::SettingsWindow::RegisterWindowClass(instance)) {
            if (settings.Create(instance, bus, config)) {
                settings_ready = true;
                if (ReadEnvFlag(L"DOCKFORGE_SETTINGS_SHOW", false)) settings.Show();
                DF_INFO("settings", "ready (window deferred until show)");
            }
        } else {
            DF_WARN("settings", "window class registration failed");
        }
    }
    bus.Subscribe<core::ConfigChanged>([](const core::ConfigChanged& event) {
        DF_INFO("settings", "config published icon_size=%d launchpad_columns=%d",
                event.config.dock.icon_size, event.config.launchpad.columns);
    });

    // Autostart switch (T-060): the settings checkbox lands in config.json
    // through the normal flush, then this subscription makes it actually
    // effective - register the scheduler task when turned on, delete it when
    // turned off (no residue; an already-absent task counts as removed). Only a
    // *change* acts: hot reloads republish the same snapshot untouched.
    bus.Subscribe<core::ConfigChanged>([&](const core::ConfigChanged& event) {
        if (event.config.behavior.autostart == last_autostart) return;
        last_autostart = event.config.behavior.autostart;
        std::string detail;
        const bool ok = core::ApplyAutostartSystem(last_autostart, &detail);
        DF_INFO("core", "autostart switch -> %d: %s (%s)", last_autostart ? 1 : 0,
                ok ? "ok" : "FAILED", detail.c_str());
    });

    // Taskbar visibility (T-061). Same shape as the autostart switch: the
    // settings checkbox lands in config.json through the normal flush, this
    // subscription applies it, and only a *change* acts (hot reloads republish
    // the same snapshot untouched). The default is off and off only ever
    // SHOWS - the most invasive feature in the app must fail towards "taskbar
    // visible". Startup applies the config's current value once, after the
    // dock window exists (see below ShowWindow), never re-asserting it later:
    // a restarted explorer brings the taskbar back by design (spike Q6).
    bool last_hide_taskbar = config.behavior.hide_taskbar;
    bool taskbar_visible_published = true;  // the steady state at startup
    // Publishes the observed C-20 state, deduplicated so a no-op apply cannot
    // spam placement work. The event carries the OBSERVED visibility, not the
    // requested one - ShowWindowAsync settles asynchronously.
    auto publish_taskbar_state = [&](bool force) {
        const bool visible = core::TaskbarCurrentlyVisible();
        if (force || visible != taskbar_visible_published) {
            taskbar_visible_published = visible;
            bus.Publish(core::TaskbarVisibilityChanged{visible});
        }
    };
    bus.Subscribe<core::ConfigChanged>([&](const core::ConfigChanged& event) {
        if (event.config.behavior.hide_taskbar == last_hide_taskbar) return;
        last_hide_taskbar = event.config.behavior.hide_taskbar;
        std::string detail;
        // Idempotent decision layer: hide when turning on, "already hidden" is
        // a success no-op, turning off shows.
        core::ShellTraywndTaskbar backend;
        const bool ok =
            core::ApplyTaskbarVisibility(last_hide_taskbar, backend, &detail);
        DF_INFO("core", "hide_taskbar switch -> %d: %s (%s)", last_hide_taskbar ? 1 : 0,
                ok ? "ok" : "FAILED", detail.c_str());
        publish_taskbar_state(false);
    });

    // C-20: the taskbar module publishes visibility flips; the dock re-reads
    // the work area in response. The spike measured the work area unchanged
    // while hidden, so today this is a cheap no-op - it exists so a future
    // Windows that DOES shrink the work area cannot leave the dock floating
    // over the (hidden) taskbar strip.
    bus.Subscribe<core::TaskbarVisibilityChanged>(
        [&](const core::TaskbarVisibilityChanged& event) {
            DF_INFO("core", "taskbar visibility -> %d; re-reading work area",
                    event.visible ? 1 : 0);
            if (platform::PrimaryDisplay(target_display)) {
                apply_dpi_override(target_display);
                apply_placement();
            }
        });

    // The Launchpad's open gesture. A registered hotkey, not a low-level keyboard hook:
    // WH_KEYBOARD_LL would sit in the input path of every process on the machine, and
    // RegisterHotKey already fails cleanly when the combination is taken (which is
    // logged, not silently ignored).
    constexpr int kLaunchpadHotkeyId = 0x4C50;  // 'LP'
    bool launchpad_hotkey = false;
    if (launchpad_ready) {
        launchpad_hotkey =
            RegisterHotKey(window.Handle(), kLaunchpadHotkeyId, MOD_CONTROL | MOD_ALT, VK_SPACE);
        if (launchpad_hotkey) {
            DF_INFO("launchpad", "hotkey registered: Ctrl+Alt+Space");
        } else {
            DF_WARN("launchpad", "RegisterHotKey failed (%lu); use the environment trigger",
                    GetLastError());
        }
    }

    // Emergency taskbar restore (T-061): Ctrl+Alt+T. Registered
    // UNCONDITIONALLY - not behind hide_taskbar, not behind any module being
    // ready - because the whole point is that the recovery must exist even
    // when the feature misbehaved. The modifier set carries MOD_NOREPEAT (C-24
    // rule): key repeat must not machine-gun ShowWindow. The handler restores
    // the taskbar AND persists hide_taskbar=false so the recovery survives a
    // restart; see the WM_HOTKEY arm.
    constexpr int kTaskbarRestoreHotkeyId = 0x5442;  // 'TB'
    const bool taskbar_hotkey = RegisterHotKey(window.Handle(), kTaskbarRestoreHotkeyId,
                                               MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'T');
    if (taskbar_hotkey) {
        DF_INFO("core", "emergency hotkey registered: Ctrl+Alt+T restores the taskbar");
    } else {
        // Logged, never fatal: losing the hotkey degrades recovery to the
        // settings switch, which is exactly why that switch keeps working too.
        DF_WARN("core", "emergency hotkey RegisterHotKey failed (%lu)", GetLastError());
    }

    // Verification hook for the Launchpad, same precedent and same reason as the hover
    // probe above: the automated probe cannot win a race against a human hand on the
    // physical mouse, and RegisterHotKey cannot be injected from another process.
    //
    // The channel is a file holding ONE command line. It is re-read at most every
    // kLaunchpadCmdPollMs, and a command whose text is unchanged from the last one is
    // ignored - otherwise "open" would re-fire every poll and the open animation would
    // restart forever.
    //
    // Commands: `open`, `close`, `page <n>`, `click <x> <y>`, `hover <x> <y>`, `leave`,
    // plus the T-042 set: `type <utf8>`, `backspace`, `key <vk>`, `press <x> <y>`,
    // `release <x> <y>`. Coordinates are DIP on display 0. Only the *source* of the
    // input is replaced: the real PressAt/ReleaseAt/Character/KeyPressed paths run, so
    // a PASS still proves the chain.
    std::wstring launchpad_cmd_file;
    if (launchpad_ready) {
        wchar_t cmd_buffer[MAX_PATH]{};
        const DWORD n = GetEnvironmentVariableW(L"DOCKFORGE_LAUNCHPAD_CMD_FILE", cmd_buffer,
                                               ARRAYSIZE(cmd_buffer));
        if (n > 0 && n < ARRAYSIZE(cmd_buffer)) {
            launchpad_cmd_file.assign(cmd_buffer, n);
            DF_INFO("launchpad", "command probe file=%s",
                    core::WideToUtf8(launchpad_cmd_file).c_str());
        }
    }
    core::ProbeCommandGate launchpad_gate;
    launchpad_gate.Prime(core::ReadProbeFileContent(launchpad_cmd_file));
    double launchpad_cmd_since_read_ms = 1e9;
    auto read_launchpad_command = [&](double delta_ms) {
        launchpad_cmd_since_read_ms += delta_ms;
        if (launchpad_cmd_since_read_ms < 50.0) return;
        launchpad_cmd_since_read_ms = 0.0;
        const std::string command = core::ReadProbeFileContent(launchpad_cmd_file);
        if (!launchpad_gate.Accept(command)) return;
        DF_DEBUG("launchpad", "probe command=%s", command.c_str());

        if (command == "open") {
            launchpad.Open(config);
        } else if (command == "close") {
            launchpad.Close();
        } else if (command == "leave") {
            launchpad.PointerLeft(0);
        } else if (command == "backspace") {
            launchpad.Backspace();
        } else if (command == "clear") {
            launchpad.ClearField();
        } else if (command.rfind("key ", 0) == 0) {
            launchpad.KeyPressed(static_cast<unsigned int>(atoi(command.c_str() + 4)));
        } else if (command.rfind("type ", 0) == 0) {
            // The probe file is UTF-8; the field wants wide characters, one call per
            // code unit - the same path a physical keyboard takes through WM_CHAR.
            const std::wstring text = core::Utf8ToWide(std::string(command.c_str() + 5));
            for (wchar_t ch : text) launchpad.Character(ch);
        } else if (command.rfind("page ", 0) == 0) {
            launchpad.KeyPressed(atoi(command.c_str() + 5) > 0 ? VK_RIGHT : VK_LEFT);
        } else if (command.rfind("click ", 0) == 0 || command.rfind("hover ", 0) == 0 ||
                   command.rfind("press ", 0) == 0 || command.rfind("release ", 0) == 0) {
            // press/release take their coordinates after the keyword; click/hover
            // after "click "/"hover " (6 chars). Find the space instead of assuming.
            const std::size_t space = command.find(' ');
            float x = 0.0f;
            float y = 0.0f;
            if (space != std::string::npos &&
                sscanf_s(command.c_str() + space + 1, "%f %f", &x, &y) == 2) {
                if (command[0] == 'c') {
                    launchpad.ClickAt(0, x, y);
                } else if (command[0] == 'h') {
                    launchpad.PointerMoved(0, x, y);
                } else if (command[0] == 'p') {
                    launchpad.PressAt(0, x, y);
                } else {
                    launchpad.ReleaseAt(0, x, y);
                }
            }
        }
    };

    // Settings probe channel (T-050), same file-channel pattern as the Launchpad.
    // Commands: `show`, `hide`, `close`, `section <name>`, `set <field> <value>`.
    // `set` takes the RecordEdit path, so the debounce, clamp, save and publish
    // all run the production code.
    std::wstring settings_cmd_file;
    if (settings_ready) {
        wchar_t buffer[MAX_PATH]{};
        const DWORD n = GetEnvironmentVariableW(L"DOCKFORGE_SETTINGS_CMD_FILE", buffer,
                                                ARRAYSIZE(buffer));
        if (n > 0 && n < ARRAYSIZE(buffer)) {
            settings_cmd_file.assign(buffer, n);
            DF_INFO("settings", "probe file=%s", core::WideToUtf8(settings_cmd_file).c_str());
        }
    }
    core::ProbeCommandGate settings_gate;
    settings_gate.Prime(core::ReadProbeFileContent(settings_cmd_file));
    double settings_cmd_since_read_ms = 1e9;
    auto read_settings_command = [&](double delta_ms) {
        settings_cmd_since_read_ms += delta_ms;
        if (settings_cmd_since_read_ms < 50.0) return;
        settings_cmd_since_read_ms = 0.0;
        if (settings_cmd_file.empty()) return;
        const std::string command = core::ReadProbeFileContent(settings_cmd_file);
        if (!settings_gate.Accept(command)) return;
        DF_DEBUG("settings", "probe command=%s", command.c_str());
        if (command == "show") {
            settings.Show();
        } else if (command == "hide") {
            settings.Hide();
        } else if (command == "close") {
            settings.Destroy();
        } else if (command.rfind("section ", 0) == 0) {
            settings.ShowSection(command.c_str() + 8);
        } else if (command.rfind("set ", 0) == 0) {
            const std::size_t first = command.find(' ');
            const std::size_t second = command.find(' ', first + 1);
            if (first != std::string::npos && second != std::string::npos) {
                settings.RecordEdit(command.substr(first + 1, second - first - 1),
                                    command.substr(second + 1));
            }
        }
    };

    // Verification hook, same precedent as DOCKFORGE_FORCE_MATERIAL and
    // DOCKFORGE_DPI_OVERRIDE: pins the hover onto one tile index so the dwell can
    // be exercised without a cursor. The automated probe cannot win a race against
    // the human hand - measured 2026-10-24, the physical mouse crossed the dock
    // mid-case and reset the timer at 125.9ms of a 400ms dwell, and SetCursorPos
    // warps were overwritten within one frame.
    //
    // The channel is a FILE, not an environment variable, because the whole
    // show/hide/re-arm cycle has to be driven from outside: an env var is fixed for
    // the process lifetime and can express "hover tile 6" but never "and now
    // leave". The file holds a decimal tile index, or -1 / anything unparsable for
    // "no hover". It is re-read at most every kHoverProbePollMs so the per-frame
    // cost stays off the hot path.
    //
    // Only the *source* of the hover is replaced. HitTest (for the real path), the
    // dwell timer, the window lookup, the popup and the thumbnail registration all
    // run the production code, so a PASS here still proves the real chain.
    std::wstring hover_probe_file;
    {
        wchar_t buffer[MAX_PATH]{};
        const DWORD n = GetEnvironmentVariableW(L"DOCKFORGE_HOVER_PROBE_FILE", buffer,
                                                ARRAYSIZE(buffer));
        if (n > 0 && n < ARRAYSIZE(buffer)) {
            hover_probe_file.assign(buffer, n);
            DF_INFO("preview", "hover probe file=%s",
                    core::WideToUtf8(hover_probe_file).c_str());
        }
    }
    int hover_probe_tile = -1;
    double hover_probe_since_read_ms = 1e9;
    double hover_debug_since_ms = 1e9;
    auto read_hover_probe = [&](double delta_ms) {
        hover_probe_since_read_ms += delta_ms;
        if (hover_probe_since_read_ms < 50.0) return;
        hover_probe_since_read_ms = 0.0;
        // Deliberately NOT gated like the command channels. This file holds a
        // level, not an event: the app re-applies it whenever the number differs
        // from the hovered tile, so re-writing the same index is a no-op by
        // design. Priming it with the file's existing content would swallow the
        // probe's next write of that same index, turning a harmless stale hover
        // into a broken test. A stale index here can only open a preview popup.
        const std::string content = core::ReadProbeFileContent(hover_probe_file);
        if (content.empty()) return;
        const int parsed = atoi(content.c_str());
        const int clamped = (parsed >= 0 && parsed < static_cast<int>(tiles.size()))
                                ? parsed
                                : -1;
        if (clamped != hover_probe_tile) {
            DF_DEBUG("preview", "hover probe tile=%d", clamped);
            hover_probe_tile = clamped;
        }
    };

    // Folder-panel probe channel (T-066), the same file-channel pattern as the
    // Launchpad's: RegisterHotKey-style gestures and a physical mouse cannot be
    // driven from a script, so the *source* of the input is replaced while
    // DrillInto/Back/Escape/Close run the exact code the mouse and WM_KEYDOWN
    // take. Commands: `open <utf8 path>`, `drill <utf8 display-name>`, `back`,
    // `esc`, `close`, `hover <row>`.
    std::wstring folder_cmd_file;
    {
        wchar_t buffer[MAX_PATH]{};
        const DWORD n = GetEnvironmentVariableW(L"DOCKFORGE_FOLDER_CMD_FILE", buffer,
                                                ARRAYSIZE(buffer));
        if (n > 0 && n < ARRAYSIZE(buffer)) {
            folder_cmd_file.assign(buffer, n);
            DF_INFO("folder", "probe file=%s", core::WideToUtf8(folder_cmd_file).c_str());
        }
    }
    // T-066 startup hook: open the panel on this path right after the first
    // placement, without a click. Evidence-only, like DOCKFORGE_TASKBAR_CRASH_TEST.
    std::wstring folder_probe_path;
    {
        wchar_t buffer[MAX_PATH * 2]{};
        const DWORD n = GetEnvironmentVariableW(L"DOCKFORGE_FOLDER_PROBE", buffer,
                                                ARRAYSIZE(buffer));
        if (n > 0 && n < ARRAYSIZE(buffer)) {
            folder_probe_path.assign(buffer, n);
            DF_INFO("folder", "probe auto-open=%s", core::WideToUtf8(folder_probe_path).c_str());
        }
    }
    bool folder_probe_opened = false;
    int folder_probe_tile_click = -1;  // consumed by the tick, runs ActivateTile
    core::ProbeCommandGate folder_gate;
    folder_gate.Prime(core::ReadProbeFileContent(folder_cmd_file));
    double folder_cmd_since_read_ms = 1e9;
    auto read_folder_command = [&](double delta_ms) {
        folder_cmd_since_read_ms += delta_ms;
        if (folder_cmd_since_read_ms < 50.0) return;
        folder_cmd_since_read_ms = 0.0;
        const std::string command = core::ReadProbeFileContent(folder_cmd_file);
        if (!folder_gate.Accept(command)) return;
        DF_INFO("folder", "probe command=%s", command.c_str());

        if (command == "back") {
            folder_panel.Back();
        } else if (command == "esc") {
            folder_panel.Escape();
        } else if (command == "close") {
            folder_panel.Close();
        } else if (command.rfind("open ", 0) == 0) {
            const std::wstring path = core::Utf8ToWide(std::string(command.c_str() + 5));
            RECT win_rect{};
            GetWindowRect(window.Handle(), &win_rect);
            const float row_scale = window.Scale() > 0.0f ? window.Scale() : 1.0f;
            const int plate_top =
                win_rect.bottom - static_cast<int>(geometry.plate_height_dip * row_scale);
            folder_panel.Open(instance, path, (win_rect.left + win_rect.right) / 2,
                              plate_top, static_cast<float>(window.Dpi()));
        } else if (command.rfind("drill ", 0) == 0) {
            const std::string name_utf8 = command.c_str() + 6;
            const std::wstring name = core::Utf8ToWide(name_utf8);
            // Match by display name among the visible rows, then drill the way
            // a real click does: ActivateRow's folder arm calls DrillInto with
            // the row's own full path; the probe re-derives that same path from
            // current folder + display name, which is what the shell displayed.
            bool matched = false;
            for (int i = 0; i < folder_panel.RowCount() && !matched; ++i) {
                if (folder_panel.RowName(i) == name) {
                    std::wstring child = folder_panel.CurrentFolder();
                    if (!child.empty() && child.back() != L'\\') child += L'\\';
                    child += name;
                    matched = folder_panel.DrillInto(child);
                }
            }
            if (!matched) {
                DF_WARN("folder", "probe drill: no row named %s", name_utf8.c_str());
            }
        } else if (command.rfind("hover ", 0) == 0) {
            // Paint-level only: the hint strip and row highlight read the
            // hovered row; SetHoveredRowForProbe is the state WM_MOUSEMOVE
            // would leave behind.
            const int row = atoi(command.c_str() + 6);
            folder_panel.SetHoveredRowForProbe(row);
            DF_INFO("folder", "probe hover row=%d", row);
        } else if (command.rfind("tile ", 0) == 0) {
            // The `tile <index>` command runs ActivateTile - the exact function
            // a physical dock click's release arm runs - on the next tick (it
            // is not captured here because it is defined later in the file).
            const int index = atoi(command.c_str() + 5);
            if (index >= 0 && index < static_cast<int>(tiles.size())) {
                folder_probe_tile_click = index;
            } else {
                DF_WARN("folder", "probe tile: index %d out of range (%zu tiles)",
                        index, tiles.size());
            }
        }
    };

    // ---- hardware control panel (T-068, C-23) ----------------------------
    // The Finder Bar itself is M2/TODO, so the panel is a top-anchored popup
    // gated behind DOCKFORGE_HW_PANEL (default off, same rule as every
    // system-visible feature). Audio goes direct COM (Core Audio, spike
    // audio_spike.cpp); media + radios go through the .NET sidecar exe
    // (MinGW cannot complete WinRT async, spike winrt_mingw.cpp). The sidecar
    // lives exactly as long as the panel is open.
    const bool hw_panel_enabled = ReadEnvFlag(L"DOCKFORGE_HW_PANEL", false);
    finderbar::AudioHardware hw_audio([&](const finderbar::VolumeChanged& e) {
        bus.Publish(e);
    });
    finderbar::MediaSidecarBridge hw_sidecar;
    finderbar::SystemStatsSampler hw_stats;
    finderbar::HardwarePanel hw_panel(&hw_audio, &hw_sidecar,
                                      [&] { return hw_stats.Update(); });
    bool hw_panel_ready = false;
    if (hw_panel_enabled) {
        if (finderbar::HardwarePanel::RegisterWindowClass(instance)) {
            // COM on the OLE-STA main thread (already initialized above); the
            // endpoint change callbacks arrive here while the loop pumps.
            if (!hw_audio.Initialize()) {
                DF_WARN("finderbar", "Core Audio init failed; audio controls unavailable");
            }
            if (hw_panel.Create(instance)) {
                // Route the audio thread's notifications into the panel's
                // WndProc (see AudioHardware::SetEventSink for why the
                // callback itself cannot do the work).
                hw_audio.SetEventSink(hw_panel.Handle());
                hw_panel_ready = true;
                if (ReadEnvFlag(L"DOCKFORGE_HW_PANEL_SHOW", false)) {
                    hw_sidecar.Start();
                    hw_panel.Open();
                }
                DF_INFO("finderbar", "hardware panel ready (window deferred until show)");
            } else {
                DF_WARN("finderbar", "hardware panel window creation failed");
            }
        } else {
            DF_WARN("finderbar", "hardware panel class registration failed");
        }
    }
    // C-23 event visibility: every hardware event lands in the log, which is
    // what the evidence probe asserts on (the same pattern the settings window
    // uses for ConfigChanged). The second subscription refreshes the panel -
    // synchronous dispatch means the repaint happens before Publish returns.
    bus.Subscribe<finderbar::VolumeChanged>([](const finderbar::VolumeChanged& e) {
        DF_INFO("finderbar", "VolumeChanged session=%s level=%.3f muted=%d",
                core::WideToUtf8(e.session_id).c_str(), static_cast<double>(e.level),
                e.muted ? 1 : 0);
    });
    bus.Subscribe<finderbar::VolumeChanged>([&](const finderbar::VolumeChanged&) {
        hw_panel.OnAudioChanged();
    });

    // Probe channel (settings/folder pattern). Commands: show, hide, media
    // <action>, volume <0..100>, mute <0|1>, radio <kind> <0|1>, session
    // <index> <0..100>, state (writes DOCKFORGE_HW_PANEL_STATE_FILE).
    std::wstring hw_cmd_file;
    std::wstring hw_state_file;
    if (hw_panel_ready) {
        wchar_t buffer[MAX_PATH]{};
        DWORD n = GetEnvironmentVariableW(L"DOCKFORGE_HW_PANEL_CMD_FILE", buffer,
                                          ARRAYSIZE(buffer));
        if (n > 0 && n < ARRAYSIZE(buffer)) hw_cmd_file.assign(buffer, n);
        n = GetEnvironmentVariableW(L"DOCKFORGE_HW_PANEL_STATE_FILE", buffer,
                                    ARRAYSIZE(buffer));
        if (n > 0 && n < ARRAYSIZE(buffer)) hw_state_file.assign(buffer, n);
        DF_INFO("finderbar", "probe cmd=%s state=%s",
                hw_cmd_file.empty() ? "<none>" : core::WideToUtf8(hw_cmd_file).c_str(),
                hw_state_file.empty() ? "<none>" : core::WideToUtf8(hw_state_file).c_str());
    }
    auto write_hw_state_file = [&]() {
        if (hw_state_file.empty()) return;
        const std::string json = hw_panel.SnapshotJson() + "\n";
        HANDLE h = CreateFileW(hw_state_file.c_str(), GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(h, json.data(), static_cast<DWORD>(json.size()), &written, nullptr);
        CloseHandle(h);
    };
    core::ProbeCommandGate hw_gate;
    hw_gate.Prime(core::ReadProbeFileContent(hw_cmd_file));
    double hw_cmd_since_read_ms = 1e9;
    auto read_hw_command = [&](double delta_ms) {
        hw_cmd_since_read_ms += delta_ms;
        if (hw_cmd_since_read_ms < 50.0) return;
        hw_cmd_since_read_ms = 0.0;
        if (hw_cmd_file.empty()) return;
        const std::string content = core::ReadProbeFileContent(hw_cmd_file);
        // Dedup on the FULL file content: the probe appends a unique token
        // line after the command so repeated identical commands (e.g. `state`
        // five times) are all delivered; only a byte-identical rewrite of the
        // whole file is a true repeat.
        if (!hw_gate.Accept(content)) return;
        const std::string command = core::ProbeCommandOf(content);
        if (command.empty()) return;
        DF_INFO("finderbar", "probe command=%s", command.c_str());
        auto report = [&](const finderbar::HardwareControlError& err) {
            if (!err.ok()) {
                DF_WARN("finderbar", "control failed: %s hr=0x%08lX",
                        core::WideToUtf8(err.message).c_str(), err.hresult);
            }
        };
        if (command == "show") {
            if (!hw_sidecar.running()) hw_sidecar.Start();
            hw_panel.Open();
        } else if (command == "hide") {
            hw_panel.Close();
            hw_sidecar.Stop();
        } else if (command == "state") {
            write_hw_state_file();
        } else if (command.rfind("media ", 0) == 0) {
            report(hw_panel.MediaCommand(command.c_str() + 6));
        } else if (command.rfind("volume ", 0) == 0) {
            const int pct = atoi(command.c_str() + 7);
            report(hw_panel.SetMasterVolume(static_cast<float>(pct) / 100.0f));
        } else if (command.rfind("mute ", 0) == 0) {
            report(hw_panel.ToggleMasterMute());
        } else if (command.rfind("radio ", 0) == 0) {
            int kind = 0, on = 0;
            if (sscanf(command.c_str() + 6, "%d %d", &kind, &on) == 2) {
                report(hw_panel.SetRadio(kind, on != 0));
            }
        } else if (command.rfind("session ", 0) == 0) {
            int index = 0, pct = 0;
            if (sscanf(command.c_str() + 8, "%d %d", &index, &pct) == 2) {
                report(hw_panel.SetSessionVolume(index,
                                                 static_cast<float>(pct) / 100.0f));
            }
        }
        write_hw_state_file();
    };

    auto repaint = [&] {
        const float dpi = static_cast<float>(window.Dpi());
        const std::int64_t paint_start = core::QueryPerformanceTicks();
        if (!surface.Paint(dpi, [&](ID2D1DeviceContext& dc,
                                    const platform::CompositionSurface::PaintInfo& info) {
                render_backend->BeginFrame(dc, dpi);
                dock::DockPaintInput input;
                input.config = &config;
                input.style = &plate_style;
                input.tiles = &tiles;
                input.backdrop = backdrop_w > 0 ? backdrop_pixels.data() : nullptr;
                input.backdrop_w = backdrop_w;
                input.backdrop_h = backdrop_h;
                input.dpi = dpi;
                input.geometry = geometry;
                input.hover = dock_hover;
                // Frozen-frame-friendly animation clock for the indeterminate
                // progress pulse (T-067); see the tick handler.
                input.clock_ms = dock_clock_ms;
                // Non-null only while a drag is in flight: the painter draws the
                // ghost and the insertion line, and skips the source slot. Passing
                // the controller's own state (not a copy) keeps the ghost and the
                // insertion slot from disagreeing mid-gesture.
                input.drag = drag.Active() ? &drag.State() : nullptr;
                input.drop = drop_pointer.active ? &drop_pointer : nullptr;
                // Weather snapshot (T-075): handed to the painter only when the
                // fixture tile is on the row; a missing pointer paints the
                // placeholder inside the weather branch.
                input.weather = weather_tile_index() >= 0 ? &weather_snapshot : nullptr;
                if (!dock_painter.Paint(dc, *render_backend, plate, input)) {
                    DF_WARN("dock", "dock paint failed at %.0f dpi", dpi);
                }
                render_backend->EndFrame();
            })) {
            return false;
        }
        const bool committed = surface.Commit();
        // Paint cost is measured separately from frame interval. The frame clock
        // only says a deadline was missed; this says whether the miss was ours
        // (drawing) or the compositor's (present), and those need different fixes.
        paint_clock.AddSample(core::QpcToMilliseconds(core::QueryPerformanceTicks() - paint_start));
        return committed;
    };

    // ---- clickable region -----------------------------------------------
    // The window is always tall enough for a fully magnified icon, so the band
    // above the plate is transparent dead space. It must not swallow clicks meant
    // for the window underneath. HTTRANSPARENT does not cross processes
    // (spikes/t010_hover question 2); SetWindowRgn does, and it clips our own
    // composition surface too (question 3).
    bool tracking_mouse = false;

    // Runs the layout for the current hover and returns where the row actually is.
    // Paint and region both come from this one call: SetWindowRgn clips composition
    // content, so a region derived from a stale layout cuts off the magnified icons
    // it was supposed to expose (measured: the row was clipped at the plate top).
    auto layout_now = [&]() -> const dock::LayoutSummary& {
        return dock_painter.UpdateLayout(geometry, dock_hover,
                                         static_cast<int>(tiles.size()));
    };

    // Two regions only: the plate alone, or the plate plus the full-magnification
    // row. Rebuilding the region every ramp frame cost real time - SetWindowRgn
    // forces a DWM round trip, and the hover benchmark measured p99 17.3ms against
    // a 16.7ms budget. A region is a hit-test shape, not an animation: the union
    // over the whole ramp is a single rectangle, so computing it at full
    // magnification covers every intermediate frame without changing per frame.
    bool region_hovered = false;
    // Diagnostic: force the region to the whole window so the reserved band above the
    // plate gets composited. Nothing in normal operation does this while idle, which is
    // why the band's appearance had never been looked at before the drop target started
    // widening the region (T-014).
    const bool full_region_probe = ReadEnvFlag(L"DOCKFORGE_FULL_REGION_PROBE");
    auto apply_region = [&]() {
        const float scale = window.Scale();
        const float width_dip = geometry.window_width_dip;
        const float height_dip = geometry.window_height_dip;
        const float plate_top_dip = height_dip - geometry.plate_height_dip;

        RECT plate{};
        plate.left = 0;
        plate.top = static_cast<LONG>(std::lround(plate_top_dip * scale));
        plate.right = static_cast<LONG>(std::lround(width_dip * scale));
        plate.bottom = static_cast<LONG>(std::lround(height_dip * scale));

        // While magnified, the icons overhang above the plate. Their footprint is
        // taken from the layout engine at full magnification, not from a guess, so a
        // click on a lifted icon lands and a click on empty space beside it does
        // not.
        RECT row{};
        bool include_row = false;
        // During a drag the ghost follows the pointer and the icons push aside, so
        // the window must be fully clickable and fully drawn: a region trimmed to
        // the plate would clip the lifted ghost the moment it leaves the dock, and
        // clipping composition content is exactly what SetWindowRgn does (T-010
        // spike conclusion 3). A drag is a transient state, so paying for a full
        // window region here does not affect the idle path.
        if (drag.Active() || drop_pointer.active || full_region_probe) {
            row.left = 0;
            row.top = 0;
            row.right = plate.right;
            row.bottom = plate.bottom;
            include_row = true;
        } else if (region_hovered) {
            dock::DockHover full;
            full.active = true;
            full.intensity = 1.0f;
            full.mouse_x_dip = dock_hover.mouse_x_dip;
            const dock::LayoutSummary& layout =
                dock_painter.UpdateLayout(geometry, full, static_cast<int>(tiles.size()));
            if (layout.max_extent > layout.min_extent) {
                const float row_origin_dip = (width_dip - geometry.content_width_dip) * 0.5f;
                // Layout extents are device pixels; the region is in device pixels
                // too, so the conversion is a single origin shift.
                row.left = static_cast<LONG>(std::lround(row_origin_dip * scale + layout.min_extent));
                row.right =
                    static_cast<LONG>(std::lround(row_origin_dip * scale + layout.max_extent));
                // Icons are bottom-aligned and grow upward, so the row reaches above
                // the plate by the tallest icon's growth. Taking plate.top here
                // instead would clip the very icons the region exists to expose.
                const float icon_dip = static_cast<float>(std::max(config.dock.icon_size, 1));
                const float row_top_dip =
                    (height_dip - dock::kPlatePaddingDip) - icon_dip * layout.peak_scale;
                row.top = static_cast<LONG>(
                    std::lround(std::min(row_top_dip, plate_top_dip) * scale));
                row.bottom = plate.bottom;
                include_row = row.right > row.left && row.top < plate.top;
            }
        }

        const int radius_px = static_cast<int>(std::lround(
            std::min(static_cast<float>(config.appearance.corner_radius),
                     std::min(width_dip, geometry.plate_height_dip) * 0.5f) *
            scale));
        DF_DEBUG("dock", "region plate=%ld,%ld,%ld,%ld row=%ld,%ld,%ld,%ld include_row=%d",
                 plate.left, plate.top, plate.right, plate.bottom, row.left, row.top,
                 row.right, row.bottom, include_row ? 1 : 0);
        platform::ApplyWindowRegion(
            window.Handle(),
            platform::CreateDockRegion(plate, radius_px, row, include_row));
    };
    apply_region();

    // ---- user shortcuts (T-069, C-24) ----------------------------------
    // Config-driven hotkeys on top of the two built-ins above. Everything
    // defaults to disabled, so a fresh install registers nothing here. The
    // destructor unregisters every held id - the no-residue exit guarantee.
    core::HotkeyHost hotkey_host(window.Handle());
    core::HotspotTracker hotspot_tracker;
    bool hotspots_active = false;

    // Dispatch for both engines (four categories, see core::kActions). Runs on
    // the main thread only: WM_HOTKEY arrives on the registering thread and the
    // hotspot poll lives inside the tick.
    auto RunShortcutAction = [&](const std::string& action) -> bool {
        const core::ActionInfo* info = core::FindAction(action);
        if (info == nullptr) {
            DF_WARN("hotkeys", "action '%s' is not in the table, ignored", action.c_str());
            return false;
        }
        DF_INFO("hotkeys", "action %s (%d) fired", action.c_str(),
                static_cast<int>(info->category));
        switch (info->category) {
            case core::HotkeyCategory::ComponentToggle:
                if (action == "toggle_dock") {
                    ShowWindow(window.Handle(),
                               IsWindowVisible(window.Handle()) ? SW_HIDE : SW_SHOW);
                    return true;
                }
                if (action == "toggle_launchpad") {
                    if (!launchpad_ready) {
                        DF_WARN("hotkeys", "launchpad not ready; toggle_launchpad no-op");
                        return false;
                    }
                    if (launchpad.Visible()) {
                        launchpad.Close();
                    } else {
                        launchpad.Open(config);
                    }
                    return true;
                }
                if (action == "toggle_settings") {
                    if (!settings_ready) {
                        DF_WARN("hotkeys", "settings window unavailable (DOCKFORGE_SETTINGS?)");
                        return false;
                    }
                    if (settings.Visible()) {
                        settings.Hide();
                    } else {
                        settings.Show();  // re-shows after a hide
                    }
                    return true;
                }
                return false;
            case core::HotkeyCategory::SystemControl:
                if (action == "toggle_taskbar") {
                    core::Config updated = config;
                    updated.behavior.hide_taskbar = !config.behavior.hide_taskbar;
                    if (core::SaveConfig(updated)) {
                        config = updated;
                        bus.Publish(core::ConfigChanged{config});
                        return true;
                    }
                    DF_WARN("hotkeys", "toggle_taskbar could not persist config");
                    return false;
                }
                if (action == "restore_taskbar") {
                    // Same body as the built-in Ctrl+Alt+T arm: force-show and
                    // persist hide_taskbar=false so the recovery survives a
                    // restart.
                    std::string detail;
                    const bool ok = core::RestoreTaskbarSystem(&detail);
                    if (config.behavior.hide_taskbar) {
                        core::Config updated = config;
                        updated.behavior.hide_taskbar = false;
                        if (core::SaveConfig(updated)) {
                            config = updated;
                            last_hide_taskbar = false;
                            bus.Publish(core::ConfigChanged{config});
                        }
                    }
                    publish_taskbar_state(true);
                    return ok;
                }
                return false;
            case core::HotkeyCategory::DisplayMode:
                if (action == "cycle_dock_side") {
                    core::Config updated = config;
                    switch (config.dock.position) {
                        case core::DockPosition::Bottom:
                            updated.dock.position = core::DockPosition::Left;
                            break;
                        case core::DockPosition::Left:
                            updated.dock.position = core::DockPosition::Right;
                            break;
                        case core::DockPosition::Right:
                            updated.dock.position = core::DockPosition::Bottom;
                            break;
                    }
                    if (core::SaveConfig(updated)) {
                        config = updated;
                        // apply_placement re-reads the edge from the config it
                        // captures, so the move itself is one call.
                        apply_placement();
                        apply_region();
                        bus.Publish(core::ConfigChanged{config});
                        return true;
                    }
                    return false;
                }
                if (action == "toggle_magnify") {
                    core::Config updated = config;
                    // 1.8f is the C-01 default; the toggle is between "off"
                    // (1.0) and that default, not between 1.0 and whatever a
                    // theme lens last applied.
                    updated.dock.magnification =
                        config.dock.magnification > 1.0f ? 1.0f : 1.8f;
                    if (core::SaveConfig(updated)) {
                        config = updated;
                        apply_region();
                        bus.Publish(core::ConfigChanged{config});
                        return true;
                    }
                    return false;
                }
                return false;
            case core::HotkeyCategory::WindowManagement:
                if (action == "minimize_all") {
                    core::MinimizeAllWindows(window.Handle(), nullptr);
                    return true;
                }
                if (action == "minimize_foreground") {
                    const HWND foreground = GetForegroundWindow();
                    if (foreground != nullptr && foreground != window.Handle()) {
                        ShowWindow(foreground, SW_MINIMIZE);
                        return true;
                    }
                    return false;
                }
                return false;
        }
        return false;
    };

    // One place applies the shortcut config: startup and every ConfigChanged
    // where the lists moved. `applied_*` snapshots what the engines actually
    // hold - the comparison CANNOT be against `config`, because the hot-reload
    // path adopts the new snapshot into `config` BEFORE publishing it (the
    // event then equals the local copy and a diff against it is always empty).
    std::vector<core::HotkeyConfig> applied_hotkeys;
    std::vector<core::HotspotConfig> applied_hotspots;
    auto ApplyShortcuts = [&]() {
        const std::vector<core::HotkeyHost::Result> results = hotkey_host.Apply(config.hotkeys);
        bool any_enabled_hotspot = false;
        for (const core::HotspotConfig& hotspot : config.hotspots) {
            if (hotspot.enabled) any_enabled_hotspot = true;
        }
        hotspots_active = any_enabled_hotspot;
        if (!hotspots_active) hotspot_tracker.Reset();
        applied_hotkeys = config.hotkeys;
        applied_hotspots = config.hotspots;

        // Conflict surface for the settings window (DoD: 提示"已被其他程序占用").
        // The window may not exist yet/any more - SetShortcutsStatus keeps the
        // text for the next CreateControls.
        std::string status;
        for (const core::HotkeyHost::Result& result : results) {
            if (result.ok) continue;
            if (result.already_registered) {
                if (!status.empty()) status += "\n";
                status += "Hotkey '" + result.id + "': 已被其他程序占用 (err=1409)";
            } else {
                if (!status.empty()) status += "\n";
                status += "Hotkey '" + result.id + "': registration failed (err=" +
                          std::to_string(result.error) + ")";
            }
        }
        if (settings_ready) settings.SetShortcutsStatus(status);
    };
    ApplyShortcuts();

    // Dock refresh on ConfigChanged (T-051/T-052): the settings window's flush
    // and the hot reload both publish synchronously inside the tick, so this
    // handler runs on the frame path. It re-applies the theme lens (the event
    // carries the base snapshot) and rebuilds the click region; the per-frame
    // layout reads `config` directly, so everything else follows on the next
    // frame with no extra work.
    bus.Subscribe<core::ConfigChanged>([&](const core::ConfigChanged& event) {
        std::string theme_error;
        if (!core::LoadThemeInto(event.config.appearance.theme, config, &theme_error)) {
            DF_WARN("core", "theme '%s' unavailable; base values in effect",
                    event.config.appearance.theme.c_str());
        }
        // T-065: the settings switches and a hot reload both land here. Turning
        // an adapter off reports 0 once (SetEnabled), turning it on re-initializes
        // on the next Update; the repaint follows from the count change.
        ApplyMessageCountConfig(event.config, message_counters);
        apply_message_counts();
        apply_region();
    });

    // T-069: shortcut re-apply. The settings flush and the hot reload both
    // publish the full snapshot; only a real change against what the engines
    // already hold does any work. The main-thread `config` copy adopts the
    // lists here so the poll and the dispatch read the fresh entries on the
    // next frame.
    bus.Subscribe<core::ConfigChanged>([&](const core::ConfigChanged& event) {
        if (core::SameHotkeys(applied_hotkeys, event.config.hotkeys) &&
            core::SameHotspots(applied_hotspots, event.config.hotspots)) {
            return;
        }
        config.hotkeys = event.config.hotkeys;
        config.hotspots = event.config.hotspots;
        ApplyShortcuts();
    });

    // T-075: weather re-apply. The settings flush and the hot reload both
    // land here; only a real change against the running config acts. Turning
    // the widget on/off appends/removes the fixture tile and re-derives the
    // geometry; a city/interval edit reconfigures the host in place (a
    // changed city clears the host's geocode cache, an unchanged one keeps
    // it, so an interval edit never re-runs the geocoder).
    bus.Subscribe<core::ConfigChanged>([&](const core::ConfigChanged& event) {
        if (core::SameWeather(config.weather, event.config.weather)) return;
        config.weather = event.config.weather;
        const bool want = config.weather.enabled && !config.weather.city.empty();
        if (SetWeatherTile(want)) {
            apply_placement();
            apply_region();
            InvalidateRect(window.Handle(), nullptr, FALSE);
        }
        if (want && !weather_host_started) {
            weather_host.SetFetcher(dock::weather::DefaultFetcher());
            weather_host.Start(config.weather);
            weather_host_started = true;
        } else if (want) {
            weather_host.Reconfigure(config.weather);
        } else if (weather_host_started) {
            weather_host.Stop();
            weather_host_started = false;
            weather_popup.Hide();
        }
        DF_INFO("weather", "config applied: enabled=%d city='%s' refresh=%dmin",
                config.weather.enabled ? 1 : 0, config.weather.city.c_str(),
                config.weather.refresh_minutes);
    });

    // T-076: notification centre re-apply. Same rule as weather: only a real
    // change against the running config acts. The sidecar is only started when
    // the master switch flips on, so an interval edit never spawns a process.
    bus.Subscribe<core::ConfigChanged>([&](const core::ConfigChanged& event) {
        if (core::SameNotifications(config.notifications, event.config.notifications)) return;
        const bool was_enabled = config.notifications.enabled;
        config.notifications = event.config.notifications;
        // Recompute from the same expression the startup block uses: the env
        // flag still wins, so a probe that forced the centre on is not turned
        // off by a config publish that carries the default (false).
        notif_enabled =
            notif_ready && ReadEnvFlag(L"DOCKFORGE_NOTIFICATIONS", config.notifications.enabled);
        notif_banner.model().SetTimeoutSeconds(config.notifications.banner_timeout_seconds);
        if (!notif_ready) return;
        if (notif_enabled && !was_enabled && !notif_sidecar.running()) {
            if (notif_sidecar.Start(notif_sidecar_path)) {
                DF_INFO("notifications", "centre started pid=%lu", notif_sidecar.pid());
            } else {
                DF_WARN("notifications", "sidecar did not start after config change");
            }
        } else if (!config.notifications.enabled && notif_sidecar.running()) {
            // Stop() closes stdin and the child exits on its own; the store is
            // kept so the history the user was reading does not vanish the
            // moment they un-tick the switch.
            notif_sidecar.Stop();
            notif_banner.model().Clear();
            notif_banner.Hide();
            notif_panel.Close();
            DF_INFO("notifications", "centre stopped");
        }
        DF_INFO("notifications",
                "config applied: enabled=%d banner=%d pulse=%d timeout=%ds poll=%ds",
                config.notifications.enabled ? 1 : 0,
                config.notifications.banner_enabled ? 1 : 0,
                config.notifications.pulse_icons ? 1 : 0,
                config.notifications.banner_timeout_seconds,
                config.notifications.poll_seconds);
    });

    // ---- OLE drop target (T-014) ----------------------------------------
    // The sink is a set of std::functions rather than a subclass because every piece
    // of state it needs is a local in this function, and threading a subclass through
    // them would mean either moving the dock's state into a class or handing the class
    // pointers to it. Both are larger changes than the feature.
    dock::DropSink drop_sink;

    // Adds one resolved path as a tile at `insert_index`.
    //
    // Returns false when the path is already on the dock: dropping a shortcut that is
    // already pinned should move it, not duplicate it. Duplicates would also break the
    // identity contract (C-15a) - two tiles with the same key, and a config list that
    // cannot say which one was removed.
    auto InsertDroppedPath = [&](const std::wstring& path, int insert_index) -> bool {
        shell::AppEntry entry;
        const shell::ResolveError error = shell::ResolveDroppedPath(path, entry);
        if (error != shell::ResolveError::None) {
            DF_WARN("dock", "drop rejected %s: %s", core::WideToUtf8(path).c_str(),
                    shell::ToString(error));
            return false;
        }
        const std::string identity = dock::IdentityOf(entry);
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            if (dock::IdentityOf(tiles[i].app) != identity) continue;
            DF_INFO("dock", "drop: %s is already on the dock at tile[%zu]", identity.c_str(), i);
            const int from = static_cast<int>(i);
            const int to = std::clamp(insert_index, 0, static_cast<int>(tiles.size()) - 1);
            if (from != to) {
                dock::MoveTile(tiles, from, to);
                dock::CommitOrder(config, tiles, std::string());
            }
            return true;
        }

        // Icon extraction is a shell round trip (60-190ms cold, evidence/T-006). It
        // runs here, on the drop, because the alternative is a tile that paints a
        // placeholder ring for a frame and then pops an icon in - and because a drop is
        // already a user-visible pause.
        dock::DockTile tile;
        tile.app = std::move(entry);
        const std::wstring source = shell::IconSourceFor(tile.app);
        if (!icons.Get(source, kDockIconPx, tile.icon)) {
            DF_WARN("dock", "drop: no icon for %s", core::WideToUtf8(source).c_str());
        }
        tile.icon_key = dock::IconKeyFor(source);

        const int at = std::clamp(insert_index, 0, static_cast<int>(tiles.size()));
        tiles.insert(tiles.begin() + at, std::move(tile));
        // Dropping onto a dock that was auto-populated is the moment it becomes
        // curated: CommitOrder writes the whole visible order, so the arrangement the
        // user is looking at is the one that survives the restart.
        dock::CommitOrder(config, tiles, std::string());
        // A brand-new tile starts at count 0; seed it from the host so a live
        // badge does not blink off for one poll interval.
        apply_message_counts();
        DF_INFO("dock", "drop: added %s at tile[%d] (%zu tiles)", identity.c_str(), at,
                tiles.size());
        return true;
    };

    dock::DockDropTarget drop_target(window.Handle(), &drop_sink);

    // Whether the window region currently covers the whole window because of a drop.
    // Tracked separately from drop_pointer because DragOver fires dozens of times a
    // second and rebuilding the region on each one is the "SetWindowRgn every frame"
    // antipattern that cost 0.6ms of p99 in T-010. The region only has to change on the
    // two transitions.
    bool region_drop = false;

    drop_sink.hover = [&](const dock::DropPointer& pointer) {
        // DragOver fires dozens of times a second - far faster than the dock can paint.
        // Repainting per callback is the same antipattern that cost 18.2ms of p99 in
        // T-010 (one WM_MOUSEMOVE, one InvalidateRect); the callback only records the
        // new pointer and the tick turns it into at most one repaint per frame.
        drop_pointer = pointer;
        drop_moved = true;
        // The row settles while an external drag is over the dock. Two reasons: the
        // magnified row moves icons under the pointer, and the insertion slot is
        // resolved against the rest grid, so a magnified row would put the caret
        // between icons that are not where the caret says they are. Hover intensity
        // decays through the normal ramp, so this reads as the dock relaxing, not as a
        // snap.
        dock_hover.active = false;
        // The region widens on entry: a drag that entered over the plate would
        // otherwise lose the pointer the moment it rose into the reserved band, and
        // OLE reports that as DragLeave (measured, evidence/T-014/run_shrink_noexpand).
        if (!region_drop) {
            region_drop = true;
            apply_region();
        }
    };
    drop_sink.leave = [&]() {
        drop_pointer = dock::DropPointer{};
        region_drop = false;
        apply_region();
        drop_moved = true;
    };
    drop_sink.commit = [&](const std::vector<std::wstring>& paths, int insert_index) {
        int added = 0;
        // Inserted in reverse so the first path ends up first in the row: each insert
        // shifts the ones after it right.
        for (auto it = paths.rbegin(); it != paths.rend(); ++it) {
            if (InsertDroppedPath(*it, insert_index)) ++added;
        }
        drop_pointer = dock::DropPointer{};
        region_drop = false;
        DF_INFO("dock", "drop: %d of %zu paths accepted", added, paths.size());
        // The window width depends on the tile count, so adding a tile is a re-place,
        // not just a repaint. Same reason as removal (T-013).
        apply_placement();
        apply_region();
        InvalidateRect(window.Handle(), nullptr, FALSE);
    };

    if (const HRESULT hr = drop_target.Register(); SUCCEEDED(hr)) {
        // The rest grid, not the magnified one: an external drag settles the row (see
        // the hover suppression in WM_MOUSEMOVE), and the caret must agree with where
        // the icons actually are.
        auto refresh_drop_grid = [&] {
            const float row_scale = window.Scale() > 0.0f ? window.Scale() : 1.0f;
            const auto centres = dock::IdleCentersDip(geometry, row_scale);
            drop_target.SetScale(row_scale);
            drop_target.SetCentres(centres);
            // The first and last centre pin down the grid the drop slot is resolved
            // against. Without them a slot that is off by a fixed pitch is
            // indistinguishable from a pointer that arrived in the wrong place.
            DF_DEBUG("dock", "drop grid scale=%.3f slots=%zu first=%.1f last=%.1f dip",
                     static_cast<double>(row_scale), centres.size(),
                     centres.empty() ? 0.0 : static_cast<double>(centres.front()),
                     centres.empty() ? 0.0 : static_cast<double>(centres.back()));
        };
        refresh_drop_grid();
        geometry_changed = refresh_drop_grid;
        DF_INFO("dock", "drop target registered (%zu slots, scale=%.3f)", tiles.size(),
                static_cast<double>(window.Scale()));
    } else {
        DF_ERROR("dock", "RegisterDragDrop failed: 0x%08lX", static_cast<unsigned long>(hr));
    }

    // One tile's click action, factored out because it now runs on button release
    // rather than on press: the press may become a drag, and only the release knows
    // which it was.
    auto ActivateTile = [&](int index) {
        if (index < 0 || index >= static_cast<int>(tiles.size())) return;
        const shell::AppEntry& app = tiles[static_cast<std::size_t>(index)].app;

        // Weather tile (T-075): there is no app to activate - a click IS the
        // manual refresh the DoD asks for. The fetch itself still happens on
        // the worker thread; RequestRefresh only wakes it.
        if (tiles[static_cast<std::size_t>(index)].weather) {
            if (weather_host_started) {
                weather_host.RequestRefresh();
                DF_INFO("weather", "manual refresh requested (tile click)");
            } else {
                DF_WARN("weather", "manual refresh requested but host is not running");
            }
            return;
        }

        // Folder tiles (T-066, ADR-0023): a left click opens the drill-through
        // panel above the dock instead of launching Explorer. Second click on
        // the same folder toggles the panel closed. The unmagnified slot centre
        // is the anchor - the panel pops over the tile the user pinned, not
        // over wherever the magnified row happened to be.
        if (folder_panel_class_ready && app.kind == shell::AppKind::Folder &&
            !app.target_path.empty()) {
            if (folder_panel.Visible() &&
                folder_panel.RootFolder() == app.target_path) {
                folder_panel.Close();
                running_tracker.Invalidate();
                return;
            }
            const float row_scale = window.Scale() > 0.0f ? window.Scale() : 1.0f;
            const auto centres = dock::IdleCentersDip(geometry, row_scale);
            RECT win_rect{};
            GetWindowRect(window.Handle(), &win_rect);
            const int anchor_x = centres.empty()
                                     ? (win_rect.left + win_rect.right) / 2
                                     : win_rect.left +
                                           static_cast<int>(centres[static_cast<
                                               std::size_t>(index)] * row_scale);
            // The panel sits just above the plate's top edge. The window is
            // taller than the plate (the magnified row needs headroom), so the
            // anchor is derived from the same geometry the region uses.
            const int plate_top_px =
                win_rect.bottom -
                static_cast<int>(geometry.plate_height_dip * row_scale);
            if (folder_panel.Open(instance, app.target_path, anchor_x, plate_top_px,
                                  window.Dpi())) {
                DF_INFO("dock", "folder panel opened for tile[%d] %s", index,
                        core::WideToUtf8(app.display_name).c_str());
            } else {
                DF_WARN("dock", "folder panel open failed; launching folder instead");
                const shell::ActionResult result = shell::ActivateOrMinimizeTimed(app);
                DF_INFO("dock", "tile[%d] fallback -> %s", index,
                        shell::ToString(result.action));
            }
            running_tracker.Invalidate();
            return;
        }

        // Enumerate once per click rather than caching: the window set changes
        // without notice (apps close, dialogs open) and EnumWindows costs ~1ms, which
        // is affordable on a click and not on a frame.
        const shell::ActionResult result = shell::ActivateOrMinimizeTimed(app);
        DF_INFO("dock", "tile[%d] %s -> %s (%s, pid=%lu, %.2fms)", index,
                core::WideToUtf8(app.display_name).c_str(), shell::ToString(result.action),
                result.route, static_cast<unsigned long>(result.process_id),
                result.elapsed_ms);
        // Split out because the two halves have different fixes: enumeration scales
        // with the desktop, the action is one window call. Both are needed to keep
        // the click path inside the 16.7ms frame budget.
        DF_DEBUG("dock", "click cost: enumerate=%.2fms total=%.2fms", result.enumerate_ms,
                 result.elapsed_ms);
        // Force the next tick to re-enumerate so the new window's indicator appears
        // immediately instead of up to 300ms later. Without this a launch looks like
        // it failed for a quarter second.
        running_tracker.Invalidate();
    };

    // T-066 verification hook: pre-seed a folder tile exactly the way an OLE
    // drop would (ResolveDroppedPath -> CommitOrder), because no script can
    // perform a real drag onto the dock. The probe then clicks the tile through
    // the `tile` command, which runs THIS ActivateTile - the same function the
    // WM_LBUTTONUP release arm runs.
    {
        wchar_t buffer[MAX_PATH * 2]{};
        const DWORD n = GetEnvironmentVariableW(L"DOCKFORGE_FOLDER_TILE", buffer,
                                                ARRAYSIZE(buffer));
        if (n > 0 && n < ARRAYSIZE(buffer)) {
            const std::wstring path(buffer, n);
            const int at = static_cast<int>(tiles.size());
            if (InsertDroppedPath(path, at)) {
                apply_placement();
                apply_region();
                InvalidateRect(window.Handle(), nullptr, FALSE);
                DF_INFO("dock", "DOCKFORGE_FOLDER_TILE seeded a folder tile at [%d]",
                        at);
            } else {
                DF_WARN("dock", "DOCKFORGE_FOLDER_TILE %s was not accepted",
                        core::WideToUtf8(path).c_str());
            }
        }
    }

    // Hides the preview popup and releases every DWM registration.
    //
    // The order is not negotiable. DWM renders into the destination window's
    // redirection surface, so a registration that outlives its destination is a
    // dangling reference inside the desktop compositor; and a registration that
    // outlives the SOURCE is a thumbnail of a dead window, which reads as a black
    // rectangle. Releasing first, hiding second, keeps both invariants.
    //
    // The popup HWND itself survives so the next preview does not pay for window
    // creation again - only the registrations are expensive to redo.
    auto HidePreview = [&] {
        if (!preview_visible && !thumbnails_pending) return;
        thumbnails.ReleaseAll();
        thumbnails_pending = false;
        preview.Hide();
        preview_visible = false;
        preview_tile = -1;
        hover_timer.Reset();
        DF_DEBUG("preview", "hidden");
    };

    core::AppLoop loop;
    loop.SetTargetIntervalMs(16);  // 60 FPS target (PERFORMANCE.md)

    // Verification hook for the T-061 crash path (evidence only, never a
    // product feature): DOCKFORGE_TASKBAR_CRASH_TEST=1 arms a countdown that
    // dereferences a null pointer 3s after startup, producing a genuine
    // unhandled exception AFTER the taskbar was hidden. The probe uses this to
    // prove SetUnhandledExceptionFilter restores the taskbar; there is no
    // other honest way to exercise a crash path on demand.
    const bool crash_test_armed = ReadEnvFlag(L"DOCKFORGE_TASKBAR_CRASH_TEST");
    double crash_test_remaining_ms = 3000.0;
    if (crash_test_armed) {
        DF_WARN("app", "crash test armed: null dereference in 3000ms "
                       "(DOCKFORGE_TASKBAR_CRASH_TEST)");
    }

    // Pull a fresh backdrop at most once per frame, and only when the worker
    // published something new. At 60 FPS with a 200ms capture interval that skips
    // the pixel copy on ~11 of every 12 frames.
    loop.SetTickHandler([&](double delta_ms) {
        bool dirty = false;
        if (crash_test_armed) {
            crash_test_remaining_ms -= delta_ms;
            if (crash_test_remaining_ms <= 0.0) {
                DF_ERROR("app", "crash test firing: dereferencing null now");
                // A volatile store through a null pointer cannot be optimized
                // away and always raises EXCEPTION_ACCESS_VIOLATION, which
                // lands in the T-061 unhandled-exception filter.
                volatile int* bomb = static_cast<volatile int*>(nullptr);
                *bomb = 0x42;
            }
        }
        if (capture_active) {
            if (capture.AcquireIfNew(backdrop_generation, backdrop_pixels, backdrop_w,
                                     backdrop_h, backdrop_generation)) {
                dirty = true;
            }
        }
        // Running indicators: the tracker polls existence on its own interval and
        // reads the foreground window every frame, so it returns true exactly when
        // the dots would look different. A false return costs one GetForegroundWindow
        // and keeps the dock asleep.
        // Message badges (T-065): the host throttles each adapter to its poll
        // interval, so Update() returns true only when some count actually moved
        // (including to 0 when a target exits or the switch turns off). A false
        // return costs one throttle check and keeps the dock asleep.
        if (message_counters.Update()) {
            apply_message_counts();
            dirty = true;
        }
        // Folder panel (T-066). The watcher restarts per level and its 500ms
        // debounce settles bursts; a settled change re-enumerates and repaints
        // the panel itself. The probe channel is a no-op without the env var.
        if (!folder_probe_path.empty() && !folder_probe_opened &&
            window.Handle() != nullptr && IsWindowVisible(window.Handle())) {
            folder_probe_opened = true;
            RECT win_rect{};
            GetWindowRect(window.Handle(), &win_rect);
            const float row_scale = window.Scale() > 0.0f ? window.Scale() : 1.0f;
            const int plate_top =
                win_rect.bottom - static_cast<int>(geometry.plate_height_dip * row_scale);
            folder_panel.Open(instance, folder_probe_path,
                              (win_rect.left + win_rect.right) / 2, plate_top,
                              static_cast<float>(window.Dpi()));
        }
        (void)folder_panel.TickWatcher();
        read_folder_command(delta_ms);
        if (folder_probe_tile_click >= 0) {
            const int index = folder_probe_tile_click;
            folder_probe_tile_click = -1;
            ActivateTile(index);
        }
        // Hardware panel (T-068): the panel self-throttles its 2s sampling
        // inside Tick; hidden costs nothing. The probe reader piggybacks the
        // same 50ms cadence as every other file channel.
        if (hw_panel_ready) {
            hw_panel.Tick(delta_ms);
            read_hw_command(delta_ms);
        }
        // Weather (T-075): the countdown to the next automatic refresh runs
        // here, and a published snapshot is adopted at most once per frame.
        // The fetch itself is on the host's worker thread; this branch only
        // ever copies - the C-04b capture rule (no pointers across threads).
        if (weather_host_started && weather_tile_index() >= 0) {
            if (weather_host.Tick(delta_ms, weather_snapshot)) {
                if (weather_snapshot.failed) {
                    DF_WARN("weather", "refresh failed: %s (showing placeholder)",
                            weather_snapshot.error.c_str());
                } else {
                    DF_INFO("weather",
                            "refreshed city=%s temp=%.0fC kind=%s days=%zu updated=%s",
                            weather_snapshot.city.c_str(), weather_snapshot.temp_c,
                            dock::weather::ToString(weather_snapshot.kind),
                            weather_snapshot.days.size(),
                            weather_snapshot.updated_at.c_str());
                }
                dirty = true;
                // A visible forecast popup restyles with the new data.
                if (weather_popup.Visible()) {
                    SYSTEMTIME now{};
                    GetLocalTime(&now);
                    char today[16] = {};
                    std::snprintf(today, sizeof(today), "%04d-%02d-%02d", now.wYear, now.wMonth,
                                  now.wDay);
                    const float popup_scale =
                        window.Scale() > 0.0f ? window.Scale() : 1.0f;
                    const auto centres = dock::IdleCentersDip(geometry, popup_scale);
                    const int index = weather_tile_index();
                    RECT win_rect{};
                    GetWindowRect(window.Handle(), &win_rect);
                    const int anchor_x =
                        win_rect.left +
                        static_cast<int>(centres[static_cast<std::size_t>(
                                             std::clamp(index, 0,
                                                         static_cast<int>(centres.size()) - 1))] *
                                         popup_scale);
                    const int plate_top = win_rect.bottom - static_cast<int>(
                                                                 geometry.plate_height_dip *
                                                                 popup_scale);
                    weather_popup.Show(weather_snapshot, today, anchor_x, plate_top,
                                       window.Dpi());
                }
            }
        }
        // The weather preview follows the hover, not a timer: the pointer
        // leaving the dock (or moving to another tile) dismisses it on the
        // next tick, the same transition contract as the thumbnail popup.
        // The probe tile counts as "the hover" here too - driving the dwell
        // from outside is the only way a script can test this popup.
        if (weather_popup.Visible()) {
            const int hovered_now = [&] {
                if (hover_probe_tile >= 0) return hover_probe_tile;
                if (dock_hover.active) {
                    return dock_painter.HitTest(dock_hover.mouse_x_dip, window.Scale());
                }
                return -1;
            }();
            if (hovered_now != weather_tile_index()) {
                weather_popup.Hide();
            }
        }
        // Screen hotspots (T-069, C-24): GetCursorPos polling throttled to
        // 10Hz inside the frame tick - the C-24 ceiling, and the spike
        // measured this exact loop shape at 0 us of process CPU over 5s.
        // `hotspots_active` is false on a default config, so the whole block
        // costs one bool test per frame until the user enables one.
        if (hotspots_active && hotspot_tracker.ShouldPoll(delta_ms)) {
            POINT cursor{};
            if (GetCursorPos(&cursor)) {
                // Hotspots live on the dock's display (primary unless pinned by
                // DOCKFORGE_TARGET_DISPLAY), in virtual-screen coordinates -
                // the same frame GetCursorPos reports. The monitor rect is
                // DPI-independent, so reading the placement state is safe.
                const RECT display_rect = target_display.monitor;
                const int fired =
                    hotspot_tracker.Poll(cursor, config.hotspots, display_rect);
                if (fired >= 0) {
                    DF_INFO("hotkeys", "hotspot '%s' entered -> %s",
                            config.hotspots[static_cast<std::size_t>(fired)].id.c_str(),
                            config.hotspots[static_cast<std::size_t>(fired)].action.c_str());
                    RunShortcutAction(
                        config.hotspots[static_cast<std::size_t>(fired)].action);
                }
            }
        }
        // ---- notification centre (T-076) --------------------------------
        // C-21: the monitor is ticked here rather than from a timer of its own
        // so the whole process keeps exactly one clock (C-12).
        material_monitor.Tick(delta_ms);
        // Outside the notif_enabled guard on purpose: the probe channel must be
        // able to open the panel and push a synthetic arrival on a machine where
        // the feature is still switched off, which is exactly how the evidence
        // screenshots are produced.
        read_notif_command(delta_ms);
        if (notif_enabled) {
            notif_since_poll_ms += delta_ms;
            // The sidecar answers a poll in 700-840ms (measured), so asking
            // faster than the configured interval only queues work. The clamp
            // lives in core::NotificationPollRange, not here.
            const double poll_ms =
                static_cast<double>(config.notifications.poll_seconds) * 1000.0;
            if (notif_since_poll_ms >= poll_ms) {
                notif_since_poll_ms = 0.0;
                if (!notif_sidecar.running()) {
                    // A crashed child is restarted at most once per poll
                    // interval: a sidecar that dies on every poll must not turn
                    // into a spawn loop.
                    if (notif_sidecar.Start(notif_sidecar_path)) {
                        DF_INFO("notifications", "sidecar restarted pid=%lu",
                                notif_sidecar.pid());
                    }
                } else {
                    notif_sidecar.RequestPoll();
                }
            }
            const notifications::NotificationDrain drain = notif_sidecar.DrainEvents();
            if (drain.ready) {
                notif_store.set_access(drain.ready_access);
                notif_panel.SetAccessStatus(drain.ready_access);
                DF_INFO("notifications", "sidecar ready access=%s",
                        notifications::ToString(drain.ready_access));
            }
            if (!drain.error.empty()) {
                DF_WARN("notifications", "protocol: %s",
                        core::WideToUtf8(drain.error).c_str());
            }
            for (const notifications::NotificationState& state : drain.states) {
                std::vector<notifications::NotificationItem> arrived;
                const bool changed = notif_store.ApplyState(state, &arrived);
                notif_panel.SetAccessStatus(notif_store.access());
                if (changed) {
                    notif_panel.SetItems(&notif_store.items());
                    dirty = true;
                }
                for (const notifications::NotificationItem& item : arrived) {
                    // ADR-0025: the log is on disk, so it gets the sender and the
                    // id and nothing else. Logging the headline would put the
                    // user's message text in a file that outlives the session -
                    // exactly what "in memory only" forbids.
                    DF_INFO("notifications", "arrived id=%u app='%s'", item.id,
                            core::WideToUtf8(item.DisplayApp()).c_str());
                    notif_deliver(item, &dirty);
                }
            }
            // The banner ages on its own clock: a card's timeout is a property
            // of the card, not of the poll interval.
            if (notif_banner.model().Tick(delta_ms)) {
                notif_banner.Refresh(window.Dpi());
            }
            // A pulse that has finished still needs one repaint to erase its
            // ring: PulseAlphaFor returns 0 after 640ms but the last painted
            // frame still shows it.
            for (std::size_t i = 0; i < tiles.size(); ++i) {
                const double start = tiles[i].pulse_start_ms;
                if (start <= 0.0) continue;
                const double total = dock::kPulsePeriodMs * dock::kPulseRepeats;
                if (dock_clock_ms - start < total) {
                    dirty = true;  // keep frames coming while the ring is alive
                } else {
                    tiles[i].pulse_start_ms = 0.0;
                    dirty = true;
                }
            }
        }
        if (running_tracker.Update(delta_ms)) {
            running_tracker.ApplyToTiles(tiles);
            dirty = true;
        }
        // Taskbar progress (T-067): throttled MSAA white-list scan; Update
        // returns true only when a reading appeared, moved, or a dialog left
        // the scan (window destroyed - state drops in the same pass, the same
        // side of the window lifecycle as the thumbnail cleanup).
        if (progress_tracker.Update(delta_ms)) {
            apply_progress();
            dirty = true;
        }
        // Overlay animation clock: drives the indeterminate-progress pulse.
        // Accumulated here (not read in Paint) so a frozen test frame is
        // deterministic and a dropped frame cannot jump the animation.
        dock_clock_ms += delta_ms;        // Hover drives both the magnification ramp and the clickable region. The
        // ramp runs at frame rate, so the tick is the only place it can advance
        // without a separate timer.
        const bool was_hovered = region_hovered;
        region_hovered = dock_hover.active;
        if (hover_moved) {
            hover_moved = false;
            dirty = true;
        }
        // Same coalescing for the drag: mouse moves arrive far faster than 60 Hz, and
        // a reorder frame that repainted per message would spend the budget on
        // duplicates. The controller's state is already current, so skipping
        // intermediate paints loses nothing.
        if (drag_moved) {
            drag_moved = false;
            dirty = true;
        }
        // Same coalescing again for the external drag: OLE's DragOver runs inside the
        // source process's modal loop and arrives at whatever rate the mouse reports.
        // Invalidating per callback measured p99 17.28ms against the 16.7ms budget.
        if (drop_moved) {
            drop_moved = false;
            dirty = true;
        }
        // The eject confirmation is time-based, so it has to advance on the tick: the
        // pointer can be held perfectly still outside the dock, and Move only fires on
        // motion. Without this the hold would never complete for a user who stops
        // moving, which is exactly what "hold to confirm" invites them to do.
        if (drag.Tick(delta_ms)) {
            dirty = true;
        }
        if (dock::AdvanceHover(dock_hover, delta_ms) || region_hovered != was_hovered) {
            dirty = true;
            // Only when the hover state flips: the region covers the full
            // magnification, so intermediate ramp frames need no region update.
            // Rebuilding it every frame cost 0.6ms of p99 (measured 17.3ms).
            if (region_hovered != was_hovered) {
                apply_region();
            }
        }

        // Window preview (T-015). The dwell timer is armed only while the pointer
        // is on a tile, and it resets on any change, so a preview never appears
        // mid-gesture or for a pointer merely crossing the dock.
        //
        // The probe file is read here, before the show block, and a change of the
        // pinned tile hides any visible popup: "leave" and "move to another tile"
        // are the two transitions the dwell contract is defined by, and driving
        // them from outside is the only way to test them while a human is using
        // the physical mouse.
        if (!hover_probe_file.empty()) {
            const int before = hover_probe_tile;
            read_hover_probe(delta_ms);
            if (hover_probe_tile != before && preview_visible) {
                HidePreview();
            }
            if (hover_probe_tile != before && weather_popup.Visible()) {
                weather_popup.Hide();
            }
        }
        if ((preview_enabled && !preview_visible) ||
            (weather_preview_enabled && !weather_popup.Visible())) {
            if (!thumbnails_pending && !drag.Active() && !drop_pointer.active) {
            const int hovered =
                hover_probe_tile >= 0
                    ? hover_probe_tile
                    : (dock_hover.active
                           ? dock_painter.HitTest(dock_hover.mouse_x_dip, window.Scale())
                           : -1);
            // Diagnostic: the probe aims at a tile centre parsed from the drop-grid
            // line, so when a dwell never fires this is the one line that says
            // whether the pointer was over a tile at all (hovered), whether the
            // dock thought it was hovering (active), how far the dwell had got
            // (elapsed), and whether any window was found for the hovered app.
            //
            // Rate-limited to 200ms, not once per frame: resolving the window count
            // runs shell::WindowsForEntry, whose MatchesEntry falls back to
            // SameExecutable -> CreateFileW file identity. Doing that every frame
            // while hovering put a synchronous file I/O on the frame path and showed
            // up as loop p99 19.7ms in the T-016 bench with paint n=2 - the bench was
            // measuring this line, not the preview. The dwell needs 400ms, so 200ms
            // still gives two samples before the popup is due.
            hover_debug_since_ms += delta_ms;
            if (hover_debug_since_ms >= 200.0) {
                hover_debug_since_ms = 0.0;
                DF_DEBUG("preview", "tick hovered=%d active=%d elapsed=%.1f windows=%zu",
                         hovered, dock_hover.active ? 1 : 0, hover_timer.ElapsedMs(),
                         (hovered >= 0 && hovered < static_cast<int>(tiles.size()))
                             ? shell::WindowsForEntry(
                                   tiles[static_cast<size_t>(hovered)].app,
                                   running_tracker.Windows())
                                   .size()
                             : 0u);
            }
            if (hover_timer.Update(hovered, static_cast<float>(delta_ms))) {
                if (hovered >= 0 && hovered < static_cast<int>(tiles.size())) {
                    const auto& tile = tiles[static_cast<size_t>(hovered)];
                    // Weather tile (T-075): the dwell shows the forecast popup
                    // instead of a thumbnail - there is no window to preview.
                    // Same dwell, same dismissal rules; only the content
                    // differs. DOCKFORGE_WEATHER_PREVIEW=0 disables it for
                    // probes (the mirror of DOCKFORGE_PREVIEW).
                    if (tile.weather) {
                        if (weather_preview_enabled && weather_popup_class_ready &&
                            weather_popup.Create(instance)) {
                            SYSTEMTIME now{};
                            GetLocalTime(&now);
                            char today[16] = {};
                            std::snprintf(today, sizeof(today), "%04d-%02d-%02d", now.wYear,
                                          now.wMonth, now.wDay);
                            const auto centres = dock::IdleCentersDip(geometry, safe_scale);
                            const int anchor_x =
                                dock_rect.left +
                                static_cast<int>(lroundf(
                                    centres[static_cast<size_t>(hovered)] * safe_scale)) +
                                static_cast<int>(lroundf(
                                    static_cast<float>(config.dock.icon_size) * safe_scale * 0.5f));
                            const int plate_top_px =
                                dock_rect.bottom -
                                static_cast<int>(geometry.plate_height_dip * safe_scale);
                            weather_popup.Show(weather_snapshot, today, anchor_x,
                                               plate_top_px, window.Dpi());
                        } else {
                            DF_DEBUG("weather", "dwell on weather tile; preview unavailable");
                        }
                        hover_timer.Reset();
                    } else {
                    // The window list comes from the running tracker's last poll,
                    // which is at most 300ms old and already paid for. Polling here
                    // would add an EnumWindows to a frame that just decided to show
                    // a preview.
                    const auto windows =
                        shell::WindowsForEntry(tile.app, running_tracker.Windows());
                    if (!windows.empty()) {
                        std::vector<HWND> sources;
                        sources.reserve(windows.size());
                        for (const auto& window : windows) {
                            sources.push_back(window.hwnd);
                        }
                        // Show the popup immediately, then register thumbnails over
                        // the following ticks: the alternative is a 594ms stall for
                        // eight slots (spikes/t015_thumb), which would blow the frame
                        // budget on the very frame the user asked for a preview.
                        if (preview.Create(instance)) {
                            thumbnails.SetDestination(preview.Handle());
                            const int registered = thumbnails.SetSources(sources);
                            thumbnails_pending = thumbnails.PendingCount() > 0;
                            const float scale = window.Scale();
                            const int slots = std::min(static_cast<int>(windows.size()),
                                                       shell::kMaxThumbnails);
                            const int width_px = static_cast<int>(lroundf(
                                (shell::kSlotPaddingDip * 2.0f +
                                 static_cast<float>(slots) *
                                         (shell::kSlotWidthDip + shell::kSlotGapDip) -
                                 shell::kSlotGapDip) *
                                scale));
                            const int height_px = static_cast<int>(lroundf(
                                (shell::kSlotPaddingDip * 2.0f + shell::kSlotHeightDip) * scale));
                            // Horizontally centred on the hovered icon, vertically
                            // sitting on top of the dock with an 8px gap. Both use
                            // the dock's real screen rectangle (dock_rect), not
                            // the placement request, so a clamped dock still puts
                            // the preview in the right place.
                            const auto centres = dock_painter.IdleCentersDip(safe_scale);
                            const int left_px =
                                dock_rect.left +
                                static_cast<int>(lroundf(
                                    centres[static_cast<size_t>(hovered)] * scale)) -
                                width_px / 2;
                            const int top_px = dock_rect.top - height_px - 8;
                            preview.Place(left_px, top_px, width_px, height_px);
                            thumbnails.UpdateDestinations(left_px, top_px, scale);
                            preview_visible = true;
                            preview_tile = hovered;
                            DF_INFO("preview",
                                    "shown tile=%d windows=%zu registered=%d pending=%zu "
                                    "rect=%d,%d %dx%d",
                                    hovered, windows.size(), (size_t)registered,
                                    (size_t)thumbnails.PendingCount(), left_px, top_px,
                                    width_px, height_px);
                            // C-17g: for packaged apps the visible top-level window is
                            // owned by ApplicationFrameHost, so hwnd and app_pid
                            // differ by design. Logging both is the DoD proof that
                            // the source was chosen through the frame enumeration
                            // rather than by accident.
                            for (size_t i = 0; i < sources.size() && i < shell::kMaxThumbnails;
                                 ++i) {
                                DF_DEBUG("preview",
                                         "source[%zu] hwnd=%p pid=%lu app_pid=%lu class=%ls "
                                         "title=%ls",
                                         i, (void*)sources[i],
                                         (unsigned long)windows[i].pid,
                                         (unsigned long)windows[i].app_pid,
                                         windows[i].class_name.c_str(),
                                         windows[i].title.c_str());
                            }
                        } else {
                            DF_WARN("preview", "popup create failed; preview disabled");
                        }
                        // The dwell is consumed either way. Without this the latch
                        // stays armed and every later frame on the same tile re-enters
                        // this branch, re-creating the popup on each pass.
                        hover_timer.Reset();
                    }
                    // Re-arm for the next dwell. The timer must NOT be reset when a
                    // preview is shown: a preview that stays visible for a long hover
                    // keeps its latch until the pointer leaves, so moving the pointer
                    // across the dock cannot re-fire on the same tile.
                }
                } else {
                    // The dwell elapsed on a tile with no window to preview. This is
                    // a legitimate state (an installed app that is not running) and
                    // it must be logged: without a line here, a probe that aims at a
                    // dead tile sees the same silence as a broken latch, and the two
                    // cannot be told apart from the outside.
                    DF_DEBUG("preview", "dwell elapsed on tile %d with no window", hovered);
                    hover_timer.Reset();
                }
            }
            }
        }

        // Registrations that did not fit in the showing frame land here, one per
        // tick, so a preview with eight windows fills in over ~8 frames instead of
        // stalling one.
        if (thumbnails_pending) {
            const int registered = thumbnails.PumpPending();
            if (registered > 0) {
                thumbnails.UpdateDestinations(dock_rect.left, dock_rect.top, window.Scale());
                dirty = true;
            }
            if (thumbnails.PendingCount() == 0) {
                thumbnails_pending = false;
            }
        }

        // Launchpad (T-041): the animation runs at frame rate and the watcher only
        // signals, so the tick is the only place either can advance. Returns true while
        // anything is animating, which is what keeps the repaint loop awake for the
        // 180ms open and 140ms close.
        if (!launchpad_cmd_file.empty()) {
            read_launchpad_command(delta_ms);
        }
        // Independent channel: the settings probe must work even when the
        // Launchpad channel is unused (measured - it sat inside this if and
        // never fired without DOCKFORGE_LAUNCHPAD_CMD_FILE).
        read_settings_command(delta_ms);

        // Config hot reload (T-051): the watcher raises at most once per settled
        // 300ms burst; the reload decision rejects invalid JSON, short-circuits
        // content-identical files (our own saves echo through the watcher), and
        // otherwise adopts + publishes the C-20 snapshot.
        if (config_watcher_running && config_watcher.ConsumeChanged(static_cast<double>(GetTickCount64()))) {
            core::Config next;
            std::string reason;
            const core::ReloadVerdict verdict = core::ReloadConfigFromDisk(config, next, &reason);
            if (verdict == core::ReloadVerdict::Adopted) {
                config = std::move(next);
                std::string theme_error;
                if (!core::LoadThemeInto(config.appearance.theme, config, &theme_error)) {
                    DF_WARN("core", "theme '%s' unavailable after reload; base values in effect",
                            config.appearance.theme.c_str());
                }
                DF_INFO("core", "config reloaded (%s); publishing ConfigChanged", reason.c_str());
                bus.Publish(core::ConfigChanged{config});
                dirty = true;
            } else if (verdict == core::ReloadVerdict::Rejected) {
                // Keep the running config; the reason was already WARN-logged
                // inside the reload decision.
                DF_WARN("core", "config reload rejected; keeping the running config");
            } else if (reason != last_reload_reason) {
                DF_DEBUG("core", "config reload skipped (%s)", reason.c_str());
            }
            last_reload_reason = reason;
        }
        if (launchpad_ready && launchpad.Tick(delta_ms, config)) {
            dirty = true;
        }

        if (dirty) {
            InvalidateRect(window.Handle(), nullptr, FALSE);
        }
    });
    loop.SetStatsHandler([&capture, &paint_clock, &running_tracker](const core::FrameClock& clock) {
        const core::FrameClock& paint = paint_clock;
        const auto stats = capture.GetStats();
        const auto& run = running_tracker.GetStats();
        DF_DEBUG("loop",
                 "frames=%zu avg=%.2fms p50=%.2fms p99=%.2fms fps=%.1f | paint n=%zu "
                 "avg=%.2fms p99=%.2fms | capture=%llu/%llu avg=%.1fms | run polls=%llu "
                 "avg=%.2fms max=%.2fms fg=%llu",
                 clock.SampleCount(), clock.AverageMs(), clock.Percentile(50.0),
                 clock.Percentile(99.0), clock.AverageFps(), paint.SampleCount(),
                 paint.AverageMs(), paint.Percentile(99.0),
                 static_cast<unsigned long long>(stats.successes),
                 static_cast<unsigned long long>(stats.attempts), stats.average_ms,
                 static_cast<unsigned long long>(run.polls), run.AveragePollMs(),
                 run.max_poll_ms, static_cast<unsigned long long>(run.foreground_changes));
    });

    window.SetMessageHook([&](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& out) {
        switch (msg) {
            case WM_PAINT: {
                // A NOREDIRECTIONBITMAP window has no GDI surface; BeginPaint is
                // only used to consume the update region.
                PAINTSTRUCT ps{};
                BeginPaint(hwnd, &ps);
                repaint();
                EndPaint(hwnd, &ps);
                out = 0;
                return true;
            }
            case WM_DPICHANGED: {
                // The dock may have been dragged to a display with a different
                // scale. wParam is authoritative for the new DPI: it is what the
                // OS will actually render the window at, while GetDpiForMonitor
                // can lag a transition. The suggested rect in lParam is ignored
                // because it assumes a normal overlapped window; we place against
                // the work area instead.
                const UINT new_dpi = LOWORD(wp);
                const UINT previous = target_display.effective_dpi;
                platform::DisplayInfo hosting;
                if (platform::DisplayForWindow(hwnd, hosting) && have_target) {
                    target_display = hosting;
                }
                // Content layout (Window::Dpi) and window sizing must read the
                // same DPI or the panel is laid out at one scale inside a window
                // sized at another. wParam is what the OS will actually render
                // at, so it wins over the enumerated monitor DPI. An explicit
                // DOCKFORGE_DPI_OVERRIDE still wins over both, otherwise the DPI
                // matrix could not be held stable while the dock moves.
                if (!dpi_forced_by_env) {
                    platform::Window::SetDpiOverride(new_dpi);
                }
                if (have_target) {
                    target_display.effective_dpi = platform::Window::DpiOverride() != 0
                                                       ? platform::Window::DpiOverride()
                                                       : new_dpi;
                }
                DF_INFO("platform", "dpi changed to %u on %s (effective %u -> %u)", new_dpi,
                        core::WideToUtf8(target_display.device).c_str(), previous,
                        target_display.effective_dpi);
                apply_placement();
                // The preview's slot rectangles are DIP-derived, so a DPI change
                // invalidates every one of them. Hiding and letting the dwell
                // timer re-arm is cheaper than re-deriving geometry the popup is
                // about to throw away anyway.
                if (preview_visible) {
                    HidePreview();
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                out = 0;
                return true;
            }
            case WM_SIZE: {
                surface.Resize(LOWORD(lp), HIWORD(lp));
                InvalidateRect(hwnd, nullptr, FALSE);
                out = 0;
                return true;
            }
            case WM_MOVE: {
                if (capture_active) {
                    RECT rc{};
                    GetWindowRect(hwnd, &rc);
                    capture.SetRegion(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
                }
                // The preview sits on top of the dock, so it has to follow the
                // dock. WM_MOVE is the only message that says the dock moved;
                // WM_DISPLAYCHANGE and WM_SETTINGCHANGE both re-enter through
                // apply_placement, which already re-places the preview.
                if (preview_visible) {
                    const float scale = window.Scale();
                    const int slots = static_cast<int>(thumbnails.Slots().size());
                    const int width_px = static_cast<int>(lroundf(
                        (shell::kSlotPaddingDip * 2.0f +
                         static_cast<float>(slots) * (shell::kSlotWidthDip + shell::kSlotGapDip) -
                         shell::kSlotGapDip) *
                        scale));
                    const int height_px = static_cast<int>(lroundf(
                        (shell::kSlotPaddingDip * 2.0f + shell::kSlotHeightDip) * scale));
                    const auto centres = dock_painter.IdleCentersDip(safe_scale);
                    const int left_px = dock_rect.left +
                                        static_cast<int>(lroundf(
                                            centres[static_cast<size_t>(preview_tile)] * scale)) -
                                        width_px / 2;
                    const int top_px = dock_rect.top - height_px - 8;
                    preview.Place(left_px, top_px, width_px, height_px);
                    thumbnails.UpdateDestinations(left_px, top_px, scale);
                }
                return false;
            }
            case WM_MOUSEMOVE: {
                // The dock never takes focus (WS_EX_NOACTIVATE), but it still gets
                // mouse messages - measured in spikes/t010_hover question 1, along
                // with WM_MOUSELEAVE after TrackMouseEvent(TME_LEAVE).
                if (!tracking_mouse) {
                    TRACKMOUSEEVENT track{};
                    track.cbSize = sizeof(track);
                    track.dwFlags = TME_LEAVE;
                    track.hwndTrack = hwnd;
                    tracking_mouse = TrackMouseEvent(&track) != FALSE;
                    if (!tracking_mouse) {
                        DF_WARN("dock", "TrackMouseEvent failed: %lu", GetLastError());
                    }
                }
                // LPARAM is in PHYSICAL pixels: a per-monitor DPI aware window gets
                // unscaled client coordinates, so the value has to be divided by the
                // window scale before it means anything to the DIP-based layout.
                const float scale = window.Scale();
                const float safe_scale = scale > 0.0f ? scale : 1.0f;
                const float x = static_cast<float>(GET_X_LPARAM(lp)) / safe_scale;
                const float y = static_cast<float>(GET_Y_LPARAM(lp)) / safe_scale;

                // A drag in flight consumes the move whether or not the pointer is
                // over an icon: the ghost follows the pointer past the dock's edge,
                // and SetCapture is what keeps those messages coming (spike
                // conclusion 1: 3 moves outside the window arrived only with capture).
                if (drag.State().phase != dock::DragPhase::Idle) {
                    // Idle centres, not the magnified ones: the insertion slot must be a
                    // function of the pointer alone. Feeding the magnified row back in
                    // makes the slot depend on which icon is currently under the pointer,
                    // and the same gesture then lands in different places depending on
                    // where it started (measured: 0 -> 4 reported 0 -> 3).
                    drag.Move(x, y, dock_painter.IdleCentersDip(safe_scale),
                              geometry.window_width_dip, geometry.window_height_dip);
                    drag_moved = true;
                    out = 0;
                    return true;
                }

                if (x != dock_hover.mouse_x_dip || !dock_hover.active) {
                    // Moving to a different tile re-arms the dwell timer, so a
                    // preview only ever appears for the tile the pointer is
                    // actually resting on. The popup itself is hidden by the
                    // tick once the timer re-arms.
                    const int previous_hover =
                        dock_hover.active ? dock_painter.HitTest(dock_hover.mouse_x_dip, safe_scale)
                                          : -1;
                    const int next_hover = dock_painter.HitTest(x, safe_scale);
                    if (preview_visible && next_hover != previous_hover) {
                        HidePreview();
                    }
                    dock_hover.mouse_x_dip = x;
                    dock_hover.active = true;
                    // Deliberately no InvalidateRect here. Windows delivers mouse
                    // moves far faster than 60 Hz, and repainting per message pushed
                    // p99 to 18.2ms against a 16.7ms budget (measured with
                    // tools/hover_bench.ps1). The tick coalesces them into at most
                    // one repaint per frame, capping latency at one frame.
                    hover_moved = true;
                }
                out = 0;
                return true;
            }
            case WM_MOUSELEAVE: {
                tracking_mouse = false;
                // A drag suppresses WM_MOUSELEAVE for its whole duration (measured:
                // the leave only arrives when no button is held), so this cannot
                // cancel a gesture by accident. Kept as an assertion rather than a
                // guard because if that ever changes, silently clearing the hover
                // would strand the ramp mid-way with no way to finish it.
                if (drag.State().phase != dock::DragPhase::Idle) {
                    DF_WARN("dock", "WM_MOUSELEAVE arrived during a drag; ignoring hover reset");
                } else {
                    dock_hover.active = false;
                    // The dwell timer must reset with the hover: the pointer is
                    // gone, so whatever it was about to preview is no longer
                    // wanted, and leaving the timer armed would let the preview
                    // fire on the frame the pointer entered another tile.
                    hover_timer.Reset();
                    if (preview_visible) {
                        HidePreview();
                    }
                    if (weather_popup.Visible()) {
                        weather_popup.Hide();
                    }
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                out = 0;
                return true;
            }
            case WM_LBUTTONDOWN: {
                // A dock press is outside the panel: the menu-like dismissal
                // (T-066) closes it before the gesture resolves, so a click on
                // another tile never fights a panel floating above the row.
                if (folder_panel.Visible()) folder_panel.Close();
                // The click handler is the ONLY place with foreground rights. Windows
                // grants them to the process that received the click; from a timer or
                // hotkey SetForegroundWindow is refused outright, and AttachThreadInput
                // does not help (measured, evidence/T-011/fg_plain.txt). Doing this
                // work here rather than deferring it to the tick is what makes raising
                // a window work without the synthetic-ALT fallback.
                const float scale = window.Scale() > 0.0f ? window.Scale() : 1.0f;
                const float x_dip = static_cast<float>(GET_X_LPARAM(lp)) / scale;
                const float y_dip = static_cast<float>(GET_Y_LPARAM(lp)) / scale;

                // Hit test against a layout recomputed for the current pointer, not
                // the last painted frame. The two normally agree, but a click that
                // arrives between a move and the next tick would otherwise be tested
                // against the previous pointer position - a 16ms window in which the
                // icon under the cursor is not the icon being aimed at. The call is
                // O(n) with no allocation, so it is cheap enough for a click path.
                layout_now();
                const int index = dock_painter.HitTest(x_dip, scale);
                if (index < 0 || index >= static_cast<int>(tiles.size())) {
                    DF_DEBUG("dock", "click at %.1f dip hit no tile", x_dip);
                    out = 0;
                    return true;
                }

                // The press does not act yet: it might become a drag. The action runs
                // on release, when the gesture has resolved into click, reorder, or
                // removal. Capture is taken now so the pointer can leave the window
                // without ending the gesture.
                //
                // The weather fixture tile never drags: it has no identity the
                // curated order could store, so a reorder would either corrupt
                // dock.tiles or silently drop the tile on the next load.
                if (!tiles[static_cast<std::size_t>(index)].weather) {
                    drag.Begin(index, x_dip, y_dip);
                    SetCapture(hwnd);
                }
                // A press means the user wants this tile, not its preview.
                if (preview_visible) {
                    HidePreview();
                }
                if (weather_popup.Visible()) {
                    weather_popup.Hide();
                }
                // The region has to widen to the whole window the moment the drag
                // starts, not on the next hover transition. It is trimmed to the plate
                // and the magnified row while idle, and the ghost follows the pointer
                // out of both - SetWindowRgn clips composition content (T-010 spike
                // conclusion 3), so leaving the old region in place cut the ghost off
                // at the plate's top edge.
                apply_region();
                // Capture is what makes the gesture survive the pointer leaving the
                // window, so a silent failure here would look like a working drag that
                // mysteriously stops tracking. Logged rather than assumed.
                {
                    const HWND captured = GetCapture();
                    DF_DEBUG("dock", "drag press tile=%d capture=%d match=%d err=%lu", index,
                             captured != nullptr, captured == hwnd, GetLastError());
                }
                out = 0;
                return true;
            }
            case WM_LBUTTONUP: {
                if (drag.State().phase == dock::DragPhase::Idle) return false;
                ReleaseCapture();
                const dock::DragController::Result result = drag.End();
                apply_region();
                drag_moved = true;
                switch (result.kind) {
                    case dock::DragController::Result::Kind::Click:
                        ActivateTile(result.source_index);
                        break;
                    case dock::DragController::Result::Kind::Reorder:
                        dock::MoveTile(tiles, result.source_index, result.insert_index);
                        DF_INFO("dock", "reordered tile[%d] -> [%d]", result.source_index,
                                result.insert_index);
                        // Persisted immediately, not on exit: a crash between the
                        // gesture and the shutdown would lose the arrangement the
                        // user just made, and it cannot be reconstructed from the
                        // scan.
                        dock::CommitOrder(config, tiles, std::string());
                        break;
                    case dock::DragController::Result::Kind::Remove: {
                        const std::string identity = dock::IdentityOf(
                            tiles[static_cast<std::size_t>(result.source_index)].app);
                        const std::string name = core::WideToUtf8(
                            tiles[static_cast<std::size_t>(result.source_index)].app.display_name);
                        if (dock::RemoveTile(tiles, result.source_index)) {
                            DF_INFO("dock", "removed tile[%d] %s", result.source_index,
                                    name.c_str());
                            dock::CommitOrder(config, tiles, identity);
                        }
                        break;
                    }
                    case dock::DragController::Result::Kind::None:
                    case dock::DragController::Result::Kind::Cancel:
                        break;
                }
                // The window shrinks when a tile is removed, and the row moves when
                // the order changes: both need a re-place, not just a repaint.
                apply_placement();
                InvalidateRect(hwnd, nullptr, FALSE);
                out = 0;
                return true;
            }
            case WM_CANCELMODE: {
                // The system took the capture away (a modal dialog, a switch to
                // another desktop). The user's intent is unknowable, so the gesture
                // is dropped rather than resolved into a click or a reorder.
                if (drag.State().phase != dock::DragPhase::Idle) {
                    drag.Cancel();
                    apply_region();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    DF_INFO("dock", "drag cancelled by the system");
                }
                return false;
            }
            case WM_HOTKEY:
                if (wp == static_cast<WPARAM>(kLaunchpadHotkeyId) && launchpad_ready) {
                    if (launchpad.Visible()) {
                        launchpad.Close();
                    } else {
                        launchpad.Open(config);
                    }
                    out = 0;
                    return true;
                }
                if (wp == static_cast<WPARAM>(kTaskbarRestoreHotkeyId)) {
                    // Emergency restore (T-061). Works without the settings
                    // window, without the config being on, and without any
                    // module being "ready" - the registration is unconditional,
                    // so this arm is the recovery of last resort while the
                    // process is alive.
                    std::string detail;
                    const bool ok = core::RestoreTaskbarSystem(&detail);
                    DF_INFO("core", "emergency restore (Ctrl+Alt+T): %s (%s)",
                            ok ? "ok" : "FAILED", detail.c_str());
                    if (config.behavior.hide_taskbar) {
                        // Persist the recovery: the user just asked for the
                        // taskbar back, so a restart must not re-hide it. The
                        // write goes through the production SaveConfig path
                        // and the publish updates the settings checkbox.
                        core::Config updated = config;
                        updated.behavior.hide_taskbar = false;
                        if (core::SaveConfig(updated)) {
                            config = updated;
                            last_hide_taskbar = false;
                            DF_INFO("core",
                                    "emergency restore persisted hide_taskbar=false");
                            bus.Publish(core::ConfigChanged{config});
                        } else {
                            DF_WARN("core",
                                    "emergency restore could not persist config; "
                                    "taskbar shown for this run only");
                        }
                    }
                    publish_taskbar_state(true);
                    out = 0;
                    return true;
                }
                // T-069: config-driven hotkeys. FindByWParam matches only the
                // ids HotkeyHost handed out (0x4800+), so the two built-ins
                // above keep their own arms and their own semantics.
                if (const core::HotkeyConfig* config_hotkey = hotkey_host.FindByWParam(wp)) {
                    DF_INFO("hotkeys", "hotkey '%s' -> %s", config_hotkey->id.c_str(),
                            config_hotkey->action.c_str());
                    RunShortcutAction(config_hotkey->action);
                    out = 0;
                    return true;
                }
                return false;
            case WM_KEYDOWN:
                if (wp == 'Q' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    out = 0;
                    return true;
                }
                return false;
            case WM_CLOSE:
                DestroyWindow(hwnd);
                loop.Quit(0);
                out = 0;
                return true;
            case WM_DESTROY:
                loop.Quit(0);
                out = 0;
                return true;
            default:
                return false;
        }
    });

    ShowWindow(window.Handle(), SW_SHOWNOACTIVATE);
    UpdateWindow(window.Handle());

    // Re-run placement now that the window is mapped: the first pass happened
    // before ShowWindow, so the work area is re-read in case the taskbar changed
    // between enumeration and mapping.
    apply_placement();

    // DOCKFORGE_SIMULATE_DPI=<dpi> posts a synthetic WM_DPICHANGED once the loop
    // is running. A single-monitor host cannot produce a genuine per-monitor DPI
    // transition, and the OS silently drops a WM_DPICHANGED that arrives from
    // another process (verified: WM_CLOSE crosses, 0x02E0 does not), so the only
    // honest way to exercise the handler is from inside the process.
    if (const auto simulated = SimulatedDpiFromEnvironment()) {
        DF_INFO("app", "posting synthetic WM_DPICHANGED dpi=%u", *simulated);
        // SendMessage, not PostMessage: the OS rejects a posted WM_DPICHANGED
        // with ERROR_MESSAGE_SYNC_ONLY (1159) because it is a system message that
        // only has meaning when delivered synchronously.
        SendMessageW(window.Handle(), WM_DPICHANGED, MAKEWPARAM(*simulated, *simulated), 0);
        DF_INFO("app", "synthetic dpi change delivered");
    }
    repaint();

    // Taskbar hide, startup application (T-061). Deliberately AFTER the dock
    // window is placed and shown: hiding the shell's taskbar is the last thing
    // startup does, so every earlier failure path (exit codes 2..5) leaves the
    // machine untouched. Applied once, never re-asserted - a restarted explorer
    // brings the taskbar back on its own, and that is the emergency path
    // working, not a bug to fix.
    if (last_hide_taskbar) {
        std::string detail;
        core::ShellTraywndTaskbar backend;
        const bool ok = core::ApplyTaskbarVisibility(true, backend, &detail);
        DF_INFO("core", "hide_taskbar startup apply: %s (%s)", ok ? "ok" : "FAILED",
                detail.c_str());
        publish_taskbar_state(false);
    } else {
        DF_INFO("core", "hide_taskbar off (default); taskbar untouched");
    }

    const int code = loop.Run();
    DF_INFO("app", "app loop exited with code %d", code);
    // Normal-exit recovery (T-061): the app hid the taskbar, so the app shows
    // it again. Runs for every return through loop.Run() - WM_CLOSE, WM_DESTROY
    // and Quit() all end here. The crash path is covered separately by the
    // SetUnhandledExceptionFilter installed at startup.
    {
        std::string detail;
        const bool ok = core::RestoreTaskbarSystem(&detail);
        DF_INFO("core", "exit restore: %s (%s)", ok ? "ok" : "FAILED", detail.c_str());
        publish_taskbar_state(true);
    }
    // Revoked before the window is destroyed: OLE keeps a reference to the HWND, and a
    // destroyed window still registered is the classic shutdown hang.
    drop_target.Revoke();
    // Same reasoning for the DWM thumbnails, and the ordering inside matters:
    // ReleaseAll() before Destroy() so no registration outlives its destination
    // window. A registration pointing at a destroyed HWND is a dangling reference
    // inside the desktop compositor, and it is invisible until shutdown hangs.
    thumbnails.ReleaseAll();
    preview.Destroy();
    // The Launchpad's overlays are their own top-level windows with their own DComp
    // targets. They go before the Dock's surface for the same reason the preview does:
    // a live composition target on a destroyed HWND is a shutdown hang, not a leak.
    if (launchpad_hotkey) UnregisterHotKey(window.Handle(), kLaunchpadHotkeyId);
    if (taskbar_hotkey) UnregisterHotKey(window.Handle(), kTaskbarRestoreHotkeyId);
    // T-076: the sidecar is a child process with a stdin pipe; leaving it
    // behind would keep a dotnet host alive after the dock is gone. Stop()
    // closes stdin (the child exits on EOF) and terminates after 2s.
    notif_sidecar.Stop();
    notif_banner.Destroy();
    notif_panel.Destroy();
    // T-069: every config-driven hotkey goes too - the DoD's "no system-level
    // residue" guarantee. The host's destructor would do the same; doing it
    // here keeps all three unregisters on the same lines of the shutdown path.
    hotkey_host.UnregisterAll();
    launchpad.Shutdown();
    capture.Stop();
    plate.Reset();
    render_backend->Shutdown();
    surface.Shutdown();
    bus.Clear();
    core::Logger::Instance().Shutdown();
    return code;
}
