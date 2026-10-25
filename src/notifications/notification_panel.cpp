// DockForge - notification history panel (see notification_panel.h).
#include "notifications/notification_panel.h"

#include "core/log.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dockforge::notifications {
namespace {

constexpr wchar_t kPanelClass[] = L"DockForgeNotificationPanel";
constexpr char kLogModule[] = "notifications";

constexpr COLORREF kBackground = RGB(24, 26, 31);
constexpr COLORREF kBorder = RGB(62, 67, 78);
constexpr COLORREF kHeaderSep = RGB(48, 52, 61);
constexpr COLORREF kRowHover = RGB(34, 37, 44);
constexpr COLORREF kText = RGB(222, 227, 235);
constexpr COLORREF kTextDim = RGB(138, 146, 160);
constexpr COLORREF kAccent = RGB(96, 156, 240);
constexpr COLORREF kDanger = RGB(214, 116, 116);

constexpr float kPadDip = 12.0f;
constexpr float kRowIconDip = 24.0f;
constexpr float kRowTitleDip = 12.0f;
constexpr float kRowBodyDip = 11.0f;

}  // namespace

// ---------------------------------------------------------------------------
// PanelLayout
// ---------------------------------------------------------------------------

PanelLayout PanelLayout::Compute(std::size_t count, std::size_t scroll) {
    PanelLayout layout;
    const auto max_rows = static_cast<std::size_t>(kPanelMaxRowsDip);
    layout.visible_rows = std::min(count, max_rows);
    // Clamp the scroll window so the list cannot be scrolled past its end into
    // an empty plate: the caller can hand in any scroll value.
    const std::size_t max_scroll = count > layout.visible_rows ? count - layout.visible_rows : 0;
    layout.first_row = std::min(scroll, max_scroll);
    layout.height_dip = static_cast<double>(kPanelHeaderHeightDip) +
                        static_cast<double>(layout.visible_rows) * kPanelRowHeightDip +
                        static_cast<double>(kPanelFooterHeightDip);
    return layout;
}

PanelHit PanelLayout::HitTest(double y_dip, double x_dip, double width_dip) const {
    PanelHit hit;
    if (y_dip < 0.0) return hit;
    if (y_dip < static_cast<double>(kPanelHeaderHeightDip)) return hit;  // header: no actions

    const double rows_bottom = static_cast<double>(kPanelHeaderHeightDip) +
                               static_cast<double>(visible_rows) * kPanelRowHeightDip;
    if (y_dip < rows_bottom) {
        const double within = y_dip - static_cast<double>(kPanelHeaderHeightDip);
        const auto slot = static_cast<std::size_t>(within / kPanelRowHeightDip);
        if (slot >= visible_rows) return hit;
        hit.kind = PanelHit::Kind::Row;
        hit.index = first_row + slot;
        return hit;
    }
    if (y_dip >= height_dip) return hit;

    // Footer: the clear button sits at the right end, everything else is inert.
    const double clear_left = width_dip - static_cast<double>(kPanelEdgeMarginDip) -
                              static_cast<double>(kPanelClearWidthDip);
    if (x_dip >= clear_left) {
        hit.kind = PanelHit::Kind::Clear;
    }
    return hit;
}

// ---------------------------------------------------------------------------
// NotificationPanel
// ---------------------------------------------------------------------------

NotificationPanel::~NotificationPanel() { Destroy(); }

bool NotificationPanel::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &NotificationPanel::StaticWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kPanelClass;
    const ATOM atom = RegisterClassExW(&wc);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        DF_ERROR(kLogModule, "panel RegisterClassExW failed %lu", GetLastError());
        return false;
    }
    return true;
}

bool NotificationPanel::Create(HINSTANCE instance) {
    if (hwnd_ != nullptr) return true;
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPanelClass, L"", WS_POPUP, 0, 0, 1,
                            1, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        DF_ERROR(kLogModule, "panel CreateWindowExW failed %lu", GetLastError());
        return false;
    }
    return true;
}

