// spikes/t007_monitor - verify per-monitor DPI + monitor enumeration before the
// main project depends on it.
//
// The questions this answers, in order:
//   1. On this host, how many monitors are there really, and what DPI does each
//      report to a PER_MONITOR_AWARE_V2 process? (An unaware process sees
//      virtualized numbers, which is how a "single 96 DPI monitor" illusion is
//      born.)
//   2. Does GetDpiForMonitor(MDT_EFFECTIVE_DPI) agree with GetDpiForWindow?
//   3. What does WM_DPICHANGED actually carry, and does wParam split into x/y?
//   4. Does SetWindowPos in physical pixels behave identically at every DPI, or
//      does the window manager reinterpret it?
//   5. Does AdjustWindowRectExForDpi matter for a WS_POPUP window? (Expected: no
//      non-client area, so no. Verified rather than assumed.)
//   6. Is there a way to observe a real DPI transition on a single-monitor host?
//      (Answer matters for evidence honesty: if not, the mixed-DPI row of
//      COMPATIBILITY.md cannot be signed off here.)
//
// Build:
//   cmake -S spikes/t007_monitor -B spikes/t007_monitor/build -G "MinGW Makefiles" \
//     -DCMAKE_MAKE_PROGRAM=D:/MSYS2/mingw64/bin/mingw32-make.exe \
//     -DCMAKE_CXX_COMPILER=D:/MSYS2/mingw64/bin/g++.exe
//   cmake --build spikes/t007_monitor/build -j 8
//
// Run:
//   spike_t007_monitor --mode=enum
//   spike_t007_monitor --mode=window --hold-ms=1500
//   spike_t007_monitor --mode=move   --hold-ms=1500
//   spike_t007_monitor --mode=simulate-dpi --dpi=144 --hold-ms=1500

#include <windows.h>
#include <shellscalingapi.h>
#include <dwmapi.h>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

struct MonitorRow {
    HMONITOR handle = nullptr;
    std::wstring device;
    RECT monitor{};
    RECT work{};
    UINT effective_dpi = 0;
    UINT raw_dpi = 0;
    bool primary = false;
};

std::string ToUtf8(const wchar_t* text) {
    if (!text) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};
    std::string out(static_cast<std::size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), need, nullptr, nullptr);
    return out;
}

std::vector<MonitorRow> EnumerateMonitors() {
    std::vector<MonitorRow> rows;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR handle, HDC, LPRECT, LPARAM param) -> BOOL {
            auto* out = reinterpret_cast<std::vector<MonitorRow>*>(param);
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (!GetMonitorInfoW(handle, &info)) return TRUE;
            MonitorRow row;
            row.handle = handle;
            row.device = info.szDevice;
            row.monitor = info.rcMonitor;
            row.work = info.rcWork;
            row.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
            // MDT_EFFECTIVE_DPI = 0, MDT_RAW_DPI = 2.
            GetDpiForMonitor(handle, MDT_EFFECTIVE_DPI, &row.effective_dpi, &row.raw_dpi);
            UINT raw_x = 0, raw_y = 0;
            GetDpiForMonitor(handle, MDT_RAW_DPI, &raw_x, &raw_y);
            row.raw_dpi = raw_x;
            out->push_back(row);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&rows));
    return rows;
}

void PrintMonitors(const char* tag) {
    const auto rows = EnumerateMonitors();
    std::printf("[%s] monitors=%zu system_dpi=%u awareness=0x%llx\n", tag, rows.size(),
                GetDpiForSystem(),
                static_cast<unsigned long long>(
                    reinterpret_cast<std::uintptr_t>(GetThreadDpiAwarenessContext())));
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        std::printf(
            "  mon[%zu] dev=%s monitor=(%ld,%ld) %ldx%ld work=(%ld,%ld) %ldx%ld primary=%d "
            "effective_dpi=%u raw_dpi=%u\n",
            i, ToUtf8(r.device.c_str()).c_str(), r.monitor.left, r.monitor.top,
            r.monitor.right - r.monitor.left, r.monitor.bottom - r.monitor.top, r.work.left,
            r.work.top, r.work.right - r.work.left, r.work.bottom - r.work.top, r.primary ? 1 : 0,
            r.effective_dpi, r.raw_dpi);
    }
}

