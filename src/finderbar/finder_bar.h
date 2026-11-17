// DockForge - the Finder Bar (T-301, CONTRACTS.md C-31/C-32).
//
// One persistent window pinned to the top edge of the primary display, showing the
// system menu, the foreground application's name and its menu titles. The layout
// arithmetic lives in bar_layout.h and is unit tested; this file owns the parts that
// need a desktop: the window, the surface, the menu plumbing and the foreground
// watcher.
//
// Four measured facts shape the design, all from evidence/T-301:
//
//   * The work area cannot be reserved. SPI_SETWORKAREA returns success on Windows 11
//     and explorer puts it straight back, so a maximized window still runs under the
//     bar. The bar therefore retracts to nothing and only occupies the top strip while
//     the pointer is there (spike Q8).
//   * A topmost window eats every click in its rectangle unless it is layered, and a
//     layered window cannot use WS_EX_NOREDIRECTIONBITMAP, which DirectComposition
//     requires. The bar instead clips itself with SetWindowRgn: the region decides hit
//     testing, and DirectComposition keeps painting the material (spike Q7).
//   * A tracked menu only dismisses itself on an outside click when its owner holds the
//     foreground. When another process owns the foreground SetForegroundWindow is
//     refused and the menu becomes stuck, so the bar always claims foreground on the
//     menu thread and closes the menu itself if that fails (spike Q6/Q7/Q8).
//   * A tracked menu can be pumped from a worker thread without stalling the frame
//     loop, and the command id it returns can be forwarded as WM_COMMAND (Q9/Q12).
//
// The menu itself is drawn by Windows. C-32 only needs the top-level titles and the
// command id, so the bar hands the foreign HMENU to TrackPopupMenuEx and posts the
// resulting WM_COMMAND back to the application. Checkmarks, accelerators, separators
// and submenus are native for free.
#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/config.h"
#include "dock/weather.h"
#include "dock/weather_popup.h"
#include "finderbar/bar_layout.h"
#include "finderbar/bar_widgets.h"
#include "finderbar/calendar_panel.h"
#include "finderbar/hardware_panel.h"
#include "finderbar/status_popup.h"
#include "finderbar/status_sampler.h"
#include "platform/composition.h"
#include "platform/display.h"
#include "render/backend.h"
#include "render/backdrop_capture.h"
#include "render/material.h"

