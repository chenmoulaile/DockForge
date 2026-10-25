// DockForge - weather hover-preview popup (T-075).
#include "dock/weather_popup.h"

#include "core/log.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dockforge::dock {
namespace {

constexpr wchar_t kPopupClass[] = L"DockForgeWeatherPopup";
constexpr char kLogModule[] = "weather";

// DIP layout, scaled once at Show()'s DPI (same one-shot scaling rule as the
// folder panel and the preview popup).
constexpr float kPanelWidthDip = 232.0f;
constexpr float kHeaderHeightDip = 36.0f;
constexpr float kRowHeightDip = 32.0f;
constexpr float kRowGlyphDip = 22.0f;
constexpr float kPadDip = 12.0f;
constexpr float kEdgeMarginDip = 8.0f;

// Palette: the folder panel / preview popup dark family, so all popovers of
// this dock read as one product.
constexpr COLORREF kBackground = RGB(24, 26, 31);
constexpr COLORREF kBorder = RGB(62, 67, 78);
constexpr COLORREF kHeaderSep = RGB(48, 52, 61);
constexpr COLORREF kText = RGB(212, 218, 227);
constexpr COLORREF kTextDim = RGB(138, 146, 160);
constexpr COLORREF kSun = RGB(250, 204, 92);
constexpr COLORREF kPrecip = RGB(140, 184, 242);

// Minimal UTF-8 → UTF-16 for the narrow strings the snapshot carries. The
// values come from JSON (UTF-8), and GDI wants wide chars.
std::wstring W(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(),
                        needed);
    return out;
}

// GDI twin of the D2D glyph (same silhouettes, Ellipse/Rectangle instead of
// rounded rects). Small enough that the two staying recognisably the same
// shape matters more than pixel parity.
void DrawGlyphGdi(HDC dc, int cx, int cy, int size, weather::Kind kind) {
    const int half = size / 2;
    const HGDIOBJ old = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCPenColor(dc, RGB(220, 226, 235));
    HBRUSH body = CreateSolidBrush(RGB(220, 226, 235));
    HBRUSH sun = CreateSolidBrush(kSun);
    HBRUSH precip = CreateSolidBrush(kPrecip);
    HGDIOBJ old_brush = SelectObject(dc, body);

    switch (kind) {
        case weather::Kind::Clear:
            SelectObject(dc, sun);
            Ellipse(dc, cx - half * 2 / 3, cy - half * 2 / 3, cx + half * 2 / 3, cy + half * 2 / 3);
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            SetDCPenColor(dc, kSun);
            Ellipse(dc, cx - half, cy - half, cx + half, cy + half);
            break;
        case weather::Kind::PartlyCloudy:
            SelectObject(dc, sun);
            Ellipse(dc, cx + size / 8, cy - half * 3 / 4, cx + size / 8 + size / 3,
                    cy - half * 3 / 4 + size / 3);
            SelectObject(dc, body);
            Ellipse(dc, cx - half * 3 / 4, cy - size / 8, cx - size / 12, cy + half);
            Ellipse(dc, cx - size / 8, cy - half, cx + half, cy);
            Rectangle(dc, cx - half * 3 / 4, cy, cx + half, cy + size / 6);
            break;
        case weather::Kind::Cloudy:
            Ellipse(dc, cx - half * 3 / 4, cy - size / 8, cx - size / 12, cy + half);
            Ellipse(dc, cx - size / 8, cy - half, cx + half, cy);
            Rectangle(dc, cx - half * 3 / 4, cy, cx + half, cy + size / 6);
            break;
        case weather::Kind::Fog:
            for (int i = 0; i < 3; ++i) {
                const int y = cy - size / 4 + i * size / 4;
                Rectangle(dc, cx - half + (i % 2) * 3, y, cx + half - (1 - i % 2) * 3, y + 2);
            }
            break;
        case weather::Kind::Rain:
            Ellipse(dc, cx - half * 2 / 3, cy - half * 4 / 5, cx, cy);
            Ellipse(dc, cx - size / 6, cy - half, cx + half * 2 / 3, cy);
            Rectangle(dc, cx - half * 2 / 3, cy - size / 8, cx + half * 2 / 3, cy + size / 8);
            SelectObject(dc, precip);
            for (int i = 0; i < 3; ++i) {
                const int x = cx - size / 3 + i * size / 3;
                Rectangle(dc, x - 1, cy + size / 5, x + 1, cy + size / 2);
            }
            break;
        case weather::Kind::Snow:
            Ellipse(dc, cx - half * 2 / 3, cy - half * 4 / 5, cx, cy);
            Ellipse(dc, cx - size / 6, cy - half, cx + half * 2 / 3, cy);
            Rectangle(dc, cx - half * 2 / 3, cy - size / 8, cx + half * 2 / 3, cy + size / 8);
            SelectObject(dc, precip);
            for (int i = 0; i < 3; ++i) {
                const int x = cx - size / 3 + i * size / 3;
                Ellipse(dc, x - 2, cy + size / 3, x + 2, cy + size / 3 + 4);
            }
            break;
        case weather::Kind::Thunder:
            Ellipse(dc, cx - half * 2 / 3, cy - half * 4 / 5, cx, cy);
            Ellipse(dc, cx - size / 6, cy - half, cx + half * 2 / 3, cy);
            Rectangle(dc, cx - half * 2 / 3, cy - size / 8, cx + half * 2 / 3, cy + size / 8);
            SelectObject(dc, sun);
            for (int i = 0; i < 3; ++i) {
                const int x = cx - size / 10 + (i % 2) * size / 6;
                const int y = cy + size / 6 + i * size / 9;
                Rectangle(dc, x, y, x + size / 8, y + size / 8);
            }
            break;
        case weather::Kind::Unknown:
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            Ellipse(dc, cx - half * 3 / 4, cy - half * 3 / 4, cx + half * 3 / 4, cy + half * 3 / 4);
            break;
    }

    SelectObject(dc, old_brush);
    SelectObject(dc, old);
    DeleteObject(body);
    DeleteObject(sun);
    DeleteObject(precip);
}

}  // namespace

