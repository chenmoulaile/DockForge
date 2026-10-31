// DockForge T-301 spike: what a Windows top menu bar can actually read, and how
// fast it can follow the foreground app.
//
// Every question below is answered by RUNNING this binary. Nothing here is
// inferred from documentation, because the documented behaviour and this host's
// behaviour have disagreed before (see STATE.json known_gotchas).
//
//   Q1. Does GetMenu -> GetMenuItemCount -> GetMenuItemInfoW return the labels
//       and command ids of a menu on a window owned by THIS process, including
//       a second level (popup -> items)?
//   Q2. Does the same read work on a window owned by ANOTHER process? Win32
//       hands out HMENU as a user handle; "documented for any window" is not
//       "measured here".
//   Q3. On this real desktop, what fraction of visible top-level windows have a
//       menu at all? This decides whether C-32's GetMenu path is the common case
//       or the exception - Windows 11 moved Explorer to a XAML command bar and
//       most modern apps draw their own menu.
//   Q4. What is the callback latency of SetWinEventHook(EVENT_SYSTEM_FOREGROUND)
//       out-of-context? T-301 promises "refresh < 100ms", so the hook has to fit
//       that budget or the bar needs a poll fallback.
//   Q5. Does PostMessage(hwnd, WM_COMMAND, MAKEWPARAM(id, 0), 0) reach a window
//       in ANOTHER process? (C-32's forward path.)
//   Q6. How long does resolving the foreground window to a display name take
//       (pid -> image path -> FileDescription)?
//   Q7. Does a full-width topmost strip at the top of the screen swallow clicks
//       (WindowFromPoint), and does WS_EX_TRANSPARENT make it click-through?
//       This is the Finder Bar's biggest product risk: a bar that eats clicks
//       is worse than no bar.
//
// Build (repo root, MSYS2 g++):
//   g++ -std=c++17 -O2 -o spikes/T-301_finderbar_menu/menu_spike.exe \
//       spikes/T-301_finderbar_menu/menu_spike.cpp -luser32 -lpsapi -lversion
#include <windows.h>
#include <psapi.h>
#include <winver.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------- timing
std::int64_t Qpc() {
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

double Ms(std::int64_t ticks) {
    static const double freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    return static_cast<double>(ticks) * 1000.0 / freq;
}

std::wstring ExeDir() {
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    std::wstring full(path, n);
    const std::size_t slash = full.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : full.substr(0, slash);
}

// ---------------------------------------------------------------- Q1/Q2: menus
struct MenuItemInfo {
    std::wstring text;
    UINT id = 0;
    bool separator = false;
    bool submenu = false;
    int submenu_count = 0;
};

struct MenuSnapshot {
    bool has_menu = false;
    int count = 0;
    bool read_ok = false;
    DWORD read_error = 0;
    std::vector<MenuItemInfo> items;
};

MenuSnapshot ReadMenu(HWND hwnd, int max_items) {
    MenuSnapshot snap;
    const HMENU menu = GetMenu(hwnd);
    if (menu == nullptr) return snap;
    snap.has_menu = true;
    const int count = GetMenuItemCount(menu);
    snap.count = count;
    for (int i = 0; i < count && static_cast<int>(snap.items.size()) < max_items; ++i) {
        MenuItemInfo item;
        wchar_t buf[256] = {};
        MENUITEMINFOW mii{};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_STRING | MIIM_ID | MIIM_FTYPE | MIIM_SUBMENU;
        mii.dwTypeData = buf;
        mii.cch = 256;
        if (GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &mii)) {
            snap.read_ok = true;
            item.text.assign(buf);
            item.id = mii.wID;
            item.separator = (mii.fType & MFT_SEPARATOR) != 0;
            item.submenu = mii.hSubMenu != nullptr;
            if (mii.hSubMenu != nullptr) item.submenu_count = GetMenuItemCount(mii.hSubMenu);
        } else if (snap.read_error == 0) {
            snap.read_error = GetLastError();
        }
        snap.items.push_back(std::move(item));
    }
    return snap;
}

