#include "dock/folder_panel.h"

#include <oleidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <shlwapi.h>

#include "core/log.h"
#include "core/paths.h"

namespace dockforge::dock {
namespace {

constexpr wchar_t kPanelClass[] = L"DockForgeFolderPanel";
constexpr char kLogModule[] = "folder";

// ---- DIP layout (scaled once at the open DPI, like every other surface) ----
constexpr float kPanelWidthDip = 300.0f;
constexpr float kRowHeightDip = 30.0f;
constexpr float kHeaderHeightDip = 34.0f;
constexpr float kHintHeightDip = 26.0f;
constexpr float kRowIconDip = 20.0f;
constexpr float kRowPadDip = 8.0f;
constexpr float kPanelGapDip = 8.0f;    // between the dock top and the panel bottom
constexpr float kEdgeMarginDip = 8.0f;  // keep-off-screen margin for x clamping
constexpr int kMaxVisibleRows = 10;
// Click-vs-drag threshold, in DIP, scaled like the layout.
constexpr float kDragThresholdDip = 5.0f;

// Palette (the preview popup's dark surface family, so the two popups read as
// one product).
constexpr COLORREF kBackground = RGB(24, 26, 31);
constexpr COLORREF kBorder = RGB(62, 67, 78);
constexpr COLORREF kHeaderSep = RGB(48, 52, 61);
constexpr COLORREF kHoverRow = RGB(44, 49, 59);
constexpr COLORREF kScrollTrack = RGB(34, 37, 44);
constexpr COLORREF kScrollThumb = RGB(96, 104, 118);
constexpr COLORREF kText = RGB(212, 218, 227);
constexpr COLORREF kTextDim = RGB(138, 146, 160);
constexpr COLORREF kBackRow = RGB(120, 156, 208);

HBRUSH BgBrush() {
    static HBRUSH brush = CreateSolidBrush(kBackground);
    return brush;
}

std::wstring LeafOf(const std::wstring& path) {
    // Trailing separators first, so "C:\dir\" reads as "dir".
    std::wstring clean = path;
    while (clean.size() > 3 && (clean.back() == L'\\' || clean.back() == L'/')) {
        clean.pop_back();
    }
    const std::size_t slash = clean.find_last_of(L"\\/");
    return slash == std::wstring::npos ? clean : clean.substr(slash + 1);
}

// OLE drag source over the shell data object. DoDragDrop owns the modal loop;
// this class only answers the two protocol calls and dies with the panel.
class PanelDropSource : public IDropSource {
  public:
    PanelDropSource() = default;
    // IUnknown has no virtual destructor in the MinGW headers, but `Release`
    // deletes `this` through the concrete type, which is well-defined; the
    // virtual keyword keeps -Wdelete-non-virtual-dtor honest about it.
    virtual ~PanelDropSource() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (out == nullptr) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *out = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (left == 0) delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape_pressed,
                                                DWORD key_state) override {
        if (escape_pressed) return DRAGDROP_S_CANCEL;
        if ((key_state & (MK_LBUTTON | MK_RBUTTON)) == 0) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

  private:
    ULONG refs_ = 1;
};

}  // namespace

FolderPanel::~FolderPanel() { Close(); }

bool FolderPanel::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &FolderPanel::StaticWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // WM_ERASEBKGND suppressed; WM_PAINT owns pixels
    wc.lpszClassName = kPanelClass;

    const ATOM atom = RegisterClassExW(&wc);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        DF_ERROR(kLogModule, "RegisterClassExW failed %lu", GetLastError());
        return false;
    }
    return true;
}