struct WindowState {
    HWND hwnd = nullptr;
    int dpi_changed_count = 0;
    UINT last_wparam_x = 0;
    UINT last_wparam_y = 0;
    RECT last_suggested{};
    bool got_wm_dpichanged = false;
    bool got_wm_dpichanged_before_create = false;
};

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        state = static_cast<WindowState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        if (state) state->hwnd = hwnd;
    }
    if (state) {
        if (msg == WM_DPICHANGED) {
            state->dpi_changed_count++;
            state->got_wm_dpichanged = true;
            // Documented split: LOWORD(wParam) = x DPI, HIWORD(wParam) = y DPI.
            state->last_wparam_x = LOWORD(wp);
            state->last_wparam_y = HIWORD(wp);
            if (lp) state->last_suggested = *reinterpret_cast<RECT*>(lp);
            std::printf("  WM_DPICHANGED #%d dpi_x=%u dpi_y=%u suggested=(%ld,%ld) %ldx%ld\n",
                        state->dpi_changed_count, state->last_wparam_x, state->last_wparam_y,
                        state->last_suggested.left, state->last_suggested.top,
                        state->last_suggested.right - state->last_suggested.left,
                        state->last_suggested.bottom - state->last_suggested.top);
            std::fflush(stdout);
            if (lp) {
                const RECT* suggested = reinterpret_cast<RECT*>(lp);
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        if (msg == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void PumpFor(int milliseconds) {
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(milliseconds);
    MSG msg{};
    while (GetTickCount64() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

HWND CreateTestWindow(HINSTANCE instance, WindowState& state, int width, int height) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DockForgeSpikeT007";
    RegisterClassExW(&wc);

    const HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName,
                                      L"spike t007", WS_POPUP, 100, 100, width, height, nullptr,
                                      nullptr, instance, &state);
    if (hwnd) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);
    }
    return hwnd;
}

int ModeEnum() {
    PrintMonitors("enum");
    // Same numbers from the "for DPI" variants, which is what layout code uses.
    std::printf("  GetSystemMetricsForDpi(SM_CXSCREEN, 96)=%d\n",
                GetSystemMetricsForDpi(SM_CXSCREEN, 96));
    std::printf("  GetSystemMetricsForDpi(SM_CXSCREEN, 144)=%d\n",
                GetSystemMetricsForDpi(SM_CXSCREEN, 144));
    std::printf("  GetSystemMetricsForDpi(SM_CXSCREEN, 192)=%d\n",
                GetSystemMetricsForDpi(SM_CXSCREEN, 192));
    return 0;
}

int ModeWindow(int hold_ms) {
    WindowState state;
    const HWND hwnd = CreateTestWindow(GetModuleHandleW(nullptr), state, 400, 120);
    if (!hwnd) {
        std::printf("CreateWindowExW failed: %lu\n", GetLastError());
        return 1;
    }
    PumpFor(200);
    const UINT dpi = GetDpiForWindow(hwnd);
    std::printf("window dpi=%d (GetDpiForWindow) scale=%.4f\n", dpi, dpi / 96.0);
    std::printf("WM_DPICHANGED on create: %s\n", state.got_wm_dpichanged ? "yes" : "no");

    RECT rc{};
    GetWindowRect(hwnd, &rc);
    std::printf("window rect=(%ld,%ld) %ldx%ld\n", rc.left, rc.top, rc.right - rc.left,
                rc.bottom - rc.top);

    // WS_POPUP has no non-client area, so AdjustWindowRectExForDpi should be a
    // no-op. Verified so layout code can skip it without guessing.
    RECT adj{0, 0, 400, 120};
    AdjustWindowRectExForDpi(&adj, WS_POPUP, FALSE, WS_EX_TOOLWINDOW, dpi);
    std::printf("AdjustWindowRectExForDpi(WS_POPUP) -> (%ld,%ld) %ldx%ld\n", adj.left, adj.top,
                adj.right - adj.left, adj.bottom - adj.top);

    PumpFor(hold_ms);
    DestroyWindow(hwnd);
    return 0;
}

int ModeMove(int hold_ms) {
    WindowState state;
    const HWND hwnd = CreateTestWindow(GetModuleHandleW(nullptr), state, 400, 120);
    if (!hwnd) return 1;
    PumpFor(200);

    const auto rows = EnumerateMonitors();
    std::printf("moving across %zu monitor(s)\n", rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        // Centre of that monitor's work area, physical pixels (PMv2 = no
        // virtualization, so these are the numbers the window manager uses).
        const int cx = r.work.left + (r.work.right - r.work.left) / 2 - 200;
        const int cy = r.work.bottom - 160;
        SetWindowPos(hwnd, HWND_TOPMOST, cx, cy, 400, 120, SWP_NOACTIVATE);
        PumpFor(300);
        RECT rc{};
        GetWindowRect(hwnd, &rc);
        std::printf("  mon[%zu] placed=(%d,%d) actual=(%ld,%ld) dpi=%u\n", i, cx, cy, rc.left,
                    rc.top, GetDpiForWindow(hwnd));
        std::fflush(stdout);
    }
    std::printf("total WM_DPICHANGED=%d\n", state.dpi_changed_count);
    PumpFor(hold_ms);
    DestroyWindow(hwnd);
    return 0;
}

int ModeSimulateDpi(int dpi, int hold_ms) {
    // There is only one monitor here, so a real DPI transition cannot be
    // triggered. What CAN be verified is the handler: send the message the
    // window manager would send and check the documented wParam split and the
    // suggested-rect application.
    WindowState state;
    const HWND hwnd = CreateTestWindow(GetModuleHandleW(nullptr), state, 400, 120);
    if (!hwnd) return 1;
    PumpFor(200);

    RECT current{};
    GetWindowRect(hwnd, &current);
    const int width = current.right - current.left;
    const int height = current.bottom - current.top;

    // Keep the window on screen after scaling; the real message carries a
    // rect that fits the new DPI's virtual desktop.
    const double scale = dpi / 96.0;
    RECT suggested{};
    suggested.left = current.left;
    suggested.top = current.top;
    suggested.right = current.left + static_cast<LONG>(width * scale);
    suggested.bottom = current.top + static_cast<LONG>(height * scale);

    std::printf("sending synthetic WM_DPICHANGED dpi=%d (real GetDpiForWindow stays %u)\n", dpi,
                GetDpiForWindow(hwnd));
    SendMessageW(hwnd, WM_DPICHANGED, MAKEWPARAM(static_cast<WORD>(dpi), static_cast<WORD>(dpi)),
                 reinterpret_cast<LPARAM>(&suggested));
    PumpFor(200);

    GetWindowRect(hwnd, &current);
    std::printf("after: count=%d dpi_x=%u dpi_y=%u rect=(%ld,%ld) %ldx%ld\n",
                state.dpi_changed_count, state.last_wparam_x, state.last_wparam_y, current.left,
                current.top, current.right - current.left, current.bottom - current.top);
    PumpFor(hold_ms);
    DestroyWindow(hwnd);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring mode = L"enum";
    int hold_ms = 800;
    int dpi = 144;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg.rfind(L"--mode=", 0) == 0) {
            mode = arg.substr(7);
        } else if (arg.rfind(L"--hold-ms=", 0) == 0) {
            hold_ms = _wtoi(arg.c_str() + 10);
        } else if (arg.rfind(L"--dpi=", 0) == 0) {
            dpi = _wtoi(arg.c_str() + 6);
        }
    }

    // Per-monitor v2 before any window exists: without it every coordinate is
    // virtualized and the whole spike measures the wrong thing.
    const BOOL aware =
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::printf("SetProcessDpiAwarenessContext(PMv2)=%d\n", aware ? 1 : 0);
    if (!aware) std::printf("  GetLastError=%lu (already set by a manifest?)\n", GetLastError());

    if (mode == L"enum") return ModeEnum();
    if (mode == L"window") return ModeWindow(hold_ms);
    if (mode == L"move") return ModeMove(hold_ms);
    if (mode == L"simulate-dpi") return ModeSimulateDpi(dpi, hold_ms);

    std::printf("unknown mode\n");
    return 2;
}