// ---------------------------------------------------------------- Q6: names
std::wstring ProcessImagePath(DWORD pid) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc == nullptr) return std::wstring();
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]));
    std::wstring out;
    if (QueryFullProcessImageNameW(proc, 0, buf, &size) != FALSE) out.assign(buf, size);
    CloseHandle(proc);
    return out;
}

std::wstring FileDescription(const std::wstring& path) {
    if (path.empty()) return std::wstring();
    DWORD dummy = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (bytes == 0) return std::wstring();
    std::vector<unsigned char> data(bytes);
    if (GetFileVersionInfoW(path.c_str(), 0, bytes, data.data()) == FALSE) return std::wstring();

    struct LangCp {
        WORD lang;
        WORD cp;
    };
    LangCp* trans = nullptr;
    UINT trans_bytes = 0;
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                       reinterpret_cast<void**>(&trans), &trans_bytes) == FALSE ||
        trans == nullptr || trans_bytes < sizeof(LangCp)) {
        return std::wstring();
    }
    wchar_t sub[128] = {};
    swprintf(sub, 128, L"\\StringFileInfo\\%04x%04x\\FileDescription", trans[0].lang,
             trans[0].cp);
    wchar_t* value = nullptr;
    UINT value_bytes = 0;
    if (VerQueryValueW(data.data(), sub, reinterpret_cast<void**>(&value), &value_bytes) == FALSE ||
        value == nullptr) {
        return std::wstring();
    }
    return std::wstring(value);
}

// ---------------------------------------------------------------- Q3: census
struct WindowRow {
    HWND hwnd = nullptr;
    std::wstring cls;
    std::wstring title;
    DWORD pid = 0;
    MenuSnapshot menu;
};

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp) {
    auto* rows = reinterpret_cast<std::vector<WindowRow>*>(lp);
    if (IsWindowVisible(hwnd) == FALSE) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) != 0) return TRUE;
    wchar_t title[256] = {};
    GetWindowTextW(hwnd, title, 256);
    if (title[0] == L'\0') return TRUE;

    WindowRow row;
    row.hwnd = hwnd;
    wchar_t cls[256] = {};
    GetClassNameW(hwnd, cls, 256);
    row.cls = cls;
    row.title = title;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    row.pid = pid;
    row.menu = ReadMenu(hwnd, 8);
    rows->push_back(std::move(row));
    return TRUE;
}

// ---------------------------------------------------------------- Q4: hook
volatile LONG g_event_count = 0;
volatile LONG g_event_hwnd_match = 0;
std::int64_t g_event_qpc = 0;
HWND g_event_hwnd = nullptr;

void CALLBACK OnForegroundEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG id_object,
                                LONG id_child, DWORD, DWORD) {
    if (event != EVENT_SYSTEM_FOREGROUND) return;
    if (id_object != OBJID_WINDOW || id_child != CHILDID_SELF) return;
    g_event_qpc = Qpc();
    g_event_hwnd = hwnd;
    InterlockedIncrement(&g_event_count);
    (void)g_event_hwnd_match;
}

void PumpFor(int ms) {
    const DWORD until = GetTickCount() + static_cast<DWORD>(ms);
    MSG msg{};
    while (GetTickCount() < until) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != FALSE) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(1);
    }
}

// ---------------------------------------------------------------- spike windows
constexpr wchar_t kMenuWindowClass[] = L"DockForgeSpikeMenuWindow";
constexpr wchar_t kChildWindowClass[] = L"DockForgeSpikeChildWindow";
constexpr UINT kMenuOpenId = 1001;
constexpr UINT kMenuSaveId = 1002;
constexpr UINT kChildCommandId = 2001;

volatile LONG g_menu_window_commands = 0;
UINT g_menu_window_last_command = 0;

