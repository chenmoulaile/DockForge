#include "finderbar/finder_bar.h"

#include "core/log.h"
#include "core/paths.h"
#include "platform/win_common.h"
#include "platform/window.h"

#include <dwrite.h>
#include <winver.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <iterator>

namespace dockforge::finderbar {
namespace {

constexpr char kModule[] = "finderbar";
constexpr wchar_t kBarClass[] = L"DockForgeFinderBar";
constexpr wchar_t kMenuOwnerClass[] = L"DockForgeFinderBarMenuOwner";

// WS_EX_NOREDIRECTIONBITMAP is mandatory for the DComp surface to be the only thing
// painted. No WS_EX_TOOLWINDOW: the bar is a first-class surface, and a tool window would
// be excluded from the shell's own window lists for no benefit.
constexpr DWORD kBarStyle = WS_POPUP;
constexpr DWORD kBarExStyle = WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP;

// Posted to the bar window when the menu thread finishes, so every surface call stays on
// the thread that owns the window (DComp is not thread safe).
constexpr UINT kMsgMenuClosed = WM_APP + 61;

// System-menu command ids. Distinct from any application id because the bar handles these
// itself and never forwards them. Derived from the header constant so a probe raising a
// command by id can never disagree with the popup that produced it.
constexpr UINT kSystemIdBase = static_cast<UINT>(kSystemMenuBase);

// The bar always draws the self-tinted plate, never a sampled backdrop, and the reason is
// measured rather than aesthetic.
//
// A backdrop-sampling tier (Blur) requires SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)
// so the plate does not sample its own previous frame and settle into a flat wash
// (T-005/T-076). But that same exclusion makes the window invisible to *every* screen
// capture on the machine: screenshots, recordings, screen shares. For a dock that is an
// acceptable trade; for a menu bar pinned across the top of the desktop it is not - a bar
// that vanishes from screenshots is worse than a bar without frost.
//
// A system backdrop (Mica, accent acrylic) is not an alternative: ADR-0008 measured that
// it ignores both SetWindowRgn and per-pixel alpha, so it would fill the full 28dip
// rectangle while the bar is retracted - exactly the overlap this design exists to avoid.
//
// So the bar takes the one option that is honest and always visible: a translucent solid
// plate, drawn by the same PlateRenderer as everything else, with the same corner radius
// and border. Over a wallpaper the result is close to what the real menu bar looks like,
// and it can be verified in a screenshot because it appears in one.
render::PlateStyle BarPlateStyle() {
    return render::StyleForTier(core::MaterialLevel::Solid, 0);
}

// The strip that summons a fully retracted bar. Three DIP is enough to hit without aiming
// and small enough that it never covers a window's own resize border, which starts at the
// very top edge of a maximized window.
constexpr float kTriggerHeightDip = 3.0f;

// macOS menu-bar text is white on a dark bar and near-black on a light one. The bar follows
// the system theme rather than inventing a third palette, so it reads as part of the shell
// instead of as an overlay (C-01 graceful degrade to a fixed palette if the query fails).
constexpr render::ColorF kTextDark{0.10f, 0.11f, 0.13f, 1.0f};
constexpr render::ColorF kTextLight{0.96f, 0.97f, 0.98f, 1.0f};
constexpr render::ColorF kHoverDark{0.0f, 0.0f, 0.0f, 0.09f};
constexpr render::ColorF kHoverLight{1.0f, 1.0f, 1.0f, 0.14f};
constexpr render::ColorF kGlyphDark{0.16f, 0.17f, 0.20f, 1.0f};
constexpr render::ColorF kGlyphLight{0.94f, 0.95f, 0.97f, 1.0f};

bool SystemUsesLightTheme() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    auto guard = MakeGuard([&key] { RegCloseKey(key); });
    DWORD value = 1;
    DWORD size = sizeof(value);
    DWORD type = 0;
    if (RegQueryValueExW(key, L"SystemUsesLightTheme", nullptr, &type,
                         reinterpret_cast<LPBYTE>(&value), &size) != ERROR_SUCCESS ||
        type != REG_DWORD) {
        return false;
    }
    return value != 0;
}

std::wstring FileNameOf(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// The DWrite factory the measurer needs. One per process: creating it costs about a
// millisecond and the layout runs on every foreground change, so a per-call factory would
// show up in the C-32 budget.
IDWriteFactory* SharedWriteFactory() {
    static IDWriteFactory* factory = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(&factory)))) {
            factory = nullptr;
        }
    }
    return factory;
}

// One live bar per process. The WinEvent hook is a C callback with no user data, and the
// process has exactly one bar, so a file-static back pointer is the honest design rather
// than a lookup table keyed by a hook nobody else can see.
FinderBar* g_active_bar = nullptr;

// Everything the menu thread needs. Filled in before the thread starts and read-only
// afterwards, so no locking is needed for the inputs; only the outputs are atomic.
struct MenuJob {
    HWND bar = nullptr;      // the bar window, used only to post the closed message
    HWND target = nullptr;   // the window that receives the forwarded WM_COMMAND
    HMENU menu = nullptr;
    bool owns_menu = false;
    int x = 0;
    int y = 0;
    std::atomic<unsigned> selected{0};
    std::atomic<bool> done{false};
};

// Only one menu can be open at a time, so one job slot is enough. The bar keeps the object
// alive until the thread reports completion.
std::atomic<MenuJob*> g_menu_job{nullptr};

