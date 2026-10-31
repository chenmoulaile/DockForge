// Tray icon for DockForge (T-302a).
//
// Why this module exists: before it, the only way to stop DockForge was Task
// Manager. That is not a missing convenience, it is a missing exit.
//
// Four facts were measured before writing any of this (spikes/T-302_tray):
//
//   Q2  Shell_NotifyIconGetRect is the existence proof. It returns S_OK with a
//       real screen rect while the icon is registered and E_FAIL after NIM_DELETE,
//       so "did the icon land" needs no window-tree archaeology. The classic
//       recipe (Shell_TrayWnd -> TrayNotifyWnd -> SysPager -> ToolbarWindow32 plus
//       TB_BUTTONCOUNT) returns -1 on Windows 11 because the notification area is
//       a XAML island: TrayNotifyWnd now holds a DesktopWindowContentBridge and
//       there is no SysPager and no toolbar at all.
//
//   Q3  A tray menu dismisses on an outside click only while the owner window holds
//       the foreground. With SetForegroundWindow refused, TrackPopupMenu sat for
//       17 seconds and then returned the default item id, which reads to the user
//       as "the menu is stuck". Escape and outside clicks both work once the owner
//       has the foreground.
//
//   Q7  EndMenu() from a WM_TIMER inside the modal loop does terminate
//       TrackPopupMenu (1406ms in the hostile case), so there is a real fallback
//       for when the foreground cannot be taken.
//
//   Q8  GetAsyncKeyState(VK_LBUTTON) reports the physical button regardless of
//       foreground rights, which is what makes that fallback able to see the click
//       it is reacting to.
//
// The menu itself is drawn by the shell, so it looks like every other tray menu and
// needs no theming of its own.

#pragma once

#include "core/config.h"
#include "platform/win_common.h"

#include <functional>
#include <string>
#include <vector>

namespace dockforge::tray {

// What the menu asked for. The window that owns the icon decides what each one
// means; this module never touches the dock or the config directly.
enum class Command {
    None = 0,
    ToggleDock = 1,
    ShowDock = 2,
    HideDock = 3,
    ToggleFinderBar = 4,
    OpenSettings = 5,
    OpenConfigFolder = 6,
    OpenLogs = 7,
    Quit = 8,
};

const char* ToString(Command command);

// A tray menu item. `label` is shown verbatim; `checked` draws the shell's own
// check mark, which is how a toggle reads as a toggle rather than a mystery.
struct MenuEntry {
    Command command = Command::None;
    std::wstring label;
    bool checked = false;
    bool enabled = true;
    // A separator has no command and ignores everything else.
    bool separator = false;

    static MenuEntry Separator();
    static MenuEntry Item(Command command, std::wstring label, bool checked = false, bool enabled = true);
};

// Everything the menu needs to describe the current state. Passed in rather than
// read from the config directly so the menu can be unit tested without a desktop.
struct MenuState {
    bool dock_visible = true;
    bool finder_bar_enabled = true;
    bool settings_available = true;
};

// Builds the menu. Pure: no window, no shell call, no config read. The order is
// the macOS-shaped one: the two visibility toggles first, then the three
// destinations, then Quit last where a destructive item belongs.
std::vector<MenuEntry> BuildMenu(const MenuState& state);

// Resolves a command name back to the enum ("toggle_dock" -> Command::ToggleDock).
// Declared next to BuildMenu on purpose: a probe sending a name and the popup building
// that row must agree, and two separate spellings of the same vocabulary is how they
// stop agreeing. Returns false for an unknown name.
bool ParseCommandName(const std::string& name, Command& out);

// The icon glyph, drawn into a premultiplied BGRA buffer.
//
// Returned as data rather than as a HICON so it can be asserted on: a runtime
// icon that the shell accepts can still be blank if the alpha or the mask is
// wrong, and "the tray shows a black square" is the classic symptom.
struct IconBitmap {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;  // premultiplied BGRA, top-down