namespace dockforge::finderbar {

// What the bar knows about the application in front. Rebuilt only when the foreground
// window changes: resolving the name costs ~1.4ms and reading the menu is a handful of
// GetMenuItemInfo calls, both far inside the 100ms budget C-32 sets, but doing it every
// frame would be waste (spike Q6).
struct ForegroundApp {
    HWND hwnd = nullptr;
    std::uint32_t pid = 0;
    std::wstring exe_path;
    std::wstring display_name;
    bool menu_readable = false;
};

// One read of a window's menu bar. `submenus` is parallel to `titles` so a click can pop
// the right dropdown without re-reading the menu.
struct AppMenuSnapshot {
    HMENU menu = nullptr;
    std::vector<std::wstring> titles;
    std::vector<HMENU> submenus;
    bool readable = false;
};

// Commands the system menu can raise. An enum rather than magic ids so main.cpp cannot
// silently mismatch a number.
enum class SystemCommand {
    None = 0,
    About = 1,
    Settings = 2,
    OpenConfigFolder = 3,
    OpenLogs = 4,
    Quit = 5,
};

// First id the system menu hands to TrackPopupMenuEx. Exposed because main.cpp's probe
// channel raises a command by id, and a second copy of this number is exactly the kind
// of silent mismatch the enum exists to prevent.
constexpr int kSystemMenuBase = 100;

// First id of the control-centre button's right-click minimal menu (T-304,
// finderbar-parity I2). Disjoint from kSystemMenuBase so OnMenuClosed can route
// the two ranges apart; the ids index CcSettingsPage (hardware_panel.h).
constexpr int kCcMenuBase = 200;

// The foreground window's application name.
//
// Precedence: the executable's FileDescription, then the file name without extension,
// then the window title. FileDescription is what a user sees in Task Manager and what
// macOS shows for an app bundle, so it is the closest equivalent (spike Q6).
std::wstring ResolveAppDisplayName(HWND hwnd);

// The same resolution for a path, exposed because a test can hand it a path without
// needing a live process.
std::wstring DisplayNameForExecutable(const std::wstring& exe_path);

// File name without extension, used when a binary carries no version resource.
std::wstring FallbackNameFromPath(const std::wstring& exe_path);

// Strips the accelerator marker the bar never shows. "&File" becomes "File"; "&&" stays
// a literal ampersand. Win32 menu titles almost always carry one, and drawing it would
// make the bar look like a Win32 app rather than a menu bar.
std::wstring StripAccelerator(const std::wstring& title);

// Decodes a menu title that a program draws itself.
//
// Modern applications (Sublime Text, VS Code, and anything else with a custom-drawn menu
// bar) put an MFT_OWNERDRAW item in the top-level menu and leave the item's text empty.
// GetMenuStringW therefore returns nothing and the bar used to fall back to showing the
// application name alone. spikes/T-301_finderbar_menu/menu_spike13.cpp found where the
// title actually lives: dwItemData points into the target process at a NUL-separated
// array of wide-character titles, and the pointer for item i lands exactly on the first
// byte of title i. This decodes that buffer, so it is a pure function over bytes and can
// be unit-tested without a live process.
//
// The decoder is deliberately strict: it stops at the first NUL, rejects a buffer with no
// terminator, rejects control characters, and rejects anything that is not a plausible
// menu title. Reading another process's memory is not a place to be optimistic, and a
// wrong title is worse than the application-name fallback it replaces.
std::wstring DecodeOwnerDrawnTitle(const std::uint8_t* bytes, std::size_t size);

// Reads at most `max_items` top-level entries. Returns readable=false for the majority of
// modern applications, which have no Win32 menu at all (spike Q3: 11 visible windows, 2
// with menus, 0 of them explorer or Chrome). The bar treats that as the normal case, not
// a failure.
AppMenuSnapshot ReadAppMenu(HWND hwnd, int max_items);

// Measures one label the way the renderer will draw it. Exposed so a probe can compare
// the bar's numbers against the real text metrics.
float MeasureBarText(const wchar_t* text, float size_dip);

// ---------------------------------------------------------------------------
// Right region, clock half (T-307, finderbar.clock.* - ADR-0053 records the new
// config keys).
//
// Format-string grammar: Windows format pictures, the same grammar GetTimeFormatEx
// and GetDateFormatEx speak and the very thing LOCALE_SSHORTTIME / LOCALE_SDATE
// return. An empty finderbar.clock.format therefore follows the system region with
// no parsing at all; a non-empty one is handed to GetTimeFormatEx verbatim.
//
// T-207 对接裁决（TASKS.md T-207「数字式格式串」；对接点注释在 config.h
// FinderBarClockConfig，本函数是实现侧的镜像）：T-207 实装前仅 finderbar.clock.*
// 生效；实装后 themes/<name>/calendar.json 的视觉字段（字体/大小/颜色）覆盖
// finderbar.clock.*，行为字段（格式串/显隐日期/显隐秒）仍以 finderbar.clock.* 为准。
// ---------------------------------------------------------------------------

// One rendered clock label: `date` ("2026/11/18" per LOCALE_SDATE) and `time`
// ("17:27" per LOCALE_SSHORTTIME or the user's picture). Either part may be empty
// per the show_date flag; the caller joins them with a single space, date first -
// macOS's menu bar reads "周六 11月18日 17:27".
struct BarClockText {
    std::wstring date;
    std::wstring time;
};

// Renders the clock for `now`. `time_picture` / `date_picture` are injected so a test
// pins the output without depending on the host machine's region (production passes
// the LOCALE_SSHORTTIME / LOCALE_SDATE lookups). `show_seconds` adjusts the effective
// time picture: ":ss" appended when missing and asked for, the seconds token removed
// when present but not asked for - both flags must be able to win over what the
// region happens to default to.
BarClockText FormatBarClock(const SYSTEMTIME& now, const core::FinderBarClockConfig& clock,
                            const wchar_t* time_picture, const wchar_t* date_picture);
std::wstring FormatBarClockText(const SYSTEMTIME& now, const core::FinderBarClockConfig& clock,
                                const wchar_t* time_picture, const wchar_t* date_picture);

class FinderBar {
public:
    FinderBar();
    ~FinderBar();
    FinderBar(const FinderBar&) = delete;
    FinderBar& operator=(const FinderBar&) = delete;

