#include "settings/settings_window.h"

#include "core/hotkeys.h"
#include "core/theme.h"
#include "core/theme_install.h"

#include "core/log.h"
#include "core/paths.h"
#include "platform/http_client.h"

#include <d2d1.h>
#include <dwrite.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>

namespace dockforge::settings {
namespace {

constexpr wchar_t kWindowClass[] = L"DockForgeSettingsWindow";
constexpr wchar_t kNavClass[] = L"DockForgeSettingsNav";
constexpr char kLogModule[] = "settings";

// 300ms debounce, frozen by C-20: every modification lands 300ms after the last
// keystroke, not 300ms after the first.
constexpr UINT kDebounceTimerId = 1;
constexpr UINT kDebounceMs = 300;

// T-085: completion signal from a theme worker thread. WPARAM carries the raw
// ThemeJobState pointer; the state stays alive in theme_jobs_ until consumed.
constexpr UINT kThemeJobDoneMsg = WM_APP + 0x85;

// Layout, in DIP at 96 DPI. Everything multiplies by the window's DPI.
constexpr int kWindowW = 880;
constexpr int kWindowH = 580;
constexpr int kNavW = 190;
constexpr int kLabelW = 170;
constexpr int kFieldW = 190;
constexpr int kRowH = 34;
constexpr int kPaneX = kNavW + 24;
constexpr int kPaneY = 28;

// Section order is the T-050 DoD order; T-069 appended "Shortcuts" before
// "About" and T-085 inserts "Themes" right after "Appearance" (the theme
// combo the section already owns is where a browser belongs). The probe
// references sections by the same lowercase names.
constexpr const wchar_t* kSectionNames[] = {
    L"Dock", L"Launchpad", L"Appearance", L"Themes", L"Behavior", L"Shortcuts", L"About",
};
constexpr const char* kSectionKeys[] = {
    "dock", "launchpad", "appearance", "themes", "behavior", "shortcuts", "about",
};
constexpr int kSectionCount = 7;

constexpr int kCtrlFirst = 100;

// Per-section control tables: one row per bound field, plus section-local
// statics. `field` empty = a display-only label.
struct ControlRow {
    const char* field;      // dotted C-01 key, empty for display-only
    const wchar_t* label;
    // Kind::Url is the T-085 index URL: trimmed free text like City but with
    // the loader's 256-byte cap instead of the city cap.
    enum class Kind { Edit, Combo, Check, Label, Action, Url } kind;
    // Combo choices, comma-separated via separate entries; null for non-combos.
    const wchar_t* const* choices;
    int choice_count;
    // T-062/T-069 action button discriminator for Kind::Action rows.
    enum class ActionTag { None, OpenLogs, ExportShortcuts, ImportShortcuts } action;
};

// The choices are the canonical strings config.json serialises (C-01), so the
// combo value round-trips without a case mapping layer.
const wchar_t* kPositionChoices[] = {L"bottom", L"left", L"right"};
const wchar_t* kMaterialChoices[] = {L"mica", L"acrylic", L"blur", L"solid"};

constexpr ControlRow kDockRows[] = {
    {"dock.icon_size", L"Icon size (32-96)", ControlRow::Kind::Edit, nullptr, 0},
    {"dock.gap", L"Gap (0-32)", ControlRow::Kind::Edit, nullptr, 0},
    {"dock.magnification", L"Magnification (1.0-2.5)", ControlRow::Kind::Edit, nullptr, 0},
    {"dock.position", L"Position", ControlRow::Kind::Combo, kPositionChoices, 3},
};

constexpr ControlRow kLaunchpadRows[] = {
    {"launchpad.columns", L"Columns (0 = adaptive)", ControlRow::Kind::Edit, nullptr, 0},
    {"launchpad.icon_size", L"Icon size (48-192)", ControlRow::Kind::Edit, nullptr, 0},
    {"launchpad.page_size", L"Page size (10-70)", ControlRow::Kind::Edit, nullptr, 0},
};

constexpr ControlRow kAppearanceRows[] = {
    {"appearance.material", L"Material", ControlRow::Kind::Combo, kMaterialChoices, 4},
    {"appearance.corner_radius", L"Corner radius (0-40)", ControlRow::Kind::Edit, nullptr, 0},
    // choices == nullptr marks the one dynamic combo: its items come from
    // core::ListThemes() at creation time, not from a static table.
    {"appearance.theme", L"Theme", ControlRow::Kind::Combo, nullptr, 0},
};

// T-085: the Themes section. The bound field row (index URL) rides the normal
// ControlRow pipeline; the two listboxes and six action buttons are created by
// CreateThemeControls after the row loop - listboxes bind to no C-01 field.
constexpr ControlRow kThemesRows[] = {
    {"themes.index_url", L"Index URL", ControlRow::Kind::Url, nullptr, 0},
};

constexpr ControlRow kBehaviorRows[] = {
    {"behavior.autostart", L"Start with Windows", ControlRow::Kind::Check, nullptr, 0},
    {"behavior.dock_visible", L"Show the dock", ControlRow::Kind::Check, nullptr, 0},
    {"behavior.hide_taskbar", L"Hide the taskbar", ControlRow::Kind::Check, nullptr, 0},
    // T-075: the weather tile. Off by default (C-01 default), so the dock keeps
    // its shape until the user asks for the tile; the city is required because
    // open-meteo has no IP-location lookup.
    {"weather.enabled", L"Weather tile", ControlRow::Kind::Check, nullptr, 0},
    {"weather.city", L"City (e.g. Beijing)", ControlRow::Kind::Edit, nullptr, 0},
    {"weather.refresh_minutes", L"Refresh minutes (5-360)", ControlRow::Kind::Edit, nullptr, 0},
    // T-065 (C-25): one independent switch per message-count adapter. The two
    // adapters ship enabled (a badge that is off until hunted down in settings
    // would never get validated); the switch lives in
    // integrations.message_counts.<adapter_id>.enabled.
    {"integrations.message_counts.weixin.exe.enabled", L"Unread badge: WeChat",
     ControlRow::Kind::Check, nullptr, 0},
    {"integrations.message_counts.qq.exe.enabled", L"Unread badge: QQ",
     ControlRow::Kind::Check, nullptr, 0},
    // T-076: the notification centre. Master switch first, then the two
    // presentation switches and the two intervals. Everything is off until
    // `enabled` is ticked, so a fresh install never spawns a sidecar process
    // the user did not ask for.
    {"notifications.enabled", L"Notification centre", ControlRow::Kind::Check, nullptr, 0},
    {"notifications.banner_enabled", L"Show banners (top-right)", ControlRow::Kind::Check,
     nullptr, 0},
    {"notifications.pulse_icons", L"Pulse the Dock icon", ControlRow::Kind::Check, nullptr, 0},
    {"notifications.banner_timeout_seconds", L"Banner seconds (2-30)",
     ControlRow::Kind::Edit, nullptr, 0},
    {"notifications.poll_seconds", L"Poll seconds (1-30)", ControlRow::Kind::Edit, nullptr, 0},
};

// T-069 (C-24): the shortcut catalog as checkbox rows. A fresh config has no
// hotkeys/hotspots at all - flipping a checkbox on writes the catalog entry
// into config.hotkeys/hotspots through SettingsModel. The combo in the label
// is the catalog default; the two built-ins (Ctrl+Alt+Space Launchpad,
// Ctrl+Alt+T taskbar restore) are always on and not listed.
constexpr ControlRow kShortcutsRows[] = {
    {"", L"Everything here is OFF until you enable it (C-24).",
     ControlRow::Kind::Label, nullptr, 0},
    {"hotkeys.toggle_dock.enabled", L"Hotkey: toggle Dock (Ctrl+Alt+D)",
     ControlRow::Kind::Check, nullptr, 0},
    {"hotkeys.toggle_launchpad.enabled", L"Hotkey: toggle Launchpad (Ctrl+Alt+L)",
     ControlRow::Kind::Check, nullptr, 0},
    {"hotkeys.toggle_taskbar.enabled", L"Hotkey: toggle taskbar (Ctrl+Alt+B)",
     ControlRow::Kind::Check, nullptr, 0},
    {"hotkeys.cycle_dock_side.enabled", L"Hotkey: cycle Dock side (Ctrl+Alt+P)",
     ControlRow::Kind::Check, nullptr, 0},
    {"hotkeys.toggle_magnify.enabled", L"Hotkey: toggle magnify (Ctrl+Alt+G)",
     ControlRow::Kind::Check, nullptr, 0},
    {"hotkeys.minimize_all.enabled", L"Hotkey: minimize all windows (Ctrl+Alt+M)",
     ControlRow::Kind::Check, nullptr, 0},
    {"hotspots.corner_tl.enabled", L"Hot corner: top-left opens Launchpad",
     ControlRow::Kind::Check, nullptr, 0},
    {"hotspots.corner_br.enabled", L"Hot corner: bottom-right minimizes all",
     ControlRow::Kind::Check, nullptr, 0},
    {"hotspots.edge_top.enabled", L"Hot edge: top edge toggles Dock",
     ControlRow::Kind::Check, nullptr, 0},
    {"hotspots.corner_tl.size_px", L"Top-left corner size (1-256)",
     ControlRow::Kind::Edit, nullptr, 0},
    {"hotspots.corner_br.size_px", L"Bottom-right corner size (1-256)",
     ControlRow::Kind::Edit, nullptr, 0},
    {"hotspots.edge_top.size_px", L"Top edge height (1-256)",
     ControlRow::Kind::Edit, nullptr, 0},
    {"", L"Export shortcuts", ControlRow::Kind::Action, nullptr, 0,
     ControlRow::ActionTag::ExportShortcuts},
    {"", L"Import shortcuts", ControlRow::Kind::Action, nullptr, 0,
     ControlRow::ActionTag::ImportShortcuts},
};

constexpr ControlRow kAboutRows[] = {
    {"", L"DockForge - a macOS-style dock and desktop", ControlRow::Kind::Label, nullptr, 0},
    {"", L"enhancement for Windows. Settings edit", ControlRow::Kind::Label, nullptr, 0},
    {"", L"%APPDATA%\\DockForge\\config.json and take", ControlRow::Kind::Label, nullptr, 0},
    {"", L"effect through ConfigChanged (C-20).", ControlRow::Kind::Label, nullptr, 0},
    // T-062: one action button, field stays empty (nothing to RecordEdit); the
    // WM_COMMAND handler matches the dedicated control id, not a config field.
    {"", L"Open log folder", ControlRow::Kind::Action, nullptr, 0},
};

constexpr const ControlRow* kSectionRows[] = {
    kDockRows, kLaunchpadRows, kAppearanceRows, kThemesRows, kBehaviorRows, kShortcutsRows,
    kAboutRows,
};
constexpr int kSectionRowCount[] = {
    static_cast<int>(std::size(kDockRows)),
    static_cast<int>(std::size(kLaunchpadRows)),
    static_cast<int>(std::size(kAppearanceRows)),
    static_cast<int>(std::size(kThemesRows)),
    static_cast<int>(std::size(kBehaviorRows)),
    static_cast<int>(std::size(kShortcutsRows)),
    static_cast<int>(std::size(kAboutRows)),
};

// The GWLP_USERDATA word carries the SettingsWindow* for both classes.
SettingsWindow* SelfFrom(HWND hwnd) {
    return reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

HFONT MakeUiFont() {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        return nullptr;
    }
    return CreateFontIndirectW(&metrics.lfMessageFont);
}

}  // namespace

// The STATIC pane containers swallow their children's control notifications:
// a BUTTON/EDIT/COMBOBOX sends WM_COMMAND to its DIRECT parent, and the system
// STATIC window procedure never forwards it up the tree. Measured with the
// T-062 probe: a real BM_CLICK on the About-page button reached the pane and
// died there - the main window's WM_COMMAND handler never ran. Every probe
// before T-062 drove RecordEdit directly and could not see this; a real user
// clicking any control has been silently losing the notification since T-050.
// Subclass the pane and forward WM_COMMAND to the main settings window
// (HWND stashed in GWLP_USERDATA at subclass time).
LRESULT CALLBACK SettingsPaneProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_COMMAND) {
        const HWND owner = reinterpret_cast<HWND>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (owner) SendMessageW(owner, WM_COMMAND, wparam, lparam);
    }
    const WNDPROC original =
        reinterpret_cast<WNDPROC>(GetPropW(hwnd, L"DockForgePaneOrigProc"));
    if (!original) return DefWindowProcW(hwnd, msg, wparam, lparam);
    return CallWindowProcW(original, hwnd, msg, wparam, lparam);
}

