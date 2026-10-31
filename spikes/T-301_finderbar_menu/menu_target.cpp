// T-301 probe fixture: a Win32 window with a real menu bar.
//
// The Finder Bar's job (C-32) is to read another application's top-level menu and
// forward a chosen item back to it as WM_COMMAND. Proving that needs a target that
// (a) has a menu, (b) records which command it received, and (c) can be pointed at a
// file so the probe can read the answer without looking at a screen.
//
// None of the applications on this host qualify: spike Q3 measured 11 visible
// top-level windows, only 2 with menus, and 0 of them explorer or Chrome. Sublime Text
// has one but its ids are private and it does nothing observable. So the fixture is
// built here rather than borrowed.
//
// Usage: menu_target.exe <state-file>
//   Writes "ready <hwnd>" then, for every WM_COMMAND received, "command <id>".
//   Exits after 30s or when the state file is deleted, so a probe can never leak it.
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>

#include <cstdio>
#include <cwchar>

namespace {

constexpr int kFileNew = 4001;
constexpr int kFileOpen = 4002;
constexpr int kFileClose = 4003;
constexpr int kEditUndo = 4010;
constexpr int kEditRedo = 4011;
constexpr int kViewZoom = 4020;
constexpr int kHelpAbout = 4030;

wchar_t g_state_path[MAX_PATH] = {};
HWND g_window = nullptr;

void Append(const char* line) {
    if (g_state_path[0] == L'\0') return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_state_path, L"a") != 0 || f == nullptr) return;
    std::fputs(line, f);
    std::fputc('\n', f);
    std::fclose(f);
}

HMENU BuildMenu() {
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, kFileNew, L"&New\tCtrl+N");
    AppendMenuW(file, MF_STRING, kFileOpen, L"&Open\tCtrl+O");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING | MF_GRAYED, kFileClose, L"&Close");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");

    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, kEditUndo, L"&Undo");
    AppendMenuW(edit, MF_STRING, kEditRedo, L"&Redo");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), L"&Edit");

    // A nested submenu: the bar must not try to render inside one, but the system popup
    // it hands the HMENU to must keep working, which is what this proves.
    HMENU zoom = CreatePopupMenu();
    AppendMenuW(zoom, MF_STRING, kViewZoom + 1, L"Zoom &In");
    AppendMenuW(zoom, MF_STRING, kViewZoom + 2, L"Zoom &Out");
    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(zoom), L"&Zoom");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, kHelpAbout, L"&About");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
    return bar;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_COMMAND: {
            char line[64] = {};
            std::snprintf(line, sizeof(line), "command %u", static_cast<unsigned>(LOWORD(wparam)));
            Append(line);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g_window = nullptr;
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 1) {
        std::wcsncpy(g_state_path, argv[1], MAX_PATH - 1);
    }
    if (g_state_path[0] != L'\0') {
        // Start clean so a stale answer from a previous run can never be mistaken for
        // this one's.
        FILE* f = nullptr;
        if (_wfopen_s(&f, g_state_path, L"w") == 0 && f != nullptr) std::fclose(f);
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"T301MenuTarget";
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return 1;
    }

    // Offset from the top-left so it does not sit under the Finder Bar: a probe clicks
    // nothing, but a human watching the run should be able to see both windows.
    g_window = CreateWindowExW(WS_EX_APPWINDOW, L"T301MenuTarget", L"T-301 menu target",
                               WS_OVERLAPPEDWINDOW, 200, 200, 640, 360, nullptr, BuildMenu(),
                               wc.hInstance, nullptr);
    if (g_window == nullptr) return 2;

    char ready[64] = {};
    std::snprintf(ready, sizeof(ready), "ready %p", static_cast<void*>(g_window));
    Append(ready);

    ShowWindow(g_window, SW_SHOWNOACTIVATE);
    UpdateWindow(g_window);
    // Claim the foreground from inside our own process. A probe that launched us cannot
    // do it on our behalf: SetForegroundWindow is refused when the caller is not the
    // foreground process, which is the same rule that shapes the bar's menu thread
    // (spike Q18). A freshly started process still has the right, so take it now.
    SetForegroundWindow(g_window);
    SetActiveWindow(g_window);
    SetFocus(g_window);

    // Self-terminating: 30s is longer than any probe step and short enough that a
    // forgotten fixture never becomes permanent desktop clutter.
    const ULONGLONG deadline = GetTickCount64() + 30000;
    MSG msg{};
    while (GetTickCount64() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
    if (g_window != nullptr) DestroyWindow(g_window);
    return 0;
}
