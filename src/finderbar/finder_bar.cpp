#include "finderbar/finder_bar.h"

#include "core/log.h"
#include "core/paths.h"
#include "dock/dock_view.h"
#include "finderbar/appbar.h"
#include "platform/win_common.h"
#include "platform/window.h"

#include <dwrite.h>
#include <winver.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <iterator>

namespace dockforge::finderbar {
namespace {

constexpr char kModule[] = "finderbar";
constexpr wchar_t kBarClass[] = L"DockForgeFinderBar";
constexpr wchar_t kMenuOwnerClass[] = L"DockForgeFinderBarMenuOwner";

// WS_EX_NOREDIRECTIONBITMAP is mandatory for the DComp surface to be the only thing
// painted. No WS_EX_TOOLWINDOW: the bar is a first-class surface, and a tool window would
// be excluded from the shell's own window lists for no benefit.
constexpr DWORD kBarStyle = WS_POPUP;
constexpr DWORD kBarExStyle = WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP;

// Posted to the bar window when the menu thread finishes, so every surface call stays on
// the thread that owns the window (DComp is not thread safe).
constexpr UINT kMsgMenuClosed = WM_APP + 61;

// System-menu command ids. Distinct from any application id because the bar handles these
// itself and never forwards them. Derived from the header constant so a probe raising a
// command by id can never disagree with the popup that produced it.
constexpr UINT kSystemIdBase = static_cast<UINT>(kSystemMenuBase);

// The bar always draws the self-tinted plate, never a sampled backdrop, and the reason is
// measured rather than aesthetic.
//
// A backdrop-sampling tier (Blur) requires SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)
// so the plate does not sample its own previous frame and settle into a flat wash
// (T-005/T-076). But that same exclusion makes the window invisible to *every* screen
// capture on the machine: screenshots, recordings, screen shares. For a dock that is an
// acceptable trade; for a menu bar pinned across the top of the desktop it is not - a bar
// that vanishes from screenshots is worse than a bar without frost.
//
// A system backdrop (Mica, accent acrylic) is not an alternative: ADR-0008 measured that
// it ignores both SetWindowRgn and per-pixel alpha, so it would fill the full 28dip
// rectangle while the bar is retracted - exactly the overlap this design exists to avoid.
//
// So the bar takes the one option that is honest and always visible: a translucent solid
// plate, drawn by the same PlateRenderer as everything else, with the same corner radius
// and border. Over a wallpaper the result is close to what the real menu bar looks like,
// and it can be verified in a screenshot because it appears in one.
render::PlateStyle BarPlateStyle() {
    return render::StyleForTier(core::MaterialLevel::Solid, 0);
}

// The region's own format pictures, looked up once per layout change rather than per
// frame: two GetLocaleInfoEx calls cost microseconds, but there is no reason to pay
// them sixty times a second when the result only changes on a locale edit.
//
// Note on the constants: the ask names LOCALE_SSHORTTIME / LOCALE_SDATE, but
// LOCALE_SDATE is the date SEPARATOR ("-" or "/"), not a picture - feeding it to
// GetDateFormatEx renders a lone slash (measured on this host, evidence/T-307). The
// complete short-date picture is LOCALE_SSHORTDATE; that is what follows the system
// region and what the user can copy verbatim, so that is what production uses.
void LocalePictures(std::wstring* time_picture, std::wstring* date_picture) {
    wchar_t buffer[64]{};
    if (time_picture != nullptr) {
        if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SSHORTTIME, buffer, 64) > 0) {
            *time_picture = buffer;
        }
    }
    if (date_picture != nullptr) {
        if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SSHORTDATE, buffer, 64) > 0) {
            *date_picture = buffer;
        }
    }
}

// Renders the configured finderbar.right table into layout inputs. Types without a
// renderer (RightTypeRegistered) are skipped entirely - no box, no gap - so a config
// referencing a widget a later task will register costs nothing today, and each task
// flips its type live by extending RightTypeRegistered alone.
std::vector<RightItem> BuildRightItems(const core::FinderBarConfig& config,
                                       const dock::weather::Snapshot& weather,
                                       const SYSTEMTIME& now, const std::wstring& time_picture,
                                       const std::wstring& date_picture) {
    std::vector<RightItem> items;
    items.reserve(config.right.size());
    for (const core::FinderBarRightItem& entry : config.right) {
        // The config stores the type as UTF-8 std::string (C-01 JSON round-trip); the
        // layout speaks wide strings.
        const std::wstring type_name = core::Utf8ToWide(entry.type);
        const std::optional<RightType> type = RightTypeFromName(type_name.c_str());
        // An unknown type string stays in the config (C-01: never drop user data) but
        // produces no box, and the skip is logged once per layout instead of every
        // frame.
        if (!type.has_value()) {
            DF_DEBUG(kModule, "finderbar.right type '%s' is not known, skipped",
                     entry.type.c_str());
            continue;
        }
        if (!RightTypeRegistered(*type)) continue;  // T-303..T-306 land here
        RightItem item;
        item.type = *type;
        item.order = entry.order;
        switch (*type) {
            case RightType::Clock:
                item.text = FormatBarClockText(now, config.clock, time_picture.c_str(),
                                               date_picture.c_str());
                item.has_icon = false;
                break;
            case RightType::WeatherBrief:
                // Same placeholder contract as the dock tile (WeatherTextFor): an
                // unread snapshot shows "--°" instead of an empty slot, and the glyph
                // shows its own Unknown placeholder ring.
                item.has_icon = true;
                item.text = dock::WeatherTextFor(weather);
                break;
            default:
                break;
        }
        items.push_back(std::move(item));
    }
    return items;
}

// The strip that summons a fully retracted bar. Three DIP is enough to hit without aiming
// and small enough that it never covers a window's own resize border, which starts at the
// very top edge of a maximized window.
constexpr float kTriggerHeightDip = 3.0f;

// macOS menu-bar text is white on a dark bar and near-black on a light one. The bar follows
// the system theme rather than inventing a third palette, so it reads as part of the shell
// instead of as an overlay (C-01 graceful degrade to a fixed palette if the query fails).
constexpr render::ColorF kTextDark{0.10f, 0.11f, 0.13f, 1.0f};
constexpr render::ColorF kTextLight{0.96f, 0.97f, 0.98f, 1.0f};
constexpr render::ColorF kHoverDark{0.0f, 0.0f, 0.0f, 0.09f};
constexpr render::ColorF kHoverLight{1.0f, 1.0f, 1.0f, 0.14f};
constexpr render::ColorF kGlyphDark{0.16f, 0.17f, 0.20f, 1.0f};
constexpr render::ColorF kGlyphLight{0.94f, 0.95f, 0.97f, 1.0f};

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

std::wstring FileNameOf(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// The DWrite factory the measurer needs. One per process: creating it costs about a
// millisecond and the layout runs on every foreground change, so a per-call factory would
// show up in the C-32 budget.
IDWriteFactory* SharedWriteFactory() {
    static IDWriteFactory* factory = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(&factory)))) {
            factory = nullptr;
        }
    }
    return factory;
}

// One live bar per process. The WinEvent hook is a C callback with no user data, and the
// process has exactly one bar, so a file-static back pointer is the honest design rather
// than a lookup table keyed by a hook nobody else can see.
FinderBar* g_active_bar = nullptr;

// Everything the menu thread needs. Filled in before the thread starts and read-only
// afterwards, so no locking is needed for the inputs; only the outputs are atomic.
struct MenuJob {
    HWND bar = nullptr;      // the bar window, used only to post the closed message
    HWND target = nullptr;   // the window that receives the forwarded WM_COMMAND
    HMENU menu = nullptr;
    bool owns_menu = false;
    int x = 0;
    int y = 0;
    std::atomic<unsigned> selected{0};
    std::atomic<bool> done{false};
};

// Only one menu can be open at a time, so one job slot is enough. The bar keeps the object
// alive until the thread reports completion.
std::atomic<MenuJob*> g_menu_job{nullptr};

