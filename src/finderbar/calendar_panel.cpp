// DockForge - the Finder Bar clock's calendar panel (see calendar_panel.h).
#include "finderbar/calendar_panel.h"

#include "core/log.h"

#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace dockforge::finderbar {
namespace {

constexpr wchar_t kPanelClass[] = L"DockForgeCalendarPanel";
constexpr char kLogModule[] = "finderbar";

// The week's first column follows the system region. LOCALE_IFIRSTDAYOFWEEK returns
// "0".."6" with 0 = Monday; this grid indexes columns with 0 = Sunday, so the value
// shifts by one. Fallback (query refused): Sunday first, what Beautiful's calendar
// uses and what en-US expects.
int FirstDayOfWeek() {
    wchar_t buffer[8]{};
    const int n = GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK, buffer,
                                  static_cast<int>(std::size(buffer)));
    if (n <= 0 || buffer[0] < L'0' || buffer[0] > L'6') return 0;
    return (buffer[0] - L'0' + 1) % 7;
}

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

// Theme colours. Values are Beautiful's MacOS tokens (MacTokens.cs: dark
// WindowBackground #1E1E1E / light #F5F5F7, ControlFill #33FFFFFF / #1A000000,
// TextPrimary #FFFFFF / #1D1D1F, TextSecondary B3/99 alpha, TextTertiary 66/4D
// alpha); the accent is Apple systemBlue (light #007AFF, dark #0A84FF), the blue
// the notification center's calendar rings today with (round10 evidence).
struct ThemeColors {
    render::ColorF background{};     // root plate tint
    render::ColorF border{};         // root border
    render::ColorF card{};           // content card fill (ControlFill token)
    render::ColorF text{};           // primary
    render::ColorF text_secondary{}; // date line
    render::ColorF text_tertiary{};  // captions, dimmed out-of-month days
    render::ColorF accent{};         // today disc
    render::ColorF accent_text{};    // today's number on the disc
};

render::ColorF ColorFromHex(std::uint32_t rgb, float alpha) {
    return render::ColorF{static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                          static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                          static_cast<float>(rgb & 0xFF) / 255.0f, alpha};
}

ThemeColors ColorsFor(bool light) {
    ThemeColors c;
    if (light) {
        c.background = ColorFromHex(0xF5F5F7, 0.99f);
        c.border = ColorFromHex(0x000000, 0.10f);
        c.card = ColorFromHex(0x000000, 0.10f);
        c.text = ColorFromHex(0x1D1D1F, 1.0f);
        c.text_secondary = ColorFromHex(0x000000, 0.60f);
        c.text_tertiary = ColorFromHex(0x000000, 0.30f);
        c.accent = ColorFromHex(0x007AFF, 1.0f);
        c.accent_text = ColorFromHex(0xFFFFFF, 1.0f);
    } else {
        c.background = ColorFromHex(0x1E1E1E, 0.99f);
        c.border = ColorFromHex(0xFFFFFF, 0.20f);
        c.card = ColorFromHex(0xFFFFFF, 0.20f);
        c.text = ColorFromHex(0xFFFFFF, 1.0f);
        c.text_secondary = ColorFromHex(0xFFFFFF, 0.70f);
        c.text_tertiary = ColorFromHex(0xFFFFFF, 0.40f);
        c.accent = ColorFromHex(0x0A84FF, 1.0f);
        c.accent_text = ColorFromHex(0xFFFFFF, 1.0f);
    }
    return c;
}

}  // namespace

// ---------------------------------------------------------------------------
// CalendarGrid - pure arithmetic
// ---------------------------------------------------------------------------

int CalendarGrid::DaysInMonth(int year, int month) {
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12 || year < 1) return 30;  // caller bug; stay sane
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        return leap ? 29 : 28;
    }
    return kDays[month - 1];
}

