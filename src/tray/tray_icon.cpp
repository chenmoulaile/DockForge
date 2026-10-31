#include "tray/tray_icon.h"

#include "core/log.h"
#include "core/paths.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>

namespace dockforge::tray {
namespace {

constexpr const char* kModule = "tray";
constexpr wchar_t kWindowClass[] = L"DockForgeTrayWindow";
constexpr UINT kCallbackMessage = TrayIcon::kCallbackMessage;
constexpr UINT kTrayIconId = 1;
// The fallback watcher. 60ms is the cadence the spike measured: fast enough that
// the menu never looks stuck, cheap enough to be irrelevant next to a frame.
constexpr UINT kWatchTimerId = 0x7A31;
constexpr UINT kWatchIntervalMs = 60;

// The shell's own check mark needs MF_CHECKED, and a checked item is what makes a
// toggle read as a toggle.
constexpr UINT kMenuIdFor(Command command) {
    return TrayIcon::kMenuCommandBase + static_cast<UINT>(command);
}

Command CommandFromMenuId(UINT id) {
    if (id < TrayIcon::kMenuCommandBase) return Command::None;
    return static_cast<Command>(id - TrayIcon::kMenuCommandBase);
}

// Per-object state for the window procedure. The window is created by TrayIcon and
// stores its owner pointer here, the same pattern the rest of the codebase uses
// for classes that own a window.
TrayIcon* ObjectOf(HWND hwnd) {
    return reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

}  // namespace

const char* ToString(Command command) {
    switch (command) {
        case Command::None: return "none";
        case Command::ToggleDock: return "toggle_dock";
        case Command::ShowDock: return "show_dock";
        case Command::HideDock: return "hide_dock";
        case Command::ToggleFinderBar: return "toggle_finderbar";
        case Command::OpenSettings: return "open_settings";
        case Command::OpenConfigFolder: return "open_config_folder";
        case Command::OpenLogs: return "open_logs";
        case Command::Quit: return "quit";
    }
    return "unknown";
}

bool ParseCommandName(const std::string& name, Command& out) {
    // Driven by the same ToString table, so adding a command and forgetting the parser
    // is impossible: a name that ToString can produce is a name this accepts.
    for (int i = 0; i <= static_cast<int>(Command::Quit); ++i) {
        const Command candidate = static_cast<Command>(i);
        if (name == ToString(candidate)) {
            out = candidate;
            return true;
        }
    }
    return false;
}

MenuEntry MenuEntry::Separator() {
    MenuEntry entry;
    entry.separator = true;
    return entry;
}

MenuEntry MenuEntry::Item(Command command, std::wstring label, bool checked, bool enabled) {
    MenuEntry entry;
    entry.command = command;
    entry.label = std::move(label);
    entry.checked = checked;
    entry.enabled = enabled;
    return entry;
}

std::vector<MenuEntry> BuildMenu(const MenuState& state) {
    std::vector<MenuEntry> entries;
    // One toggle rather than two items: a tray menu that offers "Show" while the
    // dock is visible is asking the user to reason about state it already knows.
    entries.push_back(MenuEntry::Item(Command::ToggleDock,
                                      state.dock_visible ? L"Hide Dock" : L"Show Dock"));
    entries.push_back(MenuEntry::Item(Command::ToggleFinderBar, L"Finder Bar", state.finder_bar_enabled));
    entries.push_back(MenuEntry::Separator());
    entries.push_back(MenuEntry::Item(Command::OpenSettings, L"Settings\u2026", false,
                                      state.settings_available));
    entries.push_back(MenuEntry::Item(Command::OpenConfigFolder, L"Open Config Folder"));
    entries.push_back(MenuEntry::Item(Command::OpenLogs, L"Open Logs"));
    entries.push_back(MenuEntry::Separator());
    // Last, where a destructive item belongs, and never the default: pressing Enter
    // on a tray menu must not quit the app.
    entries.push_back(MenuEntry::Item(Command::Quit, L"Quit DockForge"));
    return entries;
}

// ---------------------------------------------------------------- icon drawing

namespace {

struct Canvas {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;

    Canvas(int w, int h) : width(w), height(h), pixels(static_cast<std::size_t>(w) * h * 4, 0) {}

