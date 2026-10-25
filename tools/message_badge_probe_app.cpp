// DockForge T-065 probe target: a minimal top-level GUI window whose title
// carries the unread count, "(4) badge probe".
//
// Why a purpose-built app instead of a console: on this OS build the console
// window belongs to WindowsTerminal.exe, so a title-badge adapter matching
// "powershell.exe" never sees it (measured - tools/t065_manual_step.ps1 with
// the real adapter answered 0). WeChat/QQ own their windows directly, so the
// adapter's process-name match is right; the probe just needs a target that
// owns its window the same way.
//
// A shortcut to this exe goes into the Start Menu (created by the probe), which
// makes the AppsFolder scan surface it as a real dock tile whose
// target_path basename equals the adapter id - the full chain
// window title -> adapter -> tile match -> badge paint runs for real.
//
// Title is settable per launch: argv[1] overrides the default "(4) badge probe".
#include <windows.h>

#include <wchar.h>

namespace {

const wchar_t kClassName[] = L"DockForgeBadgeProbeWindow";

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR cmdline, int show) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return 1;

    // argv[1] = window title. wWinMain's cmdline is the raw tail, so the title
    // is everything after the first space (Launch, not CommandLineToArgvW: the
    // title is free text and must keep its spaces and parentheses).
    const wchar_t* title = L"(4) badge probe";
    if (cmdline && cmdline[0] != L'\0') {
        title = cmdline[0] == L'"' ? cmdline + 1 : cmdline;
        if (const wchar_t* end = wcschr(title, L'"')) {
            const_cast<wchar_t*>(title)[end - title] = L'\0';
        }
    }

    HWND hwnd = CreateWindowExW(0, kClassName, title, WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 360, 200, nullptr,
                                nullptr, instance, nullptr);
    if (!hwnd) return 2;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