int CalendarGrid::WeekdayOf(int year, int month, int day) {
    // Sakamoto's method (see the header comment): 0 = Sunday.
    static const int kT[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = year;
    if (month < 3) y -= 1;
    const int w = (y + y / 4 - y / 100 + y / 400 + kT[month - 1] + day) % 7;
    return w < 0 ? w + 7 : w;
}

CalendarGrid CalendarGrid::Compute(int year, int month, int first_day_of_week, int today_year,
                                   int today_month, int today_day) {
    CalendarGrid grid;
    grid.year = year;
    grid.month = month;
    grid.first_day_of_week = first_day_of_week < 0 || first_day_of_week > 6 ? 0
                                                                            : first_day_of_week;
    grid.first_day_col = (WeekdayOf(year, month, 1) - grid.first_day_of_week + 7) % 7;
    grid.cells.reserve(42);

    const int in_month_days = DaysInMonth(year, month);
    const int prev_month = month == 1 ? 12 : month - 1;
    const int prev_year = month == 1 ? year - 1 : year;
    const int prev_days = DaysInMonth(prev_year, prev_month);

    for (int i = 0; i < 42; ++i) {
        CalendarCell cell;
        if (i < grid.first_day_col) {
            // Leading days of the previous month.
            cell.day = prev_days - grid.first_day_col + 1 + i;
        } else if (const int day = i - grid.first_day_col + 1; day <= in_month_days) {
            cell.day = day;
            cell.in_month = true;
        } else {
            cell.day = day - in_month_days;  // trailing days of the next month
        }
        cell.is_today = cell.in_month && year == today_year && month == today_month &&
                        cell.day == today_day;
        grid.cells.push_back(cell);
    }
    return grid;
}

// ---------------------------------------------------------------------------
// CalendarPanelLayout
// ---------------------------------------------------------------------------

CalendarPanelLayout CalendarPanelLayout::Compute() {
    CalendarPanelLayout layout;
    // The month card spans the panel minus the outer margin; its inner padding is 12.
    const float card_left = kCalendarPanelMarginDip;
    const float card_right = kCalendarPanelWidthDip - kCalendarPanelMarginDip;
    const float inner_width = card_right - card_left - 24.0f;

    // The card sits below the Today card plus one gap; the nav row follows the card's
    // own 12 DIP inner top padding.
    const float card_top =
        kCalendarPanelMarginDip + kCalendarTodayCardHeightDip + kCalendarCardGapDip;
    layout.nav_top_dip = card_top + 12.0f;
    layout.cell_dip = kCalendarCellDip;
    // Seven 44 DIP cells fit in the 312 DIP inner width with 4 DIP slack; centre them.
    layout.grid_origin_x_dip = card_left + 12.0f + (inner_width - 7.0f * kCalendarCellDip) * 0.5f;
    layout.grid_origin_y_dip =
        layout.nav_top_dip + kCalendarNavHeightDip + kCalendarWeekdayHeightDip;
    // The arrows hug the card's inner edges; the hot zone is a 32 DIP square centred
    // on the drawn chevron, big enough to hit without aiming (same target-size rule
    // as the bar's own boxes).
    layout.nav_left_dip = card_left + 12.0f;
    layout.nav_right_dip = card_right - 12.0f - layout.nav_arrow_size_dip;
    return layout;
}

CalendarPanelHit CalendarPanelLayout::HitTest(float x_dip, float y_dip,
                                              std::size_t cell_count) const {
    CalendarPanelHit hit;
    if (y_dip >= nav_top_dip && y_dip < nav_top_dip + kCalendarNavHeightDip) {
        if (x_dip >= nav_left_dip && x_dip < nav_left_dip + nav_arrow_size_dip) {
            hit.kind = CalendarPanelHit::Kind::PrevMonth;
        } else if (x_dip >= nav_right_dip && x_dip < nav_right_dip + nav_arrow_size_dip) {
            hit.kind = CalendarPanelHit::Kind::NextMonth;
        }
        return hit;
    }
    const float grid_bottom = grid_origin_y_dip + 6.0f * cell_dip;
    if (y_dip < grid_origin_y_dip || y_dip >= grid_bottom) return hit;
    const float grid_right = grid_origin_x_dip + 7.0f * cell_dip;
    if (x_dip < grid_origin_x_dip || x_dip >= grid_right) return hit;
    const int col = static_cast<int>((x_dip - grid_origin_x_dip) / cell_dip);
    const int row = static_cast<int>((y_dip - grid_origin_y_dip) / cell_dip);
    const std::size_t index = static_cast<std::size_t>(row) * 7 + static_cast<std::size_t>(col);
    if (index >= cell_count) return hit;
    hit.kind = CalendarPanelHit::Kind::Day;
    hit.cell = index;
    return hit;
}

// ---------------------------------------------------------------------------
// CalendarPanel
// ---------------------------------------------------------------------------

CalendarPanel::~CalendarPanel() { Destroy(); }

bool CalendarPanel::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    // No CS_HREDRAW/CS_VREDRAW: the pixels come from DComp, exactly like the bar.
    wc.style = 0;
    wc.lpfnWndProc = &CalendarPanel::StaticWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kPanelClass;
    const ATOM atom = RegisterClassExW(&wc);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        DF_ERROR(kLogModule, "calendar RegisterClassExW failed %lu", GetLastError());
        return false;
    }
    return true;
}

