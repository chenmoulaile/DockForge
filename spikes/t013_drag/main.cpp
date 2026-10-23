// DockForge spike T-013 - can a region-clipped window still track a drag outside it?
//
// The dock is a window whose region is rebuilt from the plate shape (C-12). Internal
// reordering needs the pointer to leave the icon row - the user drags an icon up and
// sideways - while the window keeps receiving WM_MOUSEMOVE. Three things could break
// that, and only measurement separates them:
//
//   Q1  Does SetCapture keep delivering WM_MOUSEMOVE after the pointer leaves the
//       window rectangle entirely?
//   Q2  Does a window region clip the captured mouse messages too? A region is a
//       hit-test shape; capture bypasses hit-testing, so it should not - but the
//       region also clips the composition surface (spike T-010), so "should" is not
//       evidence.
//   Q3  Does the OS deliver WM_MOUSELEAVE while captured? If it does, a naive hover
//       handler clears the drag the moment the pointer leaves the plate.
//
// Modes:
//   --mode=capture --region --auto-drag   automated: capture, move out, report
//   --mode=target  --hold-ms=N            interactive: drag this window by hand
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

struct State {
    HWND hwnd = nullptr;
    std::vector<std::wstring> lines;
    POINT origin{};
    int width = 0;
    int height = 0;
    bool captured = false;
    bool auto_drag = false;
    bool use_region = false;
    // Control switch. Without capture, moving out of the window MUST produce
    // WM_MOUSELEAVE. If it does not, this spike cannot see leaves at all and the
    // "leave_while_captured=0" result below proves nothing.
    bool use_capture = true;
    int left_button_downs = 0;
    int moves_inside = 0;
    int moves_outside = 0;
    int moves_captured_outside = 0;
    int leaves = 0;
    int leave_while_captured = 0;
    int cancels = 0;
    int track_arms = 0;
    int track_failures = 0;
    DWORD last_track_error = 0;
    int captures = 0;
    int releases = 0;
    POINT last{};
    DWORD start_tick = 0;
};

void Add(State& s, const std::wstring& text) { s.lines.push_back(text); }

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}

// A region that covers only the bottom half of the window. If Q2 is "the region clips
// captured moves too", the upper half reports nothing and the drag dies the moment the
// icon is lifted - which is exactly the gesture the dock needs.
void ApplyHalfRegion(State& s) {
    HRGN region = CreateRectRgn(0, s.height / 2, s.width, s.height);
    SetWindowRgn(s.hwnd, region, TRUE);
    Add(s, L"region applied: bottom half only");
}

// Arms TME_LEAVE and records whether the call actually succeeded.
//
// The first version of this spike ignored the return value, and when the control run
// reported leaves=0 with no capture, the only honest reading was "this spike cannot
// observe WM_MOUSELEAVE at all" - so the captured run's leaves=0 proved nothing.
// Checking the result separates "no leave happened" from "we never asked for one".
void ArmLeave(HWND hwnd, State& s) {
    TRACKMOUSEEVENT track{};
    track.cbSize = sizeof(track);
    track.dwFlags = TME_LEAVE;
    track.hwndTrack = hwnd;
    if (TrackMouseEvent(&track)) {
        ++s.track_arms;
    } else {
        ++s.track_failures;
        s.last_track_error = GetLastError();
    }
}