LRESULT CALLBACK MenuOwnerProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_CANCELMODE) {
        DF_DEBUG(kModule, "menu owner received WM_CANCELMODE");
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// Runs the tracked menu. Lives on its own thread because TrackPopupMenuEx blocks until the
// user chooses or dismisses, and blocking the frame loop would freeze the dock (spike Q12:
// main-thread cadence 32.27ms before, 31.85ms during).
DWORD WINAPI MenuThreadProc(LPVOID param) {
    auto* job = static_cast<MenuJob*>(param);
    if (job == nullptr) return 0;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &MenuOwnerProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kMenuOwnerClass;
    RegisterClassExW(&wc);
    HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, kMenuOwnerClass, L"menu owner", WS_POPUP, 0, 0,
                                 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    if (owner == nullptr) {
        job->done.store(true);
        if (job->bar != nullptr) PostMessageW(job->bar, kMsgMenuClosed, 0, 0);
        return 0;
    }

    // The documented requirement for a popup menu to dismiss on an outside click and to
    // accept the keyboard. Spike Q8 showed both behaviours follow foreground ownership
    // exactly, so this is load-bearing, not defensive.
    if (!SetForegroundWindow(owner)) {
        DF_DEBUG(kModule, "SetForegroundWindow on the menu owner was refused");
    }
    SetFocus(owner);

    const UINT id =
        TrackPopupMenuEx(job->menu, TPM_RETURNCMD | TPM_NONOTIFY, job->x, job->y, owner, nullptr);
    job->selected.store(id);

    if (id != 0 && job->target != nullptr) {
        // C-32: forward the command to the window that owns the menu. PostMessage rather
        // than SendMessage so a slow handler cannot stall this thread's teardown.
        PostMessageW(job->target, WM_COMMAND, MAKEWPARAM(id, 0), 0);
        DF_DEBUG(kModule, "forwarded WM_COMMAND %u", id);
    }
    if (job->owns_menu && job->menu != nullptr) {
        // Only menus this module built are destroyed. A foreign HMENU belongs to the
        // application, and destroying it would take its menu bar down.
        DestroyMenu(job->menu);
    }

    DestroyWindow(owner);
    job->done.store(true);
    if (job->bar != nullptr) PostMessageW(job->bar, kMsgMenuClosed, 0, 0);
    return 0;
}

}  // namespace

// ---- name resolution ---------------------------------------------------------

float MeasureBarText(const wchar_t* text, float size_dip) {
    if (text == nullptr || *text == L'\0' || size_dip <= 0.0f) return 0.0f;
    IDWriteFactory* factory = SharedWriteFactory();
    if (factory == nullptr) return ApproximateTextWidth(text, size_dip);

    // The format matches backend_d2d's DrawLabel exactly: same family order, same weight,
    // same locale. A layout that measured with a different format would place boxes that do
    // not match the glyphs inside them.
    static IDWriteTextFormat* format = nullptr;
    static float format_size = 0.0f;
    if (format == nullptr || format_size != size_dip) {
        if (format != nullptr) {
            format->Release();
            format = nullptr;
        }
        HRESULT hr = factory->CreateTextFormat(
            L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size_dip, L"zh-CN", &format);
        if (FAILED(hr)) {
            hr = factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                           DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                           size_dip, L"zh-CN", &format);
        }
        if (FAILED(hr)) {
            format = nullptr;
            return ApproximateTextWidth(text, size_dip);
        }
        format_size = size_dip;
    }

    IDWriteTextLayout* layout = nullptr;
    const UINT32 length = static_cast<UINT32>(wcslen(text));
    if (FAILED(factory->CreateTextLayout(text, length, format, 4096.0f, size_dip * 2.0f,
                                         &layout))) {
        return ApproximateTextWidth(text, size_dip);
    }
    DWRITE_TEXT_METRICS metrics{};
    const HRESULT hr = layout->GetMetrics(&metrics);
    layout->Release();
    if (FAILED(hr)) return ApproximateTextWidth(text, size_dip);
    return metrics.widthIncludingTrailingWhitespace;
}

std::wstring FallbackNameFromPath(const std::wstring& exe_path) {
    std::wstring name = FileNameOf(exe_path);
    const std::size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name.resize(dot);
    return name;
}

std::wstring DisplayNameForExecutable(const std::wstring& exe_path) {
    if (exe_path.empty()) return L"";

    // VerQueryValue wants the translation table before it can be asked for FileDescription,
    // and that table is a list, not a single value. The first entry is what Explorer shows,
    // so it is what the bar shows.
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(exe_path.c_str(), &handle);
    if (size == 0) return FallbackNameFromPath(exe_path);

    std::vector<std::uint8_t> buffer(size);
    if (!GetFileVersionInfoW(exe_path.c_str(), 0, size, buffer.data())) {
        return FallbackNameFromPath(exe_path);
    }

    struct Translation {
        WORD language;
        WORD codepage;
    };
    Translation* translations = nullptr;
    UINT translation_bytes = 0;
    std::wstring description;
    if (VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation",
                       reinterpret_cast<LPVOID*>(&translations), &translation_bytes) &&
        translation_bytes >= sizeof(Translation) && translations != nullptr) {
        for (UINT i = 0; i < translation_bytes / sizeof(Translation) && description.empty(); ++i) {
            wchar_t query[64]{};
            swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\FileDescription",
                       translations[i].language, translations[i].codepage);
            wchar_t* value = nullptr;
            UINT value_bytes = 0;
            if (VerQueryValueW(buffer.data(), query, reinterpret_cast<LPVOID*>(&value),
                               &value_bytes) &&
                value != nullptr && value_bytes > sizeof(wchar_t)) {
                description.assign(value);
            }
        }
    }

    // A version resource can exist and still carry an empty description; the file name is
    // then the only thing a user would recognise.
    while (!description.empty() && description.back() == L' ') description.pop_back();
    if (description.empty()) return FallbackNameFromPath(exe_path);
    return description;
}

