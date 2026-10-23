// DockForge entry point.
//
// T-001 skeleton window, T-002 config wiring, T-003 frame-driven app loop,
// T-004 DirectComposition surface, T-005 render backend + material tiers,
// T-006 shell app resolution + icon cache, T-007 display enumeration + placement,
// T-009 real dock tile row (dock/dock_view.*).
#include "core/app_loop.h"
#include "core/config.h"
#include "core/event_bus.h"
#include "core/frame_clock.h"
#include "core/log.h"
#include "core/paths.h"
#include "dock/dock_view.h"
#include "dock/drag_controller.h"
#include "dock/running_tracker.h"
#include "dock/tile_order.h"
#include "platform/composition.h"
#include "platform/display.h"
#include "platform/hit_region.h"
#include "platform/window.h"
#include "render/backend.h"
#include "render/backdrop_capture.h"
#include "render/material.h"
#include "shell/app_resolver.h"
#include "shell/icon_cache.h"
#include "shell/start_menu.h"
#include "shell/window_actions.h"
#include "shell/window_registry.h"

#include <d2d1helper.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <cmath>
#include <optional>
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

platform::DockEdge ToDockEdge(core::DockPosition position) {    switch (position) {
        case core::DockPosition::Left: return platform::DockEdge::Left;
        case core::DockPosition::Right: return platform::DockEdge::Right;
        case core::DockPosition::Bottom:
        default: return platform::DockEdge::Bottom;
    }
}

bool ReadEnvFlag(const wchar_t* name) {
    wchar_t buffer[8]{};
    const DWORD n = GetEnvironmentVariableW(name, buffer, ARRAYSIZE(buffer));
    if (n == 0 || n >= ARRAYSIZE(buffer)) return false;
    return buffer[0] != L'0' && buffer[0] != L'\0';
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

    // COM is required by every shell/DirectX surface DockForge touches.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        return 1;
    }
    auto com_guard = MakeGuard([] { CoUninitialize(); });

    core::Logger::Instance().Init(L"app");
    DF_INFO("app", "DockForge %s starting", "0.1.0");
    DF_INFO("app", "config path = %s", core::WideToUtf8(core::ConfigPath()).c_str());
    ApplyDpiOverrideFromEnvironment();
    // Remembered separately from Window::DpiOverride() because the WM_DPICHANGED
    // handler writes a synthetic override of its own; without this flag an
    // operator-supplied DOCKFORGE_DPI_OVERRIDE would be silently replaced the
    // first time the dock crossed a monitor boundary.
    const bool dpi_forced_by_env = platform::Window::DpiOverride() != 0;

    core::Config config = core::LoadConfig();
    DF_INFO("app", "config: icon_size=%d gap=%d mag=%.2f falloff=%s material=%s corner=%d",
            config.dock.icon_size, config.dock.gap,
            static_cast<double>(config.dock.magnification),
            core::ToString(config.dock.hover_falloff),
            core::ToString(config.appearance.material), config.appearance.corner_radius);

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
            if (MatchesAnyPin(entry, pins)) dock_apps.push_back(entry);
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

    const platform::DockEdge edge = ToDockEdge(config.dock.position);
    auto apply_placement = [&]() {
        if (!have_target) return;
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
        DF_INFO("app",
                "dock placed edge=%d work=%ld,%ld,%ld,%ld target=%ld,%ld %dx%d "
                "rect=%ld,%ld,%ld,%ld dpi=%u",
                static_cast<int>(edge), target_display.work.left, target_display.work.top,
                target_display.work.right, target_display.work.bottom, placement.rect.left,
                placement.rect.top, placement.width_px, placement.height_px, actual.left,
                actual.top, actual.right, actual.bottom, window.Dpi());
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

    // Paint cost, measured separately from the frame interval: the frame clock only
    // says a deadline was missed, this says whether the miss was drawing or present.
    core::FrameClock paint_clock;

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
                // Non-null only while a drag is in flight: the painter draws the
                // ghost and the insertion line, and skips the source slot. Passing
                // the controller's own state (not a copy) keeps the ghost and the
                // insertion slot from disagreeing mid-gesture.
                input.drag = drag.Active() ? &drag.State() : nullptr;
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
        if (drag.Active()) {
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

    // One tile's click action, factored out because it now runs on button release
    // rather than on press: the press may become a drag, and only the release knows
    // which it was.
    auto ActivateTile = [&](int index) {
        if (index < 0 || index >= static_cast<int>(tiles.size())) return;
        const shell::AppEntry& app = tiles[static_cast<std::size_t>(index)].app;
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

    core::EventBus bus;
    core::AppLoop loop;
    loop.SetTargetIntervalMs(16);  // 60 FPS target (PERFORMANCE.md)

    // Pull a fresh backdrop at most once per frame, and only when the worker
    // published something new. At 60 FPS with a 200ms capture interval that skips
    // the pixel copy on ~11 of every 12 frames.
    loop.SetTickHandler([&](double delta_ms) {
        bool dirty = false;
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
        if (running_tracker.Update(delta_ms)) {
            running_tracker.ApplyToTiles(tiles);
            dirty = true;
        }
        // Hover drives both the magnification ramp and the clickable region. The
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
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                out = 0;
                return true;
            }
            case WM_LBUTTONDOWN: {
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
                drag.Begin(index, x_dip, y_dip);
                SetCapture(hwnd);
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

    const int code = loop.Run();
    DF_INFO("app", "app loop exited with code %d", code);
    capture.Stop();
    plate.Reset();
    render_backend->Shutdown();
    surface.Shutdown();
    bus.Clear();
    core::Logger::Instance().Shutdown();
    return code;
}