LRESULT CALLBACK MenuOwnerProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_CANCELMODE) {
        DF_DEBUG(kModule, "menu owner received WM_CANCELMODE");
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// Runs the tracked menu. Lives on its own thread because TrackPopupMenuEx blocks until the
// user chooses or dismisses, and blocking the frame loop would freeze the dock (spike Q12:
// main-thread cadence 32.27ms before, 31.85ms during).
DWORD WINAPI MenuThreadProc(LPVOID param) {
    auto* job = static_cast<MenuJob*>(param);
    if (job == nullptr) return 0;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &MenuOwnerProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kMenuOwnerClass;
    RegisterClassExW(&wc);
    HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, kMenuOwnerClass, L"menu owner", WS_POPUP, 0, 0,
                                 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    if (owner == nullptr) {
        job->done.store(true);
        if (job->bar != nullptr) PostMessageW(job->bar, kMsgMenuClosed, 0, 0);
        return 0;
    }

    // The documented requirement for a popup menu to dismiss on an outside click and to
    // accept the keyboard. Spike Q8 showed both behaviours follow foreground ownership
    // exactly, so this is load-bearing, not defensive.
    if (!SetForegroundWindow(owner)) {
        DF_DEBUG(kModule, "SetForegroundWindow on the menu owner was refused");
    }
    SetFocus(owner);

    const UINT id =
        TrackPopupMenuEx(job->menu, TPM_RETURNCMD | TPM_NONOTIFY, job->x, job->y, owner, nullptr);
    job->selected.store(id);

    if (id != 0 && job->target != nullptr) {
        // C-32: forward the command to the window that owns the menu. PostMessage rather
        // than SendMessage so a slow handler cannot stall this thread's teardown.
        PostMessageW(job->target, WM_COMMAND, MAKEWPARAM(id, 0), 0);
        DF_DEBUG(kModule, "forwarded WM_COMMAND %u", id);
    }
    if (job->owns_menu && job->menu != nullptr) {
        // Only menus this module built are destroyed. A foreign HMENU belongs to the
        // application, and destroying it would take its menu bar down.
        DestroyMenu(job->menu);
    }

    DestroyWindow(owner);
    job->done.store(true);
    if (job->bar != nullptr) PostMessageW(job->bar, kMsgMenuClosed, 0, 0);
    return 0;
}

}  // namespace

// ---- name resolution ---------------------------------------------------------

float MeasureBarText(const wchar_t* text, float size_dip) {
    if (text == nullptr || *text == L'\0' || size_dip <= 0.0f) return 0.0f;
    IDWriteFactory* factory = SharedWriteFactory();
    if (factory == nullptr) return ApproximateTextWidth(text, size_dip);

    // The format matches backend_d2d's DrawLabel exactly: same family order, same weight,
    // same locale. A layout that measured with a different format would place boxes that do
    // not match the glyphs inside them.
    static IDWriteTextFormat* format = nullptr;
    static float format_size = 0.0f;
    if (format == nullptr || format_size != size_dip) {
        if (format != nullptr) {
            format->Release();
            format = nullptr;
        }
        HRESULT hr = factory->CreateTextFormat(
            L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size_dip, L"zh-CN", &format);
        if (FAILED(hr)) {
            hr = factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                           DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                           size_dip, L"zh-CN", &format);
        }
        if (FAILED(hr)) {
            format = nullptr;
            return ApproximateTextWidth(text, size_dip);
        }
        format_size = size_dip;
    }

    IDWriteTextLayout* layout = nullptr;
    const UINT32 length = static_cast<UINT32>(wcslen(text));
    if (FAILED(factory->CreateTextLayout(text, length, format, 4096.0f, size_dip * 2.0f,
                                         &layout))) {
        return ApproximateTextWidth(text, size_dip);
    }
    DWRITE_TEXT_METRICS metrics{};
    const HRESULT hr = layout->GetMetrics(&metrics);
    layout->Release();
    if (FAILED(hr)) return ApproximateTextWidth(text, size_dip);
    return metrics.widthIncludingTrailingWhitespace;
}

// ---- clock formatting (T-307, finderbar.clock.*) ------------------------------

namespace {

// Removes the seconds token from a GetTimeFormatEx format picture, together with one
// separator on each side of it. GetTimeFormatEx's seconds token is a run of 's'
// between tokens; everything else (quoted literals, other tokens, whitespace) passes
// through untouched. "HH:mm:ss" -> "HH:mm", "h:mm:ss tt" -> "h:mm tt".
std::wstring StripSecondsPicture(const wchar_t* picture) {
    if (picture == nullptr) return {};
    std::wstring out;
    std::wstring pending;  // separators held back so the one before 's' can be dropped
    bool in_quote = false;
    for (std::size_t i = 0; picture[i] != L'\0';) {
        const wchar_t ch = picture[i];
        if (ch == L'\'') {
            in_quote = !in_quote;
            out.push_back(ch);
            pending.clear();
            ++i;
            continue;
        }
        if (in_quote) {
            out.push_back(ch);
            ++i;
            continue;
        }
        if (ch == L':' || ch == L'.') {
            pending.push_back(ch);
            ++i;
            continue;
        }
        if (ch == L's' || ch == L'S') {
            pending.clear();
            while (picture[i] == L's' || picture[i] == L'S') ++i;
            if (picture[i] == L':' || picture[i] == L'.') ++i;  // the separator after it
            continue;
        }
        out.append(pending);
        pending.clear();
        out.push_back(ch);
        ++i;
    }
    out.append(pending);
    return out;
}

// Appends a ":ss" seconds field before any AM/PM designator ('t' token), or at the
// end when the picture has none - "h:mm tt" gains seconds as "h:mm:ss tt", not as
// "h:mm tt:ss". The insertion point walks back over whitespace so the separator lands
// before the space that leads the designator ("h:mm tt" inserts at the space, not
// after it).
std::wstring AppendSecondsPicture(const wchar_t* picture) {
    if (picture == nullptr) return L"HH:mm:ss";
    std::wstring out(picture);
    bool in_quote = false;
    std::size_t t_pos = std::wstring::npos;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == L'\'') in_quote = !in_quote;
        if (!in_quote && (out[i] == L't' || out[i] == L'T')) {
            t_pos = i;
            break;
        }
    }
    if (t_pos == std::wstring::npos) {
        out += L":ss";
    } else {
        while (t_pos > 0 && out[t_pos - 1] == L' ') --t_pos;
        out.insert(t_pos, L":ss");
    }
    return out;
}

bool PictureHasSeconds(const wchar_t* picture) {
    if (picture == nullptr) return false;
    bool in_quote = false;
    for (std::size_t i = 0; picture[i] != L'\0'; ++i) {
        if (picture[i] == L'\'') in_quote = !in_quote;
        if (!in_quote && (picture[i] == L's' || picture[i] == L'S')) return true;
    }
    return false;
}

}  // namespace

BarClockText FormatBarClock(const SYSTEMTIME& now, const core::FinderBarClockConfig& clock,
                            const wchar_t* time_picture, const wchar_t* date_picture) {
    BarClockText out;

    // Time picture: an explicit finderbar.clock.format takes the time shape over
    // entirely (seconds are whatever the user wrote, show_seconds does not second-
    // guess it); only the follow-the-system default gets the show_seconds add/strip
    // pass. The bare "HH:mm" covers a machine that refuses the locale query.
    std::wstring effective;
    if (!clock.format.empty()) {
        effective = core::Utf8ToWide(clock.format);
    } else {
        effective = time_picture != nullptr && *time_picture != L'\0'
                        ? std::wstring(time_picture)
                        : std::wstring(L"HH:mm");
        if (clock.show_seconds && !PictureHasSeconds(effective.c_str())) {
            effective = AppendSecondsPicture(effective.c_str());
        } else if (!clock.show_seconds && PictureHasSeconds(effective.c_str())) {
            effective = StripSecondsPicture(effective.c_str());
        }
    }
    if (effective.empty()) effective = L"HH:mm";

    wchar_t buffer[64]{};
    if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &now, effective.c_str(), buffer, 64) > 0) {
        out.time = buffer;
    }

    if (clock.show_date) {
        std::wstring date = date_picture != nullptr && *date_picture != L'\0'
                                ? std::wstring(date_picture)
                                : std::wstring(L"yyyy/MM/dd");
        if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &now, date.c_str(), buffer, 64,
                            nullptr) > 0) {
            out.date = buffer;
        }
    }
    return out;
}

std::wstring FormatBarClockText(const SYSTEMTIME& now, const core::FinderBarClockConfig& clock,
                                const wchar_t* time_picture, const wchar_t* date_picture) {
    const BarClockText parts = FormatBarClock(now, clock, time_picture, date_picture);
    if (parts.date.empty()) return parts.time;
    if (parts.time.empty()) return parts.date;
    return parts.date + L" " + parts.time;
}

std::wstring FallbackNameFromPath(const std::wstring& exe_path) {
    std::wstring name = FileNameOf(exe_path);
    const std::size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name.resize(dot);
    return name;
}

std::wstring DisplayNameForExecutable(const std::wstring& exe_path) {
    if (exe_path.empty()) return L"";

    // VerQueryValue wants the translation table before it can be asked for FileDescription,
    // and that table is a list, not a single value. The first entry is what Explorer shows,
    // so it is what the bar shows.
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(exe_path.c_str(), &handle);
    if (size == 0) return FallbackNameFromPath(exe_path);

    std::vector<std::uint8_t> buffer(size);
    if (!GetFileVersionInfoW(exe_path.c_str(), 0, size, buffer.data())) {
        return FallbackNameFromPath(exe_path);
    }

    struct Translation {
        WORD language;
        WORD codepage;
    };
    Translation* translations = nullptr;
    UINT translation_bytes = 0;
    std::wstring description;
    if (VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation",
                       reinterpret_cast<LPVOID*>(&translations), &translation_bytes) &&
        translation_bytes >= sizeof(Translation) && translations != nullptr) {
        for (UINT i = 0; i < translation_bytes / sizeof(Translation) && description.empty(); ++i) {
            wchar_t query[64]{};
            swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\FileDescription",
                       translations[i].language, translations[i].codepage);
            wchar_t* value = nullptr;
            UINT value_bytes = 0;
            if (VerQueryValueW(buffer.data(), query, reinterpret_cast<LPVOID*>(&value),
                               &value_bytes) &&
                value != nullptr && value_bytes > sizeof(wchar_t)) {
                description.assign(value);
            }
        }
    }

    // A version resource can exist and still carry an empty description; the file name is
    // then the only thing a user would recognise.
    while (!description.empty() && description.back() == L' ') description.pop_back();
    if (description.empty()) return FallbackNameFromPath(exe_path);
    return description;
}