std::wstring ResolveAppDisplayName(HWND hwnd) {
    if (hwnd == nullptr) return L"";
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return L"";

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process != nullptr) {
        wchar_t path[MAX_PATH * 2]{};
        DWORD length = static_cast<DWORD>(std::size(path));
        const BOOL ok = QueryFullProcessImageNameW(process, 0, path, &length);
        CloseHandle(process);
        if (ok) {
            const std::wstring name = DisplayNameForExecutable(path);
            if (!name.empty()) return name;
        }
    }

    // Fall back to the window title. A process we cannot open (elevated, protected) still
    // has a title, and a title is better than an empty bar.
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) return L"";
    std::wstring title(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(hwnd, title.data(), length + 1);
    if (copied <= 0) return L"";
    title.resize(static_cast<std::size_t>(copied));
    return title;
}

std::wstring StripAccelerator(const std::wstring& title) {
    std::wstring out;
    out.reserve(title.size());
    for (std::size_t i = 0; i < title.size(); ++i) {
        if (title[i] == L'&') {
            // "&&" is a literal ampersand; a lone "&" marks the accelerator.
            if (i + 1 < title.size() && title[i + 1] == L'&') {
                out.push_back(L'&');
                ++i;
                continue;
            }
            continue;
        }
        out.push_back(title[i]);
    }
    return out;
}

AppMenuSnapshot ReadAppMenu(HWND hwnd, int max_items) {
    AppMenuSnapshot snapshot;
    if (hwnd == nullptr) return snapshot;

    // GetMenu fails for the majority of modern applications - they draw their own menu bar.
    // That is the normal case, not an error (spike Q3).
    HMENU bar = GetMenu(hwnd);
    if (bar == nullptr) return snapshot;

    const int count = GetMenuItemCount(bar);
    if (count <= 0) return snapshot;

    snapshot.menu = bar;
    snapshot.readable = true;
    const int limit = std::max(1, max_items);
    for (int i = 0; i < count; ++i) {
        wchar_t text[128]{};
        const int copied = GetMenuStringW(bar, static_cast<UINT>(i), text,
                                          static_cast<int>(std::size(text)), MF_BYPOSITION);
        if (copied <= 0) continue;
        snapshot.titles.push_back(StripAccelerator(text));
        snapshot.submenus.push_back(GetSubMenu(bar, i));
    }
    if (static_cast<int>(snapshot.titles.size()) > limit) {
        // Keep the first `limit` and let the layout decide whether they fit. The rest stay
        // reachable through the overflow box, which re-reads the full list on click.
        snapshot.titles.resize(static_cast<std::size_t>(limit));
        snapshot.submenus.resize(static_cast<std::size_t>(limit));
    }
    return snapshot;
}

// ---- FinderBar ---------------------------------------------------------------

FinderBar::FinderBar() = default;

FinderBar::~FinderBar() { Shutdown(); }

bool FinderBar::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    // No CS_HREDRAW/CS_VREDRAW: the pixels come from DComp, so invalidating on resize would
    // only produce a WM_PAINT that draws nothing.
    wc.style = 0;
    wc.lpfnWndProc = &FinderBar::StaticWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kBarClass;
    const ATOM atom = RegisterClassExW(&wc);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        DF_ERROR(kModule, "RegisterClassExW failed 0x%08lX", GetLastError());
        return false;
    }
    return true;
}

void FinderBar::SetSystemCommandHandler(SystemCommandHandler handler) {
    system_command_ = std::move(handler);
}

bool FinderBar::Initialize(HINSTANCE instance, const core::Config& config) {
    if (ready_) return true;
    instance_ = instance;
    config_ = config.finderbar;

    platform::DisplayInfo display;
    if (!platform::PrimaryDisplay(display)) {
        DF_WARN(kModule, "no primary display, Finder Bar unavailable");
        return false;
    }
    // The bar follows the primary display only. Per-monitor bars are a T-203 concern, and a
    // bar on a secondary display would duplicate a system menu it does not own.
    const UINT override_dpi = platform::Window::DpiOverride();
    if (override_dpi != 0) display.effective_dpi = override_dpi;

    // Style first: CreateWindowFor decides the capture exclusion from it, and a default
    // PlateStyle would make that decision on a tier that is not the one being drawn.
    style_ = BarPlateStyle();
    if (!CreateWindowFor(display)) return false;

    // The bar spans the display edge to edge, so a rounded plate would show the desktop
    // through its corners at the top of the screen. Radius 0 is the macOS menu-bar shape.
    DF_INFO(kModule, "Finder Bar ready display=%s dpi=%u height_dip=%.1f tier=%d",
            core::WideToUtf8(display.device).c_str(), display.effective_dpi,
            static_cast<double>(height_dip_), static_cast<int>(style_.tier));

    // SetWinEventHook reports foreground changes from any process. Spike Q4 measured 5.5ms
    // average and 8.2ms worst, well inside the 100ms budget C-32 sets.
    foreground_hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                       &FinderBar::WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (foreground_hook_ == nullptr) {
        // Without the hook the bar still works: Tick falls back to polling. Log it rather
        // than degrade silently.
        DF_WARN(kModule, "SetWinEventHook failed 0x%08lX, falling back to polling",
                GetLastError());
    }

    ready_ = true;
    g_active_bar = this;

    // The foreground window at startup never raises an event, so take the snapshot now.
    foreground_dirty_ = true;
    RefreshForeground();
    Relayout();

    SetEnabled(config_.enabled);
    return true;
}