void NotificationPanel::Destroy() {
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void NotificationPanel::SetMaterialTier(core::MaterialLevel tier) {
    if (tier == tier_) return;
    tier_ = tier;
    if (hwnd_ != nullptr) InvalidateRect(hwnd_, nullptr, FALSE);
}

void NotificationPanel::SetItems(const std::vector<NotificationItem>* items) {
    items_ = items;
    Relayout();
    // An open panel has to re-measure immediately: a new row changes the plate
    // height, and waiting for the next unrelated repaint would leave the last
    // row clipped. Refresh() is guarded by Visible(), so a closed panel costs
    // nothing here.
    if (hwnd_ != nullptr && Visible()) Refresh(static_cast<int>(scale_ * 96.0f));
}

void NotificationPanel::SetAccessStatus(AccessStatus status) { access_ = status; }

void NotificationPanel::Relayout() {
    const std::size_t count = items_ != nullptr ? items_->size() : 0;
    layout_ = PanelLayout::Compute(count, scroll_);
    // Keep the scroll window inside the list after items were cleared.
    scroll_ = layout_.first_row;
}

void NotificationPanel::Open(int dpi) {
    if (hwnd_ == nullptr) return;
    scale_ = dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
    Relayout();
    width_px_ = static_cast<int>(kPanelWidthDip * scale_);
    height_px_ = static_cast<int>(layout_.height_dip * scale_);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int margin = static_cast<int>(kPanelEdgeMarginDip * scale_);
    // Top-right, same corner the banner uses - when both are up the banner sits
    // above the panel, which is the reading order the user expects.
    const int left = std::max(static_cast<int>(work.left),
                              static_cast<int>(work.right) - margin - width_px_);
    const int top = static_cast<int>(work.top) + margin;

    SetWindowPos(hwnd_, HWND_TOPMOST, left, top, width_px_, height_px_,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd_, nullptr, FALSE);
    DF_INFO(kLogModule, "panel opened rows=%zu rect=%d,%d %dx%d", layout_.visible_rows, left, top,
            width_px_, height_px_);
}

void NotificationPanel::Close() {
    if (hwnd_ == nullptr) return;
    if (!Visible()) return;
    ShowWindow(hwnd_, SW_HIDE);
    DF_DEBUG(kLogModule, "panel closed");
}

void NotificationPanel::Refresh(int dpi) {
    if (hwnd_ == nullptr || !Visible()) return;
    scale_ = dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
    // Relayout() is the pure half (it never touches the window), so calling it
    // here is safe. It must NOT call back into Refresh: the two calling each
    // other recursed until the process died the moment a push arrived while the
    // panel was open (measured - the panel's own Repaint path).
    Relayout();
    width_px_ = static_cast<int>(kPanelWidthDip * scale_);
    height_px_ = static_cast<int>(layout_.height_dip * scale_);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, width_px_, height_px_,
                 SWP_NOMOVE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool NotificationPanel::ScrollBy(int rows) {
    const std::size_t count = items_ != nullptr ? items_->size() : 0;
    if (rows == 0 || count == 0) return false;
    const std::size_t before = layout_.first_row;
    if (rows > 0) {
        scroll_ = before + static_cast<std::size_t>(rows);
    } else {
        const std::size_t back = static_cast<std::size_t>(-rows);
        scroll_ = back > before ? 0 : before - back;
    }
    Relayout();
    return layout_.first_row != before;
}

LRESULT CALLBACK NotificationPanel::StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                                  LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW* created = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(created->lpCreateParams));
    }
    NotificationPanel* self =
        reinterpret_cast<NotificationPanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) return self->WndProc(hwnd, msg, wparam, lparam);
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT NotificationPanel::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            Paint(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const double x_dip = scale_ > 0.0f ? GET_X_LPARAM(lparam) / scale_ : 0.0;
            const double y_dip = scale_ > 0.0f ? GET_Y_LPARAM(lparam) / scale_ : 0.0;
            const PanelHit hit = layout_.HitTest(y_dip, x_dip, kPanelWidthDip);
            if (hit.kind == PanelHit::Kind::Clear) {
                if (on_clear_) on_clear_();
                Relayout();
                return 0;
            }
            if (hit.kind == PanelHit::Kind::Row && items_ != nullptr &&
                hit.index < items_->size()) {
                const NotificationItem item = (*items_)[hit.index];
                // Clicking a row is a "take me there": close the panel first so
                // the app we activate is not opened behind an always-on-top
                // plate.
                Close();
                if (on_activate_) on_activate_(item);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            ScrollBy(delta > 0 ? -1 : 1);
            return 0;
        }
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                Close();
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

void NotificationPanel::Paint(HDC dc) {
    const int w = width_px_;
    const int h = height_px_;
    if (w <= 0 || h <= 0) return;

    HDC memory = CreateCompatibleDC(dc);
    HBITMAP canvas = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ old_bitmap = SelectObject(memory, canvas);
    SetBkMode(memory, TRANSPARENT);

    RECT all{0, 0, w, h};
    HBRUSH bg = CreateSolidBrush(kBackground);
    FillRect(memory, &all, bg);
    DeleteObject(bg);

    const int pad = static_cast<int>(kPadDip * scale_);
    const int header_h = static_cast<int>(kPanelHeaderHeightDip * scale_);
    const int row_h = static_cast<int>(kPanelRowHeightDip * scale_);
    const int icon = static_cast<int>(kRowIconDip * scale_);

    HFONT title_font =
        CreateFontW(-static_cast<int>(kRowTitleDip * scale_), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT body_font =
        CreateFontW(-static_cast<int>(kRowBodyDip * scale_), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(memory, title_font);

    // Header.
    SetTextColor(memory, kText);
    RECT header_rect{pad, 0, w - pad, header_h};
    DrawTextW(memory, L"通知中心", -1, &header_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(memory, GetStockObject(DC_PEN));
    SetDCPenColor(memory, kHeaderSep);
    MoveToEx(memory, pad, header_h, nullptr);
    LineTo(memory, w - pad, header_h);

    const std::size_t count = items_ != nullptr ? items_->size() : 0;
    int y = header_h;
    if (count == 0) {
        SetTextColor(memory, kTextDim);
        SelectObject(memory, body_font);
        RECT empty_rect{pad, y, w - pad, y + row_h};
        const wchar_t* message = access_ == AccessStatus::Denied ? L"通知访问被拒绝，仅显示本应用通知"
                                                                 : L"暂无通知";
        DrawTextW(memory, message, -1, &empty_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += row_h;
    } else {
        for (std::size_t slot = 0; slot < layout_.visible_rows; ++slot) {
            const std::size_t index = layout_.first_row + slot;
            if (index >= count) break;
            const NotificationItem& item = (*items_)[index];

            const int icon_top = y + (row_h - icon) / 2;
            HBRUSH accent = CreateSolidBrush(kAccent);
            HGDIOBJ old_brush = SelectObject(memory, accent);
            HGDIOBJ old_pen = SelectObject(memory, GetStockObject(NULL_PEN));
            Ellipse(memory, pad, icon_top, pad + icon, icon_top + icon);
            SelectObject(memory, old_pen);
            SelectObject(memory, old_brush);
            DeleteObject(accent);

            const int text_left = pad + icon + pad;
            const int text_right = w - pad;
            SelectObject(memory, title_font);
            SetTextColor(memory, kText);
            RECT app_rect{text_left, y + static_cast<int>(6 * scale_), text_right,
                          y + static_cast<int>(26 * scale_)};
            const std::wstring sender = item.DisplayApp();
            DrawTextW(memory, sender.c_str(), -1, &app_rect,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

            SelectObject(memory, body_font);
            SetTextColor(memory, kTextDim);
            RECT body_rect{text_left, app_rect.bottom, text_right,
                           y + row_h - static_cast<int>(6 * scale_)};
            const std::wstring headline = item.Headline();
            DrawTextW(memory, headline.c_str(), -1, &body_rect,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

            SelectObject(memory, GetStockObject(DC_PEN));
            SetDCPenColor(memory, kHeaderSep);
            MoveToEx(memory, text_left, y + row_h - 1, nullptr);
            LineTo(memory, w - pad, y + row_h - 1);
            y += row_h;
        }
    }

    // Footer with the clear action.
    const int footer_top = h - static_cast<int>(kPanelFooterHeightDip * scale_);
    SelectObject(memory, GetStockObject(DC_PEN));
    SetDCPenColor(memory, kHeaderSep);
    MoveToEx(memory, pad, footer_top, nullptr);
    LineTo(memory, w - pad, footer_top);

    SelectObject(memory, body_font);
    SetTextColor(memory, count == 0 ? kTextDim : kDanger);
    const int clear_left = w - pad - static_cast<int>(kPanelClearWidthDip * scale_);
    RECT clear_rect{clear_left, footer_top, w - pad, h};
    DrawTextW(memory, L"清除", -1, &clear_rect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(memory, old_font);
    DeleteObject(title_font);
    DeleteObject(body_font);

    BitBlt(dc, 0, 0, w, h, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old_bitmap);
    DeleteObject(canvas);
    DeleteDC(memory);
}

std::string NotificationPanel::SnapshotJson() const {
    char buffer[256] = {};
    const std::size_t count = items_ != nullptr ? items_->size() : 0;
    std::snprintf(buffer, sizeof(buffer),
                  "{\"visible\":%s,\"tier\":\"%s\",\"count\":%zu,\"access\":\"%s\","
                  "\"visible_rows\":%zu,\"first_row\":%zu}",
                  Visible() ? "true" : "false", core::ToString(tier_), count,
                  ToString(access_), layout_.visible_rows, layout_.first_row);
    return buffer;
}

}  // namespace dockforge::notifications