WeatherPopup::~WeatherPopup() { Destroy(); }

bool WeatherPopup::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &WeatherPopup::StaticWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // WM_ERASEBKGND suppressed; WM_PAINT owns pixels
    wc.lpszClassName = kPopupClass;
    const ATOM atom = RegisterClassExW(&wc);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        DF_ERROR(kLogModule, "RegisterClassExW failed %lu", GetLastError());
        return false;
    }
    return true;
}

bool WeatherPopup::Create(HINSTANCE instance) {
    if (hwnd_ != nullptr) return true;
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPopupClass, L"",
                            WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        DF_ERROR(kLogModule, "CreateWindowExW failed %lu", GetLastError());
        return false;
    }
    return true;
}

void WeatherPopup::Destroy() {
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

SIZE WeatherPopup::SizePx() const { return SIZE{width_px_, height_px_}; }

void WeatherPopup::Show(const weather::Snapshot& snapshot, const std::string& today,
                        int anchor_x_px, int bottom_px, int dpi) {
    if (hwnd_ == nullptr) return;
    scale_ = dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
    snapshot_ = snapshot;
    today_ = today;

    const int rows = static_cast<int>(snapshot.days.size());
    width_px_ = static_cast<int>(kPanelWidthDip * scale_);
    height_px_ =
        static_cast<int>((kHeaderHeightDip + kPadDip + static_cast<float>(std::max(rows, 1)) *
                                                        kRowHeightDip +
                          kPadDip) * scale_);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int left = anchor_x_px - width_px_ / 2;
    left = std::max(left, static_cast<int>(work.left + kEdgeMarginDip * scale_));
    left = std::min(left, static_cast<int>(work.right - kEdgeMarginDip * scale_) - width_px_);
    const int top = bottom_px - height_px_ - static_cast<int>(8.0f * scale_);

    SetWindowPos(hwnd_, HWND_TOPMOST, left, top, width_px_, height_px_,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd_, nullptr, FALSE);
    DF_INFO(kLogModule, "preview shown city=%s days=%zu rect=%d,%d %dx%d",
            snapshot.city.c_str(), snapshot.days.size(), left, top, width_px_, height_px_);
}

void WeatherPopup::Hide() {
    if (hwnd_ == nullptr) return;
    if (!Visible()) return;
    ShowWindow(hwnd_, SW_HIDE);
    DF_DEBUG(kLogModule, "preview hidden");
}

LRESULT CALLBACK WeatherPopup::StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW* created = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(created->lpCreateParams));
    }
    WeatherPopup* self = reinterpret_cast<WeatherPopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) return self->WndProc(hwnd, msg, wparam, lparam);
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT WeatherPopup::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;  // painted in WM_PAINT; no flicker
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            Paint(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            // The popup is informative; a click dismisses it (the tile's own
            // click is the manual-refresh gesture, and this window sits above
            // the dock - a click that fell through would hit the wrong thing).
            Hide();
            return 0;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                Hide();
                return 0;
            }
            break;
        case WM_NCDESTROY:
            hwnd_ = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void WeatherPopup::Paint(HDC dc) {
    const int w = width_px_;
    const int h = height_px_;
    if (w <= 0 || h <= 0) return;

    // Double buffering: compose into a memory DC, BitBlt once (the folder
    // panel's measured fix for GDI flicker).
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP canvas = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ old_bitmap = SelectObject(memory, canvas);

    RECT all{0, 0, w, h};
    HBRUSH bg = CreateSolidBrush(kBackground);
    FillRect(memory, &all, bg);
    DeleteObject(bg);

    HPEN border = CreatePen(PS_SOLID, 1, kBorder);
    HGDIOBJ old_pen = SelectObject(memory, border);

    SetBkMode(memory, TRANSPARENT);

    // Header: city left, updated time right, separator under.
    const int pad = static_cast<int>(kPadDip * scale_);
    const int header_h = static_cast<int>(kHeaderHeightDip * scale_);
    const std::wstring city = snapshot_.city.empty() ? L"天气" : W(snapshot_.city);
    HFONT title_font =
        CreateFontW(-static_cast<int>(15.0f * scale_), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(memory, title_font);
    SetTextColor(memory, kText);
    RECT city_rect{pad, pad, w - pad, pad + header_h / 2 + 4};
    DrawTextW(memory, city.c_str(), -1, &city_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    HFONT dim_font = CreateFontW(-static_cast<int>(10.0f * scale_), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SelectObject(memory, dim_font);
    SetTextColor(memory, kTextDim);
    const std::wstring updated = W(snapshot_.updated_at);
    RECT time_rect{pad, pad + header_h / 2 - 2, w - pad, pad + header_h};
    DrawTextW(memory, updated.c_str(), -1, &time_rect,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(memory, GetStockObject(DC_PEN));
    SetDCPenColor(memory, kHeaderSep);
    MoveToEx(memory, pad, pad + header_h, nullptr);
    LineTo(memory, w - pad, pad + header_h);

    // Rows: label | glyph | min..max. The failure state (no usable forecast)
    // draws one dim line instead of pretending data exists.
    const int row_h = static_cast<int>(kRowHeightDip * scale_);
    const int glyph = static_cast<int>(kRowGlyphDip * scale_);
    HFONT row_font =
        CreateFontW(-static_cast<int>(12.0f * scale_), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SelectObject(memory, row_font);

    int y = pad + header_h + static_cast<int>(kPadDip * scale_ * 0.5f);
    if (snapshot_.failed || snapshot_.days.empty()) {
        SetTextColor(memory, kTextDim);
        RECT fail_rect{pad, y, w - pad, y + row_h};
        const std::wstring message =
            snapshot_.error.empty() ? L"暂无预报数据" : W(snapshot_.error);
        DrawTextW(memory, message.c_str(), -1, &fail_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        wchar_t temps[64] = {};
        for (const weather::DayForecast& day : snapshot_.days) {
            SetTextColor(memory, kText);
            RECT label_rect{pad, y, pad + static_cast<int>(56 * scale_), y + row_h};
            const std::wstring label = weather::WeekdayLabel(day.date, today_);
            DrawTextW(memory, label.c_str(), -1, &label_rect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            DrawGlyphGdi(memory, pad + static_cast<int>(86 * scale_), y + row_h / 2, glyph,
                         day.kind);

            std::swprintf(temps, std::size(temps), L"%d° / %d°",
                          static_cast<int>(std::lround(day.tmax)),
                          static_cast<int>(std::lround(day.tmin)));
            RECT temp_rect{w - pad - static_cast<int>(92 * scale_), y, w - pad, y + row_h};
            DrawTextW(memory, temps, -1, &temp_rect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            y += row_h;
        }
    }

    SelectObject(memory, old_font);
    SelectObject(memory, old_pen);
    DeleteObject(title_font);
    DeleteObject(dim_font);
    DeleteObject(row_font);
    DeleteObject(border);

    BitBlt(dc, 0, 0, w, h, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old_bitmap);
    DeleteObject(canvas);
    DeleteDC(memory);
}

}  // namespace dockforge::dock