void SettingsWindow::SubclassPane(HWND pane, HWND owner) {
    const WNDPROC original = reinterpret_cast<WNDPROC>(
        GetWindowLongPtrW(pane, GWLP_WNDPROC));
    if (!original) return;
    SetPropW(pane, L"DockForgePaneOrigProc", reinterpret_cast<HANDLE>(original));
    SetWindowLongPtrW(pane, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owner));
    SetWindowLongPtrW(pane, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&SettingsPaneProc));
}

bool PreferLightTheme() {
    wchar_t buffer[8]{};
    const DWORD n = GetEnvironmentVariableW(L"DOCKFORGE_SETTINGS_THEME", buffer,
                                            ARRAYSIZE(buffer));
    if (n > 0 && n < ARRAYSIZE(buffer)) {
        if (_wcsicmp(buffer, L"light") == 0) return true;
        if (_wcsicmp(buffer, L"dark") == 0) return false;
    }
    DWORD light = 1;
    DWORD size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light != 0;
}

SettingsWindow::~SettingsWindow() { Destroy(); }

bool SettingsWindow::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &SettingsWindow::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;

    WNDCLASSEXW nav = wc;
    nav.style = 0;
    nav.lpfnWndProc = &SettingsWindow::NavWndProc;
    nav.lpszClassName = kNavClass;

    // Re-registration is fine (the launcher may create and destroy windows many
    // times), but a class owned by a different instance is not.
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        DF_ERROR(kLogModule, "RegisterClassExW(settings) failed 0x%08lX", GetLastError());
        return false;
    }
    if (RegisterClassExW(&nav) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        DF_ERROR(kLogModule, "RegisterClassExW(settings nav) failed 0x%08lX", GetLastError());
        return false;
    }
    return true;
}