LRESULT CALLBACK MenuWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND) {
        g_menu_window_last_command = LOWORD(wp);
        InterlockedIncrement(&g_menu_window_commands);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK ChildWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND) {
        const std::wstring path = ExeDir() + L"\\spike_child_hit.txt";
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"w") == 0 && f != nullptr) {
            std::fprintf(f, "command=%u\n", static_cast<unsigned>(LOWORD(wp)));
            std::fclose(f);
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HMENU BuildMenu() {
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, kMenuOpenId, L"&Open");
    AppendMenuW(file, MF_STRING, kMenuSaveId, L"&Save");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, 1003, L"E&xit");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, 1101, L"&About");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
    return bar;
}

int RunChild() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ChildWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kChildWindowClass;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, kChildWindowClass, L"DockForgeSpikeChild",
                                WS_OVERLAPPEDWINDOW, 120, 120, 320, 200, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (hwnd == nullptr) return 2;
    HMENU bar = CreateMenu();
    HMENU popup = CreatePopupMenu();
    AppendMenuW(popup, MF_STRING, kChildCommandId, L"Child Command");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(popup), L"Child");
    SetMenu(hwnd, bar);
    ShowWindow(hwnd, SW_SHOW);

    const std::wstring ready = ExeDir() + L"\\spike_child_ready.txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, ready.c_str(), L"w") == 0 && f != nullptr) {
        std::fprintf(f, "hwnd=%llu\n",
                     static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(hwnd)));
        std::fclose(f);
    }

    const DWORD until = GetTickCount() + 60000;
    MSG msg{};
    while (GetTickCount() < until) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != FALSE) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
    return 0;
}

bool ReadFileText(const std::wstring& path, std::string& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr) return false;
    char buf[512] = {};
    const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    out.assign(buf, n);
    return true;
}

