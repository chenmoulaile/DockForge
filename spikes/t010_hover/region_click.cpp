// T-010: does a click in the dock's reserved band reach the window underneath?
//
// The dock reserves vertical space for magnified icons. That band is empty when
// idle, and it must not eat clicks aimed at whatever is behind it. This opens a
// victim window, puts the dock over it, and clicks at a set of points, reporting
// which window received each click.
//
// Both windows are topmost and belong to THIS process, so the layering is
// deterministic here - unlike spikes/t010_hover, where two topmost windows from
// separate processes were ordered unpredictably.
#include <windows.h>
#include <windowsx.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

struct Probe {
    int clicks = 0;
    POINT last{};
};

Probe g_victim;
Probe g_dock;

std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t buffer[512];
    va_list args;
    va_start(args, fmt);
    vswprintf_s(buffer, fmt, args);
    va_end(args);
    return buffer;
}

void Append(std::vector<std::wstring>& lines, const std::wstring& line) {
    lines.push_back(line);
    wprintf(L"%ls\n", line.c_str());
    fflush(stdout);
}

void WriteReport(const std::wstring& path, const std::vector<std::wstring>& lines) {
    if (path.empty()) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return;
    for (const auto& line : lines) {
        const std::string utf8 = [&] {
            const int need = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, nullptr, 0,
                                                 nullptr, nullptr);
            std::string out(static_cast<size_t>(need > 0 ? need - 1 : 0), '\0');
            if (need > 1) {
                WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, out.data(), need, nullptr,
                                    nullptr);
            }
            return out;
        }();
        fwrite(utf8.data(), 1, utf8.size(), file);
        fwrite("\r\n", 1, 2, file);
    }
    fclose(file);
}

// Magnified icon height for the dock config used by the main app: 48 DIP * 1.8.
constexpr int kIconDip = 48;
constexpr int kPaddingDip = 10;

LRESULT CALLBACK VictimProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_LBUTTONDOWN:
            ++g_victim.clicks;
            g_victim.last = POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            return 0;
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_DESTROY:
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK DockProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_LBUTTONDOWN:
            ++g_dock.clicks;
            g_dock.last = POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            return 0;
        case WM_DESTROY:
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterClassOnce(const wchar_t* name, WNDPROC proc) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = name;
    RegisterClassExW(&wc);
}