bool SettingsWindow::Create(HINSTANCE instance, core::EventBus& bus,
                            const core::Config& initial) {
    if (hwnd_) return true;
    instance_ = instance;
    bus_ = &bus;
    config_ = initial;

    const UINT dpi = GetDpiForSystem();
    const int width = MulDiv(kWindowW, static_cast<int>(dpi), 96);
    const int height = MulDiv(kWindowH, static_cast<int>(dpi), 96);

    // GWLP_USERDATA must be set before any message that touches `this` arrives;
    // CreateWindowExW delivers WM_NCCREATE with the this-pointer in CREATESTRUCT,
    // and the WndProc stores it there (same pattern as the overlay window).
    hwnd_ = CreateWindowExW(0, kWindowClass, L"DockForge Settings",
                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, width,
                            height, nullptr, nullptr, instance, this);
    if (!hwnd_) {
        DF_ERROR(kLogModule, "settings window create failed 0x%08lX", GetLastError());
        return false;
    }
    CreateControls();
    DF_INFO(kLogModule, "settings window created (%dx%d @ %u dpi)", width, height, dpi);
    return true;
}

void SettingsWindow::Destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (ui_font_) {
        DeleteObject(ui_font_);
        ui_font_ = nullptr;
    }
}

void SettingsWindow::Show() {
    if (hwnd_) ShowWindow(hwnd_, SW_SHOW);
}

void SettingsWindow::Hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                         LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    auto* self = SelfFrom(hwnd);
    if (!self) return DefWindowProcW(hwnd, msg, wparam, lparam);
    return self->HandleMessage(hwnd, msg, wparam, lparam);
}