void Report(State& s, const wchar_t* out_path) {
    Add(s, L"--- counters ---");
    Add(s, L"left_button_downs=" + std::to_wstring(s.left_button_downs));
    Add(s, L"captures=" + std::to_wstring(s.captures));
    Add(s, L"releases=" + std::to_wstring(s.releases));
    Add(s, L"moves_inside=" + std::to_wstring(s.moves_inside));
    Add(s, L"moves_outside=" + std::to_wstring(s.moves_outside));
    Add(s, L"moves_captured_outside=" + std::to_wstring(s.moves_captured_outside));
    Add(s, L"leaves=" + std::to_wstring(s.leaves));
    Add(s, L"leave_while_captured=" + std::to_wstring(s.leave_while_captured));
    Add(s, L"cancels=" + std::to_wstring(s.cancels));
    Add(s, L"track_arms=" + std::to_wstring(s.track_arms));
    Add(s, L"track_failures=" + std::to_wstring(s.track_failures) + L" last_error=" +
               std::to_wstring(s.last_track_error));

    // Q1: capture keeps delivering moves outside the window rectangle.
    const bool q1 = s.moves_captured_outside > 0;
    // Q2: a window region does not clip captured moves. Only meaningful when a region
    // was actually applied; with no region the question is vacuous.
    const bool q2 = !s.use_region || s.moves_captured_outside > 0;
    // Q3: no WM_MOUSELEAVE arrives during a drag. The control run (mode=leave, no
    // button) proves this spike can observe leaves at all, so a zero here means the
    // message genuinely did not arrive rather than that it went unseen.
    const bool q3 = s.leave_while_captured == 0;

    Add(s, std::wstring(L"q1_capture_outside_window=") + (q1 ? L"YES" : L"NO"));
    Add(s, std::wstring(L"q2_region_allows_captured_moves=") + (q2 ? L"YES" : L"NO"));
    Add(s, std::wstring(L"q3_no_leave_during_drag=") + (q3 ? L"YES" : L"NO"));

    const bool all_ok = q1 && q2 && q3;
    Add(s, std::wstring(L"verdict=") + (all_ok ? L"DRAG_CAPTURE_OK" : L"DRAG_CAPTURE_BROKEN"));

    std::wstring joined;
    for (const auto& line : s.lines) {
        joined += line;
        joined += L"\n";
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, out_path, L"wb") == 0 && f) {
        const std::string utf8 = ToUtf8(joined);
        fwrite(utf8.data(), 1, utf8.size(), f);
        fclose(f);
    }
    for (const auto& line : s.lines) {
        printf("%s\n", ToUtf8(line).c_str());
    }
}

void PumpFor(int ms) {
    const DWORD until = GetTickCount() + static_cast<DWORD>(ms);
    MSG msg{};
    while (GetTickCount() < until) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(2);
    }
}

// Drives the pointer with real input so the capture path is exercised exactly as it
// would be for a user. PostMessage would bypass capture entirely and prove nothing.
void MoveTo(int x, int y) {
    SetCursorPos(x, y);
    Sleep(30);
}

void PressLeft() {
    INPUT in[2]{};
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &in[0], sizeof(INPUT));
}