    // Source-over with premultiplied source and destination, which is the layout
    // D2D and the shell both expect. Keeping the buffer premultiplied here means
    // CreateIconFromBitmap never has to convert.
    void Blend(int x, int y, float r, float g, float b, float a) {
        if (x < 0 || y < 0 || x >= width || y >= height || a <= 0.0f) return;
        auto* p = &pixels[(static_cast<std::size_t>(y) * width + x) * 4];
        const float dst_a = p[3] / 255.0f;
        const float out_a = a + dst_a * (1.0f - a);
        if (out_a <= 0.0f) return;
        const float dr = p[2] / 255.0f;
        const float dg = p[1] / 255.0f;
        const float db = p[0] / 255.0f;
        p[0] = static_cast<std::uint8_t>(std::lround(std::min(1.0f, b * a + db * (1.0f - a)) * 255.0f));
        p[1] = static_cast<std::uint8_t>(std::lround(std::min(1.0f, g * a + dg * (1.0f - a)) * 255.0f));
        p[2] = static_cast<std::uint8_t>(std::lround(std::min(1.0f, r * a + dr * (1.0f - a)) * 255.0f));
        p[3] = static_cast<std::uint8_t>(std::lround(std::min(1.0f, out_a) * 255.0f));
    }

    void FillRoundRect(float left, float top, float right, float bottom, float radius,
                       float r, float g, float b, float a) {
        const int x0 = static_cast<int>(std::floor(left));
        const int y0 = static_cast<int>(std::floor(top));
        const int x1 = static_cast<int>(std::ceil(right));
        const int y1 = static_cast<int>(std::ceil(bottom));
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const float cx = x + 0.5f;
                const float cy = y + 0.5f;
                // Distance from the rounded rectangle: clamp into the inner box and
                // measure from there. The +0.5 gives one pixel of coverage ramp so
                // the corners are not stair-stepped at 32px.
                const float ix = std::min(std::max(cx, left + radius), right - radius);
                const float iy = std::min(std::max(cy, top + radius), bottom - radius);
                const float dx = cx - ix;
                const float dy = cy - iy;
                const float coverage = std::min(1.0f, std::max(0.0f, radius - std::sqrt(dx * dx + dy * dy) + 0.5f));
                if (coverage > 0.0f) Blend(x, y, r, g, b, a * coverage);
            }
        }
    }

    void FillCircle(float cx, float cy, float radius, float r, float g, float b, float a) {
        const int x0 = static_cast<int>(std::floor(cx - radius));
        const int y0 = static_cast<int>(std::floor(cy - radius));
        const int x1 = static_cast<int>(std::ceil(cx + radius));
        const int y1 = static_cast<int>(std::ceil(cy + radius));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float dx = x + 0.5f - cx;
                const float dy = y + 0.5f - cy;
                const float coverage = std::min(1.0f, std::max(0.0f, radius - std::sqrt(dx * dx + dy * dy) + 0.5f));
                if (coverage > 0.0f) Blend(x, y, r, g, b, a * coverage);
            }
        }
    }
};

}  // namespace

void IconBitmap::CountCoverage(int* opaque, int* coloured) const {
    int opaque_count = 0;
    int coloured_count = 0;
    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
        if (pixels[i + 3] > 200) {
            ++opaque_count;
            // Anything not near-black. The plate is a dark slate and the dots are
            // near-white, so a blank or all-black icon fails this and a real glyph
            // passes.
            if (pixels[i] > 40 || pixels[i + 1] > 40 || pixels[i + 2] > 40) ++coloured_count;
        }
    }
    if (opaque != nullptr) *opaque = opaque_count;
    if (coloured != nullptr) *coloured = coloured_count;
}