LRESULT SettingsWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CLOSE:
            // Hiding would keep a stale control state alive across external
            // config changes; destroying means the next open re-seeds from disk.
            // Deliberately NOT PostQuitMessage - the dock must keep running.
            DestroyWindow(hwnd_);
            return 0;
        case WM_NCDESTROY:
            hwnd_ = nullptr;
            DestroyControls();
            return 0;
        case WM_TIMER:
            if (wparam == kDebounceTimerId) {
                KillTimer(hwnd_, kDebounceTimerId);
                FlushPending();
                return 0;
            }
            break;
        case kThemeJobDoneMsg: {
            // T-085: an async theme job (index fetch or pack install) finished
            // on its worker thread. Consume the state and update the lists.
            if (hwnd_ == nullptr) return 0;  // window dying; state freed by owner
            auto* raw = reinterpret_cast<ThemeJobState*>(wparam);
            HandleThemeJobDone(raw);
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            const int code = HIWORD(wparam);
            if (updating_controls_) break;
            // T-062/T-069 action buttons carry no config field; match them
            // first so the field sweep below cannot misread them.
            if (code == BN_CLICKED) {
                if (id == open_logs_id_ && open_logs_id_ != 0) {
                    OpenLogsDirectory();
                    return 0;
                }
                if (id == export_shortcuts_id_ && export_shortcuts_id_ != 0) {
                    ExportShortcuts();
                    return 0;
                }
                if (id == import_shortcuts_id_ && import_shortcuts_id_ != 0) {
                    ImportShortcuts();
                    return 0;
                }
                // T-085 theme browser actions, same contract: no config field,
                // a dedicated id, one method per action.
                if (id == theme_refresh_id_ && theme_refresh_id_ != 0) {
                    ThemeRefreshIndex();
                    return 0;
                }
                if (id == theme_install_id_ && theme_install_id_ != 0) {
                    ThemeInstallSelected();
                    return 0;
                }
                if (id == theme_enable_id_ && theme_enable_id_ != 0) {
                    ThemeEnableSelected();
                    return 0;
                }
                if (id == theme_disable_id_ && theme_disable_id_ != 0) {
                    ThemeDisable();
                    return 0;
                }
                if (id == theme_delete_id_ && theme_delete_id_ != 0) {
                    ThemeDeleteSelected();
                    return 0;
                }
                if (id == theme_open_id_ && theme_open_id_ != 0) {
                    ThemeOpenFolder();
                    return 0;
                }
            }
            // EN_CHANGE fires while the user types; combos and checks report on
            // commit. All of them funnel into RecordEdit, which is also what the
            // probe channel calls.
            for (const auto& [field, control] : field_controls_) {
                if (GetDlgCtrlID(control) != id) continue;
                wchar_t text[128]{};
                if (code == CBN_SELCHANGE || code == BN_CLICKED) {
                    if (code == CBN_SELCHANGE) {
                        LRESULT index = SendMessageW(control, CB_GETCURSEL, 0, 0);
                        wchar_t item[64]{};
                        if (index >= 0 &&
                            SendMessageW(control, CB_GETLBTEXT, index,
                                         reinterpret_cast<LPARAM>(item)) > 0) {
                            RecordEdit(field, core::WideToUtf8(item));
                        }
                    } else {
                        LRESULT state = SendMessageW(control, BM_GETCHECK, 0, 0);
                        RecordEdit(field, state == BST_CHECKED ? "1" : "0");
                    }
                } else {
                    GetWindowTextW(control, text, ARRAYSIZE(text));
                    RecordEdit(field, core::WideToUtf8(text));
                }
                break;
            }
            break;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void SettingsWindow::CreateControls() {
    if (!ui_font_) ui_font_ = MakeUiFont();

    const UINT dpi = GetDpiForSystem();
    const int scale = static_cast<int>(dpi);
    auto dip = [scale](int value) { return MulDiv(value, scale, 96); };

    RECT rc{};
    GetClientRect(hwnd_, &rc);

    nav_ = CreateWindowExW(0, kNavClass, nullptr, WS_CHILD | WS_VISIBLE, 0, 0,
                           dip(kNavW), rc.bottom, hwnd_, nullptr, instance_, this);

    int next_id = kCtrlFirst;
    for (int section = 0; section < kSectionCount; ++section) {
        // A container per section keeps ShowSection a flat Show/Hide sweep.
        HWND pane = CreateWindowExW(0, L"STATIC", nullptr, WS_CHILD | WS_CLIPCHILDREN,
                                    dip(kPaneX), dip(kPaneY), rc.right - dip(kPaneX + 16),
                                    rc.bottom - dip(kPaneY + 16), hwnd_, nullptr,
                                    instance_, nullptr);
        SubclassPane(pane, hwnd_);
        section_panes_.push_back(pane);

        const ControlRow* rows = kSectionRows[section];
        for (int r = 0; r < kSectionRowCount[section]; ++r) {
            const ControlRow& row = rows[r];
            const int y = 8 + r * kRowH;
            const DWORD style = WS_CHILD | (row.kind == ControlRow::Kind::Check
                                                ? WS_VISIBLE | BS_AUTOCHECKBOX
                                                : WS_VISIBLE);
            // The label doubles as the checkbox text for checks.
            HWND label = CreateWindowExW(0, L"STATIC", row.label,
                                         WS_CHILD | WS_VISIBLE,
                                         dip(8), dip(y + (row.kind == ControlRow::Kind::Edit ||
                                                                   row.kind == ControlRow::Kind::Url
                                                               ? 4
                                                               : 0)),
                                         dip(kLabelW), dip(22), pane, nullptr, instance_, nullptr);
            all_controls_.push_back(label);

            HWND control = nullptr;
            switch (row.kind) {
                case ControlRow::Kind::Edit:
                case ControlRow::Kind::Url:  // same EDIT; the model trims/caps
                    control = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                                  ES_AUTOHSCROLL,
                                              dip(8 + kLabelW), dip(y), dip(kFieldW),
                                              dip(24), pane,
                                              reinterpret_cast<HMENU>(
                                                  static_cast<INT_PTR>(next_id)),
                                              instance_, nullptr);
                    break;
                case ControlRow::Kind::Combo: {
                    control = CreateWindowExW(0, L"COMBOBOX", L"",
                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                                  CBS_DROPDOWNLIST,
                                              dip(8 + kLabelW), dip(y), dip(kFieldW),
                                              dip(24 * 6), pane,
                                              reinterpret_cast<HMENU>(
                                                  static_cast<INT_PTR>(next_id)),
                                              instance_, nullptr);
                    if (row.choices != nullptr) {
                        for (int c = 0; c < row.choice_count; ++c) {
                            SendMessageW(control, CB_ADDSTRING, 0,
                                         reinterpret_cast<LPARAM>(row.choices[c]));
                        }
                    } else if (row.field == std::string("appearance.theme")) {
                        // Dynamic: the theme list is whatever is installed now.
                        for (const std::string& theme : core::ListThemes()) {
                            const std::wstring wide = core::Utf8ToWide(theme);
                            SendMessageW(control, CB_ADDSTRING, 0,
                                         reinterpret_cast<LPARAM>(wide.c_str()));
                        }
                    }
                    break;
                }
                case ControlRow::Kind::Check:
                    // The checkbox replaces the label entirely.
                    ShowWindow(label, SW_HIDE);
                    control = CreateWindowExW(0, L"BUTTON", row.label,
                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                                  BS_AUTOCHECKBOX,
                                              dip(8), dip(y), dip(kLabelW + kFieldW),
                                              dip(24), pane,
                                              reinterpret_cast<HMENU>(
                                                  static_cast<INT_PTR>(next_id)),
                                              instance_, nullptr);
                    break;
                case ControlRow::Kind::Label:
                    control = nullptr;
                    break;
                case ControlRow::Kind::Action:
                    // A push button bound to a local action, not to C-01. The
                    // dedicated id lives in a member (About: open_logs_id_;
                    // Shortcuts: export_/import_shortcuts_id_); the label column
                    // is hidden like the checkbox case does.
                    ShowWindow(label, SW_HIDE);
                    control = CreateWindowExW(0, L"BUTTON", row.label,
                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                                  BS_PUSHBUTTON,
                                              dip(8), dip(y), dip(160), dip(24), pane,
                                              reinterpret_cast<HMENU>(
                                                  static_cast<INT_PTR>(next_id)),
                                              instance_, nullptr);
                    if (row.action == ControlRow::ActionTag::OpenLogs) {
                        open_logs_id_ = next_id;
                    } else if (row.action == ControlRow::ActionTag::ExportShortcuts) {
                        export_shortcuts_id_ = next_id;
                    } else if (row.action == ControlRow::ActionTag::ImportShortcuts) {
                        import_shortcuts_id_ = next_id;
                    }
                    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_),
                                 TRUE);
                    break;
            }
            if (control) {
                all_controls_.push_back(control);
                if (row.field[0] != '\0') {
                    field_controls_.emplace_back(row.field, control);
                    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_),
                                  TRUE);
                }
                ++next_id;
            }
        }
        // T-069: the conflict/status line for the Shortcuts section. It is not
        // a ControlRow - it has no field and its text comes from the owner
        // (SetShortcutsStatus), not from the model - so it gets its own HWND
        // anchored below the rows. T-085 moved Shortcuts from pane index 4 to 5.
        constexpr int kShortcutsSection = 5;
        if (section_panes_.size() > kShortcutsSection &&
            section_panes_[kShortcutsSection] != nullptr) {
            HWND pane = section_panes_[kShortcutsSection];
            const int status_y = 8 + kSectionRowCount[kShortcutsSection] * kRowH + 4;
            shortcuts_status_ = CreateWindowExW(
                0, L"STATIC", L"", WS_CHILD | SS_LEFT, dip(8), dip(status_y),
                rc.right - dip(kPaneX + 32), dip(40), pane, nullptr, instance_, nullptr);
            all_controls_.push_back(shortcuts_status_);
            SendMessageW(shortcuts_status_, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_),
                         TRUE);
            SetShortcutsStatus(shortcuts_status_text_);
        }
        // T-085: the Themes pane (index 3) gets its listboxes and action
        // buttons from the dedicated creator.
        if (section == 3 && section_panes_[3] != nullptr) {
            CreateThemeControls(section_panes_[3], rc);
        }
    }
    ApplySectionVisibility();
    EchoValues(std::vector<std::string>());
    ThemeReloadLocalList();
    if (nav_) SendMessageW(nav_, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
}

