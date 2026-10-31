// DockForge T-301 spike, part 4: prove the foreign menu really appears and that a
// click on it produces a command id we can forward.
//
// Part 3 showed TrackPopupMenuEx(foreign HMENU) blocks for the full timer duration
// and returns 0 - consistent with "the menu opened and was cancelled", but also
// consistent with "the call failed silently". Neither reading is evidence. This run
// replaces the human with instruments:
//
//   * A worker thread polls for the popup menu's window class, "#32768", the class
//     user32 gives every tracked menu. Finding a visible #32768 window whose owning
//     process is US (not the child) while the call is blocked proves the foreign
//     menu was rendered by our own menu loop.
//   * GetMenuItemRect on that menu yields the real screen rectangle of each item, so
//     the spike can aim at a known item instead of guessing coordinates.
//   * A synthetic click (SetCursorPos + mouse_event) on item 1 of the File submenu
//     must make TrackPopupMenuEx return that item's id. A returned id that equals the
//     id we aimed at is the proof that the foreign menu is fully interactive.
//   * The id is then forwarded with PostMessage(WM_COMMAND), and the child writes the
//     id it received. End to end: foreign menu -> our bar -> real command in the
//     foreign process.
//
// The cursor position is saved and restored; a spike that leaves the pointer
// somewhere unexpected is a spike that breaks the next measurement.
//
// Build:
//   g++ -std=c++17 -O2 -municode -o spikes/T-301_finderbar_menu/menu_spike4.exe \
//       spikes/T-301_finderbar_menu/menu_spike4.cpp -luser32
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kClass[] = L"DockForgeSpike4Window";
constexpr UINT kAimCommand = 1002;  // "&Save", second item of the File submenu

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER) {
        EndMenu();
        return 0;
    }
    if (msg == WM_COMMAND) {
        // Child side: record what actually arrived, so the parent can prove the
        // forwarded id crossed the process boundary intact.
        const std::wstring out = [] {
            wchar_t path[MAX_PATH * 2] = {};
            const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
            std::wstring full(path, n);
            const std::size_t slash = full.find_last_of(L'\\');
            return (slash == std::wstring::npos ? std::wstring() : full.substr(0, slash)) +
                   L"\\spike4_child_command.txt";
        }();
        FILE* f = nullptr;
        if (_wfopen_s(&f, out.c_str(), L"w") == 0 && f != nullptr) {
            std::fprintf(f, "%u\n", static_cast<unsigned>(LOWORD(wp)));
            std::fclose(f);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

double NowMs() {
    static LARGE_INTEGER freq{};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
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

std::wstring ExePath() {
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    return std::wstring(path, n);
}

std::wstring ExeDir() {
    const std::wstring full = ExePath();
    const std::size_t slash = full.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : full.substr(0, slash);
}

struct FindMenuWindowProc {
    DWORD our_pid = 0;
    HWND found = nullptr;
    RECT rect{};
};

BOOL CALLBACK FindMenuWindow(HWND hwnd, LPARAM param) {
    auto* state = reinterpret_cast<FindMenuWindowProc*>(param);
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (std::wstring(cls) != L"#32768") return TRUE;
    if (IsWindowVisible(hwnd) == FALSE) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != state->our_pid) return TRUE;
    state->found = hwnd;
    GetWindowRect(hwnd, &state->rect);
    return FALSE;
}

// Child mode: a window with a real menu bar and a File submenu whose second item is
// kAimCommand. Stays alive long enough for the parent to finish.
int RunChild() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, 1001, L"&Open");
    AppendMenuW(file, MF_STRING, kAimCommand, L"&Save");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, 1003, L"E&xit");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");

    HWND w = CreateWindowExW(WS_EX_TOOLWINDOW, kClass, L"DockForge Spike4 Child",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 420, 260, nullptr,
                             bar, GetModuleHandleW(nullptr), nullptr);
    if (w == nullptr) return 2;
    ShowWindow(w, SW_SHOWNOACTIVATE);

    const std::wstring out = ExeDir() + L"\\spike4_child.txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, out.c_str(), L"w") == 0 && f != nullptr) {
        std::fprintf(f, "%lld\n", static_cast<long long>(reinterpret_cast<intptr_t>(w)));
        std::fclose(f);
    }

    MSG msg{};
    const DWORD until = GetTickCount() + 25000;
    while (GetTickCount() < until) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != FALSE) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring(argv[1]) == L"--child") return RunChild();

    std::printf("T-301 finder bar spike part 4 (pid=%lu)\n",
                static_cast<unsigned long>(GetCurrentProcessId()));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    HWND host = CreateWindowExW(WS_EX_TOPMOST, kClass, L"DockForge Spike4 Host", WS_POPUP, 0, 0,
                                200, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    // ---- child with the foreign menu ------------------------------------
    const std::wstring child_out = ExeDir() + L"\\spike4_child.txt";
    const std::wstring child_cmd_out = ExeDir() + L"\\spike4_child_command.txt";
    DeleteFileW(child_out.c_str());
    DeleteFileW(child_cmd_out.c_str());
    std::wstring cmd = std::wstring(L"\"") + ExePath() + L"\" --child";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi) ==
        FALSE) {
        std::printf("  child spawn failed err=%lu\n", static_cast<unsigned long>(GetLastError()));
        return 1;
    }
    CloseHandle(pi.hThread);
    PumpFor(700);

    HWND child = nullptr;
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, child_out.c_str(), L"r") == 0 && f != nullptr) {
            long long value = 0;
            if (std::fscanf(f, "%lld", &value) == 1) {
                child = reinterpret_cast<HWND>(static_cast<intptr_t>(value));
            }
            std::fclose(f);
        }
    }
    HMENU foreign = child != nullptr ? GetMenu(child) : nullptr;
    HMENU file_menu = foreign != nullptr ? GetSubMenu(foreign, 0) : nullptr;
    std::printf("  child=%p menu=%p file_sub=%p\n", static_cast<void*>(child),
                static_cast<void*>(foreign), static_cast<void*>(file_menu));
    if (file_menu == nullptr) {
        std::printf("  no foreign menu; aborting\n");
        return 1;
    }

    // ---- instrument thread ----------------------------------------------
    // Watches for the tracked menu window while the main thread is blocked inside
    // TrackPopupMenuEx, aims at the item we want, and clicks it.
    struct Probe {
        std::atomic<bool> menu_seen{false};
        std::atomic<bool> clicked{false};
        std::atomic<bool> item_rect_ok{false};
        std::atomic<int> aim_item{-1};
        HWND child = nullptr;
        HMENU menu = nullptr;
        RECT menu_rect{};
        RECT aim_rect{};
        POINT saved_cursor{};
        bool cursor_saved = false;
    } probe;
    probe.child = child;
    probe.menu = file_menu;

    DWORD our_pid = GetCurrentProcessId();
    HANDLE thread = CreateThread(
        nullptr, 0,
        [](LPVOID raw) -> DWORD {
            auto* state = static_cast<Probe*>(raw);
            const double deadline = NowMs() + 4000.0;
            while (NowMs() < deadline) {
                FindMenuWindowProc finder;
                finder.our_pid = GetCurrentProcessId();
                EnumWindows(FindMenuWindow, reinterpret_cast<LPARAM>(&finder));
                if (finder.found != nullptr) {
                    state->menu_rect = finder.rect;
                    state->menu_seen = true;
                    state->aim_item = 1;  // "&Save" -> kAimCommand
                    GetCursorPos(&state->saved_cursor);
                    state->cursor_saved = true;

                    // The tracked popup is on screen now, so the documented
                    // GetMenuItemRect(hwndThatOwnsTheMenu, hmenu, index, &rect)
                    // returns the item's real screen rectangle. Aiming at its centre
                    // removes the guesswork that made the first run click item 0.
                    RECT item{};
                    POINT aim{};
                    if (GetMenuItemRect(state->child, state->menu, 1, &item) != FALSE) {
                        state->item_rect_ok = true;
                        state->aim_rect = item;
                        aim.x = (item.left + item.right) / 2;
                        aim.y = (item.top + item.bottom) / 2;
                    } else {
                        // Fall back to the menu window's own item grid.
                        const int menu_h = finder.rect.bottom - finder.rect.top;
                        const int menu_w = finder.rect.right - finder.rect.left;
                        aim.x = finder.rect.left + menu_w / 2;
                        aim.y = finder.rect.top + (menu_h * 3) / 8;
                        state->aim_rect = RECT{aim.x, aim.y, aim.x, aim.y};
                    }
                    SetCursorPos(aim.x, aim.y);
                    Sleep(80);
                    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
                    Sleep(30);
                    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
                    state->clicked = true;
                    return 0;
                }
                Sleep(20);
            }
            return 1;
        },
        &probe, 0, nullptr);

    // Safety net: if the click misses, EndMenu fires and the call returns 0 rather
    // than hanging the spike.
    SetTimer(host, 1, 2500, nullptr);
    const double t0 = NowMs();
    const UINT id = TrackPopupMenuEx(file_menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN |
                                                  TPM_TOPALIGN,
                                     400, 400, host, nullptr);
    const double elapsed = NowMs() - t0;
    KillTimer(host, 1);
    WaitForSingleObject(thread, 3000);
    CloseHandle(thread);

    std::printf("  TrackPopupMenuEx(foreign) -> id=%u elapsed_ms=%.1f\n", id, elapsed);
    std::printf("  tracked menu window seen=%d rect=%ld,%ld,%ld,%ld\n",
                probe.menu_seen.load() ? 1 : 0, probe.menu_rect.left, probe.menu_rect.top,
                probe.menu_rect.right, probe.menu_rect.bottom);
    std::printf("  GetMenuItemRect ok=%d item_rect=%ld,%ld,%ld,%ld\n",
                probe.item_rect_ok.load() ? 1 : 0, probe.aim_rect.left, probe.aim_rect.top,
                probe.aim_rect.right, probe.aim_rect.bottom);
    std::printf("  click sent=%d aimed at item index=%d (expected id=%u)\n",
                probe.clicked.load() ? 1 : 0, probe.aim_item.load(), kAimCommand);

    if (probe.cursor_saved) SetCursorPos(probe.saved_cursor.x, probe.saved_cursor.y);

    const bool menu_rendered = probe.menu_seen.load();
    const bool id_matches = id == kAimCommand;

    // ---- forward whatever came back -------------------------------------
    if (id != 0 && child != nullptr) {
        const BOOL posted = PostMessageW(child, WM_COMMAND, MAKEWPARAM(id, 0), 0);
        PumpFor(300);
        unsigned received = 0;
        FILE* f = nullptr;
        if (_wfopen_s(&f, child_cmd_out.c_str(), L"r") == 0 && f != nullptr) {
            if (std::fscanf(f, "%u", &received) != 1) received = 0;
            std::fclose(f);
        }
        std::printf("  forwarded WM_COMMAND %u posted=%d child_received=%u\n", id, posted ? 1 : 0,
                    received);
        std::printf("  forward round-trip ok=%d\n", (posted != FALSE && received == id) ? 1 : 0);
    } else {
        std::printf("  no id returned; nothing to forward\n");
    }

    if (child != nullptr) PostMessageW(child, WM_CLOSE, 0, 0);
    WaitForSingleObject(pi.hProcess, 2000);
    CloseHandle(pi.hProcess);
    DestroyWindow(host);

    std::printf("\nverdict: Q9_menu_rendered=%s Q9_id_round_trip=%s\n",
                menu_rendered ? "PASS" : "FAIL", id_matches ? "PASS" : "FAIL");
    return (menu_rendered && id_matches) ? 0 : 1;
}
