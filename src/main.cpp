// DockForge entry point.
//
// T-001 skeleton window, T-002 config wiring, T-003 frame-driven app loop,
// T-004 DirectComposition surface, T-005 render backend + material tiers,
// T-006 shell app resolution + icon cache, T-007 display enumeration + placement.
#include "core/app_loop.h"
#include "core/config.h"
#include "core/event_bus.h"
#include "core/frame_clock.h"
#include "core/log.h"
#include "core/paths.h"
#include "platform/composition.h"
#include "platform/display.h"
#include "platform/window.h"
#include "render/backend.h"
#include "render/backdrop_capture.h"
#include "render/material.h"
#include "shell/app_resolver.h"
#include "shell/icon_cache.h"
#include "shell/start_menu.h"

#include <d2d1helper.h>
#include <dwmapi.h>

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

// Logical (DIP) content size of the skeleton window. T-008 replaces this with
// the real Dock layout engine; until then the box scales with the config so the
// C-01 wiring stays observable.
struct LogicalSize {
    float width;
    float height;
};

LogicalSize ComputeLogicalSize(const core::Config& config) {
    // Kept deliberately modest: the dev host runs a 192 DPI session whose
    // logical work area is only 720x480 DIP, so a bigger skeleton would hang off
    // the screen and make screenshot evidence unusable. T-008 derives the real
    // Dock width from the icon count instead.
    return LogicalSize{240.0f + static_cast<float>(config.dock.icon_size) * 3.0f,
                       static_cast<float>(config.dock.icon_size) + 56.0f};
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

    const LogicalSize logical = ComputeLogicalSize(config);

    platform::WindowDesc desc;
    desc.title = L"DockForge";
    // Window size is physical pixels; content is laid out in DIPs (ADR-0007).
    // The DPI is not known until the HWND exists, so start at 96 DPI and correct
    // after creation with an explicit resize.
    desc.width = static_cast<int>(std::lround(logical.width));
    desc.height = static_cast<int>(std::lround(logical.height));
    desc.topmost = true;
    // Content comes from DirectComposition only (ADR-0007).
    desc.composition_surface = true;
    // The skeleton window must accept keyboard focus so Ctrl+Q can be proven
    // here. The real Dock window flips this off in T-009 (it is click-driven
    // and must never steal focus).
    desc.no_activate = false;

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
        const auto placement = platform::ComputeDockPlacement(
            target_display, edge, logical.width, logical.height, 8.0f);
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

    auto repaint = [&] {
        const float dpi = static_cast<float>(window.Dpi());
        if (!surface.Paint(dpi, [&](ID2D1DeviceContext& dc,
                                    const platform::CompositionSurface::PaintInfo& info) {
                render_backend->BeginFrame(dc, dpi);
                const render::RectF panel{0.5f, 0.5f, info.width_dip - 0.5f,
                                          info.height_dip - 0.5f};
                const float radius = static_cast<float>(config.appearance.corner_radius);
                plate.Draw(dc, *render_backend, panel, radius, plate_style,
                           backdrop_w > 0 ? backdrop_pixels.data() : nullptr, backdrop_w,
                           backdrop_h, dpi);

                // The label proves DirectWrite goes through the backend contract
                // rather than straight to the D2D context, and reports what the
                // shell scan found so the running app is its own evidence.
                wchar_t label[256];
                swprintf_s(label,
                           L"DockForge \u00b7 %s \u00b7 apps %zu \u00b7 uwp %d \u00b7 id %d \u00b7 "
                           L"ns %d \u00b7 icons %d",
                           core::ToString(tier), scan.app_count, scan.uwp_count,
                           scan.desktop_id_count, scan.namespace_count, scan.icons_ready);
                render_backend->DrawLabel(label, render::RectF{0.0f, 0.0f, info.width_dip,
                                                               info.height_dip},
                                          15.0f, render::ColorF{0.925f, 0.941f, 0.973f, 1.0f},
                                          render::TextAlign::Center);
                render_backend->EndFrame();
            })) {
            return false;
        }
        return surface.Commit();
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
        // T-010 replaces this with animation-driven invalidation. Until then a
        // repaint happens only when the backdrop moved or a frame budget was
        // missed, which keeps idle CPU at zero.
        if (dirty || delta_ms > 100.0) {
            InvalidateRect(window.Handle(), nullptr, FALSE);
        }
    });
    loop.SetStatsHandler([&capture](const core::FrameClock& clock) {
        const auto stats = capture.GetStats();
        DF_DEBUG("loop",
                 "frames=%zu avg=%.2fms p50=%.2fms p99=%.2fms fps=%.1f | capture=%llu/%llu "
                 "avg=%.1fms",
                 clock.SampleCount(), clock.AverageMs(), clock.Percentile(50.0),
                 clock.Percentile(99.0), clock.AverageFps(),
                 static_cast<unsigned long long>(stats.successes),
                 static_cast<unsigned long long>(stats.attempts), stats.average_ms);
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