bool FolderPanel::Open(HINSTANCE instance, const std::wstring& folder_path,
                       int anchor_center_px, int dock_top_px, float dpi) {
    instance_ = instance;
    dpi_ = dpi > 0.0f ? dpi : 96.0f;
    scale_ = dpi_ / 96.0f;
    anchor_center_px_ = anchor_center_px;
    dock_top_px_ = dock_top_px;
    nav_ = shell::FolderNavStack(folder_path);
    scroll_row_ = 0;

    if (hwnd_ == nullptr) {
        if (!RegisterWindowClass(instance)) return false;
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPanelClass,
                                L"DockForgeFolderPanel", WS_POPUP, 0, 0, 1, 1, nullptr,
                                nullptr, instance, this);
        if (hwnd_ == nullptr) {
            DF_ERROR(kLogModule, "CreateWindowExW failed %lu", GetLastError());
            return false;
        }
    }
    was_activated_ = false;  // per-open: the transient rule applies afresh

    // Fonts follow the DPI the panel is opened at; per-monitor moves re-open
    // the panel, so per-open creation is correct and simple.
    if (font_ == nullptr) {
        const int body_px = -MulDiv(12, static_cast<int>(dpi_), 72);
        font_ = CreateFontW(body_px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        font_bold_ = CreateFontW(body_px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                 L"Segoe UI");
    }

    // Every status renders a panel: denied and empty show their placeholder
    // body, so the panel never fails to appear after a successful click.
    Reload("open");
    LayoutAndShow();
    // The panel takes foreground so the keyboard (Esc, Backspace) reaches it
    // without a first click. From a click handler this is allowed; from the
    // startup probe hook a refusal is logged and the first click still
    // activates the panel.
    if (!SetForegroundWindow(hwnd_)) {
        DF_DEBUG(kLogModule, "SetForegroundWindow refused on open (probe path?)");
    }
    DF_INFO(kLogModule, "panel open root=%s dpi=%.0f status=%s rows=%d",
            core::WideToUtf8(nav_.Root()).c_str(), static_cast<double>(dpi_),
            shell::ToString(status_), static_cast<int>(rows_.size()));
    return true;
}

void FolderPanel::Close() {
    watcher_.Stop();
    if (hwnd_ == nullptr) return;
    ShowWindow(hwnd_, SW_HIDE);
    if (font_ != nullptr) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    if (font_bold_ != nullptr) {
        DeleteObject(font_bold_);
        font_bold_ = nullptr;
    }
    for (auto& entry : icons_) {
        if (entry.second != nullptr) DestroyIcon(entry.second);
    }
    icons_.clear();
    rows_.clear();
    hovered_row_ = -1;
    press_active_ = false;
    press_row_ = -1;
    // Info, not debug: the close line is the probe's evidence for the Esc and
    // click-outside dismissals, and a close is never per-frame traffic.
    DF_INFO(kLogModule, "panel closed");
}

bool FolderPanel::Visible() const {
    return hwnd_ != nullptr && IsWindowVisible(hwnd_) != FALSE;
}

bool FolderPanel::DrillInto(const std::wstring& full_path) {
    if (full_path.empty()) return false;
    // Only directory paths may push: clicking a file must not re-enumerate.
    const DWORD attrs = GetFileAttributesW(full_path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        DF_DEBUG(kLogModule, "drill refused (not a directory): %s",
                 core::WideToUtf8(full_path).c_str());
        return false;
    }
    nav_.Push(full_path);
    scroll_row_ = 0;
    Reload("drill");
    return true;
}

bool FolderPanel::Back() {
    if (!nav_.Pop()) return false;
    scroll_row_ = 0;
    Reload("back");
    return true;
}

void FolderPanel::Escape() {
    if (!Visible()) return;
    // Unwind one level at a time, close at the root - the T-042 policy, shared
    // by the physical WM_KEYDOWN path and the probe command.
    if (!nav_.AtRoot()) {
        Back();
        return;
    }
    Close();
}

void FolderPanel::ForceRefresh() { Reload("refresh"); }

bool FolderPanel::TickWatcher() {
    if (!Visible()) {
        if (watcher_.Running()) watcher_.Stop();
        return false;
    }
    if (!watcher_.Running()) {
        // One root: the level being shown. ReadDirectoryChangesW has no
        // "follow the user" mode, so each level restarts the watcher.
        watcher_.Stop();
        const std::vector<std::wstring> roots{nav_.Current()};
        watcher_.Start(roots, 500.0);
        return false;
    }
    const double now_ms = static_cast<double>(GetTickCount64());
    if (watcher_.ConsumeChanged(now_ms)) {
        ForceRefresh();
        return true;
    }
    return false;
}

