// DockForge - the Finder Bar clock's calendar panel (T-307).
//
// A frameless month calendar that opens under the Finder Bar's clock (the same
// anchor macOS uses: the menu-bar clock drops a calendar card straight down, flush
// with the right edge). Sizing follows Beautiful's NotificationCenterWindow.xaml -
// 360x640 DIP, root corner radius 12, content cards radius 10, today ringed in the
// accent blue - read as measurements, not copied pixels (ASSETS.md: parameters and
// metrics have no licence burden, code is written from scratch here).
//
// Two halves, the same split notification_panel.h established and this file follows
// (its window model: RegisterWindowClass / Create / Open / Close plus a PanelHit
// style hit-test):
//
//   * CalendarGrid / CalendarPanelLayout are PURE: month arithmetic and the mapping
//     from a click to a day cell or a month arrow. The one thing that can silently
//     break here is the mapping from "what the user sees" to "what a click means",
//     and that is not observable from a screenshot - so it is what gets unit tests.
//   * CalendarPanel is the window: a DComp surface (WS_EX_NOREDIRECTIONBITMAP +
//     platform::CompositionSurface, the Finder Bar's own path) drawn with the same
//     PlateRenderer the bar uses, Solid tier only. ADR-0045's reasoning applies
//     unchanged: the panel hangs off the top of the desktop, a capture-excluded
//     tier would make it invisible to every screenshot (including this task's own
//     evidence), and ADR-0008 measured that system backdrops ignore per-pixel alpha
//     and rounded radius. So: self-tinted plate, radius 12, verifiable.
//
// Dismissal is the hardware panel's rule (spike-tested there): Esc, or the loss of
// activation after the panel once held it - a click outside deactivates the panel
// and closes it. There is deliberately no global mouse hook chasing every click.
#pragma once

#include "platform/composition.h"
#include "platform/win_common.h"
#include "render/backend.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace dockforge::finderbar {

// DIP metrics (Beautiful NotificationCenterWindow.xaml + its Today card).
inline constexpr float kCalendarPanelWidthDip = 360.0f;
inline constexpr float kCalendarPanelHeightDip = 640.0f;
inline constexpr float kCalendarRootRadiusDip = 12.0f;
inline constexpr float kCalendarCardRadiusDip = 10.0f;
inline constexpr float kCalendarPanelMarginDip = 12.0f;
inline constexpr float kCalendarCardGapDip = 10.0f;
// Today card height: "Today" caption (11) + clock (34) + date line (12) + paddings.
inline constexpr float kCalendarTodayCardHeightDip = 104.0f;
// Month card height: nav row (32) + weekday row (24) + six 44 DIP rows + inner pad.
inline constexpr float kCalendarMonthCardHeightDip = 352.0f;
inline constexpr float kCalendarNavHeightDip = 32.0f;
inline constexpr float kCalendarWeekdayHeightDip = 24.0f;
inline constexpr float kCalendarCellDip = 44.0f;

// One day cell of the 6x7 grid. `day` is the day-of-month shown; out-of-month
// leading/trailing days are drawn dimmed (in_month=false) so the grid never has
// holes - macOS fills them the same way.
struct CalendarCell {
    int day = 0;
    bool in_month = false;
    bool is_today = false;
};

// Pure month arithmetic and geometry for the 6x7 grid. All inputs are explicit, so
// a test can pin "2026-11" against a known weekday without the host machine's
// clock or locale leaking in.
struct CalendarGrid {
    int year = 0;
    int month = 0;             // 1..12
    int first_day_of_week = 0; // 0 = Sunday .. 6 = Saturday (column of the leftmost header)
    int first_day_col = 0;     // grid column of the 1st of the month
    std::vector<CalendarCell> cells;  // always 42, row-major

    // Days in `month` (1..12), Gregorian, leap years handled.
    static int DaysInMonth(int year, int month);
    // Weekday of a date: 0 = Sunday .. 6 = Saturday. Sakamoto's method - pure
    // arithmetic, no host locale, no time zone, trivially testable.
    static int WeekdayOf(int year, int month, int day);
    // Builds the grid for `year`/`month` with the week starting on
    // `first_day_of_week`. Today's date (today_y/m/d) only drives the is_today flag;
    // a different month never lights a cell.
    static CalendarGrid Compute(int year, int month, int first_day_of_week, int today_year,
                                int today_month, int today_day);
};

// What a click on the panel hit. Same shape as notification_panel.h's PanelHit so
// callers can treat both panels with one pattern.
struct CalendarPanelHit {
    enum class Kind { None, PrevMonth, NextMonth, Day };
    Kind kind = Kind::None;
    std::size_t cell = 0;  // valid for Kind::Day; index into CalendarGrid::cells
};

// Pure geometry of the month card: origin of the day grid, the two month-arrow hit
// boxes, and the mapping from a click (DIP, from the panel's top-left) to a hit.
struct CalendarPanelLayout {
    float grid_origin_x_dip = 0.0f;
    float grid_origin_y_dip = 0.0f;
    float cell_dip = kCalendarCellDip;
    float nav_left_dip = 0.0f;   // prev/next arrow hot zones, full nav row height
    float nav_right_dip = 0.0f;
    float nav_arrow_size_dip = 32.0f;
    float nav_top_dip = 0.0f;

    static CalendarPanelLayout Compute();

    CalendarPanelHit HitTest(float x_dip, float y_dip, std::size_t cell_count) const;
};

class CalendarPanel {
  public:
    CalendarPanel() = default;
    ~CalendarPanel();

    CalendarPanel(const CalendarPanel&) = delete;
    CalendarPanel& operator=(const CalendarPanel&) = delete;

    static bool RegisterWindowClass(HINSTANCE instance);
    bool Create(HINSTANCE instance);
    void Destroy();

    bool IsValid() const { return hwnd_ != nullptr; }
    bool Visible() const { return hwnd_ != nullptr && IsWindowVisible(hwnd_) != FALSE; }

    // Opens the panel, right edge aligned `inset_px` in from `monitor.right`, top
    // edge at `bar_bottom_px` (the bar's own bottom, so the card hangs off it like
    // macOS's calendar drops off the menu bar). Clamped into the monitor rect.
    void Open(int dpi, const RECT& monitor, int bar_bottom_px, int right_inset_px);
    void Close();

    // Navigates one month; returns true when the visible month changed.
    bool StepMonth(int delta);

    std::string SnapshotJson() const;

  private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    void Repaint();
    void RebuildGrid();

    HWND hwnd_ = nullptr;
    platform::CompositionSurface surface_;
    std::unique_ptr<render::IRenderBackend> backend_;
    // No PlateRenderer here on purpose: its Solid tint is fixed dark and does not
    // follow the theme, while this panel must read in both light and dark mode. The
    // plate is therefore the ADR-0008 self-tinted rounded rect painted directly, with
    // the theme's own background token (ADR-0045's capture exclusion reasoning still
    // applies - the panel stays Solid tier and never hides from screenshots).
    CalendarGrid grid_{};
    CalendarPanelLayout layout_{};
    float scale_ = 1.0f;
    int width_px_ = 0;
    int height_px_ = 0;
};

}  // namespace dockforge::finderbar