void ReleaseLeft() {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof(INPUT));
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* s = reinterpret_cast<State*>(ref);
    switch (msg) {
        case WM_LBUTTONDOWN: {
            ++s->left_button_downs;
            if (s->use_capture) {
                SetCapture(hwnd);
                ++s->captures;
                s->captured = true;
            }
            // Tracking leave is what a hover handler does. If leave arrives during the
            // drag, the handler must ignore it (Q3).
            ArmLeave(hwnd, *s);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (s->captured) {
                ReleaseCapture();
                ++s->releases;
                s->captured = false;
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &pt);
            RECT rc{};
            GetWindowRect(hwnd, &rc);
            const bool inside = pt.x >= rc.left && pt.x < rc.right && pt.y >= rc.top &&
                                pt.y < rc.bottom;
            if (inside) {
                ++s->moves_inside;
            } else {
                ++s->moves_outside;
                if (GetCapture() == hwnd) ++s->moves_captured_outside;
            }
            s->last = pt;
            return 0;
        }
        case WM_MOUSELEAVE: {
            ++s->leaves;
            if (GetCapture() == hwnd) ++s->leave_while_captured;
            return 0;
        }
        case WM_CANCELMODE: {
            // Capture lost to something else (a modal loop, a foreground change).
            // A drag handler must treat this as "abort the drag", not "keep dragging".
            ++s->cancels;
            s->captured = false;
            return 0;
        }
        case WM_RBUTTONDOWN:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::wstring mode = L"target";
    std::wstring out = L"spikes/t013_drag/drag.txt";
    int hold_ms = 4000;
    bool auto_drag = false;
    bool use_region = false;
    bool use_capture = true;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg.rfind(L"--mode=", 0) == 0) mode = arg.substr(7);
        else if (arg.rfind(L"--out=", 0) == 0) out = arg.substr(6);
        else if (arg.rfind(L"--hold-ms=", 0) == 0) hold_ms = _wtoi(arg.c_str() + 10);
        else if (arg == L"--auto-drag") auto_drag = true;
        else if (arg == L"--region") use_region = true;
        else if (arg == L"--no-capture") use_capture = false;
    }

    State state;
    state.auto_drag = auto_drag;
    state.use_region = use_region;
    state.use_capture = use_capture;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DockForgeSpikeT013";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);

    state.width = 600;
    state.height = 200;
    const int x = 400;
    const int y = 400;
    state.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName,
                                 L"T-013 drag capture spike", WS_POPUP | WS_VISIBLE, x, y,
                                 state.width, state.height, nullptr, nullptr, wc.hInstance,
                                 nullptr);
    if (!state.hwnd) {
        printf("CreateWindowExW failed: %lu\n", GetLastError());
        return 1;
    }
    SetWindowSubclass(state.hwnd, WndProc, 1, reinterpret_cast<DWORD_PTR>(&state));
    SetWindowPos(state.hwnd, HWND_TOPMOST, x, y, state.width, state.height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (use_region) ApplyHalfRegion(state);
    PumpFor(300);

    Add(state, L"window=" + std::to_wstring(x) + L"," + std::to_wstring(y) + L" " +
                  std::to_wstring(state.width) + L"x" + std::to_wstring(state.height));
    Add(state, std::wstring(L"region=") + (use_region ? L"bottom-half" : L"none"));
    Add(state, std::wstring(L"capture=") + (use_capture ? L"on" : L"off (control)"));

    if (mode == L"leave") {
        // Minimal control: no button, no capture. Move in, then out. If this does not
        // produce exactly one WM_MOUSELEAVE, the spike cannot observe leave at all and
        // every leave-based conclusion in this file is void.
        MoveTo(x + state.width / 2, y + state.height / 2);
        PumpFor(200);
        ArmLeave(state.hwnd, state);
        MoveTo(x + state.width / 2, y - 150);
        PumpFor(400);
        Add(state, std::wstring(L"control_leaves=") + std::to_wstring(state.leaves));
    } else if (mode == L"capture" && auto_drag) {
        // Press inside the region, drag up out of the window, then sideways, then
        // release. Every leg is outside the window rect after the first move.
        MoveTo(x + state.width / 2, y + state.height * 3 / 4);
        PumpFor(80);
        PressLeft();
        PumpFor(80);

        MoveTo(x + state.width / 2, y + state.height / 4);   // inside rect, above region
        PumpFor(40);
        MoveTo(x + state.width / 2, y - 120);                // above the window entirely
        PumpFor(40);
        MoveTo(x + state.width / 2 + 200, y - 160);          // out and sideways
        PumpFor(40);
        MoveTo(x + state.width + 260, y + state.height / 2); // right of the window
        PumpFor(40);

        ReleaseLeft();
        PumpFor(80);
    } else {
        printf("interactive: drag the window with the left button, right-click to finish\n");
        const DWORD until = GetTickCount() + static_cast<DWORD>(hold_ms);
        MSG msg{};
        while (GetTickCount() < until) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(5);
        }
        if (IsWindow(state.hwnd)) DestroyWindow(state.hwnd);
    }

    Report(state, out.c_str());
    if (IsWindow(state.hwnd)) DestroyWindow(state.hwnd);
    return 0;
}