std::wstring ResolveAppDisplayName(HWND hwnd) {
    if (hwnd == nullptr) return L"";
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return L"";

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process != nullptr) {
        wchar_t path[MAX_PATH * 2]{};
        DWORD length = static_cast<DWORD>(std::size(path));
        const BOOL ok = QueryFullProcessImageNameW(process, 0, path, &length);
        CloseHandle(process);
        if (ok) {
            const std::wstring name = DisplayNameForExecutable(path);
            if (!name.empty()) return name;
        }
    }

    // Fall back to the window title. A process we cannot open (elevated, protected) still
    // has a title, and a title is better than an empty bar.
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) return L"";
    std::wstring title(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(hwnd, title.data(), length + 1);
    if (copied <= 0) return L"";
    title.resize(static_cast<std::size_t>(copied));
    return title;
}

std::wstring StripAccelerator(const std::wstring& title) {
    std::wstring out;
    out.reserve(title.size());
    for (std::size_t i = 0; i < title.size(); ++i) {
        if (title[i] == L'&') {
            // "&&" is a literal ampersand; a lone "&" marks the accelerator.
            if (i + 1 < title.size() && title[i + 1] == L'&') {
                out.push_back(L'&');
                ++i;
                continue;
            }
            continue;
        }
        out.push_back(title[i]);
    }
    return out;
}

// Decoding rules, all measured against menu_spike13.cpp's dump of Sublime Text's menu bar
// (a single 128-byte block: "&File\0&Edit\0&Selection\0F&ind\0&View\0&Goto\0&Tools\0
// &Project\0Prefere&nces\0&Help\0").
//
// A title is a NUL-terminated run of UTF-16 code units. The buffer has no length prefix
// and no other structure, so the only safe way to read it is to walk to the terminator and
// refuse anything that does not look like a title by the time we get there.
std::wstring DecodeOwnerDrawnTitle(const std::uint8_t* bytes, std::size_t size) {
    if (bytes == nullptr || size < 2) return {};
    std::wstring title;
    bool terminated = false;
    // 128 characters is far beyond any real menu title (the longest measured is
    // "Prefere&nces" at 12) and keeps a garbage pointer from walking megabytes.
    constexpr std::size_t kMaxTitleChars = 128;
    for (std::size_t offset = 0; offset + 1 < size && title.size() < kMaxTitleChars;
         offset += 2) {
        const wchar_t ch = static_cast<wchar_t>(bytes[offset] | (bytes[offset + 1] << 8));
        if (ch == L'\0') {
            terminated = true;
            break;
        }
        // A title is printable text plus the accelerator marker. Anything below 0x20 is a
        // control code: either the pointer is not on a title, or the encoding is not what
        // the spike measured. Both mean "do not guess".
        if (ch < 0x20) return {};
        // Lone surrogates and non-characters come from misaligned reads, not from menus.
        if (ch >= 0xD800 && ch <= 0xDFFF) return {};
        title.push_back(ch);
    }
    // No terminator inside the window we were given means the read stopped in the middle of
    // a string, so what we have is a prefix and not a title. Returning it would put a
    // truncated label on the bar; the caller falls back to the application name instead.
    if (!terminated || title.empty()) return {};
    return title;
}

AppMenuSnapshot ReadAppMenu(HWND hwnd, int max_items) {
    AppMenuSnapshot snapshot;
    if (hwnd == nullptr) return snapshot;

    // GetMenu fails for the majority of modern applications - they draw their own menu bar.
    // That is the normal case, not an error (spike Q3).
    HMENU bar = GetMenu(hwnd);
    if (bar == nullptr) return snapshot;

    const int count = GetMenuItemCount(bar);
    if (count <= 0) return snapshot;

    snapshot.menu = bar;
    snapshot.readable = true;

    // Owner-drawn menu bars (Sublime Text, VS Code, anything with a custom-painted menu)
    // carry no item text at all: every top-level item is MFT_OWNERDRAW with an empty
    // string. The titles are still there, in the target process, reachable through
    // dwItemData (spike Q26). Reading them needs PROCESS_VM_READ, which fails for an
    // elevated target; that failure is silent and falls back to the application name,
    // which is the same outcome as a window with no menu at all.
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = nullptr;
    if (pid != 0) {
        process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    }
    const auto close_process = MakeGuard([&] {
        if (process != nullptr) CloseHandle(process);
    });

    const int limit = std::max(1, max_items);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_FTYPE | MIIM_STRING | MIIM_SUBMENU | MIIM_DATA | MIIM_ID;
        wchar_t text[128]{};
        info.dwTypeData = text;
        info.cch = static_cast<UINT>(std::size(text));
        if (!GetMenuItemInfoW(bar, static_cast<UINT>(i), TRUE, &info)) continue;

        std::wstring title;
        if ((info.fType & MFT_OWNERDRAW) != 0 && process != nullptr) {
            // dwItemData is a pointer in the target's address space. The spike measured it
            // landing exactly on the first byte of this item's title, inside a shared
            // NUL-separated array, so one bounded read is enough.
            std::uint8_t buffer[512]{};
            SIZE_T got = 0;
            if (ReadProcessMemory(process, reinterpret_cast<const void*>(info.dwItemData),
                                  buffer, sizeof(buffer), &got) &&
                got >= 2) {
                title = DecodeOwnerDrawnTitle(buffer, static_cast<std::size_t>(got));
            }
            if (title.empty()) {
                DF_DEBUG("finderbar", "owner-draw title for item %d unreadable (pid=%lu)", i,
                         static_cast<unsigned long>(pid));
            }
        }
        if (title.empty() && info.cch > 0 && text[0] != L'\0') {
            title = text;
        }
        if (title.empty()) continue;

        snapshot.titles.push_back(StripAccelerator(title));
        snapshot.submenus.push_back(info.hSubMenu);
    }
    if (static_cast<int>(snapshot.titles.size()) > limit) {
        // Keep the first `limit` and let the layout decide whether they fit. The rest stay
        // reachable through the overflow box, which re-reads the full list on click.
        snapshot.titles.resize(static_cast<std::size_t>(limit));
        snapshot.submenus.resize(static_cast<std::size_t>(limit));
    }
    return snapshot;
}

// ---- FinderBar ---------------------------------------------------------------

FinderBar::FinderBar() = default;

FinderBar::~FinderBar() { Shutdown(); }

bool FinderBar::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    // No CS_HREDRAW/CS_VREDRAW: the pixels come from DComp, so invalidating on resize would
    // only produce a WM_PAINT that draws nothing.
    wc.style = 0;
    wc.lpfnWndProc = &FinderBar::StaticWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kBarClass;
    const ATOM atom = RegisterClassExW(&wc);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        DF_ERROR(kModule, "RegisterClassExW failed 0x%08lX", GetLastError());
        return false;
    }
    return true;
}

void FinderBar::SetSystemCommandHandler(SystemCommandHandler handler) {
    system_command_ = std::move(handler);
}

