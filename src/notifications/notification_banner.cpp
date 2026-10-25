// DockForge - the top-right notification banner (see notification_banner.h).
#include "notifications/notification_banner.h"

#include "core/log.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dockforge::notifications {
namespace {

constexpr wchar_t kBannerClass[] = L"DockForgeNotificationBanner";
constexpr char kLogModule[] = "notifications";

// Palette: the folder panel / weather popup dark family. A banner that arrives
// from the system must not look like a different product than the dock it
// appears above.
constexpr COLORREF kBackground = RGB(24, 26, 31);
constexpr COLORREF kBorder = RGB(62, 67, 78);
constexpr COLORREF kText = RGB(222, 227, 235);
constexpr COLORREF kTextDim = RGB(138, 146, 160);
constexpr COLORREF kAccent = RGB(96, 156, 240);

// Card internals, DIP.
constexpr float kCardPadDip = 12.0f;
constexpr float kCardIconDip = 26.0f;
constexpr float kCardTitleDip = 13.0f;
constexpr float kCardBodyDip = 11.0f;
constexpr float kCardLineGapDip = 3.0f;

std::wstring W(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

// Two lines of body at most, with an ellipsis. A toast body can be a paragraph;
// a banner that grows to fit it stops being a banner.
std::wstring ClampBody(const std::wstring& text) {
    constexpr std::size_t kMaxBodyChars = 140;
    if (text.size() <= kMaxBodyChars) return text;
    return text.substr(0, kMaxBodyChars - 1) + L"\u2026";
}

}  // namespace

// ---------------------------------------------------------------------------
// BannerModel
// ---------------------------------------------------------------------------

void BannerModel::SetTimeoutSeconds(int seconds) {
    timeout_seconds_ = std::max(1, seconds);
    // Applying a shorter timeout takes effect immediately rather than after the
    // current cards have already outlived it - otherwise turning the timeout
    // down looks broken for as long as the oldest card is up.
    for (BannerCard& card : cards_) {
        if (card.age_ms > static_cast<double>(timeout_seconds_) * 1000.0) {
            card.age_ms = static_cast<double>(timeout_seconds_) * 1000.0;
        }
    }
}

bool BannerModel::Push(const NotificationItem& item) {
    if (item.id == 0) return false;
    // A repeated arrival of the same toast (the sidecar pushes on change) must
    // not stack a second copy of a card that is already on screen.
    for (const BannerCard& card : cards_) {
        if (card.item.id == item.id) return false;
    }
    BannerCard card;
    card.item = item;
    card.age_ms = 0.0;
    cards_.insert(cards_.begin(), std::move(card));  // newest first
    bool changed = true;
    if (cards_.size() > kMaxBannerCards) {
        cards_.resize(kMaxBannerCards);
    }
    return changed;
}

bool BannerModel::Tick(double delta_ms) {
    if (cards_.empty()) return false;
    if (delta_ms > 0.0) {
        const double limit = static_cast<double>(timeout_seconds_) * 1000.0;
        for (BannerCard& card : cards_) card.age_ms += delta_ms;
        const std::size_t before = cards_.size();
        cards_.erase(std::remove_if(cards_.begin(), cards_.end(),
                                    [limit](const BannerCard& card) {
                                        return card.age_ms >= limit;
                                    }),
                     cards_.end());
        if (cards_.size() != before) return true;
    }
    return false;
}

bool BannerModel::Dismiss(std::size_t index) {
    if (index >= cards_.size()) return false;
    cards_.erase(cards_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

void BannerModel::Clear() { cards_.clear(); }

double BannerModel::HeightDip() const {
    if (cards_.empty()) return 0.0;
    const double count = static_cast<double>(cards_.size());
    return count * kBannerCardHeightDip + (count - 1.0) * kBannerGapDip;
}

std::size_t BannerModel::CardAtDip(double y_dip) const {
    if (y_dip < 0.0) return kNoCard;
    const double pitch = kBannerCardHeightDip + kBannerGapDip;
    const double slot = std::floor(y_dip / pitch);
    if (slot < 0.0) return kNoCard;
    const auto index = static_cast<std::size_t>(slot);
    if (index >= cards_.size()) return kNoCard;
    // The gap between cards belongs to no card: a click that lands in the 8 DIP
    // seam should dismiss nothing rather than the card above or below it.
    const double within = y_dip - slot * pitch;
    if (within > kBannerCardHeightDip) return kNoCard;
    return index;
}

// ---------------------------------------------------------------------------
// NotificationBanner
// ---------------------------------------------------------------------------

BYTE BannerAlphaForTier(core::MaterialLevel tier) {
    // Solid is fully opaque; every translucent tier lets the desktop through.
    // The numbers match the plate tints in render::StyleForTier closely enough
    // that the banner reads as the same material family.
    switch (tier) {
        case core::MaterialLevel::Mica: return 236;
        case core::MaterialLevel::Acrylic: return 244;
        case core::MaterialLevel::Blur: return 248;
        case core::MaterialLevel::Solid: return 255;
    }
    return 255;
}

NotificationBanner::~NotificationBanner() { Destroy(); }

bool NotificationBanner::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &NotificationBanner::StaticWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // WM_ERASEBKGND suppressed; WM_PAINT owns pixels
    wc.lpszClassName = kBannerClass;
    const ATOM atom = RegisterClassExW(&wc);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        DF_ERROR(kLogModule, "banner RegisterClassExW failed %lu", GetLastError());
        return false;
    }
    return true;
}

bool NotificationBanner::Create(HINSTANCE instance) {
    if (hwnd_ != nullptr) return true;
    // WS_EX_NOACTIVATE: a banner that takes focus steals the keystroke the user
    // was typing in the app that just notified them. Clicks still arrive.
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kBannerClass,
                            L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        DF_ERROR(kLogModule, "banner CreateWindowExW failed %lu", GetLastError());
        return false;
    }
    return true;
}