// SendInput posts to the thread queue. Without a pump the window procedures never
// run, so every probe would report "none" and the test would look like a pass-through
// success for the wrong reason.
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
// Clicks at a screen point and reports which of the two windows took it.
void ClickAt(std::vector<std::wstring>& lines, HWND dock, HWND victim, int x, int y,
             const wchar_t* label) {
    const int before_dock = g_dock.clicks;
    const int before_victim = g_victim.clicks;

    SetCursorPos(x, y);
    PumpFor(80);

    INPUT input[2]{};
    input[0].type = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    input[1].type = INPUT_MOUSE;
    input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, input, sizeof(INPUT));
    PumpFor(150);

    const int dockDelta = g_dock.clicks - before_dock;
    const int victimDelta = g_victim.clicks - before_victim;
    const wchar_t* taker = dockDelta > 0 ? L"dock" : (victimDelta > 0 ? L"victim" : L"none");
    Append(lines,
           Format(L"click %-22ls at (%d,%d) -> %ls (dock+%d victim+%d)", label, x, y, taker,
                  dockDelta, victimDelta));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // Without this the client coordinates and the window rect disagree at any DPI
    // above 96, and every click lands somewhere unintended.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::wstring report;
    int hold_ms = 4000;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg.rfind(L"--out=", 0) == 0) {
            report = arg.substr(6);
        } else if (arg.rfind(L"--hold-ms=", 0) == 0) {
            hold_ms = _wtoi(arg.c_str() + 10);
        }
    }

    std::vector<std::wstring> lines;

    const UINT dpi = GetDpiForSystem();
    const double scale = dpi / 96.0;
    const int icon_px = static_cast<int>(kIconDip * scale);
    const int padding_px = static_cast<int>(kPaddingDip * scale);
    // Same geometry as the real dock: 12 tiles, 8 DIP gaps, 1.8x magnification.
    const int tiles = 12;
    const int gap_px = static_cast<int>(8 * scale);
    const int content_px = tiles * icon_px + (tiles - 1) * gap_px;
    const int plate_height_px = icon_px + 2 * padding_px;
    const int window_height_px =
        static_cast<int>(icon_px * 1.8) + 2 * padding_px;
    const int window_width_px = content_px + 2 * padding_px + 64;

    Append(lines, Format(L"dpi=%u scale=%.2f", dpi, scale));
    Append(lines, Format(L"window=%dx%d plate_height=%d", window_width_px, window_height_px,
                          plate_height_px));

    RegisterClassOnce(L"T010Victim", VictimProc);
    RegisterClassOnce(L"T010Dock", DockProc);

    const int screen_w = GetSystemMetrics(SM_CXSCREEN);
    const int screen_h = GetSystemMetrics(SM_CYSCREEN);
    const int x = (screen_w - window_width_px) / 2;
    const int y = screen_h - window_height_px - 60;

    HWND victim = CreateWindowExW(WS_EX_TOPMOST, L"T010Victim", L"victim", WS_POPUP,
                                  x - 120, y - 120, window_width_px + 240,
                                  window_height_px + 240, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    if (!victim) {
        Append(lines, L"victim window creation failed");
        WriteReport(report, lines);
        return 1;
    }
    ShowWindow(victim, SW_SHOWNOACTIVATE);
    UpdateWindow(victim);

    HWND dock = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                L"T010Dock", L"dock", WS_POPUP, x, y, window_width_px,
                                window_height_px, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!dock) {
        Append(lines, L"dock window creation failed");
        WriteReport(report, lines);
        return 1;
    }

    // Idle region: the plate only. The band above it is excluded.
    const int plate_top = window_height_px - plate_height_px;
    const int radius = static_cast<int>(22 * scale);
    HRGN plate = CreateRoundRectRgn(0, plate_top, window_width_px + 1,
                                    window_height_px + 1, radius * 2, radius * 2);
    SetWindowRgn(dock, plate, TRUE);
    ShowWindow(dock, SW_SHOWNOACTIVATE);
    UpdateWindow(dock);

    // Make the layering explicit instead of trusting creation order: two topmost
    // windows are NOT ordered by creation (spikes/t010_hover question 4).
    SetWindowPos(dock, HWND_TOPMOST, x, y, window_width_px, window_height_px,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowPos(victim, HWND_TOPMOST, x - 120, y - 120, window_width_px + 240,
                 window_height_px + 240, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    // Raising the victim above the dock would break the test; the dock must be on
    // top for "did the click pass through" to mean anything. HWND_TOPMOST on the
    // dock now puts it above the victim because it was just raised.
    SetWindowPos(dock, HWND_TOPMOST, x, y, window_width_px, window_height_px,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    Sleep(300);

    const int mid_x = x + window_width_px / 2;
    // Inside the plate: the dock must take the click.
    ClickAt(lines, dock, victim, mid_x, y + window_height_px - padding_px - icon_px / 2,
            L"inside plate");
    // In the reserved band above the plate: the victim must take it.
    ClickAt(lines, dock, victim, mid_x, y + plate_top / 2, L"reserved band");
    // Just above the window: victim.
    ClickAt(lines, dock, victim, mid_x, y - 20, L"above window");

    Append(lines, Format(L"total dock_clicks=%d victim_clicks=%d", g_dock.clicks,
                          g_victim.clicks));
    const bool inside_ok = g_dock.clicks >= 1;
    Append(lines, Format(L"verdict=%ls",
                          inside_ok && g_victim.clicks >= 1 ? L"PASS_THROUGH_OK"
                                                            : L"CLICK_SWALLOWED"));

    Sleep(static_cast<DWORD>(hold_ms > 0 ? hold_ms : 0));

    DestroyWindow(dock);
    DestroyWindow(victim);
    WriteReport(report, lines);
    return 0;
}