bool FinderBar::Initialize(HINSTANCE instance, const core::Config& config) {
    if (ready_) return true;
    instance_ = instance;
    config_ = config.finderbar;

    platform::DisplayInfo display;
    if (!platform::PrimaryDisplay(display)) {
        DF_WARN(kModule, "no primary display, Finder Bar unavailable");
        return false;
    }
    // The bar follows the primary display only. Per-monitor bars are a T-203 concern, and a
    // bar on a secondary display would duplicate a system menu it does not own.
    const UINT override_dpi = platform::Window::DpiOverride();
    if (override_dpi != 0) display.effective_dpi = override_dpi;

    // Style first: CreateWindowFor decides the capture exclusion from it, and a default
    // PlateStyle would make that decision on a tier that is not the one being drawn.
    style_ = BarPlateStyle();
    if (!CreateWindowFor(display)) return false;

    // Right-region surfaces (T-307). The forecast popup is the dock's GDI popover class
    // (its window class registers idempotently); the calendar panel is the DComp card
    // the clock opens. Both are created here so a click later just shows them - and a
    // failure degrades to a logged warning with the bar itself still up, never a
    // crash in the shell surface.
    if (dock::WeatherPopup::RegisterWindowClass(instance) && weather_popup_.Create(instance)) {
        weather_popup_ready_ = true;
    } else {
        DF_WARN(kModule, "weather popup unavailable; weather_brief clicks will no-op");
    }
    calendar_panel_ = std::make_unique<CalendarPanel>();
    if (!CalendarPanel::RegisterWindowClass(instance) || !calendar_panel_->Create(instance)) {
        DF_WARN(kModule, "calendar panel unavailable; clock clicks will no-op");
        calendar_panel_.reset();
    }

    // The bar spans the display edge to edge, so a rounded plate would show the desktop
    // through its corners at the top of the screen. Radius 0 is the macOS menu-bar shape.
    DF_INFO(kModule, "Finder Bar ready display=%s dpi=%u height_dip=%.1f tier=%d",
            core::WideToUtf8(display.device).c_str(), display.effective_dpi,
            static_cast<double>(height_dip_), static_cast<int>(style_.tier));

    // SetWinEventHook reports foreground changes from any process. Spike Q4 measured 5.5ms
    // average and 8.2ms worst, well inside the 100ms budget C-32 sets.
    foreground_hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                       &FinderBar::WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (foreground_hook_ == nullptr) {
        // Without the hook the bar still works: Tick falls back to polling. Log it rather
        // than degrade silently.
        DF_WARN(kModule, "SetWinEventHook failed 0x%08lX, falling back to polling",
                GetLastError());
    }

    ready_ = true;
    g_active_bar = this;

    // The foreground window at startup never raises an event, so take the snapshot now.
    foreground_dirty_ = true;
    RefreshForeground();
    Relayout();

    SetEnabled(config_.enabled);
    return true;
}

bool FinderBar::CreateWindowFor(const platform::DisplayInfo& display) {
    display_ = display;
    dpi_ = display.effective_dpi ? display.effective_dpi : 96;
    width_dip_ = platform::PixelsToDip(display.monitor.right - display.monitor.left, dpi_);
    height_dip_ = static_cast<float>(config_.height);
    width_px_ = display.monitor.right - display.monitor.left;
    height_px_ = std::max(1, platform::DipToPixels(height_dip_, dpi_));

    window_ = CreateWindowExW(kBarExStyle, kBarClass, L"DockForge Finder Bar", kBarStyle,
                              display.monitor.left, display.monitor.top, width_px_, height_px_,
                              nullptr, nullptr, instance_, this);
    if (window_ == nullptr) {
        DF_ERROR(kModule, "CreateWindowExW failed 0x%08lX", GetLastError());
        return false;
    }

    if (!surface_.Initialize(window_)) {
        DF_ERROR(kModule, "CompositionSurface::Initialize failed");
        DestroyWindowAndSurface();
        return false;
    }
    if (!surface_.Resize(width_px_, height_px_)) {
        DF_ERROR(kModule, "CompositionSurface::Resize(%dx%d) failed", width_px_, height_px_);
        DestroyWindowAndSurface();
        return false;
    }

    backend_ = render::CreateD2DBackend();
    if (!backend_ || !backend_->Initialize()) {
        DF_ERROR(kModule, "D2D backend initialization failed");
        DestroyWindowAndSurface();
        return false;
    }

    // Keep the bar out of its own backdrop capture, but only when the plate actually
    // samples that capture. Without the exclusion the frost samples the plate it is about
    // to draw and settles into a flat wash (T-005/T-076 lesson); with it, the bar is also
    // invisible to every screenshot, screen recording and streaming tool on the machine.
    // So it is applied only for the tiers that need it - the solid tier draws a flat tint
    // and has nothing to protect.
    if (style_.use_capture) {
        SetWindowDisplayAffinity(window_, WDA_EXCLUDEFROMCAPTURE);
    } else {
        SetWindowDisplayAffinity(window_, WDA_NONE);
    }
    capture_.Start(window_, display.monitor.left, display.monitor.top, width_px_, height_px_);
    DF_DEBUG(kModule, "bar window %dx%d px at %ld,%ld", width_px_, height_px_,
             display.monitor.left, display.monitor.top);
    return true;
}

void FinderBar::DestroyWindowAndSurface() {
    capture_.Stop();
    if (menu_thread_ != nullptr) {
        // A menu left running would keep its own window alive and outlive the bar. Waiting
        // is bounded: TrackPopupMenuEx returns as soon as the owner window is destroyed.
        if (g_menu_job.load() != nullptr && window_ != nullptr) {
            PostMessageW(window_, kMsgMenuClosed, 0, 0);
        }
        WaitForSingleObject(menu_thread_, 1500);
        CloseHandle(menu_thread_);
        menu_thread_ = nullptr;
    }
    menu_running_ = false;
    if (backend_) {
        backend_->Shutdown();
        backend_.reset();
    }
    plate_.Reset();
    surface_.Shutdown();
    backdrop_pixels_.clear();
    backdrop_generation_ = 0;
    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
}

void FinderBar::Shutdown() {
    if (!ready_ && window_ == nullptr) return;
    if (foreground_hook_ != nullptr) {
        UnhookWinEvent(foreground_hook_);
        foreground_hook_ = nullptr;
    }
    if (g_active_bar == this) g_active_bar = nullptr;
    // The right-region surfaces outlive the bar window (they are owned by this object,
    // not by its HWND), so they go down with the bar here, not inside
    // DestroyWindowAndSurface.
    if (calendar_panel_) {
        calendar_panel_->Destroy();
        calendar_panel_.reset();
    }
    weather_popup_.Destroy();
    weather_popup_ready_ = false;
    DestroyWindowAndSurface();
    ready_ = false;
    enabled_ = false;
}