    bool valid() const { return width > 0 && height > 0 && pixels.size() == static_cast<std::size_t>(width) * height * 4; }
    // Number of pixels with meaningful alpha, and how many of those are not
    // near-black. Both are needed: a filled black square has plenty of the first
    // and none of the second.
    void CountCoverage(int* opaque, int* coloured) const;
};

// Renders the DockForge tray glyph: a rounded plate with three dots, the same
// mark the Finder Bar uses for its system menu, so the two read as one product.
IconBitmap RenderIcon(int size_px);

// Turns the bitmap into a HICON the shell will accept. Returns nullptr on failure.
// The caller owns the handle and must DestroyIcon it.
HICON CreateIconFromBitmap(const IconBitmap& bitmap);

// Which way the icon is being interacted with. The tray reports both a "primary"
// and a "context" gesture; on a tray icon the primary click conventionally opens
// the menu, and that is what this module does - it has no window to restore.
enum class Gesture {
    Primary,
    Context,
    DoubleClick,
};

// Owns the icon for the lifetime of the object. Not copyable, not movable: it
// registers itself by HWND and re-registers on TaskbarCreated, both of which need
// a stable address.
class TrayIcon {
public:
    using CommandHandler = std::function<void(Command)>;
    // The label shown on hover. Kept separate from the menu so a caller can put
    // the version or the foreground app name in it later.
    using TooltipProvider = std::function<std::wstring()>;
    using MenuStateProvider = std::function<MenuState()>;

    TrayIcon();
    ~TrayIcon();
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // Registers the window class. Safe to call more than once.
    static bool RegisterWindowClass(HINSTANCE instance);

    // `owner` is the window that receives the tray callback. It must outlive this
    // object. The icon is created hidden-capable: Add() is separate so the caller
    // controls when the icon appears.
    bool Initialize(HINSTANCE instance, HWND owner);

    // Adds the icon to the notification area. Idempotent: calling it twice with the
    // icon already present issues NIM_MODIFY instead of failing. Q4 in the spike
    // measured NIM_ADD returning 0 for an existing icon, so an unconditional
    // re-add is not safe.
    bool Add();
    // Removes the icon. Safe when the icon is not present.
    void Remove();
    bool Visible() const { return added_; }

    // Re-registers after explorer restarted. Called from the owner's message hook
    // when it sees the TaskbarCreated broadcast.
    bool HandleTaskbarCreated();

    void SetCommandHandler(CommandHandler handler);
    void SetTooltipProvider(TooltipProvider provider);
    void SetMenuStateProvider(MenuStateProvider provider);

    // Handles the tray callback. `wparam`/`lparam` are the raw values from the
    // callback message; version-4 packing is decoded here so callers do not have
    // to remember that the anchor point lives in wParam and the event in lParam.
    // Returns true when the message was a tray callback.
    bool HandleCallback(HWND hwnd, WPARAM wparam, LPARAM lparam);

    // Shows the menu at the cursor. Public so a probe can drive it without a
    // physical right click, which cannot be injected reliably.
    void ShowMenu();

    // True while the menu is up. The dock uses this to keep painting instead of
    // assuming the app is idle.
    bool MenuOpen() const { return menu_open_; }

    // The last command dispatched, for probes and logs. Cleared by ConsumeCommand.
    Command LastCommand() const { return last_command_; }
    Command ConsumeCommand();

    // Runs a command as if the user had picked it from the menu, without opening one.
    // A probe cannot use the menu path for this: TrackPopupMenu is modal, so a probe that
    // drove it would block its own thread until a human clicked. This goes through the
    // same Dispatch the popup uses, so the handler under test is the real one.
    void InvokeCommand(Command command) { Dispatch(command); }

    // Existence proof straight from the shell. False means the shell has no icon
    // for us, which is exactly what Remove() must cause.
    bool QueryRect(RECT* out) const;

    // Probe/debug summary. One line of JSON, matching every other module.
    std::string SnapshotJson() const;

    HWND Handle() const { return window_; }
    HWND Owner() const { return owner_; }

    // The command the popup returned. Static so the modal loop can record it
    // without capturing `this` in a window procedure.
    static constexpr UINT kMenuCommandBase = 200;

    // The message the shell sends to this icon's window. Public because a probe (and
    // the dock's own message hook) may want to synthesize one; a private constant would
    // force every caller to duplicate the value, which is exactly how a tray icon ends up
    // listening on a channel nothing talks to.
    static constexpr UINT kCallbackMessage = WM_APP + 140;

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    void Dispatch(Command command);
    // The menu the tray would show right now. One place so the popup and the snapshot
    // can never disagree about what is on the menu.
    MenuState CurrentMenuState() const;
    void ShowContextMenu(int x, int y);
    void UpdateTooltip();

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND owner_ = nullptr;
    HICON icon_ = nullptr;
    bool added_ = false;
    bool menu_open_ = false;
    UINT taskbar_created_ = 0;
    Command last_command_ = Command::None;
    CommandHandler command_handler_;
    TooltipProvider tooltip_provider_;
    MenuStateProvider menu_state_provider_;
    int menus_shown_ = 0;
    int callbacks_ = 0;
};

}  // namespace dockforge::tray