// T-085 Themes pane layout. Everything here is DIP-scaled like the rest of the
// window; the two listboxes and the status line bind to no C-01 field.
void SettingsWindow::CreateThemeControls(HWND pane, RECT client) {
    const UINT dpi = GetDpiForSystem();
    auto dip = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    int next_id = kCtrlFirst + 200;  // past every bound row id

    const int pane_w = client.right - dip(kPaneX + 32);
    auto add_button = [&](int x, int y, int w, const wchar_t* label, int* id_out) {
        HWND button = CreateWindowExW(0, L"BUTTON", label,
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      dip(x), dip(y), dip(w), dip(24), pane,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(next_id)),
                                      instance_, nullptr);
        SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
        all_controls_.push_back(button);
        *id_out = next_id;
        ++next_id;
    };

    // Remote browser list.
    HWND remote_label =
        CreateWindowExW(0, L"STATIC", L"Community themes (remote index):", WS_CHILD | WS_VISIBLE,
                        dip(8), dip(50), dip(320), dip(18), pane, nullptr, instance_, nullptr);
    all_controls_.push_back(remote_label);
    SendMessageW(remote_label, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
    theme_remote_list_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_HASSTRINGS | WS_VSCROLL,
        dip(8), dip(72), pane_w, dip(110), pane, nullptr, instance_, nullptr);
    all_controls_.push_back(theme_remote_list_);
    SendMessageW(theme_remote_list_, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
    add_button(8, 190, 150, L"Refresh index", &theme_refresh_id_);
    add_button(170, 190, 150, L"Install selected", &theme_install_id_);

    // Local manager list.
    HWND local_label =
        CreateWindowExW(0, L"STATIC", L"Installed themes:", WS_CHILD | WS_VISIBLE, dip(8),
                        dip(224), dip(320), dip(18), pane, nullptr, instance_, nullptr);
    all_controls_.push_back(local_label);
    SendMessageW(local_label, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
    theme_local_list_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_HASSTRINGS | WS_VSCROLL,
        dip(8), dip(246), pane_w, dip(110), pane, nullptr, instance_, nullptr);
    all_controls_.push_back(theme_local_list_);
    SendMessageW(theme_local_list_, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
    add_button(8, 364, 140, L"Enable selected", &theme_enable_id_);
    add_button(160, 364, 140, L"Use default theme", &theme_disable_id_);
    add_button(312, 364, 140, L"Delete selected", &theme_delete_id_);
    add_button(464, 364, 160, L"Open themes folder", &theme_open_id_);

    // Status line: every async outcome lands here, never silently.
    theme_status_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT, dip(8), dip(400),
                                    pane_w, dip(56), pane, nullptr, instance_, nullptr);
    all_controls_.push_back(theme_status_);
    SendMessageW(theme_status_, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
    ThemeSetStatus(theme_status_text_);
}

void SettingsWindow::DestroyControls() {
    for (HWND control : all_controls_) {
        if (control && IsWindow(control)) DestroyWindow(control);
    }
    all_controls_.clear();
    field_controls_.clear();
    section_panes_.clear();
    shortcuts_status_ = nullptr;
    open_logs_id_ = 0;
    export_shortcuts_id_ = 0;
    import_shortcuts_id_ = 0;
    theme_remote_list_ = nullptr;
    theme_local_list_ = nullptr;
    theme_status_ = nullptr;
    theme_refresh_id_ = 0;
    theme_install_id_ = 0;
    theme_enable_id_ = 0;
    theme_disable_id_ = 0;
    theme_delete_id_ = 0;
    theme_open_id_ = 0;
    theme_entries_.clear();
    if (nav_ && IsWindow(nav_)) DestroyWindow(nav_);
    nav_ = nullptr;
}

void SettingsWindow::ApplySectionVisibility() {
    for (int i = 0; i < static_cast<int>(section_panes_.size()); ++i) {
        if (section_panes_[i]) {
            ShowWindow(section_panes_[i], i == active_section_ ? SW_SHOW : SW_HIDE);
        }
    }
    if (nav_) InvalidateRect(nav_, nullptr, TRUE);
}

void SettingsWindow::ShowSection(const std::string& name) {
    for (int i = 0; i < kSectionCount; ++i) {
        if (name == kSectionKeys[i]) {
            if (active_section_ != i) {
                active_section_ = i;
                ApplySectionVisibility();
                DF_DEBUG(kLogModule, "section -> %s", name.c_str());
            }
            return;
        }
    }
}

// A tiny classifier: the echo path must treat combos, checks and edits
// differently and the control HWND is all it has.
static bool IsComboControl(HWND control) {
    wchar_t cls[32]{};
    GetClassNameW(control, cls, ARRAYSIZE(cls));
    // The registered class name is "ComboBox" (mixed case); an exact wcscmp
    // against "COMBOBOX" never matched, the echo fell through to SetWindowTextW,
    // and a CBS_DROPDOWNLIST combo - which has no editable text - stayed empty.
    return _wcsicmp(cls, L"combobox") == 0;
}

void SettingsWindow::EchoValues(const std::vector<std::string>& fields) {
    updating_controls_ = true;
    // No field list means "seed everything" (creation or ReloadConfig).
    for (const auto& [field, control] : field_controls_) {
        if (!fields.empty() &&
            std::find(fields.begin(), fields.end(), field) == fields.end()) {
            continue;
        }
        const std::string value = SettingsModel::CurrentValueText(config_, field);
        const std::wstring wide = core::Utf8ToWide(value);
        if (IsComboControl(control)) {
            for (int i = 0; i < SendMessageW(control, CB_GETCOUNT, 0, 0); ++i) {
                wchar_t item[64]{};
                SendMessageW(control, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(item));
                if (wide == item) {
                    SendMessageW(control, CB_SETCURSEL, i, 0);
                    break;
                }
            }
        } else if ((GetWindowLongW(control, GWL_STYLE) & BS_AUTOCHECKBOX) != 0) {
            SendMessageW(control, BM_SETCHECK,
                         value == "1" ? BST_CHECKED : BST_UNCHECKED, 0);
        } else {
            SetWindowTextW(control, wide.c_str());
        }
    }
    updating_controls_ = false;
}

void SettingsWindow::RecordEdit(const std::string& field, const std::string& text) {
    if (!SettingsModel::KnowsField(field)) {
        DF_WARN(kLogModule, "ignoring unknown field '%s'", field.c_str());
        return;
    }
    model_.Record(field, text);
    RestartDebounceTimer();
}

void SettingsWindow::RestartDebounceTimer() {
    if (hwnd_) SetTimer(hwnd_, kDebounceTimerId, kDebounceMs, nullptr);
}

// T-062 About page: opens %LOCALAPPDATA%\DockForge\logs in Explorer. The
// directory is ensured first (the Logger normally created it, but the About
// page must not depend on call order).
void SettingsWindow::OpenLogsDirectory() {
    const std::wstring dir = core::LogsDir();
    core::EnsureDirectory(dir);
    HINSTANCE result = ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr,
                                     SW_SHOWNORMAL);
    // ShellExecuteW returns a value > 32 on success; anything else is an error
    // code. Logged so a failure is visible, never silent.
    const intptr_t err = reinterpret_cast<intptr_t>(result);
    if (err > 32) {
        DF_INFO(kLogModule, "opened log folder (%s)",
                core::WideToUtf8(dir).c_str());
    } else {
        DF_WARN(kLogModule, "open log folder failed, ShellExecuteW=%zd dir=%s", err,
                core::WideToUtf8(dir).c_str());
    }
}