void FolderPanel::Reload(const char* reason) {
    const shell::FolderListing listing = shell::ListFolder(nav_.Current());
    status_ = listing.status;
    rows_.clear();

    if (status_ == shell::FolderStatus::Ok) {
        if (!nav_.AtRoot()) {
            Row back;
            back.name = L"..  返回上级";
            back.is_back = true;
            rows_.push_back(std::move(back));
        }
        for (const shell::FolderEntry& entry : listing.entries) {
            Row row;
            row.name = entry.display_name;
            row.full_path = entry.full_path;
            row.is_folder = entry.is_folder;
            rows_.push_back(std::move(row));
        }
    } else if (!nav_.AtRoot()) {
        // Even a denied or vanished subfolder keeps the way back.
        Row back;
        back.name = L"..  返回上级";
        back.is_back = true;
        rows_.push_back(std::move(back));
    }

    // Icons from the previous level are wrong for this one.
    for (auto& entry : icons_) {
        if (entry.second != nullptr) DestroyIcon(entry.second);
    }
    icons_.clear();
    // The hover state belonged to the previous level's rows.
    hovered_row_ = -1;
    hint_parts_.clear();
    hint_lines_ = 0;

    // Info, not debug: reload happens on open/drill/back/watcher-settle, which
    // is exactly the evidence chain the probe reads. Never per-frame traffic.
    DF_INFO(kLogModule, "reload(%s) status=%s rows=%zu %.2fms", reason,
            shell::ToString(status_), rows_.size(), listing.elapsed_ms);
    if (hwnd_ != nullptr && Visible()) {
        LayoutAndShow();  // the row count changes the height
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

int FolderPanel::RowHeightPx() const {
    return static_cast<int>(kRowHeightDip * scale_);
}
int FolderPanel::HeaderHeightPx() const {
    return static_cast<int>(kHeaderHeightDip * scale_);
}
int FolderPanel::HintHeightPx() const {
    return static_cast<int>(kHintHeightDip * scale_);
}

void FolderPanel::UpdateHintParts() {
    hint_parts_.clear();
    hint_lines_ = 0;
    if (hwnd_ == nullptr || font_ == nullptr) return;

    std::wstring text;
    if (status_ != shell::FolderStatus::Ok) {
        text = shell::PlaceholderFor(status_);
    } else if (hovered_row_ >= 0 && hovered_row_ < static_cast<int>(rows_.size())) {
        const Row& r = rows_[static_cast<std::size_t>(hovered_row_)];
        text = r.is_back ? L"返回上一级目录" : r.name;
    }
    if (text.empty()) return;

    // Chars per line, measured once: GetTextExtentExPointW answers "how many
    // of these code units fit max_w px", which is exactly the wrap decision a
    // space-free file name needs.
    HDC dc = GetDC(hwnd_);
    const HFONT old = static_cast<HFONT>(SelectObject(dc, font_));
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    hint_line_px_ = tm.tmHeight;
    const int max_w = width_px_ - 2 * static_cast<int>(kRowPadDip * scale_);
    constexpr int kHintMaxLines = 3;
    std::size_t pos = 0;
    for (int line = 0; line < kHintMaxLines && pos < text.size(); ++line) {
        if (line == kHintMaxLines - 1) {
            // Last line: everything that is left, ellipsised by DrawText if it
            // still overflows (a name beyond three lines is pathological).
            hint_parts_.push_back(text.substr(pos));
            pos = text.size();
            break;
        }
        int fit = 0;
        if (GetTextExtentExPointW(dc, text.c_str() + pos,
                                  static_cast<int>(text.size() - pos), max_w, &fit,
                                  nullptr, nullptr) &&
            fit > 0 && static_cast<std::size_t>(fit) < text.size() - pos) {
            hint_parts_.push_back(text.substr(pos, static_cast<std::size_t>(fit)));
            pos += static_cast<std::size_t>(fit);
        } else {
            // Either it all fits (draw one line) or the extent call failed
            // (degenerate DC) - fall back to a single ellipsised line.
            hint_parts_.push_back(text.substr(pos));
            pos = text.size();
        }
    }
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    hint_lines_ = static_cast<int>(hint_parts_.size());
}

void FolderPanel::RelayoutForHover() {
    if (hwnd_ == nullptr || !Visible()) return;
    UpdateHintParts();
    const int extra = (hint_lines_ > 1 ? hint_lines_ - 1 : 0) * hint_line_px_;
    const int height =
        HeaderHeightPx() + visible_rows_ * RowHeightPx() + HintHeightPx() + extra;
    if (height == height_px_) return;
    // Grow upward: the bottom edge stays anchored just above the dock.
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    int y = rc.bottom - height;
    if (y < work.top) y = work.top;
    height_px_ = height;
    SetWindowPos(hwnd_, HWND_TOPMOST, rc.left, y, width_px_, height_px_,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void FolderPanel::LayoutAndShow() {
    if (hwnd_ == nullptr) return;
    width_px_ = static_cast<int>(kPanelWidthDip * scale_);

    // The placeholder body still uses one row so an empty or denied folder
    // shows a visible panel rather than a sliver.
    int content_rows = 1;
    if (status_ == shell::FolderStatus::Ok && !rows_.empty()) {
        content_rows = static_cast<int>(rows_.size());
    }
    visible_rows_ = content_rows < kMaxVisibleRows ? content_rows : kMaxVisibleRows;
    height_px_ = HeaderHeightPx() + visible_rows_ * RowHeightPx() + HintHeightPx();

    // Centre on the anchor, clamp into the primary work area. The dock lives on
    // one display - the same assumption apply_placement already makes.
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = anchor_center_px_ - width_px_ / 2;
    if (x < work.left + static_cast<int>(kEdgeMarginDip * scale_)) {
        x = work.left + static_cast<int>(kEdgeMarginDip * scale_);
    }
    if (x + width_px_ > work.right - static_cast<int>(kEdgeMarginDip * scale_)) {
        x = work.right - static_cast<int>(kEdgeMarginDip * scale_) - width_px_;
    }
    int y = dock_top_px_ - height_px_ - static_cast<int>(kPanelGapDip * scale_);
    if (y < work.top) y = work.top;

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width_px_, height_px_,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

int FolderPanel::HitRow(int x_px, int y_px) const {
    if (x_px < 0 || x_px >= width_px_) return -1;
    const int top = HeaderHeightPx();
    if (y_px < top || y_px >= top + visible_rows_ * RowHeightPx()) return -1;
    const int index = scroll_row_ + (y_px - top) / RowHeightPx();
    if (index >= static_cast<int>(rows_.size())) return -1;
    return index;
}

void FolderPanel::ActivateRow(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    const Row& r = rows_[static_cast<std::size_t>(row)];
    if (r.is_back) {
        Back();
        return;
    }
    if (r.is_folder) {
        DrillInto(r.full_path);
        return;
    }
    // A file row opens with its default handler (SEE_MASK_FLAG_NO_UI: a missing
    // association fails in the log, not with a shell error dialog over the
    // panel).
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    info.lpFile = r.full_path.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        DF_WARN(kLogModule, "open file %s failed: %lu",
                core::WideToUtf8(r.full_path).c_str(), GetLastError());
    }
}

void FolderPanel::StartDragOut(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    const Row& r = rows_[static_cast<std::size_t>(row)];
    if (r.is_back || r.full_path.empty()) return;

    PIDLIST_ABSOLUTE abs_child = nullptr;
    HRESULT hr = SHParseDisplayName(r.full_path.c_str(), nullptr, &abs_child, 0, nullptr);
    if (FAILED(hr)) {
        DF_WARN(kLogModule, "drag-out parse failed 0x%08lX for %s",
                static_cast<unsigned long>(hr), core::WideToUtf8(r.full_path).c_str());
        return;
    }
    IDataObject* data = nullptr;
    {
        // Parent ABSOLUTE pidl + child RELATIVE pidl: the spike-proven shape
        // (spikes/T-066_shellfolder_enum, QueryGetData(CF_HDROP)=S_OK).
        PIDLIST_ABSOLUTE parent_abs = ILClone(abs_child);
        ILRemoveLastID(parent_abs);
        IShellFolder* parent_folder = nullptr;
        PCUITEMID_CHILD rel = nullptr;
        if (SUCCEEDED(SHBindToParent(abs_child, IID_PPV_ARGS(&parent_folder), &rel))) {
            ITEMIDLIST_RELATIVE* rel_copy = ILClone(rel);
            PCUITEMID_CHILD rel_array[1] = {rel_copy};
            hr = SHCreateDataObject(parent_abs, 1, rel_array, nullptr,
                                    IID_PPV_ARGS(&data));
            ILFree(rel_copy);
            parent_folder->Release();
        }
        ILFree(parent_abs);
        ILFree(abs_child);
    }
    if (FAILED(hr) || data == nullptr) {
        DF_WARN(kLogModule, "SHCreateDataObject failed 0x%08lX for %s",
                static_cast<unsigned long>(hr), core::WideToUtf8(r.full_path).c_str());
        return;
    }
    PanelDropSource* source = new (std::nothrow) PanelDropSource();
    if (source == nullptr) {
        data->Release();
        return;
    }

    dragging_ = true;
    press_active_ = false;
    press_row_ = -1;
    if (GetCapture() == hwnd_) ReleaseCapture();
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT dd =
        DoDragDrop(data, source, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK,
                   &effect);
    dragging_ = false;
    DF_INFO(kLogModule, "drag-out %s -> hr=0x%08lX effect=0x%08lX",
            core::WideToUtf8(r.full_path).c_str(), static_cast<unsigned long>(dd),
            static_cast<unsigned long>(effect));
    source->Release();
    data->Release();
}

HICON FolderPanel::IconFor(const Row& row) {
    if (row.is_back || row.full_path.empty()) return nullptr;
    const auto it = icons_.find(row.full_path);
    if (it != icons_.end()) return it->second;

    SHFILEINFOW sfi{};
    HICON icon = nullptr;
    if (SHGetFileInfoW(row.full_path.c_str(), 0, &sfi, sizeof(sfi),
                       SHGFI_ICON | SHGFI_LARGEICON | SHGFI_ADDOVERLAYS)) {
        icon = sfi.hIcon;
    } else {
        // Virtual-namespace children (no filesystem path): generic glyphs so
        // the row still reads instead of showing a hole.
        const DWORD flags =
            row.is_folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        if (SHGetFileInfoW(L"*", flags, &sfi, sizeof(sfi),
                           SHGFI_USEFILEATTRIBUTES | SHGFI_ICON | SHGFI_LARGEICON)) {
            icon = sfi.hIcon;
        }
    }
    icons_[row.full_path] = icon;
    return icon;
}

void FolderPanel::Paint(HDC dc) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int w = rc.right - rc.left;

    // Double buffer: a popup repainting over the desktop without one flickers
    // on every hover move.
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, rc.bottom);
    HBITMAP old = static_cast<HBITMAP>(SelectObject(mem, bmp));

    RECT full = rc;
    FillRect(mem, &full, BgBrush());

    HPEN border_pen = CreatePen(PS_SOLID, 1, kBorder);
    HGDIOBJ old_pen = SelectObject(mem, border_pen);
    HGDIOBJ old_brush = SelectObject(mem, GetStockObject(NULL_BRUSH));
    Rectangle(mem, 0, 0, w, rc.bottom);
    SelectObject(mem, old_pen);
    SelectObject(mem, old_brush);
    DeleteObject(border_pen);

    SetBkMode(mem, TRANSPARENT);

    // Header: the current level's leaf name.
    RECT header{0, 0, w, HeaderHeightPx()};
    SetTextColor(mem, kText);
    const std::wstring leaf =
        nav_.AtRoot() ? LeafOf(nav_.Root()) : LeafOf(nav_.Current());
    const HFONT old_font = static_cast<HFONT>(SelectObject(mem, font_bold_));
    DrawTextW(mem, leaf.c_str(), static_cast<int>(leaf.size()), &header,
              DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(mem, old_font);

    const HBRUSH hover_brush = CreateSolidBrush(kHoverRow);
    const HBRUSH track_brush = CreateSolidBrush(kScrollTrack);
    const HBRUSH thumb_brush = CreateSolidBrush(kScrollThumb);
    const int rows_top = HeaderHeightPx();
    const int row_h = RowHeightPx();
    const int pad = static_cast<int>(kRowPadDip * scale_);
    const int icon_px = static_cast<int>(kRowIconDip * scale_);

    // Rows first (the back row must survive a denied sublevel), then the
    // placeholder body for whatever space the rows left.
    int drawn = 0;
    int row = scroll_row_;
    for (; drawn < visible_rows_ && row < static_cast<int>(rows_.size());
         ++drawn, ++row) {
        const Row& r = rows_[static_cast<std::size_t>(row)];
        RECT row_rc{0, rows_top + drawn * row_h, w, rows_top + (drawn + 1) * row_h};
        if (row == hovered_row_) FillRect(mem, &row_rc, hover_brush);

        if (icon_px > 0) {
            const HICON icon = IconFor(r);
            if (icon != nullptr) {
                const int icon_y = row_rc.top + (row_h - icon_px) / 2;
                DrawIconEx(mem, pad, icon_y, icon, icon_px, icon_px, 0, nullptr,
                           DI_NORMAL);
            }
        }
        // Name, truncated in the row; the full name lives in the hint strip
        // while hovered (the DoD pair: hover shows what truncation hides).
        RECT text_rc{row_rc.left + pad + icon_px + pad, row_rc.top,
                     row_rc.right - pad * 2, row_rc.bottom};
        SetTextColor(mem, r.is_back ? kBackRow : kText);
        DrawTextW(mem, r.name.c_str(), static_cast<int>(r.name.size()), &text_rc,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if (status_ != shell::FolderStatus::Ok || rows_.empty()) {
        // Friendly placeholder body: denied, failed, or simply empty. It fills
        // the rows area the listing did not use.
        RECT body{0, rows_top + drawn * row_h, w,
                  rows_top + visible_rows_ * row_h};
        if (body.bottom <= body.top) body = RECT{0, rows_top, w, rows_top + row_h};
        SetTextColor(mem, kTextDim);
        const std::wstring text = status_ != shell::FolderStatus::Ok
                                      ? shell::PlaceholderFor(status_)
                                      : shell::PlaceholderFor(shell::FolderStatus::Ok);
        DrawTextW(mem, text.c_str(), static_cast<int>(text.size()), &body,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    }

    // Scroll thumb when the level has more rows than fit.
    if (static_cast<int>(rows_.size()) > visible_rows_) {
        const int track_h = visible_rows_ * row_h;
        const float frac =
            static_cast<float>(visible_rows_) / static_cast<float>(rows_.size());
        const int thumb_h =
            track_h > 0 ? static_cast<int>(static_cast<float>(track_h) * frac) : 0;
        const int max_scroll = static_cast<int>(rows_.size()) - visible_rows_;
        const int thumb_y =
            rows_top + (max_scroll > 0
                            ? static_cast<int>(
                                  static_cast<float>(scroll_row_) /
                                  static_cast<float>(max_scroll) *
                                  static_cast<float>(track_h - thumb_h))
                            : 0);
        RECT track{w - 5, rows_top, w - 2, rows_top + track_h};
        FillRect(mem, &track, track_brush);
        RECT thumb{w - 5, thumb_y, w - 2, thumb_y + thumb_h};
        FillRect(mem, &thumb, thumb_brush);
    }
    DeleteObject(hover_brush);
    DeleteObject(track_brush);
    DeleteObject(thumb_brush);

    // Hint strip: the hovered row's FULL display name, wrapped by character
    // count (UpdateHintParts, up to three lines) - the DoD pair is "rows
    // truncate, hover reveals the name". No hovered row: the strip stays
    // empty except for the status placeholder when the level failed.
    const int hint_top = rows_top + visible_rows_ * row_h;
    const int hint_pad = static_cast<int>(kRowPadDip * scale_);
    RECT hint_sep{0, hint_top - 1, w, hint_top};
    const HBRUSH sep_brush = CreateSolidBrush(kHeaderSep);
    FillRect(mem, &hint_sep, sep_brush);
    DeleteObject(sep_brush);

    if (!hint_parts_.empty()) {
        SetTextColor(mem, kTextDim);
        const HFONT hint_font = static_cast<HFONT>(SelectObject(mem, font_));
        int y = hint_top;
        for (std::size_t i = 0; i < hint_parts_.size(); ++i) {
            RECT line{hint_pad, y, w - hint_pad, y + hint_line_px_ + 4};
            DrawTextW(mem, hint_parts_[i].c_str(),
                      static_cast<int>(hint_parts_[i].size()), &line,
                      DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
            y += hint_line_px_ + 2;
        }
        SelectObject(mem, hint_font);
    }

    BitBlt(dc, 0, 0, w, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

LRESULT CALLBACK FolderPanel::StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                            LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    auto* self = reinterpret_cast<FolderPanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) return DefWindowProcW(hwnd, msg, wparam, lparam);
    return self->WndProc(hwnd, msg, wparam, lparam);
}

LRESULT FolderPanel::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            Paint(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_MOUSEMOVE: {
            // Arm the leave event once per entry so WM_MOUSELEAVE can clear
            // the hovered row; a hint that outlives the pointer is a lie.
            if (!mouse_tracked_) {
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd;
                TrackMouseEvent(&track);
                mouse_tracked_ = true;
            }
            const int row = HitRow(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            if (row != hovered_row_) {
                hovered_row_ = row;
                RelayoutForHover();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (press_active_ && !dragging_ && press_row_ >= 0) {
                const int dx = GET_X_LPARAM(lparam) - press_point_.x;
                const int dy = GET_Y_LPARAM(lparam) - press_point_.y;
                const float threshold = kDragThresholdDip * scale_;
                if (static_cast<float>(dx * dx + dy * dy) > threshold * threshold) {
                    StartDragOut(press_row_);
                }
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            if (hovered_row_ != -1) {
                hovered_row_ = -1;
                RelayoutForHover();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            mouse_tracked_ = false;
            return 0;

        case WM_LBUTTONDOWN: {
            const int row = HitRow(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            press_active_ = true;
            press_row_ = row;
            press_point_ = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            SetCapture(hwnd);
            return 0;
        }

        case WM_LBUTTONUP: {
            if (press_active_ && !dragging_) {
                const int row = HitRow(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
                // The click is the row the press AND the release agree on; a
                // release elsewhere cancels, matching the dock tile gesture.
                if (row == press_row_) ActivateRow(row);
            }
            press_active_ = false;
            press_row_ = -1;
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;
        }

        case WM_CAPTURECHANGED:
            // Something else took the capture (the drag loop, a menu). The
            // gesture is over; StartDragOut already cleared the press state.
            press_active_ = false;
            press_row_ = -1;
            return 0;

        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            const int lines = delta > 0 ? -3 : 3;
            const int max_scroll = static_cast<int>(rows_.size()) - visible_rows_;
            const int next = scroll_row_ + lines;
            scroll_row_ = next < 0 ? 0 : (next > max_scroll ? max_scroll : next);
            hovered_row_ = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                Escape();
                return 0;
            }
            if (wparam == VK_BACK) {
                Back();
                return 0;
            }
            return 0;

        case WM_ACTIVATE:
            // Click outside (another window taking focus) closes the panel -
            // the menu-like dismissal the DoD asks for. WA_INACTIVE while
            // opening is NOT a dismissal: the OS can deactivate the popup
            // between its creation and the foreground grant when it lands
            // (measured: SystemSettings/TextInputHost foreground flapping
            // closed a panel 76ms after open, before the probe could shoot
            // it). Closing on that transient would make the panel
            // undependable, so only a loss AFTER activation is a dismissal.
            if (LOWORD(wparam) == WA_INACTIVE && was_activated_ && Visible()) {
                Close();
            }
            if (LOWORD(wparam) != WA_INACTIVE) {
                was_activated_ = true;
            }
            return 0;

        case WM_DESTROY:
            hwnd_ = nullptr;
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

std::wstring FolderPanel::RowName(int index) const {
    if (index < 0 || index >= static_cast<int>(rows_.size())) return {};
    return rows_[static_cast<std::size_t>(index)].name;
}

void FolderPanel::SetHoveredRowForProbe(int row) {
    if (!Visible()) return;
    const int clamped = (row >= 0 && row < static_cast<int>(rows_.size())) ? row : -1;
    if (clamped != hovered_row_) {
        hovered_row_ = clamped;
        RelayoutForHover();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

}  // namespace dockforge::dock