IconBitmap RenderIcon(int size_px) {
    IconBitmap result;
    const int size = size_px > 0 ? size_px : 16;
    Canvas canvas(size, size);
    const float plate = static_cast<float>(size);

    // The plate is inset horizontally and vertically so the glyph has the same
    // optical weight as the icons next to it: a full-bleed square reads as bigger
    // than everything around it.
    canvas.FillRoundRect(plate * 0.04f, plate * 0.20f, plate * 0.96f, plate * 0.80f, plate * 0.20f,
                         0.16f, 0.18f, 0.22f, 1.0f);
    for (int i = 0; i < 3; ++i) {
        const float cx = plate * (0.28f + 0.22f * static_cast<float>(i));
        canvas.FillCircle(cx, plate * 0.50f, plate * 0.075f, 0.96f, 0.97f, 0.98f, 1.0f);
    }

    result.width = size;
    result.height = size;
    result.pixels = std::move(canvas.pixels);
    return result;
}

HICON CreateIconFromBitmap(const IconBitmap& bitmap) {
    if (!bitmap.valid()) return nullptr;

    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = bitmap.width;
    header.bV5Height = -bitmap.height;  // top-down, matching the buffer order
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP colour = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS,
                                      &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (colour == nullptr || bits == nullptr) {
        if (colour != nullptr) DeleteObject(colour);
        return nullptr;
    }
    std::memcpy(bits, bitmap.pixels.data(), bitmap.pixels.size());

    // A 32bpp colour bitmap carries its own alpha, but the mask must still be a
    // real bitmap: CreateIconIndirect fails outright when hbmMask is null.
    HBITMAP mask = CreateBitmap(bitmap.width, bitmap.height, 1, 1, nullptr);
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = colour;
    info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(colour);
    if (mask != nullptr) DeleteObject(mask);
    return icon;
}

// ---------------------------------------------------------------- TrayIcon

TrayIcon::TrayIcon() = default;

TrayIcon::~TrayIcon() {
    if (window_ != nullptr) {
        Remove();
        DestroyWindow(window_);
        window_ = nullptr;
    }
    if (icon_ != nullptr) {
        DestroyIcon(icon_);
        icon_ = nullptr;
    }
}

bool TrayIcon::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TrayIcon::StaticWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    if (RegisterClassExW(&wc) != 0) return true;
    // A second registration is not a failure: the class belongs to the process, not
    // to one object, and a probe that creates two instances must not trip on it.
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool TrayIcon::Initialize(HINSTANCE instance, HWND owner) {
    if (window_ != nullptr) return true;
    instance_ = instance;
    owner_ = owner;

    // The icon gets its own message window rather than the dock's, because the dock
    // window can be hidden (Hide Dock) and the tray must keep working while it is.
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"DockForgeTray", WS_POPUP, 0, 0, 1, 1,
                              nullptr, nullptr, instance, this);
    if (window_ == nullptr) {
        DF_ERROR(kModule, "CreateWindowExW failed err=%lu", GetLastError());
        return false;
    }
    // CreateWindowExW does not send WM_NCCREATE with the CREATESTRUCT pointer as the
    // last argument on all paths, so bind the object explicitly instead of relying
    // on it.
    SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");

    const int size = GetSystemMetrics(SM_CXSMICON);
    icon_ = CreateIconFromBitmap(RenderIcon(size));
    if (icon_ == nullptr) {
        DF_WARN(kModule, "icon creation failed; the tray entry will have no glyph");
    }
    DF_INFO(kModule, "ready icon_px=%d taskbar_created=0x%x", size, taskbar_created_);
    return true;
}

bool TrayIcon::Add() {
    if (window_ == nullptr) return false;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kCallbackMessage;
    data.hIcon = icon_;
    const std::wstring tip = tooltip_provider_ ? tooltip_provider_() : std::wstring(L"DockForge");
    wcsncpy_s(data.szTip, tip.c_str(), _TRUNCATE);

    // Q4 measured NIM_ADD returning 0 when the icon is already registered, so an
    // unconditional add is not idempotent. Try the add, and fall back to a modify
    // when the shell says it already knows us.
    BOOL ok = Shell_NotifyIconW(NIM_ADD, &data);
    if (!ok) {
        ok = Shell_NotifyIconW(NIM_MODIFY, &data);
        DF_DEBUG(kModule, "NIM_ADD refused, NIM_MODIFY=%d", ok ? 1 : 0);
    }
    data.uVersion = NOTIFYICON_VERSION_4;
    const BOOL versioned = Shell_NotifyIconW(NIM_SETVERSION, &data);
    if (!versioned) {
        DF_WARN(kModule, "NIM_SETVERSION(4) refused; the callback payload will be the old layout");
    }
    added_ = ok != FALSE;
    if (!added_) {
        DF_ERROR(kModule, "the shell refused the tray icon (err=%lu)", GetLastError());
    }
    return added_;
}