// T-069: the import/export file lives next to config.json so a user can copy
// shortcuts between machines with one file (C-24 import/export).
static std::wstring ShortcutsFilePath() {
    return core::ConfigDir() + L"\\shortcuts.json";
}

void SettingsWindow::SetShortcutsStatus(const std::string& text) {
    shortcuts_status_text_ = text;
    if (shortcuts_status_ && IsWindow(shortcuts_status_)) {
        SetWindowTextW(shortcuts_status_, core::Utf8ToWide(text).c_str());
    }
}

void SettingsWindow::ExportShortcuts() {
    core::EnsureDirectory(core::ConfigDir());
    const std::wstring path = ShortcutsFilePath();
    const std::string text = core::ExportShortcutsJson(config_);

    // Same write shape as config.cpp's temp+rename, minus the atomic swap:
    // shortcuts.json is a copy source, not the live config, so a torn file
    // costs a refused import rather than a bad startup.
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const std::string message = "Export failed: cannot write shortcuts.json (err=" +
                                    std::to_string(GetLastError()) + ")";
        DF_WARN(kLogModule, "%s", message.c_str());
        SetShortcutsStatus(message);
        return;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written,
                              nullptr);
    CloseHandle(handle);
    if (!ok || written != text.size()) {
        SetShortcutsStatus("Export failed: short write to shortcuts.json");
        return;
    }
    char line[160]{};
    std::snprintf(line, sizeof(line),
                  "Exported %zu hotkey(s), %zu hotspot(s) to shortcuts.json",
                  config_.hotkeys.size(), config_.hotspots.size());
    DF_INFO(kLogModule, "%s", line);
    SetShortcutsStatus(line);
}

void SettingsWindow::ImportShortcuts() {
    const std::wstring path = ShortcutsFilePath();
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        SetShortcutsStatus("Import failed: shortcuts.json not found next to config.json");
        return;
    }
    std::string text;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(handle, &size) && size.QuadPart > 0 &&
        size.QuadPart < 4 * 1024 * 1024) {
        text.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(handle, text.data(), static_cast<DWORD>(text.size()), &got, nullptr)) {
            text.clear();
        }
        text.resize(got);
    }
    CloseHandle(handle);

    std::string error;
    if (!core::ImportShortcutsJson(config_, text, &error)) {
        const std::string message = "Import failed: " + error;
        DF_WARN(kLogModule, "%s", message.c_str());
        SetShortcutsStatus(message);
        return;
    }
    // The merge changed config_: persist, re-seed the controls, publish. Same
    // order as FlushPending so the C-20 echo rule holds.
    const bool saved = core::SaveConfig(config_);
    DF_INFO(kLogModule, "shortcuts import: %zu hotkey(s), %zu hotspot(s), saved=%d",
            config_.hotkeys.size(), config_.hotspots.size(), saved ? 1 : 0);
    EchoValues(std::vector<std::string>());
    char line[160]{};
    std::snprintf(line, sizeof(line), "Imported %zu hotkey(s), %zu hotspot(s) from "
                                      "shortcuts.json (saved=%d)",
                  config_.hotkeys.size(), config_.hotspots.size(), saved ? 1 : 0);
    SetShortcutsStatus(line);
    if (bus_ != nullptr) {
        bus_->Publish(core::ConfigChanged{config_});
    }
}

void SettingsWindow::ReloadConfig(const core::Config& config) {
    config_ = config;
    model_.Clear();
    EchoValues(std::vector<std::string>());
    DF_DEBUG(kLogModule, "reloaded config from owner");
}

// ------------------------------------------------------------------ T-085

// The theme methods share one async pattern: the worker thread owns nothing
// but its shared_ptr<ThemeJobState> and a HWND copy, so a window closed
// mid-download cannot touch a dead object. The completion PostMessage either
// lands (window alive) or fails harmlessly (window gone); the state dies with
// the last shared_ptr either way.

bool SettingsWindow::ThemeJobBusy(ThemeJobKind kind) const {
    for (const auto& job : theme_jobs_) {
        if (job->kind == kind) {
            std::lock_guard<std::mutex> guard(job->mutex);
            if (!job->done) return true;
        }
    }
    return false;
}

void SettingsWindow::ThemeSetStatus(const std::string& text) {
    theme_status_text_ = text;
    if (theme_status_ && IsWindow(theme_status_)) {
        SetWindowTextW(theme_status_, core::Utf8ToWide(text).c_str());
    }
}

void SettingsWindow::ThemeRefreshIndex() {
    if (ThemeJobBusy(ThemeJobKind::kIndex)) {
        ThemeSetStatus("An index fetch is already running...");
        return;
    }
    const std::string url = config_.themes.index_url.empty()
                                ? core::DefaultThemeIndexUrl()
                                : config_.themes.index_url;
    auto state = std::make_shared<ThemeJobState>();
    state->kind = ThemeJobKind::kIndex;
    theme_jobs_.push_back(state);
    DF_INFO(kLogModule, "theme browser: index refresh requested url=%s", url.c_str());
    ThemeSetStatus("Fetching index: " + url);

    HWND hwnd = hwnd_;
    std::thread([state, hwnd, url]() {
        std::string error;
        std::vector<core::ThemeIndexEntry> entries;
        const bool ok = core::FetchThemeIndex(url, entries, &error);
        {
            std::lock_guard<std::mutex> guard(state->mutex);
            if (ok) {
                char line[160];
                std::snprintf(line, sizeof(line), "Index loaded: %zu theme(s) from %s",
                              entries.size(), url.c_str());
                state->status = line;
                state->entries = std::move(entries);
            } else {
                state->status = "Index fetch failed: " + error +
                                " (check the Index URL, or the source may be offline)";
            }
            state->done = true;
        }
        PostMessageW(hwnd, kThemeJobDoneMsg, reinterpret_cast<WPARAM>(state.get()), 0);
    }).detach();
}