    // Registers the bar's own window class. Deliberately not platform::Window: that class
    // posts WM_QUIT on WM_DESTROY, and the bar is created and destroyed whenever the user
    // toggles it, which would take the whole process down (T-015/T-041/T-076).
    static bool RegisterWindowClass(HINSTANCE instance);

    // Raises a system-menu command. The bar does not perform the action itself: main.cpp
    // owns the settings window, the config path and the exit path.
    using SystemCommandHandler = std::function<void(SystemCommand)>;
    void SetSystemCommandHandler(SystemCommandHandler handler);

    bool Initialize(HINSTANCE instance, const core::Config& config);
    void Shutdown();
    bool Ready() const { return ready_; }

    // Advances the reveal animation, the foreground snapshot and repaints when anything
    // changed. Returns true when a repaint happened.
    bool Tick(double delta_ms, const core::Config& config);

    // Applies a new configuration without recreating the window: height, the three groups
    // and the auto-hide flag can all change while running.
    void ReloadConfig(const core::Config& config);

    // Shows or hides the whole bar. Hiding removes the window from the screen entirely
    // rather than making it transparent: a transparent bar would still eat clicks.
    void SetEnabled(bool enabled);
    bool Enabled() const { return enabled_; }

    HWND Handle() const { return window_; }
    const ForegroundApp& Foreground() const { return foreground_; }
    const std::vector<BarBox>& Boxes() const { return boxes_; }
    const AutoHideState& AutoHide() const { return auto_hide_; }
    bool MenuOpen() const { return menu_running_; }

    // T-302b: how much of the screen the shell is holding for the bar, in physical
    // pixels. Zero means no reservation, which is also what a probe asserts after the
    // bar is turned off - a strip left behind with no bar on it is invisible damage.
    int ReservedStripPx() const { return reserved_strip_px_; }
    // True when the bar is retracting instead of pinning, either because the user asked
    // for it or because the reservation was refused.
    bool AutoHiding() const { return AutoHideActive(); }

    // Opens the menu belonging to box `index` exactly as a click would. Public so the
    // probe channel can exercise the real TrackPopupMenuEx path without a cursor: the
    // alternative is driving a system popup with synthetic mouse events, which is what
    // the spike had to do and which no probe should have to repeat.
    bool OpenMenuForBox(std::size_t box_index);

    // Raises a SystemCommand as if the user picked it from the system menu. Same code
    // path as a real selection, minus the popup.
    bool RunSystemCommand(int command_id);

    // Forwards one item of the menu shown in box `box_index` to the application as
    // WM_COMMAND, exactly as a real selection would. `item` is the position inside that
    // top-level menu (0-based, matching GetMenuItemID's own indexing). Exposed for the
    // probe channel: the alternative is driving a system popup with synthetic mouse
    // events, which is what the spike had to do and which no probe should repeat.
    bool ForwardMenuItem(std::size_t box_index, int item);

    // Pins the tracked application to `hwnd` instead of asking the window manager which
    // window is in front. Zero clears the pin.
    //
    // This is the same precedent as DOCKFORGE_HOVER_PROBE_FILE: only the *source* of the
    // input is replaced. Naming, menu reading, layout, painting, the menu thread and the
    // WM_COMMAND forwarding all run the production path, so a PASS here still proves the
    // real chain. It exists because a probe process cannot win a foreground race against
    // a terminal window that keeps reclaiming it (measured: SetForegroundWindow from the
    // probe returned true and the bar still saw Windows Terminal).
    void SetForegroundOverride(HWND hwnd);

    // Pushes the weather snapshot the bar's weather_brief entry renders. The host lives
    // in main.cpp and already ticks once per frame; the bar only ever receives copies
    // (the C-04b rule - no pointers across threads, no callbacks into the bar).
    void SetWeatherSnapshot(const dock::weather::Snapshot& snapshot);

    // True while the configured right region contains a weather_brief entry - main.cpp
    // reads this to decide whether the weather host must run for the bar at all.
    bool WantsWeatherSnapshot() const;