void FinderBar::SetEnabled(bool enabled) {
    enabled_ = enabled;
    if (window_ == nullptr) return;
    if (!enabled) {
        // A hidden bar must not leave a calendar card or a forecast floating above the
        // desktop it was anchored to.
        CloseCalendarPanel();
        weather_popup_.Hide();
        capture_.Stop();
        // Give the strip back before hiding. A reservation left behind after the bar
        // is gone would keep every maximized window 28dip short of the top edge with
        // nothing there to explain why - the exact class of invisible damage that makes
        // people distrust a desktop tweak.
        appbar::ReleaseTopStrip();
        reserved_strip_px_ = 0;
        // A hidden window keeps its region, which would still be a hit target. Reset it to
        // empty before hiding so nothing is left behind if the window is shown again by
        // another path.
        HRGN empty = CreateRectRgn(0, 0, 0, 0);
        if (empty != nullptr && SetWindowRgn(window_, empty, FALSE) == 0) DeleteObject(empty);
        region_height_px_ = 0;
        ShowWindow(window_, SW_HIDE);
        return;
    }

    // Reserve the top strip so a pinned bar does not cover a maximized window's title
    // bar. T-302b measured that this works (the work area really moves and a maximized
    // window really moves down by the reserved height) where SPI_SETWORKAREA silently
    // did nothing.
    //
    // If the reservation fails the bar must not pin itself: covering every title bar is
    // worse than hiding. So a failed reservation forces auto-hide for this session and
    // the reason is logged, not swallowed.
    if (!config_.auto_hide) {
        const bool reserved = appbar::ReserveTopStrip(window_, height_px_);
        reserved_strip_px_ = reserved ? height_px_ : 0;
        if (!reserved) {
            DF_WARN(kModule, "top strip reservation failed; falling back to auto-hide");
            auto_hide_fallback_ = true;
        } else {
            auto_hide_fallback_ = false;
        }
    } else {
        appbar::ReleaseTopStrip();
        reserved_strip_px_ = 0;
        auto_hide_fallback_ = false;
    }

    SetWindowPos(window_, HWND_TOPMOST, display_.monitor.left, display_.monitor.top, width_px_,
                 height_px_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    capture_.Start(window_, display_.monitor.left, display_.monitor.top, width_px_, height_px_);
    // A pinned bar is always out. The retracted start state only exists for the
    // auto-hide mode, where the bar must not appear over a title bar before it is
    // asked for.
    const bool retracting = AutoHideActive();
    auto_hide_ = AutoHideState{};
    auto_hide_.progress = retracting ? 0.0f : 1.0f;
    auto_hide_.shown = !retracting;
    region_height_px_ = -1;
    ApplyRegion();
    Repaint();
}

void FinderBar::ReloadConfig(const core::Config& config) {
    const bool was_enabled = config_.enabled;
    config_ = config.finderbar;

    style_ = BarPlateStyle();
    if (window_ != nullptr) {
        // The exclusion is tied to the tier, so a material change has to move it too.
        SetWindowDisplayAffinity(window_,
                                 style_.use_capture ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
    }

    const float new_height = static_cast<float>(config_.height);
    if (std::abs(new_height - height_dip_) > 0.01f) {
        height_dip_ = new_height;
        height_px_ = std::max(1, platform::DipToPixels(height_dip_, dpi_));
        if (window_ != nullptr) {
            SetWindowPos(window_, HWND_TOPMOST, display_.monitor.left, display_.monitor.top,
                         width_px_, height_px_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            surface_.Resize(width_px_, height_px_);
            capture_.SetRegion(display_.monitor.left, display_.monitor.top, width_px_, height_px_);
            region_height_px_ = -1;
            // The reserved strip has to track the bar's height: a stale reservation would
            // leave the work area short by the old height, and the bar would float above a
            // gap. Re-reserve only when one is actually held.
            if (reserved_strip_px_ > 0) {
                appbar::ReleaseTopStrip();
                if (appbar::ReserveTopStrip(window_, height_px_)) {
                    reserved_strip_px_ = height_px_;
                } else {
                    reserved_strip_px_ = 0;
                    auto_hide_fallback_ = true;
                }
            }
        }
    }

    if (was_enabled != config_.enabled) {
        SetEnabled(config_.enabled);
        return;
    }
    if (!enabled_) return;

    // A reload can flip auto_hide, which decides whether the strip is reserved at all.
    // Without this the bar keeps holding 28dip of every maximized window while it is
    // happily retracting to a hairline - the exact combination the reservation exists to
    // avoid. Checked against what is actually held rather than against the previous
    // config value, so a fallback reservation is released by the same code.
    if (window_ != nullptr) {
        const bool want_reserved = !AutoHideActive();
        if (want_reserved && reserved_strip_px_ == 0) {
            if (appbar::ReserveTopStrip(window_, height_px_)) {
                reserved_strip_px_ = height_px_;
                auto_hide_fallback_ = false;
                DF_INFO(kModule, "reserved top strip height_px=%d", height_px_);
            } else {
                auto_hide_fallback_ = true;
                DF_WARN(kModule, "could not reserve the top strip; falling back to auto-hide");
            }
        } else if (!want_reserved && reserved_strip_px_ > 0) {
            appbar::ReleaseTopStrip();
            reserved_strip_px_ = 0;
            DF_INFO(kModule, "released the top strip (auto-hide is on)");
        }
    }

    // The menu list depends on max_menu_items, so a change there needs a fresh read.
    if (config_.show_app_menu) {
        menu_ = ReadAppMenu(foreground_.hwnd, config_.max_menu_items);
        foreground_.menu_readable = menu_.readable;
    }
    Relayout();
    ApplyRegion();
    Repaint();
}

void FinderBar::SetForegroundOverride(HWND hwnd) {
    foreground_override_ = hwnd;
    foreground_dirty_ = true;
    DF_INFO(kModule, "foreground override -> %p", static_cast<void*>(hwnd));
}

void FinderBar::RefreshForeground() {
    foreground_dirty_ = false;
    const HWND hwnd =
        foreground_override_ != nullptr ? foreground_override_ : GetForegroundWindow();
    if (hwnd == foreground_.hwnd && hwnd != nullptr) return;

    foreground_ = ForegroundApp{};
    menu_ = AppMenuSnapshot{};
    if (hwnd == nullptr) return;

    // Our own windows are never "the application in front": the bar holds focus only while
    // a menu is open, and showing "DockForge" there would replace the user's app name with
    // our own.
    if (hwnd == window_) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return;

    foreground_.hwnd = hwnd;
    foreground_.pid = pid;
    foreground_.display_name = ResolveAppDisplayName(hwnd);
    if (config_.show_app_menu) menu_ = ReadAppMenu(hwnd, config_.max_menu_items);
    foreground_.menu_readable = menu_.readable;
    DF_DEBUG(kModule, "foreground %p pid=%lu name=%s menus=%zu", static_cast<void*>(hwnd), pid,
             core::WideToUtf8(foreground_.display_name).c_str(), menu_.titles.size());
}

void FinderBar::Relayout() {
    // Right region first (T-307): its measured width is what LayoutBar reserves as the
    // status area. Passing 0 here was the T-301 placeholder - with the right region
    // live, reserving nothing would let a long menu title run under the clock, which
    // is exactly the overlap the reservation exists to prevent.
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::wstring time_picture;
    std::wstring date_picture;
    LocalePictures(&time_picture, &date_picture);
    clock_text_ = FormatBarClockText(now, config_.clock, time_picture.c_str(),
                                     date_picture.c_str());
    const std::vector<RightItem> right_items =
        BuildRightItems(config_, weather_, now, time_picture, date_picture);
    right_boxes_ = LayoutRightGroup(right_items, width_dip_, height_dip_, &MeasureBarText,
                                    &right_summary_);

    BarLayoutParams params;
    params.width_dip = width_dip_;
    params.height_dip = height_dip_;
    params.show_system_menu = config_.show_system_menu;
    params.show_app_name = config_.show_app_name;
    params.show_app_menu = config_.show_app_menu;
    params.max_menu_items = config_.max_menu_items;
    params.status_width_dip = right_summary_.total_width_dip;
    params.app_name = foreground_.display_name;

    boxes_ = LayoutBar(params, menu_.titles, &MeasureBarText, &layout_);
}

// Recomputes only when the clock's rendered text actually changed: the tick loop runs
// at frame cadence, and relaying out every frame would re-measure the whole bar for a
// string that changes once a minute (or a second, when show_seconds is on).
void FinderBar::UpdateRightItems() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::wstring time_picture;
    std::wstring date_picture;
    LocalePictures(&time_picture, &date_picture);
    const std::wstring text = FormatBarClockText(now, config_.clock, time_picture.c_str(),
                                                 date_picture.c_str());
    if (text != clock_text_) Relayout();
}

void FinderBar::ApplyRegion() {
    if (window_ == nullptr || !enabled_) return;

    // The region is both the click target and the clip for what the bar draws (a DComp
    // surface is clipped by SetWindowRgn exactly like GDI content is). So its height has
    // to match the part of the plate the geometry leaves on screen - otherwise the
    // retracted bar's hairline is laid out, offset into view and then silently cut away.
    //
    // A pinned bar is always full height: the strip is reserved, so nothing is underneath
    // it to protect. Only the auto-hide mode keeps the hairline, and only when the
    // reservation could not be taken - there, a full-height bar would steal clicks aimed
    // at a maximized window's title bar (spike Q7 measured exactly that).
    const bool interactive = !AutoHideActive() || AutoHideInteractive(auto_hide_);
    const int wanted =
        interactive ? height_px_
                    : std::max(1, platform::DipToPixels(kAutoHideHairlineDip, dpi_));
    if (wanted == region_height_px_) return;

    HRGN region = CreateRectRgn(0, 0, width_px_, wanted);
    if (region == nullptr) return;
    // SetWindowRgn takes ownership of the region on success.
    if (SetWindowRgn(window_, region, TRUE) == 0) {
        DeleteObject(region);
        return;
    }
    region_height_px_ = wanted;
}

float FinderBar::OffsetDip() const {
    if (!AutoHideActive()) return 0.0f;
    return AutoHideOffsetDip(auto_hide_, height_dip_);
}

void FinderBar::Repaint() {
    if (window_ == nullptr || !surface_.IsValid() || !backend_) return;

    const float offset = OffsetDip();
    const float width = width_dip_;
    const float height = height_dip_;

    // The backdrop is the desktop behind the bar. Capture runs on its own thread, so this is
    // a non-blocking read of the newest frame; no frame yet means the plate falls back to
    // its tint, which is the Solid tier and still correct (C-01 graceful degrade).
    const std::uint8_t* backdrop = nullptr;
    int backdrop_w = 0;
    int backdrop_h = 0;
    if (style_.use_capture) {
        std::uint64_t generation = 0;
        if (capture_.AcquireIfNew(backdrop_generation_, backdrop_pixels_, backdrop_w, backdrop_h,
                                  generation)) {
            backdrop_generation_ = generation;
        }
        if (!backdrop_pixels_.empty()) backdrop = backdrop_pixels_.data();
    }

    const bool light = SystemUsesLightTheme();
    const render::ColorF text_color = light ? kTextDark : kTextLight;
    const render::ColorF hover_color = light ? kHoverDark : kHoverLight;
    const render::ColorF glyph_color = light ? kGlyphDark : kGlyphLight;

    const bool painted =
        surface_.Paint(dpi_, [&](ID2D1DeviceContext& dc,
                                 const platform::CompositionSurface::PaintInfo&) {
            backend_->BeginFrame(dc, static_cast<float>(dpi_));

            const render::RectF plate{0.0f, offset, width, offset + height};
            plate_.Draw(dc, *backend_, plate, 0.0f, style_, backdrop, backdrop_w, backdrop_h,
                        static_cast<float>(dpi_));

            for (std::size_t i = 0; i < boxes_.size(); ++i) {
                const BarBox& box = boxes_[i];
                const render::RectF rect{box.rect.left, box.rect.top + offset, box.rect.right,
                                         box.rect.bottom + offset};
                if (box.is_system_menu) {
                    // Drawn, not typed: the Apple mark is in no font we can ship and
                    // shipping it is a trademark problem (ASSETS.md). Three stacked bars
                    // read as a menu affordance at 14 DIP without imitating the mark.
                    const float size = kSystemMenuGlyphDip;
                    const float cx = rect.left + kMenuTitlePaddingDip * 0.5f + size * 0.5f;
                    const float cy = rect.CenterY();
                    const float bar_height = std::max(1.5f, size * 0.14f);
                    const float gap = size * 0.20f;
                    const float widths[3] = {size, size * 0.72f, size * 0.86f};
                    for (int bar = 0; bar < 3; ++bar) {
                        const float top = cy - (bar_height * 1.5f + gap) +
                                          static_cast<float>(bar) * (bar_height + gap);
                        const render::RectF line{cx - widths[bar] * 0.5f, top,
                                                 cx + widths[bar] * 0.5f, top + bar_height};
                        backend_->FillRoundedRect(line, bar_height * 0.5f, glyph_color);
                    }
                    continue;
                }
                if (static_cast<int>(i) == hover_box_ && !menu_running_) {
                    backend_->FillRoundedRect(rect, 5.0f, hover_color);
                }
                backend_->DrawLabel(box.label.c_str(), rect, kMenuFontDip, text_color,
                                    render::TextAlign::Center);
            }

            // Right region (T-307): glyph first, then text, the macOS order. The
            // weather glyph reuses dock::DrawWeatherGlyph verbatim (dock_view.h:153 /
            // dock_view.cpp:223 - the ask's grep shows this is the only renderer that
            // exists; weather.cpp draws nothing). The Unknown kind draws its own
            // placeholder ring, so an empty snapshot stays visibly occupied.
            for (std::size_t i = 0; i < right_boxes_.size(); ++i) {
                const RightBox& box = right_boxes_[i];
                const render::RectF rect{box.rect.left, box.rect.top + offset, box.rect.right,
                                         box.rect.bottom + offset};
                if (static_cast<int>(i) == hover_right_ && !menu_running_) {
                    backend_->FillRoundedRect(rect, 5.0f, hover_color);
                }
                if (box.has_icon && box.type == RightType::WeatherBrief) {
                    const render::RectF icon{box.icon.left, box.icon.top + offset,
                                             box.icon.right, box.icon.bottom + offset};
                    dock::DrawWeatherGlyph(*backend_, icon,
                                           weather_.valid && !weather_.failed
                                               ? weather_.kind
                                               : dock::weather::Kind::Unknown);
                }
                if (!box.text.empty()) {
                    backend_->DrawLabel(box.text.c_str(), rect, kMenuFontDip, text_color,
                                        render::TextAlign::Center);
                }
            }

            backend_->EndFrame();
        });

    if (!painted) {
        DF_WARN(kModule, "surface paint failed");
        return;
    }
    surface_.Commit();
    ++paints_;
}

bool FinderBar::Tick(double delta_ms, const core::Config& config) {
    if (!ready_ || window_ == nullptr || !enabled_) return false;

    // A config change normally arrives through ReloadConfig, but the foreground name and the
    // menu list also depend on finderbar keys, so a cheap identity check catches the case
    // where a caller forgot. T-307 adds the right table and the clock settings to the
    // same check: a user editing finderbar.right or finderbar.clock.* in the file must
    // not wait for an unrelated finderbar key to change.
    if (config.finderbar.max_menu_items != config_.max_menu_items ||
        config.finderbar.show_app_menu != config_.show_app_menu ||
        config.finderbar.height != config_.height ||
        !core::SameRightItems(config.finderbar.right, config_.right) ||
        config.finderbar.clock.format != config_.clock.format ||
        config.finderbar.clock.show_date != config_.clock.show_date ||
        config.finderbar.clock.show_seconds != config_.clock.show_seconds) {
        ReloadConfig(config);
    }

    // Foreground changes normally arrive through the hook. When the hook could not be
    // installed, poll instead: a bar that silently stops tracking the app in front is worse
    // than one that polls a single cheap call. A pinned override is never polled - it is
    // the answer by definition, and comparing it against the real foreground would mark
    // the bar dirty on every single frame.
    if (foreground_hook_ == nullptr && foreground_override_ == nullptr) {
        const HWND current = GetForegroundWindow();
        if (current != foreground_.hwnd) foreground_dirty_ = true;
    }
    // Snapshot before refreshing: RefreshForeground clears the flag, so reading it afterwards
    // always yields false and the frame that just rebuilt the layout is never painted. That
    // produced a bar whose state JSON listed four menu titles while the screen still showed
    // the previous application's name and no menus at all.
    const bool foreground_changed = foreground_dirty_;
    if (foreground_changed) {
        RefreshForeground();
        Relayout();
    }

    // The clock rolls over at most once per minute (per second with show_seconds);
    // comparing the cached text is the cheap dirty check, and it runs BEFORE the hit
    // tests so a rollover that re-lays the bar out never leaves this frame's hover
    // index pointing into the previous layout.
    if (!menu_running_) UpdateRightItems();

    // Pointer tracking is polled rather than event driven: a retracted bar has an empty
    // region, so Windows sends it no mouse messages at all.
    POINT cursor{};
    GetCursorPos(&cursor);
    const float cursor_x_dip = platform::PixelsToDip(cursor.x - display_.monitor.left, dpi_);
    const float cursor_y_dip = platform::PixelsToDip(cursor.y - display_.monitor.top, dpi_);
    pointer_in_trigger_ = cursor_y_dip >= 0.0f && cursor_y_dip <= kTriggerHeightDip &&
                          cursor_x_dip >= 0.0f && cursor_x_dip <= width_dip_;
    const float offset = OffsetDip();
    pointer_in_bar_ = cursor_y_dip >= offset && cursor_y_dip <= offset + height_dip_ &&
                      cursor_x_dip >= 0.0f && cursor_x_dip <= width_dip_;

    const AutoHideState before = auto_hide_;
    if (AutoHideActive()) {
        // An open menu pins the bar out: retracting under a menu would move the click target
        // while the user is aiming at it.
        const bool pin = menu_running_;
        AdvanceAutoHide(auto_hide_, pointer_in_trigger_ || pin, pointer_in_bar_ || pin, delta_ms);
    } else {
        auto_hide_.progress = 1.0f;
        auto_hide_.shown = true;
    }

    const int previous_hover = hover_box_;
    const int previous_right_hover = hover_right_;
    hover_box_ = -1;
    hover_right_ = -1;
    if (pointer_in_bar_ && AutoHideInteractive(auto_hide_) && !menu_running_) {
        // The right region is queried first: its boxes live in the same row as the
        // left group's but never overlap it (LayoutBar reserves right_summary_'s
        // width), so at most one of the two hit tests can match.
        const std::size_t right_index = RightBoxAtDip(cursor_x_dip, cursor_y_dip - offset);
        if (right_index != kNoMenu) {
            hover_right_ = static_cast<int>(right_index);
        } else {
            const std::size_t index = BoxAtDip(cursor_x_dip, cursor_y_dip - offset);
            if (index != kNoMenu) hover_box_ = static_cast<int>(index);
        }
    }

    const bool moved = std::abs(before.progress - auto_hide_.progress) > 0.0005f;
    const bool hover_changed = previous_hover != hover_box_ || previous_right_hover != hover_right_;
    if (!moved && !hover_changed && !foreground_changed) return false;

    if (moved) ApplyRegion();
    Repaint();
    return true;
}

std::size_t FinderBar::BoxAtDip(float x_dip, float y_dip) const {
    return BoxIndexAt(boxes_.data(), boxes_.size(), x_dip, y_dip);
}

std::size_t FinderBar::RightBoxAtDip(float x_dip, float y_dip) const {
    return RightBoxIndexAt(right_boxes_.data(), right_boxes_.size(), x_dip, y_dip);
}

void FinderBar::OnClick(float x_dip, float y_dip) {
    if (menu_running_) return;
    // Right region first: the boxes do not overlap the left group's, but the clock is
    // the anchor macOS users aim at, so its path stays short and readable.
    const std::size_t right_index = RightBoxAtDip(x_dip, y_dip);
    if (right_index != kNoMenu) {
        OnRightClick(right_boxes_[right_index]);
        return;
    }
    const std::size_t index = BoxAtDip(x_dip, y_dip);
    if (index == kNoMenu) return;
    const BarBox& box = boxes_[index];
    if (box.is_system_menu) {
        OpenSystemMenu(index);
    } else if (box.is_app_name) {
        // macOS opens a menu of the application's own windows here. That belongs to T-204
        // (window list); until then the name is a label, not a button.
        return;
    } else if (box.is_overflow) {
        OpenOverflowMenu(index);
    } else {
        OpenAppMenu(index);
    }
}

void FinderBar::OnRightClick(const RightBox& box) {
    switch (box.type) {
        case RightType::Clock: {
            // macOS: the menu-bar clock drops a calendar card straight down. Toggled:
            // a second click on the clock closes the open panel, matching the popup
            // behaviour every macOS status item has.
            if (calendar_panel_ != nullptr && calendar_panel_->Visible()) {
                CloseCalendarPanel();
                return;
            }
            if (calendar_panel_ == nullptr) {
                DF_WARN(kModule, "calendar panel unavailable this run");
                return;
            }
            const int bar_bottom_px = display_.monitor.top + height_px_;
            const int inset_px = std::max(0, platform::DipToPixels(kTrailingInsetDip, dpi_));
            calendar_panel_->Open(dpi_, display_.monitor, bar_bottom_px, inset_px);
            break;
        }
        case RightType::WeatherBrief: {
            if (!weather_popup_ready_) {
                DF_WARN(kModule, "weather popup unavailable this run");
                return;
            }
            // The popup's Show anchors its BOTTOM edge above `bottom_px`; the bar sits
            // at the top of the screen, so the forecast must hang BELOW it. Passing
            // bar_bottom + popup height + gap lands the top edge exactly on the bar's
            // bottom. The height formula mirrors weather_popup.cpp's own constants
            // (kHeaderHeightDip + 2*kPadDip + one row per forecast day); duplicating
            // three DIP numbers beats reaching into that file's private namespace.
            const int rows =
                static_cast<int>(std::max<std::size_t>(weather_.days.size(), 1));
            const float popup_dip = 36.0f + 12.0f * 2.0f + rows * 32.0f;
            const float scale = Scale();
            const float centre_x_dip = (box.rect.left + box.rect.right) * 0.5f;
            const int anchor_x_px = display_.monitor.left +
                                    static_cast<int>(centre_x_dip * scale);
            const int bottom_px = display_.monitor.top + height_px_ +
                                  static_cast<int>((popup_dip + 8.0f) * scale);
            SYSTEMTIME now{};
            GetLocalTime(&now);
            char today[16]{};
            std::snprintf(today, sizeof(today), "%04d-%02d-%02d", now.wYear, now.wMonth,
                          now.wDay);
            weather_popup_.Show(weather_, today, anchor_x_px, bottom_px, dpi_);
            break;
        }
        default:
            // Registered types whose panel belongs to a later task (none today) land
            // here; clicking them is a no-op rather than a crash.
            break;
    }
}

void FinderBar::SetWeatherSnapshot(const dock::weather::Snapshot& snapshot) {
    // Copy, never pointer (C-04b). A changed snapshot re-lays the right region so the
    // temperature text and the glyph kind move together on the next paint.
    const bool changed = snapshot.valid != weather_.valid || snapshot.failed != weather_.failed ||
                         snapshot.temp_c != weather_.temp_c || snapshot.kind != weather_.kind ||
                         snapshot.days.size() != weather_.days.size();
    if (!changed && weather_.days.size() == snapshot.days.size()) return;
    weather_ = snapshot;
    Relayout();
    Repaint();
}

bool FinderBar::WantsWeatherSnapshot() const {
    for (const core::FinderBarRightItem& item : config_.right) {
        const std::wstring type_name = core::Utf8ToWide(item.type);
        const std::optional<RightType> type = RightTypeFromName(type_name.c_str());
        if (type.has_value() && *type == RightType::WeatherBrief) return true;
    }
    return false;
}

bool FinderBar::OpenCalendarPanel() {
    if (!ready_ || window_ == nullptr || !enabled_) return false;
    if (calendar_panel_ == nullptr) return false;
    // Toggle, matching the click path: a second probe "calendar" closes the open
    // panel instead of re-showing it, exactly what a second click on the clock does.
    if (calendar_panel_->Visible()) {
        CloseCalendarPanel();
        return false;
    }
    const int bar_bottom_px = display_.monitor.top + height_px_;
    const int inset_px = std::max(0, platform::DipToPixels(kTrailingInsetDip, dpi_));
    calendar_panel_->Open(dpi_, display_.monitor, bar_bottom_px, inset_px);
    return calendar_panel_->Visible();
}

void FinderBar::CloseCalendarPanel() {
    if (calendar_panel_ != nullptr) calendar_panel_->Close();
}

bool FinderBar::OpenWeatherPopup() {
    if (!ready_ || window_ == nullptr || !enabled_) return false;
    if (!weather_popup_ready_) return false;
    for (const RightBox& box : right_boxes_) {
        if (box.type == RightType::WeatherBrief) {
            OnRightClick(box);
            return weather_popup_.Visible();
        }
    }
    return false;
}

bool FinderBar::OpenMenuForBox(std::size_t box_index) {
    if (!ready_ || window_ == nullptr || !enabled_) return false;
    if (menu_running_) return false;
    if (box_index >= boxes_.size()) return false;
    OnClick(boxes_[box_index].rect.left + 1.0f,
            boxes_[box_index].rect.top + height_dip_ * 0.5f);
    return menu_running_;
}

bool FinderBar::RunSystemCommand(int command_id) {
    const int index = command_id - static_cast<int>(kSystemIdBase);
    if (index <= 0 || index > static_cast<int>(SystemCommand::Quit)) return false;
    HandleSystemCommand(command_id);
    return true;
}

bool FinderBar::ForwardMenuItem(std::size_t box_index, int item) {
    if (!ready_ || !enabled_) return false;
    if (box_index >= boxes_.size()) return false;
    const BarBox& box = boxes_[box_index];
    if (box.menu_index == kNoMenu || box.menu_index >= menu_.submenus.size()) return false;
    HMENU submenu = menu_.submenus[box.menu_index];
    if (submenu == nullptr) return false;
    const int count = GetMenuItemCount(submenu);
    if (item < 0 || item >= count) return false;
    // GetMenuItemID returns -1 for separators and for items that open a submenu; both are
    // real states a user can click and neither carries a command to forward.
    //
    // Zero is NOT a rejection case. It was one here until Sublime Text proved otherwise:
    // its "New File" command is id 0 (measured, menu_spike13.log), so treating 0 as
    // "no command" silently swallowed the first item of every menu. The only value the
    // API uses to mean "nothing" is -1.
    const UINT id = static_cast<UINT>(GetMenuItemID(submenu, item));
    if (id == static_cast<UINT>(-1)) {
        DF_DEBUG(kModule, "menu item %d of box %zu is a separator or submenu", item, box_index);
        return false;
    }
    if (foreground_.hwnd == nullptr) return false;
    // C-32's forwarding rule, verbatim: no injection, no hooking, just the message the
    // application's own menu would have received.
    const BOOL posted =
        PostMessageW(foreground_.hwnd, WM_COMMAND, MAKEWPARAM(id, 0), 0);
    DF_INFO(kModule, "forwarded WM_COMMAND %u to %p posted=%d", id,
            static_cast<void*>(foreground_.hwnd), posted ? 1 : 0);
    return posted != FALSE;
}

void FinderBar::StartMenuJob(HMENU menu, HWND target, bool owns_menu, int x, int y) {
    if (menu == nullptr) return;
    if (menu_thread_ != nullptr) {
        // A previous job finished; reap it so the handle does not leak.
        CloseHandle(menu_thread_);
        menu_thread_ = nullptr;
    }

    // The job must outlive this call: the thread reads it until TrackPopupMenuEx returns.
    // One menu at a time is a menu-bar invariant, so a single slot is enough. It cannot be a
    // member because MenuJob is private to this translation unit, and it is not reset by
    // whole-struct assignment because std::atomic is not assignable.
    static MenuJob job;
    job.bar = window_;
    job.target = target;
    job.menu = menu;
    job.owns_menu = owns_menu;
    job.x = x;
    job.y = y;
    job.selected.store(0);
    job.done.store(false);

    g_menu_job.store(&job);
    menu_running_ = true;
    menu_thread_ = CreateThread(nullptr, 0, &MenuThreadProc, &job, 0, nullptr);
    if (menu_thread_ == nullptr) {
        menu_running_ = false;
        g_menu_job.store(nullptr);
        if (owns_menu) DestroyMenu(menu);
        DF_ERROR(kModule, "CreateThread for the menu failed 0x%08lX", GetLastError());
        return;
    }
    ++menu_opens_;
    Repaint();
}

void FinderBar::OpenAppMenu(std::size_t box_index) {
    if (box_index >= boxes_.size()) return;
    const BarBox& box = boxes_[box_index];
    if (box.menu_index == kNoMenu || box.menu_index >= menu_.submenus.size()) return;
    HMENU submenu = menu_.submenus[box.menu_index];
    if (submenu == nullptr) return;

    const float scale = Scale();
    const int x = display_.monitor.left + static_cast<int>(box.rect.left * scale);
    const int y = display_.monitor.top + height_px_;
    DF_DEBUG(kModule, "opening menu '%s' for %p at %d,%d",
             core::WideToUtf8(box.label).c_str(), static_cast<void*>(foreground_.hwnd), x, y);
    // The submenu belongs to the application: it is handed to the system, never destroyed.
    StartMenuJob(submenu, foreground_.hwnd, false, x, y);
}

void FinderBar::OpenSystemMenu(std::size_t box_index) {
    if (box_index >= boxes_.size()) return;
    const BarBox& box = boxes_[box_index];

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;
    AppendMenuW(menu, MF_STRING, kSystemIdBase + static_cast<UINT>(SystemCommand::About),
                L"About DockForge");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kSystemIdBase + static_cast<UINT>(SystemCommand::Settings),
                L"Settings\x2026");
    AppendMenuW(menu, MF_STRING,
                kSystemIdBase + static_cast<UINT>(SystemCommand::OpenConfigFolder),
                L"Open Config Folder");
    AppendMenuW(menu, MF_STRING, kSystemIdBase + static_cast<UINT>(SystemCommand::OpenLogs),
                L"Open Logs");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kSystemIdBase + static_cast<UINT>(SystemCommand::Quit),
                L"Quit DockForge");

    const float scale = Scale();
    const int x = display_.monitor.left + static_cast<int>(box.rect.left * scale);
    const int y = display_.monitor.top + height_px_;
    StartMenuJob(menu, window_, true, x, y);
}

