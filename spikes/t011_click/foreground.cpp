// DockForge T-011 spike, part 2: does a click on our own no-activate window grant
// foreground rights?
//
// The dock window is WS_EX_NOACTIVATE, so clicking it does not make our process
// foreground. SetForegroundWindow was refused in the first spike (verdict
// FOREGROUND_REFUSED, and AttachThreadInput did not help). But Windows grants
// foreground rights to the process that received the last input event, and a real
// click on our window may be exactly that. This decides whether the dock needs a
// synthetic-input workaround or can just call SetForegroundWindow.
//
// Modes: plain | click | altkey
//   plain   : call SetForegroundWindow with no input at all (baseline, expected refuse)
//   click   : build a no-activate window, click it, activate from the handler
//   altkey  : press and release ALT first, then activate (the documented unlock)
#include <windows.h>
#include <dwmapi.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

std::string ToUtf8(const wchar_t* text) {
    if (text == nullptr) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};
    std::string out(static_cast<std::size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), need, nullptr, nullptr);
    return out;
}

struct FindState {
    const std::wstring* needle;
    HWND found;
};

HWND FindAppWindow(const std::wstring& needle) {
    FindState state{&needle, nullptr};
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* s = reinterpret_cast<FindState*>(param);
            if (!IsWindowVisible(hwnd)) return TRUE;
            if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
            if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) return TRUE;
            DWORD cloaked = 0;
            if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
                cloaked != 0) {
                return TRUE;
            }
            wchar_t title[512]{};
            GetWindowTextW(hwnd, title, 511);
            if (title[0] == L'\0') return TRUE;
            if (std::wstring(title).find(*s->needle) == std::wstring::npos) return TRUE;
            s->found = hwnd;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&state));
    return state.found;
}

HWND g_target = nullptr;
HWND g_dock = nullptr;
bool g_activated = false;
HWND g_foreground_after = nullptr;

LRESULT CALLBACK DockProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_LBUTTONDOWN: {
            // This is the real click path: the user clicked the dock, now activate.
            const BOOL ok = SetForegroundWindow(g_target);
            Sleep(120);
            g_foreground_after = GetForegroundWindow();
            g_activated = (g_foreground_after == g_target);
            std::printf("  [dock] WM_LBUTTONDOWN set_fg=%d ok=%d\n", ok ? 1 : 0,
                        g_activated ? 1 : 0);
            PostQuitMessage(0);
            return 0;
        }
        case WM_CLOSE:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void PumpFor(int ms) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(ms);
    MSG msg{};
    while (GetTickCount() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}

int ModeClick(const std::wstring& needle) {
    g_target = FindAppWindow(needle);
    if (g_target == nullptr) {
        std::printf("target=%s not_found\n", ToUtf8(needle.c_str()).c_str());
        return 2;
    }
    std::printf("target=%p\n", static_cast<void*>(g_target));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DockProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"T011DockProbe";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    // Same extended style as the real dock: topmost, no activate, tool window.
    g_dock = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                             L"T011DockProbe", L"probe", WS_POPUP, 200, 200, 240, 80,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (g_dock == nullptr) {
        std::printf("dock window creation failed: %lu\n", GetLastError());
        return 3;
    }
    ShowWindow(g_dock, SW_SHOWNOACTIVATE);
    UpdateWindow(g_dock);
    PumpFor(150);

    RECT rc{};
    GetWindowRect(g_dock, &rc);
    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2;
    std::printf("dock=%p rect=%ld,%ld,%ld,%ld click_at=%d,%d\n", static_cast<void*>(g_dock),
                rc.left, rc.top, rc.right, rc.bottom, cx, cy);

    SetCursorPos(cx, cy);
    PumpFor(60);

    INPUT input[2]{};
    input[0].type = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    input[1].type = INPUT_MOUSE;
    input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, input, sizeof(INPUT));

    PumpFor(600);

    std::printf("foreground_after_click=%p activated=%d\n",
                static_cast<void*>(g_foreground_after), g_activated ? 1 : 0);
    std::printf("verdict=%s\n", g_activated ? "CLICK_GRANTS_FOREGROUND" : "CLICK_NOT_ENOUGH");

    DestroyWindow(g_dock);
    return 0;
}

int ModePlain(const std::wstring& needle) {
    HWND target = FindAppWindow(needle);
    if (target == nullptr) {
        std::printf("target=%s not_found\n", ToUtf8(needle.c_str()).c_str());
        return 2;
    }
    const HWND before = GetForegroundWindow();
    const BOOL ok = SetForegroundWindow(target);
    Sleep(150);
    const HWND after = GetForegroundWindow();
    std::printf("set_fg=%d before=%p after=%p\n", ok ? 1 : 0, static_cast<void*>(before),
                static_cast<void*>(after));
    std::printf("verdict=%s\n", after == target ? "ACTIVATED" : "REFUSED");
    return 0;
}

int ModeAltKey(const std::wstring& needle) {
    HWND target = FindAppWindow(needle);
    if (target == nullptr) {
        std::printf("target=%s not_found\n", ToUtf8(needle.c_str()).c_str());
        return 2;
    }
    // A synthetic ALT press makes Windows treat the caller as having received
    // input, which lifts the foreground lock for the next SetForegroundWindow.
    INPUT keys[2]{};
    keys[0].type = INPUT_KEYBOARD;
    keys[0].ki.wVk = VK_MENU;
    keys[1].type = INPUT_KEYBOARD;
    keys[1].ki.wVk = VK_MENU;
    keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, keys, sizeof(INPUT));
    Sleep(60);

    ShowWindow(target, SW_RESTORE);
    const BOOL ok = SetForegroundWindow(target);
    Sleep(150);
    const HWND after = GetForegroundWindow();
    std::printf("set_fg=%d after=%p target=%p\n", ok ? 1 : 0, static_cast<void*>(after),
                static_cast<void*>(target));
    std::printf("verdict=%s\n", after == target ? "ALTKEY_ACTIVATED" : "ALTKEY_REFUSED");
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::wstring mode = L"plain";
    std::wstring target = L"Notepad";
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg.rfind(L"--mode=", 0) == 0) mode = arg.substr(7);
        else if (arg.rfind(L"--target=", 0) == 0) target = arg.substr(9);
    }

    if (mode == L"plain") return ModePlain(target);
    if (mode == L"click") return ModeClick(target);
    if (mode == L"altkey") return ModeAltKey(target);
    std::printf("unknown mode\n");
    return 1;
}