    // Opens the month calendar under the clock box, exactly as a click would. Public so
    // the probe channel can drive the panel without synthetic mouse events (same
    // precedent as OpenMenuForBox).
    bool OpenCalendarPanel();
    void CloseCalendarPanel();
    bool CalendarPanelVisible() const { return calendar_panel_ && calendar_panel_->Visible(); }

    // Opens the 3-day forecast under the weather_brief box, exactly as a click would.
    // Probe-path mirror of OpenCalendarPanel: a probe cannot reliably drive a
    // foreground-claiming surface with synthetic mouse events.
    bool OpenWeatherPopup();

    // T-306: opens (or toggles closed) the status detail popup under the battery /
    // network box, exactly as a click would. Probe-path mirror of the same panel:
    // a probe cannot drive a foreground-claiming surface with synthetic mouse
    // events, so the click action itself is what the probe calls.
    bool OpenStatusPopup();
    void CloseStatusPopup();
    bool StatusPopupVisible() const {
        return status_popup_ && status_popup_->Visible();
    }
    // T-304: the control centre. The panel object itself is owned by main.cpp
    // (it holds the AudioHardware/MediaSidecarBridge the panel controls); the bar
    // only anchors it under the right-region button and toggles it, exactly like
    // the calendar/status panels but without owning the COM objects behind it.
    // SetHardwarePanel must be called before the first click; a null pointer
    // turns the button into a logged no-op, never a crash.
    void SetHardwarePanel(HardwarePanel* panel);
    bool OpenControlCenterPanel();
    void CloseControlCenterPanel();
    bool ControlCenterPanelVisible() const {
        return control_center_ != nullptr && control_center_->visible();
    }
    // The right-click minimal menu (finderbar-parity I2): opens the system
    // settings pages the control centre's controls correspond to. Returns false
    // when no control-centre box sits under the point. Public so the probe
    // channel can drive the real TrackPopupMenuEx path without a cursor.
    bool OpenControlCenterSettingsMenu(float x_dip, float y_dip);
    // T-306: the ime item's click action - send WM_IME_CONTROL
    // (IMC_SETCONVERSIONMODE) with the flipped native bit to the tracked window's
    // default IME window. Exposed for the probe channel the same way; the bar's
    // own OnRightClick path calls StatusHost::ToggleImeConversionMode through this
    // method too, so the probe certifies the production path.
    bool ToggleImeMode();

    const std::vector<RightBox>& RightBoxes() const { return right_boxes_; }

    // Centre widgets (T-302): the laid-out monitoring row and its host. The host
    // owns the sampling (light four on this bar's Tick chain, temperature on its
    // own worker thread); the boxes are what Repaint draws.
    const std::vector<WidgetBox>& WidgetBoxes() const { return widget_boxes_; }
    const WidgetSnapshot& WidgetSnapshot() const { return widgets_.Snapshot(); }
    std::string WidgetsJson() const { return widgets_.SnapshotJson(); }

    // One-line JSON for the probe channel. Everything a probe needs to assert on without a
    // screenshot.
    std::string SnapshotJson() const;

private:
    bool CreateWindowFor(const platform::DisplayInfo& display);
    void DestroyWindowAndSurface();
    void RefreshForeground();
    void Relayout();
    void ApplyRegion();
    void Repaint();
    void StartMenuJob(HMENU menu, HWND target, bool owns_menu, int x, int y);
    void OpenAppMenu(std::size_t box_index);
    void OpenSystemMenu(std::size_t box_index);
    void OpenOverflowMenu(std::size_t box_index);
    void HandleSystemCommand(int command_id);
    void OnClick(float x_dip, float y_dip);
    void OnRightClick(const RightBox& box);
    void OnMenuClosed();
    void UpdateRightItems();
    std::size_t BoxAtDip(float x_dip, float y_dip) const;
    std::size_t RightBoxAtDip(float x_dip, float y_dip) const;
    float OffsetDip() const;
    // Whether the bar is currently in auto-hide mode. True when the user asked for it, and
    // also when the top-strip reservation failed: in that case the bar must retract rather
    // than sit on top of every maximized window's title bar.
    bool AutoHideActive() const { return config_.auto_hide || auto_hide_fallback_; }
    float Scale() const { return dpi_ > 0 ? static_cast<float>(dpi_) / 96.0f : 1.0f; }

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG object,
                                      LONG child, DWORD thread, DWORD time);

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    platform::CompositionSurface surface_;
    render::PlateRenderer plate_;
    render::BackdropCapture capture_;
    std::unique_ptr<render::IRenderBackend> backend_;
    platform::DisplayInfo display_{};
    render::PlateStyle style_{};
    core::FinderBarConfig config_{};
    bool ready_ = false;
    bool enabled_ = false;