void FinderBar::OpenOverflowMenu(std::size_t box_index) {
    if (box_index >= boxes_.size()) return;
    const BarBox& box = boxes_[box_index];

    // The titles past max_menu_items are not in `menu_` because ReadAppMenu truncated it.
    // The overflow box re-reads the full list: it is a rare click, and a second read is
    // cheaper than keeping two parallel copies in sync.
    const AppMenuSnapshot full = ReadAppMenu(foreground_.hwnd, 64);
    if (full.submenus.size() <= menu_.submenus.size()) return;

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;
    for (std::size_t i = menu_.submenus.size(); i < full.submenus.size(); ++i) {
        if (full.submenus[i] == nullptr) continue;
        // A submenu entry is handed to the system, not copied, so the application's menu
        // stays live. Destroying this wrapper would destroy those with it, which is why
        // owns_menu stays false for the overflow menu.
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(full.submenus[i]),
                    full.titles[i].c_str());
    }

    const float scale = Scale();
    const int x = display_.monitor.left + static_cast<int>(box.rect.left * scale);
    const int y = display_.monitor.top + height_px_;
    StartMenuJob(menu, foreground_.hwnd, false, x, y);
}

void FinderBar::HandleSystemCommand(int command_id) {
    const int index = command_id - static_cast<int>(kSystemIdBase);
    if (index <= 0 || index > static_cast<int>(SystemCommand::Quit)) return;
    const auto command = static_cast<SystemCommand>(index);
    DF_INFO(kModule, "system menu command %d", index);
    if (system_command_) system_command_(command);
}

