// T-302 tray icon spike, round 4: the self-rescue path.
//
// Round 3 isolated the rule: the menu only dismisses on an outside click when the
// owner window holds the foreground. Scenario 0 had SetForegroundWindow return 0,
// the menu never entered menu mode, and TrackPopupMenu sat there for 17 seconds
// before returning the default item. For a tray icon that failure is unacceptable -
// a menu that will not close is worse than no menu.
//
// So the production path needs a fallback. The candidate is EndMenu() from a
// WM_TIMER inside the modal loop, armed only when SetForegroundWindow failed. The
// timer already proves it runs during TrackPopupMenu (round 3 saw the popup from
// inside it). What is unproven is whether EndMenu() actually terminates the loop,
// and whether the click can be detected at all without foreground rights.
//
// Questions:
//   Q7  Does EndMenu() from a modal-loop timer make TrackPopupMenu return?
//   Q8  Can an outside click be detected with GetAsyncKeyState while the menu is up?
//   Q9  How long does the fallback take to fire? (it must feel instant, not 17s)
//
// Build:
//   g++ -std=c++17 -O2 -o tray_spike4.exe tray_spike4.cpp -luser32 -lgdi32 -lcomctl32

#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <commctrl.h>

#include <cstdio>

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

constexpr UINT_PTR kWatchTimerId = 11;
constexpr UINT kMenuClosedMessage = WM_APP + 77;

HWND g_window = nullptr;
bool g_popup_seen = false;
RECT g_popup_rect{};
bool g_click_sent = false;
bool g_down_seen = false;
bool g_end_menu_called = false;
int g_watch_ticks = 0;

void SendClick(int x, int y) {
    SetCursorPos(x, y);
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    Log("  [click] at %d,%d SendInput=%u", x, y, SendInput(2, inputs, sizeof(INPUT)));
}

// The fallback watcher. It runs inside TrackPopupMenu's modal pump, so everything
// it does has to be cheap and must not assume foreground rights.
void WatchFromTimer() {
    ++g_watch_ticks;
    if (!g_popup_seen) {
        HWND popup = FindWindowW(L"#32768", nullptr);
        if (popup == nullptr) return;
        g_popup_seen = true;
        GetWindowRect(popup, &g_popup_rect);
        Log("  [watch] popup rect=%ld,%ld,%ld,%ld", g_popup_rect.left, g_popup_rect.top,
            g_popup_rect.right, g_popup_rect.bottom);
        return;
    }
    if (!g_click_sent) {
        g_click_sent = true;
        SendClick(g_popup_rect.left - 400, g_popup_rect.bottom + 300);
        return;
    }
    // GetAsyncKeyState reads the physical button state regardless of who owns the
    // foreground, which is exactly why it is the right detector here.
    const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (down && !g_down_seen) {
        g_down_seen = true;
        POINT cursor{};
        GetCursorPos(&cursor);
        const bool inside = cursor.x >= g_popup_rect.left && cursor.x < g_popup_rect.right &&
                            cursor.y >= g_popup_rect.top && cursor.y < g_popup_rect.bottom;
        Log("  [watch] button down at %ld,%ld inside_popup=%d -> EndMenu", cursor.x, cursor.y,
            inside ? 1 : 0);
        g_end_menu_called = true;
        EndMenu();
    }
}

UINT ShowMenu(HWND hwnd, int x, int y, int* elapsed_out, bool* foreground_ok) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1001, L"Show Dock");
    AppendMenuW(menu, MF_STRING, 1002, L"Hide Dock");
    AppendMenuW(menu, MF_STRING, 1004, L"Quit DockForge");
    SetMenuDefaultItem(menu, 1003, FALSE);

    const ULONGLONG start = GetTickCount64();
    *foreground_ok = SetForegroundWindow(hwnd) != FALSE;
    Log("  SetForegroundWindow=%d", *foreground_ok ? 1 : 0);

    g_popup_seen = false;
    g_click_sent = false;
    g_down_seen = false;
    g_end_menu_called = false;
    g_watch_ticks = 0;
    SetTimer(hwnd, kWatchTimerId, 60, nullptr);
    const UINT id = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, x, y, 0, hwnd, nullptr));
    KillTimer(hwnd, kWatchTimerId);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    if (elapsed_out != nullptr) *elapsed_out = static_cast<int>(GetTickCount64() - start);
    DestroyMenu(menu);
    return id;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_TIMER && wp == kWatchTimerId) {
        WatchFromTimer();
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

}  // namespace

int main() {
    g_log = std::fopen("tray_spike4.log", "w");
    Log("tray spike round 4: EndMenu self-rescue");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TraySpike4Window";
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log("RegisterClassExW failed err=%lu", GetLastError());
        return 1;
    }
    g_window = CreateWindowExW(WS_EX_TOOLWINDOW, L"TraySpike4Window", L"tray spike 4", WS_POPUP, 0, 0, 1, 1,
                               nullptr, nullptr, wc.hInstance, nullptr);
    if (g_window == nullptr) {
        Log("CreateWindowExW failed err=%lu", GetLastError());
        return 1;
    }

    // Force the hostile precondition: give the foreground to a real visible window
    // that is not ours, so SetForegroundWindow on a hidden popup is refused.
    HWND hostile = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (hostile != nullptr) SetForegroundWindow(hostile);
    Sleep(120);
    Log("foreground before the menu = %p ours=%p", static_cast<void*>(GetForegroundWindow()),
        static_cast<void*>(g_window));

    int elapsed = 0;
    bool foreground_ok = false;
    const UINT id = ShowMenu(g_window, 1200, 800, &elapsed, &foreground_ok);
    Log("[Q7] result=%u elapsed_ms=%d foreground_ok=%d watch_ticks=%d end_menu=%d",
        id, elapsed, foreground_ok ? 1 : 0, g_watch_ticks, g_end_menu_called ? 1 : 0);
    Log("[Q8] popup_seen=%d click_sent=%d button_down_seen=%d", g_popup_seen ? 1 : 0,
        g_click_sent ? 1 : 0, g_down_seen ? 1 : 0);

    const bool q7 = g_end_menu_called && elapsed < 3000;
    Log("verdict: Q7_endmenu_closes=%s Q8_click_detected=%s Q9_fast=%s",
        q7 ? "PASS" : "FAIL", g_down_seen ? "PASS" : "FAIL", (elapsed < 3000) ? "PASS" : "FAIL");

    DestroyWindow(g_window);
    if (g_log != nullptr) std::fclose(g_log);
    return 0;
}