void PrintMenu(const MenuSnapshot& snap, int indent) {
    if (!snap.has_menu) {
        std::printf("%*s<no menu>\n", indent, "");
        return;
    }
    std::printf("%*scount=%d read_ok=%d read_error=%lu\n", indent, "", snap.count,
                snap.read_ok ? 1 : 0, static_cast<unsigned long>(snap.read_error));
    for (const MenuItemInfo& item : snap.items) {
        const std::wstring text = item.text.empty() ? L"<empty>" : item.text;
        std::printf("%*s[%s] id=%u sep=%d submenu=%d subcount=%d\n", indent + 2, "",
                    std::string(text.begin(), text.end()).c_str(), item.id,
                    item.separator ? 1 : 0, item.submenu ? 1 : 0, item.submenu_count);
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring(argv[1]) == L"--child") return RunChild();

    std::printf("T-301 finder bar menu spike (pid=%lu)\n",
                static_cast<unsigned long>(GetCurrentProcessId()));

    // ---- Q1: own-process menu, two levels --------------------------------
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MenuWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kMenuWindowClass;
    RegisterClassExW(&wc);
    HWND own = CreateWindowExW(0, kMenuWindowClass, L"DockForge Spike Menu",
                               WS_OVERLAPPEDWINDOW, 80, 80, 360, 240, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    HMENU own_bar = BuildMenu();
    SetMenu(own, own_bar);
    ShowWindow(own, SW_SHOW);
    PumpFor(120);

    std::printf("\nQ1 own-process menu\n");
    const MenuSnapshot own_top = ReadMenu(own, 8);
    PrintMenu(own_top, 2);
    if (!own_top.items.empty() && own_top.items[0].submenu) {
        const HMENU sub = GetSubMenu(GetMenu(own), 0);
        MENUITEMINFOW mii{};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_SUBMENU;
        GetMenuItemInfoW(GetMenu(own), 0, TRUE, &mii);
        (void)sub;
        (void)mii;
        std::printf("  level2 via GetSubMenu(0):\n");
        const int sub_count = GetMenuItemCount(GetSubMenu(GetMenu(own), 0));
        for (int i = 0; i < sub_count; ++i) {
            wchar_t buf[128] = {};
            MENUITEMINFOW item{};
            item.cbSize = sizeof(item);
            item.fMask = MIIM_STRING | MIIM_ID | MIIM_FTYPE;
            item.dwTypeData = buf;
            item.cch = 128;
            if (GetMenuItemInfoW(GetSubMenu(GetMenu(own), 0), static_cast<UINT>(i), TRUE,
                                 &item) == FALSE) {
                std::printf("    item %d read failed err=%lu\n", i,
                            static_cast<unsigned long>(GetLastError()));
                continue;
            }
            std::printf("    item %d text=%s id=%u sep=%d\n", i,
                        std::string(buf, buf + wcslen(buf)).c_str(), item.wID,
                        (item.fType & MFT_SEPARATOR) ? 1 : 0);
        }
    }

    // ---- Q2: another process's menu -------------------------------------
    std::printf("\nQ2 cross-process menu\n");
    const std::wstring ready_path = ExeDir() + L"\\spike_child_ready.txt";
    const std::wstring hit_path = ExeDir() + L"\\spike_child_hit.txt";
    DeleteFileW(ready_path.c_str());
    DeleteFileW(hit_path.c_str());

    wchar_t self[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, self, static_cast<DWORD>(sizeof(self) / sizeof(self[0])));
    std::wstring cmdline = std::wstring(L"\"") + self + L"\" --child";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd(cmdline.begin(), cmdline.end());
    cmd.push_back(L'\0');
    const BOOL spawned = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                        nullptr, &si, &pi);
    std::printf("  child spawned=%d\n", spawned ? 1 : 0);

    HWND child_hwnd = nullptr;
    for (int i = 0; i < 300 && spawned; ++i) {
        std::string text;
        if (ReadFileText(ready_path, text) && !text.empty()) {
            const std::size_t eq = text.find('=');
            if (eq != std::string::npos) {
                child_hwnd = reinterpret_cast<HWND>(
                    static_cast<UINT_PTR>(std::strtoull(text.c_str() + eq + 1, nullptr, 10)));
            }
            break;
        }
        Sleep(10);
    }
    std::printf("  child hwnd=%llu valid=%d\n",
                static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(child_hwnd)),
                (child_hwnd != nullptr && IsWindow(child_hwnd)) ? 1 : 0);
    if (child_hwnd != nullptr && IsWindow(child_hwnd) != FALSE) {
        const MenuSnapshot child_menu = ReadMenu(child_hwnd, 8);
        PrintMenu(child_menu, 2);
        if (!child_menu.items.empty() && child_menu.items[0].submenu) {
            const int sub_count = GetMenuItemCount(GetSubMenu(GetMenu(child_hwnd), 0));
            std::printf("    child level2 count=%d\n", sub_count);
            for (int i = 0; i < sub_count; ++i) {
                wchar_t buf[128] = {};
                MENUITEMINFOW item{};
                item.cbSize = sizeof(item);
                item.fMask = MIIM_STRING | MIIM_ID;
                item.dwTypeData = buf;
                item.cch = 128;
                if (GetMenuItemInfoW(GetSubMenu(GetMenu(child_hwnd), 0), static_cast<UINT>(i),
                                     TRUE, &item) == FALSE) {
                    std::printf("    child item %d read failed err=%lu\n", i,
                                static_cast<unsigned long>(GetLastError()));
                    continue;
                }
                std::printf("    child item %d text=%s id=%u\n", i,
                            std::string(buf, buf + wcslen(buf)).c_str(), item.wID);
            }
        }
    }

    // ---- Q5: cross-process WM_COMMAND -----------------------------------
    std::printf("\nQ5 cross-process WM_COMMAND forward\n");
    bool child_hit = false;
    if (child_hwnd != nullptr && IsWindow(child_hwnd) != FALSE) {
        const BOOL posted =
            PostMessageW(child_hwnd, WM_COMMAND, MAKEWPARAM(kChildCommandId, 0), 0);
        std::printf("  PostMessage(WM_COMMAND id=%u) -> %d err=%lu\n", kChildCommandId,
                    posted ? 1 : 0, static_cast<unsigned long>(GetLastError()));
        for (int i = 0; i < 200; ++i) {
            std::string text;
            if (ReadFileText(hit_path, text) && !text.empty()) {
                std::printf("  child wrote: %s", text.c_str());
                child_hit = true;
                break;
            }
            Sleep(10);
        }
    }
    // Same-process forward as the control case: proves the message shape even if
    // the cross-process leg is refused by UIPI.
    const LONG before = g_menu_window_commands;
    PostMessageW(own, WM_COMMAND, MAKEWPARAM(kMenuOpenId, 0), 0);
    PumpFor(80);
    std::printf("  own-window forward: before=%ld after=%ld last_id=%u\n", before,
                g_menu_window_commands, g_menu_window_last_command);

    if (spawned) {
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    // ---- Q3: desktop census ---------------------------------------------
    std::printf("\nQ3 desktop menu census\n");
    std::vector<WindowRow> rows;
    EnumWindows(&EnumProc, reinterpret_cast<LPARAM>(&rows));
    int with_menu = 0;
    int read_failed = 0;
    std::vector<std::size_t> hits;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (!rows[i].menu.has_menu) continue;
        ++with_menu;
        if (!rows[i].menu.read_ok) ++read_failed;
        if (hits.size() < 6) hits.push_back(i);
    }
    std::printf("  visible top-level windows=%zu with_menu=%d read_failed=%d\n", rows.size(),
                with_menu, read_failed);
    for (std::size_t idx : hits) {
        const WindowRow& row = rows[idx];
        std::printf("  - class=%s pid=%lu items=%d title=%s\n",
                    std::string(row.cls.begin(), row.cls.end()).c_str(),
                    static_cast<unsigned long>(row.pid), row.menu.count,
                    std::string(row.title.begin(), row.title.end()).c_str());
    }
    // Which of the apps a dock user actually runs expose a menu?
    const wchar_t* known[] = {L"explorer.exe", L"notepad.exe", L"chrome.exe", L"Code.exe"};
    for (const wchar_t* name : known) {
        int total = 0;
        int menus = 0;
        for (const WindowRow& row : rows) {
            const std::wstring path = ProcessImagePath(row.pid);
            const std::size_t slash = path.find_last_of(L'\\');
            const std::wstring base = slash == std::wstring::npos ? path : path.substr(slash + 1);
            if (_wcsicmp(base.c_str(), name) != 0) continue;
            ++total;
            if (row.menu.has_menu) ++menus;
        }
        std::printf("  known %ls: windows=%d with_menu=%d\n", name, total, menus);
    }

    // ---- Q6: foreground name resolution ---------------------------------
    std::printf("\nQ6 foreground name resolution\n");
    HWND fg = GetForegroundWindow();
    DWORD fg_pid = 0;
    GetWindowThreadProcessId(fg, &fg_pid);
    const std::int64_t t0 = Qpc();
    const std::wstring fg_path = ProcessImagePath(fg_pid);
    const std::int64_t t1 = Qpc();
    const std::wstring fg_desc = FileDescription(fg_path);
    const std::int64_t t2 = Qpc();
    std::printf("  hwnd=%llu pid=%lu path=%s\n",
                static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(fg)),
                static_cast<unsigned long>(fg_pid),
                std::string(fg_path.begin(), fg_path.end()).c_str());
    std::printf("  description=%s\n", std::string(fg_desc.begin(), fg_desc.end()).c_str());
    std::printf("  path_ms=%.3f description_ms=%.3f total_ms=%.3f\n", Ms(t1 - t0), Ms(t2 - t1),
                Ms(t2 - t0));

    // ---- Q4: foreground event latency -----------------------------------
    std::printf("\nQ4 SetWinEventHook(EVENT_SYSTEM_FOREGROUND) latency\n");
    HWND second = CreateWindowExW(0, kMenuWindowClass, L"DockForge Spike Menu 2",
                                  WS_OVERLAPPEDWINDOW, 480, 80, 360, 240, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    ShowWindow(second, SW_SHOW);
    PumpFor(120);
    HWINEVENTHOOK hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                        nullptr, OnForegroundEvent, 0, 0,
                                        WINEVENT_OUTOFCONTEXT);
    std::printf("  hook=%d\n", hook != nullptr ? 1 : 0);
    double worst = 0.0;
    double total = 0.0;
    int samples = 0;
    for (int round = 0; round < 5; ++round) {
        HWND target = (round % 2 == 0) ? second : own;
        const LONG before = g_event_count;
        const std::int64_t call_qpc = Qpc();
        const BOOL ok = SetForegroundWindow(target);
        const std::int64_t returned = Qpc();
        double delta = -1.0;
        for (int i = 0; i < 400; ++i) {
            if (g_event_count != before) {
                delta = Ms(g_event_qpc - call_qpc);
                break;
            }
            PumpFor(1);
        }
        std::printf("  round %d target=%s SetForegroundWindow=%d call_ms=%.3f event_ms=%.3f "
                    "match=%d\n",
                    round, target == second ? "second" : "own", ok ? 1 : 0, Ms(returned - call_qpc),
                    delta, g_event_hwnd == target ? 1 : 0);
        if (delta >= 0.0) {
            total += delta;
            ++samples;
            if (delta > worst) worst = delta;
        }
    }
    std::printf("  samples=%d avg_ms=%.3f worst_ms=%.3f\n", samples,
                samples > 0 ? total / samples : 0.0, worst);
    if (hook != nullptr) UnhookWinEvent(hook);

    // ---- Q7: does a top strip eat clicks? -------------------------------
    std::printf("\nQ7 top strip hit testing\n");
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int screen_w = GetSystemMetrics(SM_CXSCREEN);
    const int strip_h = 56;  // 28 DIP at 192 DPI
    HWND strip = CreateWindowExW(WS_EX_TOPMOST, kMenuWindowClass, L"DockForge Spike Strip",
                                 WS_POPUP, 0, 0, screen_w, strip_h, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
    ShowWindow(strip, SW_SHOW);
    SetWindowPos(strip, HWND_TOPMOST, 0, 0, screen_w, strip_h, SWP_SHOWWINDOW);
    PumpFor(150);
    const POINT mid_left{40, strip_h / 2};
    const POINT mid_centre{screen_w / 2, strip_h / 2};
    const POINT below{screen_w / 2, strip_h + 40};
    HWND hit_left = WindowFromPoint(mid_left);
    HWND hit_centre = WindowFromPoint(mid_centre);
    HWND hit_below = WindowFromPoint(below);
    std::printf("  strip visible=%d rect=%d,%d,%d,%d work_top=%ld\n",
                IsWindowVisible(strip) ? 1 : 0, 0, 0, screen_w, strip_h, work.top);
    std::printf("  WindowFromPoint(40,%d)=%llu is_strip=%d\n", strip_h / 2,
                static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(hit_left)),
                hit_left == strip ? 1 : 0);
    std::printf("  WindowFromPoint(%d,%d)=%llu is_strip=%d\n", screen_w / 2, strip_h / 2,
                static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(hit_centre)),
                hit_centre == strip ? 1 : 0);
    std::printf("  WindowFromPoint(%d,%d)=%llu is_strip=%d\n", screen_w / 2, strip_h + 40,
                static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(hit_below)),
                hit_below == strip ? 1 : 0);

    SetWindowLongPtrW(strip, GWL_EXSTYLE,
                      GetWindowLongPtrW(strip, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
    PumpFor(80);
    const HWND hit_after = WindowFromPoint(mid_centre);
    std::printf("  after WS_EX_TRANSPARENT WindowFromPoint(%d,%d)=%llu is_strip=%d\n",
                screen_w / 2, strip_h / 2,
                static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(hit_after)),
                hit_after == strip ? 1 : 0);

    DestroyWindow(strip);
    DestroyWindow(second);
    DestroyWindow(own);
    std::printf("\nverdict: Q1=%s Q2=%s Q5=%s\n",
                own_top.has_menu && own_top.read_ok ? "PASS" : "FAIL",
                child_hwnd != nullptr && IsWindow(child_hwnd) ? "see numbers" : "no child",
                child_hit ? "PASS" : "FAIL");
    return 0;
}
