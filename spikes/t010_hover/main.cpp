// T-010 spike: hover input and hit-testing on a topmost, non-activating window.
//
// Three questions the dock cannot answer by reading documentation:
//   1. Does WM_MOUSEMOVE reach a WS_EX_NOACTIVATE topmost popup that never takes
//      focus? If not, hover magnification has no input at all.
//   2. Does WM_MOUSELEAVE arrive after TrackMouseEvent(TME_LEAVE)?
//   3. Does returning HTTRANSPARENT from WM_NCHITTEST let a click fall through to
//      a window owned by ANOTHER process? The SDK documents the pass-through only
//      for windows in the same thread, so this must be measured, not assumed.
//
// Two processes are needed for question 3, so this binary has two roles:
//   --mode=victim    plain window that logs clicks it receives
//   --mode=overlay   topmost noactivate window that logs mouse input and can
//                    synthesize a click at its own centre
#include <windows.h>
#include <windowsx.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr wchar_t kOverlayClass[] = L"SpikeT010Overlay";
constexpr wchar_t kVictimClass[] = L"SpikeT010Victim";

struct Options {
    std::wstring mode = L"overlay";
    int x = 400;
    int y = 400;
    int width = 600;
    int height = 160;
    int hold_ms = 3000;
    bool transparent_hittest = false;
    bool auto_move = false;
    bool click = false;
    bool region = false;
    std::wstring out;
};

Options g_options;
FILE* g_out = nullptr;

void Emit(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    std::printf("%s\n", buffer);
    std::fflush(stdout);
    if (g_out != nullptr) {
        std::fprintf(g_out, "%s\n", buffer);
        std::fflush(g_out);
    }
}

long long NowMs() {
    static LARGE_INTEGER frequency{};
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<long long>(counter.QuadPart * 1000 / frequency.QuadPart);
}