    AutoHideState auto_hide_{};
    ForegroundApp foreground_{};
    AppMenuSnapshot menu_{};
    std::vector<BarBox> boxes_;
    // Right region (T-307): rendered entries for the configured finderbar.right table,
    // laid out from the right edge inwards. total_width_dip feeds LayoutBar's
    // status_width_dip so the left group can never run under the right region.
    std::vector<RightBox> right_boxes_;
    RightGroupSummary right_summary_{};
    // Centre widgets (T-302): sampling host + the laid-out monitoring row between
    // the left group and the right region.
    WidgetHost widgets_;
    std::vector<WidgetBox> widget_boxes_;
    WidgetGroupSummary widget_summary_{};
    // The displayed clock text, cached so the tick loop only relayouts when a minute
    // (or second, if shown) actually rolled over.
    std::wstring clock_text_;
    // Weather snapshot pushed in by main.cpp; a copy, never a pointer (C-04b).
    dock::weather::Snapshot weather_{};
    // The calendar panel the clock opens. Created lazily on first use; hidden on Esc /
    // click-outside / deactivation (calendar_panel.h documents the dismissal rule).
    std::unique_ptr<CalendarPanel> calendar_panel_;
    BarLayoutSummary layout_{};
    float width_dip_ = 0.0f;
    float height_dip_ = kDefaultBarHeightDip;
    UINT dpi_ = 96;
    int width_px_ = 0;
    int height_px_ = 0;
    int region_height_px_ = -1;

    // Backdrop capture runs on its own thread and is polled, never waited on. Keeping the
    // last frame alive between reads means the plate always has pixels to blur instead of
    // flickering to the flat tint whenever a capture has not landed yet.
    std::vector<std::uint8_t> backdrop_pixels_;
    std::uint64_t backdrop_generation_ = 0;

    // Pointer state, polled per tick. A retracted bar has an empty window region, so it
    // receives no WM_MOUSEMOVE at all; polling is the only way to notice the pointer
    // arriving at the top edge (spike Q10: 0.0006ms per call).
    bool pointer_in_trigger_ = false;
    bool pointer_in_bar_ = false;
    int hover_box_ = -1;
    int hover_right_ = -1;  // hover index into right_boxes_, same polled rule

    // The forecast popup a weather_brief click opens. The bar owns its own instance:
    // the dock's popup belongs to the dock tile's hover path, and the two surfaces
    // must not fight over one popup window.
    dock::WeatherPopup weather_popup_;
    bool weather_popup_ready_ = false;

    // Right-region status (T-306): the 1s sampler for ime/battery/network and the
    // detail popup a battery/network click drops. The host samples on this bar's
    // Tick chain only when the right table renders one of the three types.
    StatusHost status_host_;
    std::unique_ptr<StatusPopup> status_popup_;
    bool status_popup_ready_ = false;

    // T-304: the control-centre panel, owned by main.cpp (it holds the audio and
    // sidecar COM objects the panel drives). Raw pointer, never dereferenced
    // before SetHardwarePanel.
    HardwarePanel* control_center_ = nullptr;

    bool menu_running_ = false;
    HANDLE menu_thread_ = nullptr;

    // T-302b: the reserved top strip. Non-zero while the shell is holding space for the
    // bar, so a maximized window stops below it instead of sliding under it.
    int reserved_strip_px_ = 0;
    // Set when the reservation was refused. Forces auto-hide for the rest of the session;
    // cleared on the next successful reservation.
    bool auto_hide_fallback_ = false;

    // Probe-only: when non-null, the tracked application is this window regardless of
    // what GetForegroundWindow says. Never set by production code.
    HWND foreground_override_ = nullptr;

    HWINEVENTHOOK foreground_hook_ = nullptr;
    bool foreground_dirty_ = true;
    std::uint64_t foreground_events_ = 0;    std::uint64_t paints_ = 0;
    std::uint64_t menu_opens_ = 0;
    SystemCommandHandler system_command_;
};

}  // namespace dockforge::finderbar
