// T-302 tray icon spike, round 3: the dismissal path, isolated.
//
// Round 2 showed the menu returning the DEFAULT item id (1003) instead of 0 when
// an outside click was injected from a WM_TIMER inside TrackPopupMenu's modal
// loop. That is not the documented behaviour, so one of the preconditions is
// wrong. The candidates, each testable:
//
//   A. SetForegroundWindow was refused, so the menu never entered "menu mode"
//      and never treated the click as a dismissal.
//   B. The injected click landed on a window that consumed it, and the menu
//      closed for a different reason.
//   C. TPM_NONOTIFY changes the outside-click path.
//   D. mouse_event injection from inside the modal loop is not equivalent to a
//      real click (it may be posted to the queue rather than to the menu).
//
// Each scenario re-runs the menu with one variable changed and reports the id,
// the elapsed time, and whether the popup actually disappeared. Escape is tested
// as the control: if Escape dismisses but a click does not, the problem is the
// click, not the menu.
//
// Build:
//   g++ -std=c++17 -O2 -o tray_spike3.exe tray_spike3.cpp \
//       -luser32 -lshell32 -lgdi32 -lcomctl32

#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

FILE* g_log = nullptr;

void Log(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    std::fputs(line, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    if (g_log != nullptr) {
        std::fputs(line, g_log);
        std::fputc('\n', g_log);
        std::fflush(g_log);
    }
}

constexpr UINT kCallbackMessage = WM_APP + 100;
constexpr UINT kTrayIconId = 1;
constexpr UINT_PTR kProbeTimerId = 7;

HWND g_window = nullptr;
int g_scenario = 0;
bool g_probe_done = false;
bool g_popup_seen = false;
RECT g_popup_rect{};
char g_action[128] = {};

void SnapshotPopup(const char* tag) {
    HWND popup = FindWindowW(L"#32768", nullptr);
    if (popup == nullptr) {
        Log("  [%s] no popup window", tag);
        return;
    }
    RECT rect{};
    GetWindowRect(popup, &rect);
    Log("  [%s] popup rect=%ld,%ld,%ld,%ld visible=%d", tag, rect.left, rect.top, rect.right, rect.bottom,
        IsWindowVisible(popup) ? 1 : 0);
}

// Delivers a click at a screen point using SendInput, which goes through the same
// path as a physical mouse. mouse_event is the deprecated wrapper around it; using
// the modern call removes one suspect from the list.
void SendClick(int x, int y) {
    SetCursorPos(x, y);
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    const UINT sent = SendInput(2, inputs, sizeof(INPUT));
    Log("  [click] at %d,%d SendInput=%u", x, y, sent);
}

void SendEscape() {
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_ESCAPE;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_ESCAPE;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    const UINT sent = SendInput(2, inputs, sizeof(INPUT));
    Log("  [escape] SendInput=%u", sent);
}

// Runs inside the modal loop, so it can act while the menu is up.
void ProbeFromTimer() {
    if (g_probe_done) return;
    if (!g_popup_seen) {
        HWND popup = FindWindowW(L"#32768", nullptr);
        if (popup == nullptr) return;
        g_popup_seen = true;
        GetWindowRect(popup, &g_popup_rect);
        Log("  [timer] popup found rect=%ld,%ld,%ld,%ld", g_popup_rect.left, g_popup_rect.top,
            g_popup_rect.right, g_popup_rect.bottom);
    }
    g_probe_done = true;
    if (g_scenario == 0) {
        // Far outside on both axes.
        SendClick(g_popup_rect.left - 400, g_popup_rect.bottom + 300);
    } else if (g_scenario == 1) {
        // Escape, the control case.
        SendEscape();
    } else if (g_scenario == 2) {
        // Just outside the right edge, one pixel beyond.
        SendClick(g_popup_rect.right + 8, g_popup_rect.top + 20);
    } else if (g_scenario == 3) {
        // Inside the menu, on the first item: proves the harness can select at all.
        SendClick(g_popup_rect.left + 20, g_popup_rect.top + 14);
    }
}

UINT ShowMenu(HWND hwnd, int x, int y, int* elapsed_out) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1001, L"Show Dock");
    AppendMenuW(menu, MF_STRING, 1002, L"Hide Dock");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 1003, L"Settings\u2026");
    AppendMenuW(menu, MF_STRING, 1004, L"Quit DockForge");
    SetMenuDefaultItem(menu, 1003, FALSE);

    const ULONGLONG start = GetTickCount64();
    const BOOL foreground = SetForegroundWindow(hwnd);
    const HWND actual_foreground = GetForegroundWindow();
    Log("  SetForegroundWindow=%d foreground=%p ours=%p match=%d", foreground ? 1 : 0,
        static_cast<void*>(actual_foreground), static_cast<void*>(hwnd),
        actual_foreground == hwnd ? 1 : 0);

    g_probe_done = false;
    g_popup_seen = false;
    SetTimer(hwnd, kProbeTimerId, 120, nullptr);
    const UINT id = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, x, y, 0, hwnd, nullptr));
    KillTimer(hwnd, kProbeTimerId);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    if (elapsed_out != nullptr) *elapsed_out = static_cast<int>(GetTickCount64() - start);
    DestroyMenu(menu);
    return id;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == kCallbackMessage) return 0;
    if (message == WM_TIMER && wp == kProbeTimerId) {
        ProbeFromTimer();
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

const char* ScenarioName(int index) {
    switch (index) {
        case 0: return "click far outside";
        case 1: return "escape";
        case 2: return "click just outside right edge";
        case 3: return "click inside first item";
        default: return "?";
    }
}

}  // namespace

int main() {
    g_log = std::fopen("tray_spike3.log", "w");
    Log("tray spike round 3: dismissal path");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TraySpike3Window";
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log("RegisterClassExW failed err=%lu", GetLastError());
        return 1;
    }
    g_window = CreateWindowExW(WS_EX_TOOLWINDOW, L"TraySpike3Window", L"tray spike 3", WS_POPUP, 0, 0, 1, 1,
                               nullptr, nullptr, wc.hInstance, nullptr);
    if (g_window == nullptr) {
        Log("CreateWindowExW failed err=%lu", GetLastError());
        return 1;
    }

    int results[4] = {-1, -1, -1, -1};
    for (int scenario = 0; scenario < 4; ++scenario) {
        g_scenario = scenario;
        Log("\n=== scenario %d: %s ===", scenario, ScenarioName(scenario));
        SnapshotPopup("before");
        int elapsed = 0;
        results[scenario] = static_cast<int>(ShowMenu(g_window, 1200, 800, &elapsed));
        Log("  result=%d elapsed_ms=%d", results[scenario], elapsed);
        SnapshotPopup("after");
        Sleep(250);
    }

    // A click far outside must yield 0 (dismissed). A click on the first item must
    // yield 1001 (selected). Escape must yield 0. The just-outside case is the one
    // that decides whether the shell treats a near miss as a dismissal.
    Log("\nverdict: outside_click=%d escape=%d just_outside=%d inside=%d",
        results[0], results[1], results[2], results[3]);

    DestroyWindow(g_window);
    if (g_log != nullptr) std::fclose(g_log);
    return 0;
}