long long g_start = 0;
int g_move_count = 0;
int g_leave_count = 0;
int g_click_count = 0;
int g_victim_click_count = 0;
bool g_tracking = false;

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEMOVE: {
            ++g_move_count;
            if (!g_tracking) {
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd;
                const BOOL ok = TrackMouseEvent(&track);
                g_tracking = true;
                Emit("track=requested ok=%d", ok ? 1 : 0);
            }
            if (g_move_count <= 3) {
                Emit("event=mousemove t=%lld x=%d y=%d", NowMs() - g_start,
                     GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            }
            return 0;
        }
        case WM_MOUSELEAVE: {
            ++g_leave_count;
            g_tracking = false;
            Emit("event=mouseleave t=%lld", NowMs() - g_start);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            ++g_click_count;
            Emit("event=overlay_click t=%lld", NowMs() - g_start);
            return 0;
        }
        case WM_NCHITTEST: {
            if (!g_options.transparent_hittest) return HTCLIENT;
            const LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
            if (hit == HTCLIENT) {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ScreenToClient(hwnd, &pt);
                // Only the middle band is opaque; the rest must fall through.
                if (pt.y >= g_options.height / 2 - 20 && pt.y <= g_options.height / 2 + 20) {
                    return HTCLIENT;
                }
            }
            return HTTRANSPARENT;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

LRESULT CALLBACK VictimProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_LBUTTONDOWN:
            ++g_victim_click_count;
            Emit("event=victim_click t=%lld count=%d", NowMs() - g_start, g_victim_click_count);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void PumpFor(int milliseconds) {
    const long long deadline = NowMs() + milliseconds;
    MSG msg{};
    while (NowMs() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}

void SendClickAt(int screen_x, int screen_y) {
    SetCursorPos(screen_x, screen_y);
    Sleep(60);
    INPUT inputs[2]{};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    const UINT sent = SendInput(2, inputs, sizeof(INPUT));
    Emit("sendinput=click sent=%u at=%d,%d", sent, screen_x, screen_y);
}

int RunVictim() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = VictimProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kVictimClass;
    if (RegisterClassExW(&wc) == 0) {
        Emit("victim=register_failed err=%lu", GetLastError());
        return 1;
    }
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, kVictimClass, L"victim", WS_POPUP,
                                g_options.x, g_options.y, g_options.width, g_options.height,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd == nullptr) {
        Emit("victim=create_failed err=%lu", GetLastError());
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    Emit("victim=ready rect=%d,%d %dx%d", g_options.x, g_options.y, g_options.width,
         g_options.height);
    PumpFor(g_options.hold_ms);
    Emit("victim=done clicks=%d", g_victim_click_count);
    return 0;
}

int RunOverlay() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kOverlayClass;
    if (RegisterClassExW(&wc) == 0) {
        Emit("overlay=register_failed err=%lu", GetLastError());
        return 1;
    }
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, kOverlayClass, L"overlay",
                                WS_POPUP, g_options.x, g_options.y, g_options.width,
                                g_options.height, nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd == nullptr) {
        Emit("overlay=create_failed err=%lu", GetLastError());
        return 1;
    }
    if (g_options.region) {
        // Only the middle band is inside the region; the rest of the window is
        // transparent dead space that must not swallow clicks.
        HRGN rgn = CreateRectRgn(0, g_options.height / 2 - 20, g_options.width,
                                 g_options.height / 2 + 20);
        const int applied = SetWindowRgn(hwnd, rgn, TRUE);
        Emit("region=applied ret=%d", applied);
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    Emit("overlay=ready rect=%d,%d %dx%d hittest=%s region=%d", g_options.x, g_options.y,
         g_options.width, g_options.height,
         g_options.transparent_hittest ? "transparent" : "opaque", g_options.region ? 1 : 0);

    // Park the cursor far away first so the first move into the window is a real
    // transition rather than a no-op at the current position.
    SetCursorPos(g_options.x - 200, g_options.y - 200);
    PumpFor(200);

    if (g_options.auto_move) {
        const int cx = g_options.x + g_options.width / 2;
        const int cy = g_options.y + g_options.height / 2;
        for (int i = 1; i <= 5; ++i) {
            SetCursorPos(g_options.x + g_options.width * i / 6, cy);
            PumpFor(40);
        }
        Emit("auto_move=done moves=%d", g_move_count);
        SetCursorPos(g_options.x - 200, g_options.y - 200);
        PumpFor(200);
        Emit("auto_leave=done leaves=%d", g_leave_count);
    }

    if (g_options.click) {
        SendClickAt(g_options.x + g_options.width / 2, g_options.y + g_options.height / 2);
        PumpFor(200);
        // Second click lands in the transparent band above the opaque strip.
        SendClickAt(g_options.x + g_options.width / 2, g_options.y + 20);
        PumpFor(200);
    }

    PumpFor(g_options.hold_ms);
    Emit("overlay=summary moves=%d leaves=%d clicks=%d", g_move_count, g_leave_count,
         g_click_count);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg.rfind(L"--mode=", 0) == 0) {
            g_options.mode = arg.substr(7);
        } else if (arg.rfind(L"--rect=", 0) == 0) {
            int x = 0, y = 0, w = 0, h = 0;
            if (swscanf_s(arg.c_str() + 7, L"%d,%d,%d,%d", &x, &y, &w, &h) == 4) {
                g_options.x = x;
                g_options.y = y;
                g_options.width = w;
                g_options.height = h;
            }
        } else if (arg.rfind(L"--hold-ms=", 0) == 0) {
            g_options.hold_ms = _wtoi(arg.c_str() + 10);
        } else if (arg == L"--transparent") {
            g_options.transparent_hittest = true;
        } else if (arg == L"--auto-move") {
            g_options.auto_move = true;
        } else if (arg == L"--click") {
            g_options.click = true;
        } else if (arg == L"--region") {
            g_options.region = true;
        } else if (arg.rfind(L"--out=", 0) == 0) {
            g_options.out = arg.substr(6);
        }
    }

    if (!g_options.out.empty()) {
        _wfopen_s(&g_out, g_options.out.c_str(), L"wb");
    }
    g_start = NowMs();

    const int code = g_options.mode == L"victim" ? RunVictim() : RunOverlay();
    if (g_out != nullptr) std::fclose(g_out);
    return code;
}