void FinderBar::OnMenuClosed() {
    // The menu thread only posts; every surface call stays on this thread.
    menu_running_ = false;
    MenuJob* job = g_menu_job.exchange(nullptr);
    if (job != nullptr) {
        const unsigned selected = job->selected.load();
        if (job->target == window_ && selected >= kSystemIdBase) {
            HandleSystemCommand(static_cast<int>(selected));
        }
    }
    if (menu_thread_ != nullptr) {
        CloseHandle(menu_thread_);
        menu_thread_ = nullptr;
    }
    Repaint();
}

LRESULT CALLBACK FinderBar::StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    auto* self = reinterpret_cast<FinderBar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) return DefWindowProcW(hwnd, msg, wparam, lparam);
    return self->WndProc(hwnd, msg, wparam, lparam);
}

LRESULT FinderBar::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            // Take the click: the bar is the target, not a decoration over the app.
            return MA_ACTIVATE;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            // The pixels come from DComp; validating the update region is all that is
            // required, otherwise Windows repaints forever.
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            const float scale = Scale();
            const float x = static_cast<float>(GET_X_LPARAM(lparam)) / scale;
            const float y = static_cast<float>(GET_Y_LPARAM(lparam)) / scale;
            OnClick(x, y - OffsetDip());
            return 0;
        }

        case kMsgMenuClosed:
            OnMenuClosed();
            return 0;

        case appbar::kAppBarCallbackMessage: {
            // The shell's appbar notifications arrive on this window because ABM_NEW
            // registered it. ABN_FULLSCREENAPP matters: a fullscreen video must not have
            // its top strip covered, so the bar hides while it is set.
            appbar::HandleAppBarMessage(wparam, lparam);
            if (wparam == ABN_FULLSCREENAPP) {
                const bool fullscreen = appbar::GetAppBarStats().fullscreen_app;
                if (fullscreen && enabled_) {
                    ShowWindow(hwnd, SW_HIDE);
                } else if (!fullscreen && enabled_) {
                    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                }
            }
            return 0;
        }

        case WM_DISPLAYCHANGE: {
            platform::DisplayInfo display;
            if (platform::PrimaryDisplay(display)) {
                const UINT override_dpi = platform::Window::DpiOverride();
                if (override_dpi != 0) display.effective_dpi = override_dpi;
                display_ = display;
                dpi_ = display.effective_dpi ? display.effective_dpi : 96;
                width_dip_ =
                    platform::PixelsToDip(display.monitor.right - display.monitor.left, dpi_);
                width_px_ = display.monitor.right - display.monitor.left;
                height_px_ = std::max(1, platform::DipToPixels(height_dip_, dpi_));
                SetWindowPos(hwnd, HWND_TOPMOST, display.monitor.left, display.monitor.top,
                             width_px_, height_px_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
                surface_.Resize(width_px_, height_px_);
                capture_.SetRegion(display.monitor.left, display.monitor.top, width_px_,
                                   height_px_);
                region_height_px_ = -1;
                Relayout();
                ApplyRegion();
                Repaint();
            }
            return 0;
        }

        case WM_DESTROY:
            // Deliberately no PostQuitMessage: the bar is destroyed when the user turns it
            // off, and quitting the process there would look like a crash.
            window_ = nullptr;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void CALLBACK FinderBar::WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG, LONG, DWORD,
                                      DWORD) {
    if (event != EVENT_SYSTEM_FOREGROUND) return;
    FinderBar* bar = g_active_bar;
    if (bar == nullptr) return;
    // The hook fires on the thread that installed it (WINEVENT_OUTOFCONTEXT), so touching
    // the bar here is safe; it only marks the snapshot stale and lets Tick do the work.
    if (hwnd != bar->foreground_.hwnd) bar->foreground_dirty_ = true;
    ++bar->foreground_events_;
}