void NotificationBanner::Destroy() {
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void NotificationBanner::SetMaterialTier(core::MaterialLevel tier) {
    if (tier == tier_) return;
    tier_ = tier;
    if (hwnd_ == nullptr) return;
    const BYTE alpha = BannerAlphaForTier(tier);
    SetLayeredWindowAttributes(hwnd_, 0, alpha, LWA_ALPHA);
    DF_INFO(kLogModule, "banner material tier=%s alpha=%u", core::ToString(tier),
            static_cast<unsigned>(alpha));
}

void NotificationBanner::Refresh(int dpi) {
    if (hwnd_ == nullptr) return;
    if (model_.empty()) {
        Hide();
        return;
    }
    scale_ = dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
    width_px_ = static_cast<int>(kBannerWidthDip * scale_);
    height_px_ = static_cast<int>(model_.HeightDip() * scale_);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int margin = static_cast<int>(kBannerEdgeMarginDip * scale_);
    const int left = std::max(static_cast<int>(work.left),
                              static_cast<int>(work.right) - margin - width_px_);
    const int top = static_cast<int>(work.top) + margin;

    // WS_EX_LAYERED must be on before SetLayeredWindowAttributes, and it is
    // added once here rather than in Create so a banner that never shows costs
    // nothing.
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE,
                      GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hwnd_, 0, BannerAlphaForTier(tier_), LWA_ALPHA);

    SetWindowPos(hwnd_, HWND_TOPMOST, left, top, width_px_, height_px_,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void NotificationBanner::Hide() {
    if (hwnd_ == nullptr) return;
    if (!Visible()) return;
    ShowWindow(hwnd_, SW_HIDE);
    DF_DEBUG(kLogModule, "banner hidden");
}

LRESULT CALLBACK NotificationBanner::StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                                   LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW* created = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(created->lpCreateParams));
    }
    NotificationBanner* self =
        reinterpret_cast<NotificationBanner*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) return self->WndProc(hwnd, msg, wparam, lparam);
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT NotificationBanner::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
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
            const double y_dip =
                scale_ > 0.0f ? static_cast<double>(GET_Y_LPARAM(lparam)) / scale_ : 0.0;
            const std::size_t index = model_.CardAtDip(y_dip);
            if (index == BannerModel::kNoCard) return 0;
            const NotificationItem item = model_.cards()[index].item;
            // Dismiss before activating: the card has been read, and the app is
            // about to take focus, so leaving it up would show a stale stack
            // when the user comes back.
            model_.Dismiss(index);
            Refresh(static_cast<int>(scale_ * 96.0f));
            if (on_activate_) on_activate_(item);
            return 0;
        }
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                model_.Clear();
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