void TrayIcon::Remove() {
    if (window_ == nullptr || !added_) {
        added_ = false;
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayIconId;
    const BOOL ok = Shell_NotifyIconW(NIM_DELETE, &data);
    DF_INFO(kModule, "removed ok=%d", ok ? 1 : 0);
    added_ = false;
}

bool TrayIcon::HandleTaskbarCreated() {
    // Explorer restarted and every icon is gone. NIM_ADD has to be re-issued, and
    // the flag has to be cleared first or Add() will try to modify an icon the
    // shell no longer knows about.
    added_ = false;
    const bool ok = Add();
    DF_INFO(kModule, "re-registered after TaskbarCreated ok=%d", ok ? 1 : 0);
    return ok;
}

void TrayIcon::SetCommandHandler(CommandHandler handler) { command_handler_ = std::move(handler); }
void TrayIcon::SetTooltipProvider(TooltipProvider provider) { tooltip_provider_ = std::move(provider); }
void TrayIcon::SetMenuStateProvider(MenuStateProvider provider) { menu_state_provider_ = std::move(provider); }

MenuState TrayIcon::CurrentMenuState() const {
    return menu_state_provider_ ? menu_state_provider_() : MenuState{};
}

void TrayIcon::UpdateTooltip() {
    if (!added_ || tooltip_provider_ == nullptr) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_TIP | NIF_SHOWTIP;
    const std::wstring tip = tooltip_provider_();
    wcsncpy_s(data.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void TrayIcon::Dispatch(Command command) {
    last_command_ = command;
    DF_INFO(kModule, "command %s", ToString(command));
    if (command_handler_) command_handler_(command);
}

Command TrayIcon::ConsumeCommand() {
    const Command command = last_command_;
    last_command_ = Command::None;
    return command;
}

bool TrayIcon::HandleCallback(HWND hwnd, WPARAM wparam, LPARAM lparam) {
    ++callbacks_;
    // NOTIFYICON_VERSION_4 packs the anchor point into wParam and the event into the
    // low word of lParam. That is the layout Q2b measured (anchor=1200,800,
    // event=WM_RBUTTONUP, id=1), and it is the only layout this module accepts -
    // guessing between v3 and v4 at runtime is how a menu ends up opening in the
    // wrong corner.
    const int anchor_x = static_cast<short>(LOWORD(wparam));
    const int anchor_y = static_cast<short>(HIWORD(wparam));
    const UINT event = LOWORD(lparam);

    switch (event) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            // A tray icon has no window to restore, so the primary click opens the
            // menu. This is what the user expects from a status item.
            ShowContextMenu(anchor_x, anchor_y);
            return true;
        case WM_CONTEXTMENU:
            ShowContextMenu(anchor_x, anchor_y);
            return true;
        case NIN_BALLOONUSERCLICK:
            return true;
        default:
            return true;
    }
}

void TrayIcon::ShowMenu() {
    POINT cursor{};
    GetCursorPos(&cursor);
    ShowContextMenu(cursor.x, cursor.y);
}

void TrayIcon::ShowContextMenu(int x, int y) {
    if (menu_open_) return;
    const MenuState state = CurrentMenuState();
    const std::vector<MenuEntry> entries = BuildMenu(state);

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;
    UINT position = 0;
    for (const MenuEntry& entry : entries) {
        if (entry.separator) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            ++position;
            continue;
        }
        UINT flags = MF_STRING;
        if (entry.checked) flags |= MF_CHECKED;
        if (!entry.enabled) flags |= MF_GRAYED;
        AppendMenuW(menu, flags, kMenuIdFor(entry.command), entry.label.c_str());
        ++position;
    }

    menu_open_ = true;
    ++menus_shown_;

    // The documented recipe, and Q3 measured why both halves matter: without the
    // foreground the menu never enters menu mode and ignores outside clicks, and
    // without the trailing null post the next tray click is swallowed.
    const BOOL foreground = SetForegroundWindow(window_);
    SetTimer(window_, kWatchTimerId, kWatchIntervalMs, nullptr);
    const UINT selected =
        static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, x, y, 0, window_, nullptr));
    KillTimer(window_, kWatchTimerId);
    PostMessageW(window_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    menu_open_ = false;

    DF_INFO(kModule, "menu closed selected=%u foreground=%d", selected, foreground ? 1 : 0);
    if (selected != 0) Dispatch(CommandFromMenuId(selected));
}