bool FinderBar::CreateWindowFor(const platform::DisplayInfo& display) {
    display_ = display;
    dpi_ = display.effective_dpi ? display.effective_dpi : 96;
    width_dip_ = platform::PixelsToDip(display.monitor.right - display.monitor.left, dpi_);
    height_dip_ = static_cast<float>(config_.height);
    width_px_ = display.monitor.right - display.monitor.left;
    height_px_ = std::max(1, platform::DipToPixels(height_dip_, dpi_));

    window_ = CreateWindowExW(kBarExStyle, kBarClass, L"DockForge Finder Bar", kBarStyle,
                              display.monitor.left, display.monitor.top, width_px_, height_px_,
                              nullptr, nullptr, instance_, this);
    if (window_ == nullptr) {
        DF_ERROR(kModule, "CreateWindowExW failed 0x%08lX", GetLastError());
        return false;
    }

    if (!surface_.Initialize(window_)) {
        DF_ERROR(kModule, "CompositionSurface::Initialize failed");
        DestroyWindowAndSurface();
        return false;
    }
    if (!surface_.Resize(width_px_, height_px_)) {
        DF_ERROR(kModule, "CompositionSurface::Resize(%dx%d) failed", width_px_, height_px_);
        DestroyWindowAndSurface();
        return false;
    }

    backend_ = render::CreateD2DBackend();
    if (!backend_ || !backend_->Initialize()) {
        DF_ERROR(kModule, "D2D backend initialization failed");
        DestroyWindowAndSurface();
        return false;
    }

    // Keep the bar out of its own backdrop capture, but only when the plate actually
    // samples that capture. Without the exclusion the frost samples the plate it is about
    // to draw and settles into a flat wash (T-005/T-076 lesson); with it, the bar is also
    // invisible to every screenshot, screen recording and streaming tool on the machine.
    // So it is applied only for the tiers that need it - the solid tier draws a flat tint
    // and has nothing to protect.
    if (style_.use_capture) {
        SetWindowDisplayAffinity(window_, WDA_EXCLUDEFROMCAPTURE);
    } else {
        SetWindowDisplayAffinity(window_, WDA_NONE);
    }
    capture_.Start(window_, display.monitor.left, display.monitor.top, width_px_, height_px_);
    DF_DEBUG(kModule, "bar window %dx%d px at %ld,%ld", width_px_, height_px_,
             display.monitor.left, display.monitor.top);
    return true;
}

void FinderBar::DestroyWindowAndSurface() {
    capture_.Stop();
    if (menu_thread_ != nullptr) {
        // A menu left running would keep its own window alive and outlive the bar. Waiting
        // is bounded: TrackPopupMenuEx returns as soon as the owner window is destroyed.
        if (g_menu_job.load() != nullptr && window_ != nullptr) {
            PostMessageW(window_, kMsgMenuClosed, 0, 0);
        }
        WaitForSingleObject(menu_thread_, 1500);
        CloseHandle(menu_thread_);
        menu_thread_ = nullptr;
    }
    menu_running_ = false;
    if (backend_) {
        backend_->Shutdown();
        backend_.reset();
    }
    plate_.Reset();
    surface_.Shutdown();
    backdrop_pixels_.clear();
    backdrop_generation_ = 0;
    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
}

void FinderBar::Shutdown() {
    if (!ready_ && window_ == nullptr) return;
    if (foreground_hook_ != nullptr) {
        UnhookWinEvent(foreground_hook_);
        foreground_hook_ = nullptr;
    }
    if (g_active_bar == this) g_active_bar = nullptr;
    DestroyWindowAndSurface();
    ready_ = false;
    enabled_ = false;
}