void SettingsWindow::ThemeInstallSelected() {
    if (theme_remote_list_ == nullptr) return;
    const LRESULT sel = SendMessageW(theme_remote_list_, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= static_cast<LRESULT>(theme_entries_.size())) {
        ThemeSetStatus("Select a theme in the community list first.");
        return;
    }
    if (ThemeJobBusy(ThemeJobKind::kInstall)) {
        ThemeSetStatus("An install is already running...");
        return;
    }
    const core::ThemeIndexEntry entry = theme_entries_[static_cast<std::size_t>(sel)];
    auto state = std::make_shared<ThemeJobState>();
    state->kind = ThemeJobKind::kInstall;
    theme_jobs_.push_back(state);
    DF_INFO(kLogModule, "theme browser: install requested name=%s sha=%s...", entry.name.c_str(), entry.sha256.substr(0, 12).c_str());
    ThemeSetStatus("Downloading '" + entry.name + "' ...");

    HWND hwnd = hwnd_;
    std::thread([state, hwnd, entry]() {
        std::string status;
        bool installed = false;
        constexpr std::size_t kMaxPackBytes = 64u << 20;  // ADR-0026 sanity bound
        const platform::HttpBytesResult downloaded =
            platform::HttpGetBytes(entry.download_url, 30000);
        if (!downloaded.ok) {
            char line[160];
            std::snprintf(line, sizeof(line), "Download failed (status=%lu err=%lu)",
                          downloaded.status, downloaded.error);
            status = std::string("Download of '") + entry.name + "' failed: " + line;
        } else if (downloaded.body.size() > kMaxPackBytes) {
            status = std::string("Download of '") + entry.name +
                     "' refused: larger than the 64MB bound";
        } else if (entry.size_bytes > 0 &&
                   static_cast<long long>(downloaded.body.size()) != entry.size_bytes) {
            char line[160];
            std::snprintf(line, sizeof(line),
                          "size mismatch: index says %lld bytes, server sent %zu",
                          entry.size_bytes, downloaded.body.size());
            status = std::string("Download of '") + entry.name + "' refused: " + line;
        } else {
            const core::ThemeInstallResult result =
                core::InstallThemePackBytes(downloaded.body, entry.sha256, entry.name);
            installed = result.ok;
            status = result.ok ? std::string("Installed '") + entry.name +
                                     "' - switch to it under Installed themes"
                               : std::string("Install refused: ") + result.error;
        }
        {
            std::lock_guard<std::mutex> guard(state->mutex);
            state->status = std::move(status);
            state->installed = installed;
            state->done = true;
        }
        PostMessageW(hwnd, kThemeJobDoneMsg, reinterpret_cast<WPARAM>(state.get()), 0);
    }).detach();
}

void SettingsWindow::HandleThemeJobDone(void* raw_state) {
    auto* raw = static_cast<ThemeJobState*>(raw_state);
    std::shared_ptr<ThemeJobState> state;
    for (auto it = theme_jobs_.begin(); it != theme_jobs_.end(); ++it) {
        if (it->get() == raw) {
            state = *it;
            theme_jobs_.erase(it);
            break;
        }
    }
    if (!state) return;  // unknown or already consumed

    ThemeJobKind kind = ThemeJobKind::kIndex;
    std::string status;
    std::vector<core::ThemeIndexEntry> entries;
    bool refresh_local = false;
    {
        std::lock_guard<std::mutex> guard(state->mutex);
        kind = state->kind;
        status = state->status;
        entries = state->entries;
        refresh_local = state->installed;
    }

    if (kind == ThemeJobKind::kIndex) {
        theme_entries_ = std::move(entries);
        if (theme_remote_list_ && IsWindow(theme_remote_list_)) {
            SendMessageW(theme_remote_list_, LB_RESETCONTENT, 0, 0);
            for (const core::ThemeIndexEntry& entry : theme_entries_) {
                char line[192];
                std::snprintf(line, sizeof(line), "%s  -  %s%s%s",
                              entry.name.c_str(), entry.author.c_str(),
                              entry.version.empty() ? "" : " v", entry.version.c_str());
                const std::wstring wide = core::Utf8ToWide(line);
                SendMessageW(theme_remote_list_, LB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(wide.c_str()));
            }
        }
    }
    if (refresh_local) ThemeReloadLocalList();
    DF_INFO(kLogModule, "theme browser: %s", status.c_str());
    ThemeSetStatus(status);
}

void SettingsWindow::ThemeReloadLocalList() {
    if (!theme_local_list_ || !IsWindow(theme_local_list_)) return;
    SendMessageW(theme_local_list_, LB_RESETCONTENT, 0, 0);
    for (const core::ThemeDetails& details : core::ListThemeDetails()) {
        std::string line = details.name;
        if (details.builtin) {
            line += "  (built-in)";
        } else {
            if (!details.author.empty()) line += "  -  " + details.author;
            if (!details.version.empty()) line += " v" + details.version;
        }
        if (details.name == config_.appearance.theme) line += "   [active]";
        const std::wstring wide = core::Utf8ToWide(line);
        SendMessageW(theme_local_list_, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(wide.c_str()));
    }
}

// Shared selection reader: returns the raw theme name (before any " [active]"
// suffix decoration) for the listbox row the user clicked.
bool SettingsWindow::ThemeSelectedName(bool local, std::string& name_out) const {
    const HWND list = local ? theme_local_list_ : theme_remote_list_;
    if (!list || !IsWindow(list)) return false;
    const LRESULT sel = SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (sel < 0) return false;
    if (local) {
        const std::vector<core::ThemeDetails> details = core::ListThemeDetails();
        if (sel >= static_cast<LRESULT>(details.size())) return false;
        name_out = details[static_cast<std::size_t>(sel)].name;
        return true;
    }
    if (sel >= static_cast<LRESULT>(theme_entries_.size())) return false;
    name_out = theme_entries_[static_cast<std::size_t>(sel)].name;
    return true;
}