bool TrayIcon::QueryRect(RECT* out) const {
    if (window_ == nullptr) return false;
    NOTIFYICONIDENTIFIER identifier{};
    identifier.cbSize = sizeof(identifier);
    identifier.hWnd = window_;
    identifier.uID = kTrayIconId;
    RECT rect{};
    if (FAILED(Shell_NotifyIconGetRect(&identifier, &rect))) return false;
    if (out != nullptr) *out = rect;
    return true;
}

std::string TrayIcon::SnapshotJson() const {
    nlohmann::json j;
    j["added"] = added_;
    j["callbacks"] = callbacks_;
    j["menu_open"] = menu_open_;
    j["menus_shown"] = menus_shown_;
    j["last_command"] = ToString(last_command_);
    j["window"] = reinterpret_cast<std::uintptr_t>(window_);
    // The menu the tray would show right now. A probe can assert the Quit entry exists
    // and that the dock row reads "Hide dock" without opening a modal popup, which is the
    // only way to test the menu's contents without a cursor on a system window.
    const std::vector<MenuEntry> entries = BuildMenu(CurrentMenuState());
    nlohmann::json menu = nlohmann::json::array();
    for (const MenuEntry& entry : entries) {
        menu.push_back({{"command", ToString(entry.command)},
                        {"label", core::WideToUtf8(entry.label)},
                        {"checked", entry.checked},
                        {"enabled", entry.enabled},
                        {"separator", entry.separator}});
    }
    j["menu"] = menu;
    RECT rect{};
    if (QueryRect(&rect)) {
        j["rect"] = {{"left", rect.left}, {"top", rect.top}, {"right", rect.right}, {"bottom", rect.bottom}};
    } else {
        j["rect"] = nullptr;
    }
    return j.dump();
}

LRESULT CALLBACK TrayIcon::StaticWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* self = static_cast<TrayIcon*>(create->lpCreateParams);
        if (self != nullptr) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->window_ = hwnd;
        }
    }
    TrayIcon* self = ObjectOf(hwnd);
    if (self != nullptr) return self->WndProc(hwnd, message, wparam, lparam);
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT TrayIcon::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kCallbackMessage) {
        HandleCallback(hwnd, wparam, lparam);
        return 0;
    }
    if (taskbar_created_ != 0 && message == taskbar_created_) {
        HandleTaskbarCreated();
        return 0;
    }
    if (message == WM_TIMER && wparam == kWatchTimerId) {
        // The self-rescue from Q7/Q8. When the foreground cannot be taken the menu
        // ignores outside clicks, so the click has to be detected here and the modal
        // loop ended by hand. GetAsyncKeyState is the only detector that works
        // without foreground rights.
        static bool click_handled = false;
        if (!menu_open_) {
            click_handled = false;
            return 0;
        }
        HWND popup = FindWindowW(L"#32768", nullptr);
        if (popup == nullptr) return 0;
        if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
            click_handled = false;
            return 0;
        }
        if (click_handled) return 0;
        click_handled = true;
        RECT rect{};
        GetWindowRect(popup, &rect);
        POINT cursor{};
        GetCursorPos(&cursor);
        const bool inside = cursor.x >= rect.left && cursor.x < rect.right && cursor.y >= rect.top &&
                            cursor.y < rect.bottom;
        if (!inside) {
            DF_DEBUG(kModule, "outside click at %ld,%ld -> EndMenu", cursor.x, cursor.y);
            EndMenu();
        }
        return 0;
    }
    if (message == WM_DESTROY) {
        // Deliberately no PostQuitMessage: this window shares the process message
        // loop with the dock, and quitting here would take the whole app down when
        // the tray is torn down.
        window_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace dockforge::tray