void FinderBar::SetEnabled(bool enabled) {
    enabled_ = enabled;
    if (window_ == nullptr) return;
    if (!enabled) {
        capture_.Stop();
        // A hidden window keeps its region, which would still be a hit target. Reset it to
        // empty before hiding so nothing is left behind if the window is shown again by
        // another path.
        HRGN empty = CreateRectRgn(0, 0, 0, 0);
        if (empty != nullptr && SetWindowRgn(window_, empty, FALSE) == 0) DeleteObject(empty);
        region_height_px_ = 0;
        ShowWindow(window_, SW_HIDE);
        return;
    }

    SetWindowPos(window_, HWND_TOPMOST, display_.monitor.left, display_.monitor.top, width_px_,
                 height_px_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    capture_.Start(window_, display_.monitor.left, display_.monitor.top, width_px_, height_px_);
    // auto_hide starts retracted so the bar never appears on top of a maximized window's
    // title bar before the user asks for it; without auto_hide it is always out.
    auto_hide_ = AutoHideState{};
    auto_hide_.progress = config_.auto_hide ? 0.0f : 1.0f;
    auto_hide_.shown = !config_.auto_hide;
    region_height_px_ = -1;
    ApplyRegion();
    Repaint();
}

void FinderBar::ReloadConfig(const core::Config& config) {
    const bool was_enabled = config_.enabled;
    config_ = config.finderbar;

    style_ = BarPlateStyle();
    if (window_ != nullptr) {
        // The exclusion is tied to the tier, so a material change has to move it too.
        SetWindowDisplayAffinity(window_,
                                 style_.use_capture ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
    }

    const float new_height = static_cast<float>(config_.height);
    if (std::abs(new_height - height_dip_) > 0.01f) {
        height_dip_ = new_height;
        height_px_ = std::max(1, platform::DipToPixels(height_dip_, dpi_));
        if (window_ != nullptr) {
            SetWindowPos(window_, HWND_TOPMOST, display_.monitor.left, display_.monitor.top,
                         width_px_, height_px_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            surface_.Resize(width_px_, height_px_);
            capture_.SetRegion(display_.monitor.left, display_.monitor.top, width_px_, height_px_);
            region_height_px_ = -1;
        }
    }

    if (was_enabled != config_.enabled) {
        SetEnabled(config_.enabled);
        return;
    }
    if (!enabled_) return;

    // The menu list depends on max_menu_items, so a change there needs a fresh read.
    if (config_.show_app_menu) {
        menu_ = ReadAppMenu(foreground_.hwnd, config_.max_menu_items);
        foreground_.menu_readable = menu_.readable;
    }
    Relayout();
    ApplyRegion();
    Repaint();
}

void FinderBar::SetForegroundOverride(HWND hwnd) {
    foreground_override_ = hwnd;
    foreground_dirty_ = true;
    DF_INFO(kModule, "foreground override -> %p", static_cast<void*>(hwnd));
}

void FinderBar::RefreshForeground() {
    foreground_dirty_ = false;
    const HWND hwnd =
        foreground_override_ != nullptr ? foreground_override_ : GetForegroundWindow();
    if (hwnd == foreground_.hwnd && hwnd != nullptr) return;

    foreground_ = ForegroundApp{};
    menu_ = AppMenuSnapshot{};
    if (hwnd == nullptr) return;

    // Our own windows are never "the application in front": the bar holds focus only while
    // a menu is open, and showing "DockForge" there would replace the user's app name with
    // our own.
    if (hwnd == window_) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return;

    foreground_.hwnd = hwnd;
    foreground_.pid = pid;
    foreground_.display_name = ResolveAppDisplayName(hwnd);
    if (config_.show_app_menu) menu_ = ReadAppMenu(hwnd, config_.max_menu_items);
    foreground_.menu_readable = menu_.readable;
    DF_DEBUG(kModule, "foreground %p pid=%lu name=%s menus=%zu", static_cast<void*>(hwnd), pid,
             core::WideToUtf8(foreground_.display_name).c_str(), menu_.titles.size());
}

void FinderBar::Relayout() {
    BarLayoutParams params;
    params.width_dip = width_dip_;
    params.height_dip = height_dip_;
    params.show_system_menu = config_.show_system_menu;
    params.show_app_name = config_.show_app_name;
    params.show_app_menu = config_.show_app_menu;
    params.max_menu_items = config_.max_menu_items;
    // Nothing is reserved at the trailing edge yet: T-302 owns that area and will pass a
    // width once it exists. Reserving space for widgets that do not exist would leave a
    // visible gap at the right edge for no reason.
    params.status_width_dip = 0.0f;
    params.app_name = foreground_.display_name;

    boxes_ = LayoutBar(params, menu_.titles, &MeasureBarText, &layout_);
}

void FinderBar::ApplyRegion() {
    if (window_ == nullptr || !enabled_) return;

    // The region is both the click target and the clip for what the bar draws (a DComp
    // surface is clipped by SetWindowRgn exactly like GDI content is). So its height has
    // to match the part of the plate the geometry leaves on screen - otherwise the
    // retracted bar's hairline is laid out, offset into view and then silently cut away.
    //
    // A fully retracted bar keeps only the hairline: enough to see that the bar is there,
    // not enough to steal a click aimed at a maximized window's title bar (spike Q7
    // measured that a 28dip strip does steal it, and Q8 measured that reserving work area
    // to avoid the overlap is not possible on this OS).
    const bool interactive = !config_.auto_hide || AutoHideInteractive(auto_hide_);
    const int wanted =
        interactive ? height_px_
                    : std::max(1, platform::DipToPixels(kAutoHideHairlineDip, dpi_));
    if (wanted == region_height_px_) return;

    HRGN region = CreateRectRgn(0, 0, width_px_, wanted);
    if (region == nullptr) return;
    // SetWindowRgn takes ownership of the region on success.
    if (SetWindowRgn(window_, region, TRUE) == 0) {
        DeleteObject(region);
        return;
    }
    region_height_px_ = wanted;
}

float FinderBar::OffsetDip() const {
    if (!config_.auto_hide) return 0.0f;
    return AutoHideOffsetDip(auto_hide_, height_dip_);
}

void FinderBar::Repaint() {
    if (window_ == nullptr || !surface_.IsValid() || !backend_) return;

    const float offset = OffsetDip();
    const float width = width_dip_;
    const float height = height_dip_;

    // The backdrop is the desktop behind the bar. Capture runs on its own thread, so this is
    // a non-blocking read of the newest frame; no frame yet means the plate falls back to
    // its tint, which is the Solid tier and still correct (C-01 graceful degrade).
    const std::uint8_t* backdrop = nullptr;
    int backdrop_w = 0;
    int backdrop_h = 0;
    if (style_.use_capture) {
        std::uint64_t generation = 0;
        if (capture_.AcquireIfNew(backdrop_generation_, backdrop_pixels_, backdrop_w, backdrop_h,
                                  generation)) {
            backdrop_generation_ = generation;
        }
        if (!backdrop_pixels_.empty()) backdrop = backdrop_pixels_.data();
    }

    const bool light = SystemUsesLightTheme();
    const render::ColorF text_color = light ? kTextDark : kTextLight;
    const render::ColorF hover_color = light ? kHoverDark : kHoverLight;
    const render::ColorF glyph_color = light ? kGlyphDark : kGlyphLight;

    const bool painted =
        surface_.Paint(dpi_, [&](ID2D1DeviceContext& dc,
                                 const platform::CompositionSurface::PaintInfo&) {
            backend_->BeginFrame(dc, static_cast<float>(dpi_));

            const render::RectF plate{0.0f, offset, width, offset + height};
            plate_.Draw(dc, *backend_, plate, 0.0f, style_, backdrop, backdrop_w, backdrop_h,
                        static_cast<float>(dpi_));

            for (std::size_t i = 0; i < boxes_.size(); ++i) {
                const BarBox& box = boxes_[i];
                const render::RectF rect{box.rect.left, box.rect.top + offset, box.rect.right,
                                         box.rect.bottom + offset};
                if (box.is_system_menu) {
                    // Drawn, not typed: the Apple mark is in no font we can ship and
                    // shipping it is a trademark problem (ASSETS.md). Three stacked bars
                    // read as a menu affordance at 14 DIP without imitating the mark.
                    const float size = kSystemMenuGlyphDip;
                    const float cx = rect.left + kMenuTitlePaddingDip * 0.5f + size * 0.5f;
                    const float cy = rect.CenterY();
                    const float bar_height = std::max(1.5f, size * 0.14f);
                    const float gap = size * 0.20f;
                    const float widths[3] = {size, size * 0.72f, size * 0.86f};
                    for (int bar = 0; bar < 3; ++bar) {
                        const float top = cy - (bar_height * 1.5f + gap) +
                                          static_cast<float>(bar) * (bar_height + gap);
                        const render::RectF line{cx - widths[bar] * 0.5f, top,
                                                 cx + widths[bar] * 0.5f, top + bar_height};
                        backend_->FillRoundedRect(line, bar_height * 0.5f, glyph_color);
                    }
                    continue;
                }
                if (static_cast<int>(i) == hover_box_ && !menu_running_) {
                    backend_->FillRoundedRect(rect, 5.0f, hover_color);
                }
                backend_->DrawLabel(box.label.c_str(), rect, kMenuFontDip, text_color,
                                    render::TextAlign::Center);
            }

            backend_->EndFrame();
        });

    if (!painted) {
        DF_WARN(kModule, "surface paint failed");
        return;
    }
    surface_.Commit();
    ++paints_;
}

bool FinderBar::Tick(double delta_ms, const core::Config& config) {
    if (!ready_ || window_ == nullptr || !enabled_) return false;

    // A config change normally arrives through ReloadConfig, but the foreground name and the
    // menu list also depend on finderbar keys, so a cheap identity check catches the case
    // where a caller forgot.
    if (config.finderbar.max_menu_items != config_.max_menu_items ||
        config.finderbar.show_app_menu != config_.show_app_menu ||
        config.finderbar.height != config_.height) {
        ReloadConfig(config);
    }

    // Foreground changes normally arrive through the hook. When the hook could not be
    // installed, poll instead: a bar that silently stops tracking the app in front is worse
    // than one that polls a single cheap call. A pinned override is never polled - it is
    // the answer by definition, and comparing it against the real foreground would mark
    // the bar dirty on every single frame.
    if (foreground_hook_ == nullptr && foreground_override_ == nullptr) {
        const HWND current = GetForegroundWindow();
        if (current != foreground_.hwnd) foreground_dirty_ = true;
    }
    // Snapshot before refreshing: RefreshForeground clears the flag, so reading it afterwards
    // always yields false and the frame that just rebuilt the layout is never painted. That
    // produced a bar whose state JSON listed four menu titles while the screen still showed
    // the previous application's name and no menus at all.
    const bool foreground_changed = foreground_dirty_;
    if (foreground_changed) {
        RefreshForeground();
        Relayout();
    }

    // Pointer tracking is polled rather than event driven: a retracted bar has an empty
    // region, so Windows sends it no mouse messages at all.
    POINT cursor{};
    GetCursorPos(&cursor);
    const float cursor_x_dip = platform::PixelsToDip(cursor.x - display_.monitor.left, dpi_);
    const float cursor_y_dip = platform::PixelsToDip(cursor.y - display_.monitor.top, dpi_);
    pointer_in_trigger_ = cursor_y_dip >= 0.0f && cursor_y_dip <= kTriggerHeightDip &&
                          cursor_x_dip >= 0.0f && cursor_x_dip <= width_dip_;
    const float offset = OffsetDip();
    pointer_in_bar_ = cursor_y_dip >= offset && cursor_y_dip <= offset + height_dip_ &&
                      cursor_x_dip >= 0.0f && cursor_x_dip <= width_dip_;

    const AutoHideState before = auto_hide_;
    if (config_.auto_hide) {
        // An open menu pins the bar out: retracting under a menu would move the click target
        // while the user is aiming at it.
        const bool pin = menu_running_;
        AdvanceAutoHide(auto_hide_, pointer_in_trigger_ || pin, pointer_in_bar_ || pin, delta_ms);
    } else {
        auto_hide_.progress = 1.0f;
        auto_hide_.shown = true;
    }

    const int previous_hover = hover_box_;
    hover_box_ = -1;
    if (pointer_in_bar_ && AutoHideInteractive(auto_hide_) && !menu_running_) {
        const std::size_t index = BoxAtDip(cursor_x_dip, cursor_y_dip - offset);
        if (index != kNoMenu) hover_box_ = static_cast<int>(index);
    }

    const bool moved = std::abs(before.progress - auto_hide_.progress) > 0.0005f;
    const bool hover_changed = previous_hover != hover_box_;
    if (!moved && !hover_changed && !foreground_changed) return false;

    if (moved) ApplyRegion();
    Repaint();
    return true;
}

std::size_t FinderBar::BoxAtDip(float x_dip, float y_dip) const {
    return BoxIndexAt(boxes_.data(), boxes_.size(), x_dip, y_dip);
}

void FinderBar::OnClick(float x_dip, float y_dip) {
    if (menu_running_) return;
    const std::size_t index = BoxAtDip(x_dip, y_dip);
    if (index == kNoMenu) return;
    const BarBox& box = boxes_[index];
    if (box.is_system_menu) {
        OpenSystemMenu(index);
    } else if (box.is_app_name) {
        // macOS opens a menu of the application's own windows here. That belongs to T-204
        // (window list); until then the name is a label, not a button.
        return;
    } else if (box.is_overflow) {
        OpenOverflowMenu(index);
    } else {
        OpenAppMenu(index);
    }
}

bool FinderBar::OpenMenuForBox(std::size_t box_index) {
    if (!ready_ || window_ == nullptr || !enabled_) return false;
    if (menu_running_) return false;
    if (box_index >= boxes_.size()) return false;
    OnClick(boxes_[box_index].rect.left + 1.0f,
            boxes_[box_index].rect.top + height_dip_ * 0.5f);
    return menu_running_;
}

bool FinderBar::RunSystemCommand(int command_id) {
    const int index = command_id - static_cast<int>(kSystemIdBase);
    if (index <= 0 || index > static_cast<int>(SystemCommand::Quit)) return false;
    HandleSystemCommand(command_id);
    return true;
}

bool FinderBar::ForwardMenuItem(std::size_t box_index, int item) {
    if (!ready_ || !enabled_) return false;
    if (box_index >= boxes_.size()) return false;
    const BarBox& box = boxes_[box_index];
    if (box.menu_index == kNoMenu || box.menu_index >= menu_.submenus.size()) return false;
    HMENU submenu = menu_.submenus[box.menu_index];
    if (submenu == nullptr) return false;
    const int count = GetMenuItemCount(submenu);
    if (item < 0 || item >= count) return false;
    // GetMenuItemID returns -1 for separators and for items that open a submenu; both
    // are real states a user can click, and neither carries a command to forward.
    const UINT id = static_cast<UINT>(GetMenuItemID(submenu, item));
    if (id == static_cast<UINT>(-1) || id == 0) {
        DF_DEBUG(kModule, "menu item %d of box %zu has no command id", item, box_index);
        return false;
    }
    if (foreground_.hwnd == nullptr) return false;
    // C-32's forwarding rule, verbatim: no injection, no hooking, just the message the
    // application's own menu would have received.
    const BOOL posted =
        PostMessageW(foreground_.hwnd, WM_COMMAND, MAKEWPARAM(id, 0), 0);
    DF_INFO(kModule, "forwarded WM_COMMAND %u to %p posted=%d", id,
            static_cast<void*>(foreground_.hwnd), posted ? 1 : 0);
    return posted != FALSE;
}

void FinderBar::StartMenuJob(HMENU menu, HWND target, bool owns_menu, int x, int y) {
    if (menu == nullptr) return;
    if (menu_thread_ != nullptr) {
        // A previous job finished; reap it so the handle does not leak.
        CloseHandle(menu_thread_);
        menu_thread_ = nullptr;
    }

    // The job must outlive this call: the thread reads it until TrackPopupMenuEx returns.
    // One menu at a time is a menu-bar invariant, so a single slot is enough. It cannot be a
    // member because MenuJob is private to this translation unit, and it is not reset by
    // whole-struct assignment because std::atomic is not assignable.
    static MenuJob job;
    job.bar = window_;
    job.target = target;
    job.menu = menu;
    job.owns_menu = owns_menu;
    job.x = x;
    job.y = y;
    job.selected.store(0);
    job.done.store(false);

    g_menu_job.store(&job);
    menu_running_ = true;
    menu_thread_ = CreateThread(nullptr, 0, &MenuThreadProc, &job, 0, nullptr);
    if (menu_thread_ == nullptr) {
        menu_running_ = false;
        g_menu_job.store(nullptr);
        if (owns_menu) DestroyMenu(menu);
        DF_ERROR(kModule, "CreateThread for the menu failed 0x%08lX", GetLastError());
        return;
    }
    ++menu_opens_;
    Repaint();
}

void FinderBar::OpenAppMenu(std::size_t box_index) {
    if (box_index >= boxes_.size()) return;
    const BarBox& box = boxes_[box_index];
    if (box.menu_index == kNoMenu || box.menu_index >= menu_.submenus.size()) return;
    HMENU submenu = menu_.submenus[box.menu_index];
    if (submenu == nullptr) return;

    const float scale = Scale();
    const int x = display_.monitor.left + static_cast<int>(box.rect.left * scale);
    const int y = display_.monitor.top + height_px_;
    DF_DEBUG(kModule, "opening menu '%s' for %p at %d,%d",
             core::WideToUtf8(box.label).c_str(), static_cast<void*>(foreground_.hwnd), x, y);
    // The submenu belongs to the application: it is handed to the system, never destroyed.
    StartMenuJob(submenu, foreground_.hwnd, false, x, y);
}

void FinderBar::OpenSystemMenu(std::size_t box_index) {
    if (box_index >= boxes_.size()) return;
    const BarBox& box = boxes_[box_index];

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;
    AppendMenuW(menu, MF_STRING, kSystemIdBase + static_cast<UINT>(SystemCommand::About),
                L"About DockForge");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kSystemIdBase + static_cast<UINT>(SystemCommand::Settings),
                L"Settings\x2026");
    AppendMenuW(menu, MF_STRING,
                kSystemIdBase + static_cast<UINT>(SystemCommand::OpenConfigFolder),
                L"Open Config Folder");
    AppendMenuW(menu, MF_STRING, kSystemIdBase + static_cast<UINT>(SystemCommand::OpenLogs),
                L"Open Logs");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kSystemIdBase + static_cast<UINT>(SystemCommand::Quit),
                L"Quit DockForge");

    const float scale = Scale();
    const int x = display_.monitor.left + static_cast<int>(box.rect.left * scale);
    const int y = display_.monitor.top + height_px_;
    StartMenuJob(menu, window_, true, x, y);
}

void FinderBar::OpenOverflowMenu(std::size_t box_index) {
    if (box_index >= boxes_.size()) return;
    const BarBox& box = boxes_[box_index];

    // The titles past max_menu_items are not in `menu_` because ReadAppMenu truncated it.
    // The overflow box re-reads the full list: it is a rare click, and a second read is
    // cheaper than keeping two parallel copies in sync.
    const AppMenuSnapshot full = ReadAppMenu(foreground_.hwnd, 64);
    if (full.submenus.size() <= menu_.submenus.size()) return;

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;
    for (std::size_t i = menu_.submenus.size(); i < full.submenus.size(); ++i) {
        if (full.submenus[i] == nullptr) continue;
        // A submenu entry is handed to the system, not copied, so the application's menu
        // stays live. Destroying this wrapper would destroy those with it, which is why
        // owns_menu stays false for the overflow menu.
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(full.submenus[i]),
                    full.titles[i].c_str());
    }

    const float scale = Scale();
    const int x = display_.monitor.left + static_cast<int>(box.rect.left * scale);
    const int y = display_.monitor.top + height_px_;
    StartMenuJob(menu, foreground_.hwnd, false, x, y);
}