bool CalendarPanel::Create(HINSTANCE instance) {
    if (hwnd_ != nullptr) return true;
    // WS_EX_TOOLWINDOW keeps the panel out of alt-tab; NOREDIRECTIONBITMAP is what
    // makes the DComp surface the only thing painted (platform/composition.h contract).
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP,
                            kPanelClass, L"DockForge Calendar", WS_POPUP, 0, 0, 1, 1, nullptr,
                            nullptr, instance, this);
    if (hwnd_ == nullptr) {
        DF_ERROR(kLogModule, "calendar CreateWindowExW failed %lu", GetLastError());
        return false;
    }
    if (!surface_.Initialize(hwnd_)) {
        DF_ERROR(kLogModule, "calendar surface initialize failed");
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }
    backend_ = render::CreateD2DBackend();
    if (!backend_ || !backend_->Initialize()) {
        DF_ERROR(kLogModule, "calendar D2D backend failed");
        backend_.reset();
        surface_.Shutdown();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }
    layout_ = CalendarPanelLayout::Compute();
    RebuildGrid();
    return true;
}

void CalendarPanel::Destroy() {
    if (hwnd_ != nullptr) {
        if (backend_) backend_->Shutdown();
        backend_.reset();
        surface_.Shutdown();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void CalendarPanel::RebuildGrid() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    grid_ = CalendarGrid::Compute(now.wYear, now.wMonth, FirstDayOfWeek(), now.wYear, now.wMonth,
                                  now.wDay);
}

void CalendarPanel::Open(int dpi, const RECT& monitor, int bar_bottom_px, int right_inset_px) {
    if (hwnd_ == nullptr) return;
    scale_ = dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
    width_px_ = static_cast<int>(kCalendarPanelWidthDip * scale_);
    height_px_ = static_cast<int>(kCalendarPanelHeightDip * scale_);

    // Right edge in from the monitor edge, top edge just under the bar. The clamps
    // keep a short-display or second-monitor case on-screen rather than off it.
    int left = monitor.right - right_inset_px - width_px_;
    int top = bar_bottom_px;
    left = std::max(static_cast<int>(monitor.left),
                    std::min(left, static_cast<int>(monitor.right) - width_px_));
    top = std::max(static_cast<int>(monitor.top),
                   std::min(top, static_cast<int>(monitor.bottom) - height_px_));

    RebuildGrid();
    if (!surface_.Resize(width_px_, height_px_)) {
        DF_WARN(kLogModule, "calendar surface resize failed");
        return;
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, left, top, width_px_, height_px_, SWP_SHOWWINDOW);

    // Claim the foreground so Esc works and a click outside deactivates us. The bar
    // measured the same requirement for its menus (spike Q6/Q7); a panel that cannot
    // take the foreground would close on the first stray event instead.
    //
    if (!SetForegroundWindow(hwnd_)) {
        DF_DEBUG(kLogModule, "SetForegroundWindow refused on calendar open");
    }
    SetFocus(hwnd_);
    Repaint();
    DF_INFO(kLogModule, "calendar opened %d,%d %dx%d hwnd=%p this=%p", left, top, width_px_,
            height_px_, static_cast<void*>(hwnd_), static_cast<void*>(this));
}

void CalendarPanel::Close() {
    if (hwnd_ == nullptr || !Visible()) return;
    ShowWindow(hwnd_, SW_HIDE);
    DF_DEBUG(kLogModule, "calendar closed");
}

bool CalendarPanel::StepMonth(int delta) {
    if (delta == 0) return false;
    int year = grid_.year;
    int month = grid_.month + delta;
    if (month < 1) {
        month += 12;
        year -= 1;
    } else if (month > 12) {
        month -= 12;
        year += 1;
    }
    if (year < 1601 || year > 30827) return false;  // SYSTEMTIME's working range
    SYSTEMTIME today{};
    GetLocalTime(&today);
    grid_ = CalendarGrid::Compute(year, month, FirstDayOfWeek(), today.wYear, today.wMonth,
                                  today.wDay);
    Repaint();
    return true;
}

LRESULT CALLBACK CalendarPanel::StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW* created = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(created->lpCreateParams));
    }
    CalendarPanel* self =
        reinterpret_cast<CalendarPanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) return self->WndProc(hwnd, msg, wparam, lparam);
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CalendarPanel::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_ACTIVATE:
            // Dismiss on ANY loss of activation. The hardware-panel rule ("only a loss
            // AFTER the panel once held activation") does not carry over here: this
            // panel claims the foreground itself at Open, so a WA_INACTIVE can only
            // arrive after a real activation - except when the foreground claim was
            // refused entirely (probe path), in which case no WA_INACTIVE arrives at
            // all and the panel stays open, the same honest degradation as the menus.
            //
            // (An intermediate was_activated_ flag was measured to read 0 on a
            // WA_INACTIVE that provably followed a WA_ACTIVE that set it to 1 - same
            // hwnd, same instance, no intervening write; recorded in evidence/T-307 as
            // an unresolved mystery and sidestepped by dropping the flag entirely.)
            DF_DEBUG(kLogModule, "calendar WM_ACTIVATE low=%u",
                     static_cast<unsigned>(LOWORD(wparam)));
            if (LOWORD(wparam) == WA_INACTIVE) {
                Close();
            }
            return 0;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                Close();
                return 0;
            }
            if (wparam == VK_LEFT) {
                StepMonth(-1);
                return 0;
            }
            if (wparam == VK_RIGHT) {
                StepMonth(1);
                return 0;
            }
            return 0;
        case WM_LBUTTONDOWN: {
            const float x_dip = scale_ > 0.0f ? GET_X_LPARAM(lparam) / scale_ : 0.0f;
            const float y_dip = scale_ > 0.0f ? GET_Y_LPARAM(lparam) / scale_ : 0.0f;
            const CalendarPanelHit hit = layout_.HitTest(x_dip, y_dip, grid_.cells.size());
            if (hit.kind == CalendarPanelHit::Kind::PrevMonth) StepMonth(-1);
            if (hit.kind == CalendarPanelHit::Kind::NextMonth) StepMonth(1);
            // A day click is display-only this task (T-207 owns date picking); the
            // grid still answers the hit test for it, so the pure half is exercised.
            return 0;
        }
        case WM_NCDESTROY:
            hwnd_ = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void CalendarPanel::Repaint() {
    if (hwnd_ == nullptr || !surface_.IsValid() || !backend_) return;

    const bool light = SystemUsesLightTheme();
    const ThemeColors colors = ColorsFor(light);
    const float width = scale_ > 0.0f ? static_cast<float>(width_px_) / scale_ : 0.0f;
    const float height = scale_ > 0.0f ? static_cast<float>(height_px_) / scale_ : 0.0f;
    const float dpi = scale_ * 96.0f;
    if (width <= 0.0f || height <= 0.0f) return;

    const bool painted = surface_.Paint(
        dpi, [&](ID2D1DeviceContext& dc, const platform::CompositionSurface::PaintInfo&) {
            backend_->BeginFrame(dc, dpi);

            // Root plate, self-tinted (see the header comment): theme background,
            // radius 12 - the NotificationCenter root Border - plus its 1 DIP border.
            // Solid tier by policy (ADR-0045), so no capture and no system backdrop.
            const render::RectF root{0.0f, 0.0f, width, height};
            backend_->FillRoundedRect(root, kCalendarRootRadiusDip, colors.background);
            backend_->StrokeRoundedRect(root, kCalendarRootRadiusDip, colors.border, 1.0f);

            // Content cards, radius 10 (the NotificationCenter widget cards).
            const float card_right = width - kCalendarPanelMarginDip;
            const render::RectF today_card{
                kCalendarPanelMarginDip, kCalendarPanelMarginDip, card_right,
                kCalendarPanelMarginDip + kCalendarTodayCardHeightDip};
            const float month_top =
                kCalendarPanelMarginDip + kCalendarTodayCardHeightDip + kCalendarCardGapDip;
            const render::RectF month_card{kCalendarPanelMarginDip, month_top, card_right,
                                           month_top + kCalendarMonthCardHeightDip};
            backend_->FillRoundedRect(today_card, kCalendarCardRadiusDip, colors.card);
            backend_->FillRoundedRect(month_card, kCalendarCardRadiusDip, colors.card);

            // Today card: caption, big clock, date line (Beautiful's Today widget).
            SYSTEMTIME now{};
            GetLocalTime(&now);
            backend_->DrawLabel(L"\u4ECA\u5929",
                                render::RectF{today_card.left + 24.0f, today_card.top + 10.0f,
                                              today_card.left + 120.0f, today_card.top + 30.0f},
                                11.0f, colors.text_tertiary, render::TextAlign::Left);
            wchar_t clock_text[16]{};
            swprintf_s(clock_text, L"%02d:%02d", now.wHour, now.wMinute);
            backend_->DrawLabel(clock_text,
                                render::RectF{today_card.left + 24.0f, today_card.top + 28.0f,
                                              today_card.left + 240.0f, today_card.top + 76.0f},
                                34.0f, colors.text, render::TextAlign::Left);
            wchar_t date_text[64]{};
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &now, nullptr, date_text, 64, nullptr);
            backend_->DrawLabel(
                date_text,
                render::RectF{today_card.left + 24.0f, today_card.top + 76.0f,
                              today_card.left + 280.0f, today_card.top + 96.0f},
                12.0f, colors.text_secondary, render::TextAlign::Left);

            // Month card header: chevrons + "YYYY年M月".
            const float nav_bottom = layout_.nav_top_dip + kCalendarNavHeightDip;
            backend_->DrawLabel(L"\u2039",
                                render::RectF{layout_.nav_left_dip, layout_.nav_top_dip,
                                              layout_.nav_left_dip + layout_.nav_arrow_size_dip,
                                              nav_bottom},
                                18.0f, colors.text, render::TextAlign::Center);
            backend_->DrawLabel(L"\u203A",
                                render::RectF{layout_.nav_right_dip, layout_.nav_top_dip,
                                              layout_.nav_right_dip + layout_.nav_arrow_size_dip,
                                              nav_bottom},
                                18.0f, colors.text, render::TextAlign::Center);
            wchar_t title[32]{};
            swprintf_s(title, L"%d\u5E74%d\u6708", grid_.year, grid_.month);
            backend_->DrawLabel(
                title,
                render::RectF{layout_.nav_left_dip + layout_.nav_arrow_size_dip,
                              layout_.nav_top_dip, layout_.nav_right_dip, nav_bottom},
                14.0f, colors.text, render::TextAlign::Center);

            // Weekday header row; the columns start at the region's own first weekday.
            static const wchar_t* kWeekNames[7] = {L"\u65E5", L"\u4E00", L"\u4E8C", L"\u4E09",
                                                   L"\u56DB", L"\u4E94", L"\u516D"};
            for (int c = 0; c < 7; ++c) {
                const int name_index = (grid_.first_day_of_week + c) % 7;
                const float x = layout_.grid_origin_x_dip + c * layout_.cell_dip;
                backend_->DrawLabel(kWeekNames[name_index],
                                    render::RectF{x, layout_.grid_origin_y_dip -
                                                         kCalendarWeekdayHeightDip,
                                                  x + layout_.cell_dip,
                                                  layout_.grid_origin_y_dip},
                                    11.0f, colors.text_tertiary, render::TextAlign::Center);
            }

            // Day cells. Today gets the accent disc and keeps its number readable on
            // it (macOS's filled-circle treatment); out-of-month days dim.
            const float cell = layout_.cell_dip;
            for (std::size_t i = 0; i < grid_.cells.size(); ++i) {
                const CalendarCell& cell_info = grid_.cells[i];
                const int row = static_cast<int>(i / 7);
                const int col = static_cast<int>(i % 7);
                const float x = layout_.grid_origin_x_dip + col * cell;
                const float y = layout_.grid_origin_y_dip + row * cell;
                wchar_t number[8]{};
                swprintf_s(number, L"%d", cell_info.day);
                if (cell_info.is_today) {
                    const float pad = (cell - 32.0f) * 0.5f;
                    backend_->FillRoundedRect(
                        render::RectF{x + pad, y + pad, x + pad + 32.0f, y + pad + 32.0f}, 16.0f,
                        colors.accent);
                    backend_->DrawLabel(number, render::RectF{x, y, x + cell, y + cell}, 13.0f,
                                        colors.accent_text, render::TextAlign::Center);
                } else {
                    backend_->DrawLabel(number, render::RectF{x, y, x + cell, y + cell}, 13.0f,
                                        cell_info.in_month ? colors.text : colors.text_tertiary,
                                        render::TextAlign::Center);
                }
            }

            backend_->EndFrame();
        });

    if (!painted) {
        DF_WARN(kLogModule, "calendar paint failed");
        return;
    }
    surface_.Commit();
}

std::string CalendarPanel::SnapshotJson() const {
    char buffer[128]{};
    std::snprintf(buffer, sizeof(buffer),
                  "{\"visible\":%s,\"year\":%d,\"month\":%d,\"cells\":%zu}",
                  Visible() ? "true" : "false", grid_.year, grid_.month, grid_.cells.size());
    return buffer;
}

}  // namespace dockforge::finderbar