void SettingsWindow::ThemeEnableSelected() {
    std::string name;
    if (!ThemeSelectedName(true, name)) {
        ThemeSetStatus("Select an installed theme first.");
        return;
    }
    // The normal chain: RecordEdit -> 300ms debounce -> SaveConfig ->
    // ConfigChanged -> the theme lens applies the pack. Identical to typing
    // into the Appearance combo, which is the point.
    DF_INFO(kLogModule, "theme browser: enable %s via RecordEdit", name.c_str());
    RecordEdit("appearance.theme", name);
    ThemeSetStatus("Switching theme to '" + name + "'...");
}

void SettingsWindow::ThemeDisable() {
    DF_INFO(kLogModule, "theme browser: disable, back to default");
    RecordEdit("appearance.theme", "default");
    ThemeSetStatus("Switching theme to 'default'...");
}

void SettingsWindow::ThemeDeleteSelected() {
    std::string name;
    if (!ThemeSelectedName(true, name)) {
        ThemeSetStatus("Select an installed theme first.");
        return;
    }
    if (name == "default" || name == "dark") {
        ThemeSetStatus("Built-in themes cannot be deleted.");
        return;
    }
    // Deleting the active theme first falls back to 'default', so the dock
    // never references a pack that is gone (T-052 would also fall back at
    // load, but the config should not carry a dangling id).
    if (config_.appearance.theme == name) {
        RecordEdit("appearance.theme", "default");
    }
    DF_INFO(kLogModule, "theme browser: delete %s requested", name.c_str());
    std::string error;
    if (core::UninstallTheme(name, error)) {
        ThemeReloadLocalList();
        ThemeSetStatus("Deleted '" + name + "'.");
    } else {
        ThemeSetStatus("Delete failed: " + error);
    }
}

void SettingsWindow::ThemeOpenFolder() {
    const std::wstring dir = core::ThemesDir();
    core::EnsureDirectory(dir);
    const intptr_t err = reinterpret_cast<intptr_t>(
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (err > 32) {
        DF_INFO(kLogModule, "opened themes folder (%s)", core::WideToUtf8(dir).c_str());
        ThemeSetStatus("Opened the themes folder.");
    } else {
        DF_WARN(kLogModule, "open themes folder failed, ShellExecuteW=%zd dir=%s", err,
                core::WideToUtf8(dir).c_str());
        ThemeSetStatus("Opening the themes folder failed.");
    }
}

void SettingsWindow::FlushPending() {
    if (!model_.HasPending()) return;
    std::vector<std::string> changed;
    std::vector<std::string> rejected;
    const int applied = model_.Apply(config_, &changed, &rejected);
    model_.Clear();
    if (!rejected.empty()) {
        DF_WARN(kLogModule, "%zu edit(s) rejected", rejected.size());
    }
    if (applied <= 0) return;

    // C-20: echo the clamped result so the UI shows disk truth, then persist,
    // then publish. The save failing does not stop the publish - the in-memory
    // config is still what the window holds, but the log records the gap.
    EchoValues(changed);
    const bool saved = core::SaveConfig(config_);
    DF_INFO(kLogModule, "flush applied=%d changed=%zu saved=%d", applied, changed.size(),
            saved ? 1 : 0);
    if (bus_ != nullptr) {
        bus_->Publish(core::ConfigChanged{config_});
    }
}

void SettingsWindow::PaintNav() {
    // Left navigation, drawn with D2D on the child's own redirection surface. The
    // palette follows PreferLightTheme so the window works in both system modes
    // without inventing a theme system before T-052.
    if (!nav_) return;
    static ID2D1Factory* factory = nullptr;
    if (factory == nullptr) {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory);
    }
    if (factory == nullptr) return;

    RECT rc{};
    GetClientRect(nav_, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;

    ID2D1HwndRenderTarget* rt = nullptr;
    D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(rc.right),
                                   static_cast<UINT32>(rc.bottom));
    HRESULT hr = factory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
                                                 D2D1::HwndRenderTargetProperties(nav_, size),
                                                 &rt);
    if (FAILED(hr) || rt == nullptr) return;

    const bool light = PreferLightTheme();
    const D2D1_COLOR_F bg = light ? D2D1::ColorF(0xF3F3F3, 1.0f)
                                  : D2D1::ColorF(0x202124, 1.0f);
    const D2D1_COLOR_F fg = light ? D2D1::ColorF(0x1B1B1B, 1.0f)
                                  : D2D1::ColorF(0xE8EAED, 1.0f);
    const D2D1_COLOR_F accent = light ? D2D1::ColorF(0x0B57D0, 1.0f)
                                      : D2D1::ColorF(0x8AB4F8, 1.0f);
    const D2D1_COLOR_F hoverBg = light ? D2D1::ColorF(0xE1E3E8, 1.0f)
                                       : D2D1::ColorF(0x2B2C2F, 1.0f);

    rt->BeginDraw();
    rt->Clear(bg);

    static IDWriteFactory* write = nullptr;
    if (write == nullptr) {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(&write));
    }
    static IDWriteTextFormat* format = nullptr;
    if (write != nullptr && format == nullptr) {
        write->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                14.0f, L"en-US", &format);
    }
    if (format != nullptr) {
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        for (int i = 0; i < kSectionCount; ++i) {
            const D2D1_RECT_F row = D2D1::RectF(6.0f, 8.0f + 36.0f * i,
                                                static_cast<float>(rc.right) - 6.0f,
                                                8.0f + 36.0f * (i + 1));
            if (i == active_section_) {
                ID2D1SolidColorBrush* brush = nullptr;
                rt->CreateSolidColorBrush(hoverBg, &brush);
                if (brush) {
                    rt->FillRoundedRectangle(
                        D2D1::RoundedRect(row, 8.0f, 8.0f), brush);
                    brush->Release();
                }
            }
            ID2D1SolidColorBrush* textBrush = nullptr;
            rt->CreateSolidColorBrush(i == active_section_ ? accent : fg, &textBrush);
            if (textBrush) {
                rt->DrawTextW(kSectionNames[i],
                              static_cast<UINT32>(wcslen(kSectionNames[i])), format,
                              D2D1::RectF(row.left + 10.0f, row.top + 6.0f, row.right,
                                          row.bottom),
                              textBrush);
                textBrush->Release();
            }
        }
    }
    rt->EndDraw();
    rt->Release();
}

LRESULT CALLBACK SettingsWindow::NavWndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                            LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    auto* self = SelfFrom(hwnd);
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            if (self) self->PaintNav();
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const int y = GET_Y_LPARAM(lparam);
            if (self) {
                const int index = std::clamp((y - 8) / 36, 0, kSectionCount - 1);
                self->ShowSection(kSectionKeys[index]);
            }
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;  // the D2D frame covers the whole client area
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace dockforge::settings