void FinderBar::HandleSystemCommand(int command_id) {
    const int index = command_id - static_cast<int>(kSystemIdBase);
    if (index <= 0 || index > static_cast<int>(SystemCommand::Quit)) return;
    const auto command = static_cast<SystemCommand>(index);
    DF_INFO(kModule, "system menu command %d", index);
    if (system_command_) system_command_(command);
}

void FinderBar::OnMenuClosed() {
    // The menu thread only posts; every surface call stays on this thread.
    menu_running_ = false;
    MenuJob* job = g_menu_job.exchange(nullptr);
    if (job != nullptr) {
        const unsigned selected = job->selected.load();
        if (job->target == window_ && selected >= kSystemIdBase) {
            HandleSystemCommand(static_cast<int>(selected));
        }
    }
    if (menu_thread_ != nullptr) {
        CloseHandle(menu_thread_);
        menu_thread_ = nullptr;
    }
    Repaint();
}

LRESULT CALLBACK FinderBar::StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    auto* self = reinterpret_cast<FinderBar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) return DefWindowProcW(hwnd, msg, wparam, lparam);
    return self->WndProc(hwnd, msg, wparam, lparam);
}

LRESULT FinderBar::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            // Take the click: the bar is the target, not a decoration over the app.
            return MA_ACTIVATE;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            // The pixels come from DComp; validating the update region is all that is
            // required, otherwise Windows repaints forever.
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            const float scale = Scale();
            const float x = static_cast<float>(GET_X_LPARAM(lparam)) / scale;
            const float y = static_cast<float>(GET_Y_LPARAM(lparam)) / scale;
            OnClick(x, y - OffsetDip());
            return 0;
        }

        case kMsgMenuClosed:
            OnMenuClosed();
            return 0;

        case WM_DISPLAYCHANGE: {
            platform::DisplayInfo display;
            if (platform::PrimaryDisplay(display)) {
                const UINT override_dpi = platform::Window::DpiOverride();
                if (override_dpi != 0) display.effective_dpi = override_dpi;
                display_ = display;
                dpi_ = display.effective_dpi ? display.effective_dpi : 96;
                width_dip_ =
                    platform::PixelsToDip(display.monitor.right - display.monitor.left, dpi_);
                width_px_ = display.monitor.right - display.monitor.left;
                height_px_ = std::max(1, platform::DipToPixels(height_dip_, dpi_));
                SetWindowPos(hwnd, HWND_TOPMOST, display.monitor.left, display.monitor.top,
                             width_px_, height_px_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
                surface_.Resize(width_px_, height_px_);
                capture_.SetRegion(display.monitor.left, display.monitor.top, width_px_,
                                   height_px_);
                region_height_px_ = -1;
                Relayout();
                ApplyRegion();
                Repaint();
            }
            return 0;
        }

        case WM_DESTROY:
            // Deliberately no PostQuitMessage: the bar is destroyed when the user turns it
            // off, and quitting the process there would look like a crash.
            window_ = nullptr;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void CALLBACK FinderBar::WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG, LONG, DWORD,
                                      DWORD) {
    if (event != EVENT_SYSTEM_FOREGROUND) return;
    FinderBar* bar = g_active_bar;
    if (bar == nullptr) return;
    // The hook fires on the thread that installed it (WINEVENT_OUTOFCONTEXT), so touching
    // the bar here is safe; it only marks the snapshot stale and lets Tick do the work.
    if (hwnd != bar->foreground_.hwnd) bar->foreground_dirty_ = true;
    ++bar->foreground_events_;
}