void NotificationBanner::Paint(HDC dc) {
    const int w = width_px_;
    const int h = height_px_;
    if (w <= 0 || h <= 0) return;

    HDC memory = CreateCompatibleDC(dc);
    HBITMAP canvas = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ old_bitmap = SelectObject(memory, canvas);
    SetBkMode(memory, TRANSPARENT);

    // The plate background is only filled when the tier is translucent: a fully
    // opaque tier still fills (the layered window's alpha is 255, so the fill is
    // what the user sees), which keeps the paint path identical across tiers.
    RECT all{0, 0, w, h};
    HBRUSH bg = CreateSolidBrush(kBackground);
    FillRect(memory, &all, bg);
    DeleteObject(bg);

    const int pad = static_cast<int>(kCardPadDip * scale_);
    const int card_h = static_cast<int>(kBannerCardHeightDip * scale_);
    const int gap = static_cast<int>(kBannerGapDip * scale_);
    const int icon = static_cast<int>(kCardIconDip * scale_);

    HPEN border = CreatePen(PS_SOLID, 1, kBorder);
    HGDIOBJ old_pen = SelectObject(memory, border);

    HFONT title_font =
        CreateFontW(-static_cast<int>(kCardTitleDip * scale_), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT body_font =
        CreateFontW(-static_cast<int>(kCardBodyDip * scale_), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(memory, title_font);

    int y = 0;
    for (const BannerCard& card : model_.cards()) {
        // Card frame.
        HBRUSH card_bg = CreateSolidBrush(kBackground);
        RECT frame{0, y, w, y + card_h};
        FillRect(memory, &frame, card_bg);
        DeleteObject(card_bg);
        SelectObject(memory, border);
        Rectangle(memory, 0, y, w, y + card_h);

        // Icon block: a rounded accent square with the sender's first character.
        // Drawing the real app icon would mean an icon lookup per arrival on the
        // paint thread; the initial is the honest cheap version and stays
        // legible at 26 DIP.
        const int icon_top = y + (card_h - icon) / 2;
        HBRUSH accent = CreateSolidBrush(kAccent);
        HGDIOBJ old_brush = SelectObject(memory, accent);
        HGDIOBJ old_icon_pen = SelectObject(memory, GetStockObject(NULL_PEN));
        Ellipse(memory, pad, icon_top, pad + icon, icon_top + icon);
        SelectObject(memory, old_icon_pen);
        SelectObject(memory, old_brush);
        DeleteObject(accent);

        const std::wstring sender = card.item.DisplayApp();
        if (!sender.empty()) {
            HFONT initial_font =
                CreateFontW(-static_cast<int>(13.0f * scale_), 0, 0, 0, FW_BOLD, 0, 0, 0,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            HGDIOBJ prev = SelectObject(memory, initial_font);
            SetTextColor(memory, RGB(255, 255, 255));
            RECT initial_rect{pad, icon_top, pad + icon, icon_top + icon};
            DrawTextW(memory, sender.substr(0, 1).c_str(), -1, &initial_rect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(memory, prev);
            DeleteObject(initial_font);
        }

        const int text_left = pad + icon + pad;
        const int text_right = w - pad;
        SelectObject(memory, title_font);
        SetTextColor(memory, kText);
        RECT sender_rect{text_left, y + pad - static_cast<int>(2 * scale_), text_right,
                         y + pad + static_cast<int>(kCardTitleDip * scale_) + 2};
        DrawTextW(memory, sender.c_str(), -1, &sender_rect,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(memory, body_font);
        SetTextColor(memory, kText);
        const std::wstring headline = card.item.Headline();
        RECT head_rect{text_left, sender_rect.bottom + static_cast<int>(kCardLineGapDip * scale_),
                       text_right, y + card_h - pad};
        DrawTextW(memory, ClampBody(headline).c_str(), -1, &head_rect,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

        const std::wstring detail = ClampBody(card.item.Detail());
        if (!detail.empty()) {
            SetTextColor(memory, kTextDim);
            RECT detail_rect{text_left, head_rect.bottom, text_right, y + card_h - pad};
            DrawTextW(memory, detail.c_str(), -1, &detail_rect,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        y += card_h + gap;
    }

    SelectObject(memory, old_font);
    SelectObject(memory, old_pen);
    DeleteObject(title_font);
    DeleteObject(body_font);
    DeleteObject(border);

    BitBlt(dc, 0, 0, w, h, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old_bitmap);
    DeleteObject(canvas);
    DeleteDC(memory);
}

std::string NotificationBanner::SnapshotJson() const {
    // Counts and ids only. Never title/body/app text: the state file is on disk
    // and ADR-0025 says notification content does not go there.
    char buffer[256] = {};
    std::snprintf(buffer, sizeof(buffer),
                  "{\"visible\":%s,\"tier\":\"%s\",\"cards\":%zu,\"timeout_seconds\":%d}",
                  Visible() ? "true" : "false", core::ToString(tier_), model_.size(),
                  model_.timeout_seconds());
    return buffer;
}

}  // namespace dockforge::notifications