std::string FinderBar::SnapshotJson() const {
    nlohmann::json j;
    j["ready"] = ready_;
    j["enabled"] = enabled_;
    j["menu_open"] = menu_running_;
    j["auto_hide"] = config_.auto_hide;
    // T-302b: auto_hide alone no longer says what the bar is doing. It may be retracting
    // because the reservation was refused, and a probe that only reads `auto_hide` would
    // report "pinned" while the bar hides itself. Both facts are published separately.
    j["auto_hiding"] = AutoHideActive();
    j["reserved_strip_px"] = reserved_strip_px_;
    j["auto_hide_fallback"] = auto_hide_fallback_;
    j["progress"] = auto_hide_.progress;
    j["offset_dip"] = OffsetDip();
    j["interactive"] = !AutoHideActive() || AutoHideInteractive(auto_hide_);
    j["width_dip"] = width_dip_;
    j["height_dip"] = height_dip_;
    j["dpi"] = dpi_;
    j["window"] = reinterpret_cast<std::uintptr_t>(window_);
    j["paints"] = paints_;
    j["menu_opens"] = menu_opens_;
    j["foreground_events"] = foreground_events_;
    j["foreground_hwnd"] = reinterpret_cast<std::uintptr_t>(foreground_.hwnd);
    j["foreground_name"] = core::WideToUtf8(foreground_.display_name);
    j["menu_readable"] = foreground_.menu_readable;
    j["hover_box"] = hover_box_;

    // T-307: the right region, its measured width and the clock text a probe asserts
    // on without a screenshot.
    j["clock_text"] = core::WideToUtf8(clock_text_);
    j["right_width_dip"] = right_summary_.total_width_dip;
    j["calendar_visible"] = CalendarPanelVisible();
    nlohmann::json right_boxes = nlohmann::json::array();
    for (const RightBox& box : right_boxes_) {
        right_boxes.push_back({{"type", core::WideToUtf8(RightTypeName(static_cast<int>(box.type)))},
                               {"left", box.rect.left},
                               {"right", box.rect.right},
                               {"text", core::WideToUtf8(box.text)},
                               {"icon", box.has_icon}});
    }
    j["right_boxes"] = right_boxes;

    nlohmann::json boxes = nlohmann::json::array();
    for (const BarBox& box : boxes_) {
        boxes.push_back(
            {{"label", core::WideToUtf8(box.label)},
             {"left", box.rect.left},
             {"right", box.rect.right},
             {"system", box.is_system_menu},
             {"name", box.is_app_name},
             {"overflow", box.is_overflow},
             {"menu_index", box.menu_index == kNoMenu ? -1 : static_cast<int>(box.menu_index)}});
    }
    j["boxes"] = boxes;

    j["menu_titles"] = nlohmann::json::array();
    for (const std::wstring& title : menu_.titles) {
        j["menu_titles"].push_back(core::WideToUtf8(title));
    }
    j["layout"] = {{"placed_menus", layout_.placed_menus},
                   {"overflowed", layout_.overflowed},
                   {"left_group_width_dip", layout_.left_group_width_dip},
                   {"status_start_dip", layout_.status_start_dip},
                   {"clipped", layout_.clipped}};
    return j.dump();
}

}  // namespace dockforge::finderbar