std::string FinderBar::SnapshotJson() const {
    nlohmann::json j;
    j["ready"] = ready_;
    j["enabled"] = enabled_;
    j["menu_open"] = menu_running_;
    j["auto_hide"] = config_.auto_hide;
    j["progress"] = auto_hide_.progress;
    j["offset_dip"] = OffsetDip();
    j["interactive"] = !config_.auto_hide || AutoHideInteractive(auto_hide_);
    j["width_dip"] = width_dip_;
    j["height_dip"] = height_dip_;
    j["dpi"] = dpi_;
    j["window"] = reinterpret_cast<std::uintptr_t>(window_);
    j["paints"] = paints_;
    j["menu_opens"] = menu_opens_;
    j["foreground_events"] = foreground_events_;
    j["foreground_hwnd"] = reinterpret_cast<std::uintptr_t>(foreground_.hwnd);
    j["foreground_name"] = core::WideToUtf8(foreground_.display_name);
    j["menu_readable"] = foreground_.menu_readable;
    j["hover_box"] = hover_box_;

    nlohmann::json boxes = nlohmann::json::array();
    for (const BarBox& box : boxes_) {
        boxes.push_back(
            {{"label", core::WideToUtf8(box.label)},
             {"left", box.rect.left},
             {"right", box.rect.right},
             {"system", box.is_system_menu},
             {"name", box.is_app_name},
             {"overflow", box.is_overflow},
             {"menu_index", box.menu_index == kNoMenu ? -1 : static_cast<int>(box.menu_index)}});
    }
    j["boxes"] = boxes;

    j["menu_titles"] = nlohmann::json::array();
    for (const std::wstring& title : menu_.titles) {
        j["menu_titles"].push_back(core::WideToUtf8(title));
    }
    j["layout"] = {{"placed_menus", layout_.placed_menus},
                   {"overflowed", layout_.overflowed},
                   {"left_group_width_dip", layout_.left_group_width_dip},
                   {"status_start_dip", layout_.status_start_dip},
                   {"clipped", layout_.clipped}};
    return j.dump();
}

}  // namespace dockforge::finderbar
