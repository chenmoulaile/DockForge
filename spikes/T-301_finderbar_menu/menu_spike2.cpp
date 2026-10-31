// DockForge T-301 spike, part 2: the two questions that decide whether a top
// Finder Bar is usable at all.
//
// Q7. Does a topmost strip at y=0..h swallow clicks?
//     Part 1 measured WS_EX_TRANSPARENT having NO effect on WindowFromPoint.
//     This run re-measures that with DPI awareness ON (part 1 was virtualized:
//     GetSystemMetrics said 1440 wide on a 2880-wide desktop), and adds the
//     layered variants.
//
// Q8. Can the bar reserve the top of the work area (SPI_SETWORKAREA) so that
//     maximized windows stop at the bar's bottom edge instead of sliding under
//     it? This is the decisive question: a 28 DIP bar that covers the top of
//     every maximized window's title bar is worse than no bar, and macOS solves
//     it by reserving the space. Measured here:
//       a) does SPI_SETWORKAREA move the work area at all,
//       b) does a window created maximized AFTER the change respect it,
//       c) does an ALREADY maximized window move when the work area changes,
//       d) does the change outlive this process (read from a child process),
//       e) does restoring put the desktop back exactly.
//
// Every step restores the work area it found, including on the error paths.
//
// Build:
//   g++ -std=c++17 -O2 -municode -o spikes/T-301_finderbar_menu/menu_spike2.exe \
//       spikes/T-301_finderbar_menu/menu_spike2.cpp -luser32
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

void EnableDpiAwareness() {
    // -4 = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2. Without this every
    // coordinate below is virtualized and the measurements are wrong by the
    // scale factor (STATE.json known_gotchas).
    using Fn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto fn = reinterpret_cast<Fn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (fn != nullptr && fn(reinterpret_cast<DPI_AWARENESS_CONTEXT>(-4)) != FALSE) return;
    }
    SetProcessDPIAware();
}

constexpr wchar_t kClass[] = L"DockForgeSpike2Window";

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
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
    const DWORD n = GetModuleFileNameW(nullptr, path,
                                       static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    return std::wstring(path, n);
}

std::wstring ExeDir() {
    const std::wstring full = ExePath();
    const std::size_t slash = full.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : full.substr(0, slash);
}

RECT WorkArea() {
    RECT r{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);
    return r;
}

bool SetWorkArea(const RECT& r) {
    return SystemParametersInfoW(SPI_SETWORKAREA, 0, const_cast<RECT*>(&r),
                                 SPIF_UPDATEINIFILE | SPIF_SENDCHANGE) != FALSE;
}

void PrintRect(const char* tag, const RECT& r) {
    std::printf("  %s = %ld,%ld,%ld,%ld (%ldx%ld)\n", tag, r.left, r.top, r.right, r.bottom,
                r.right - r.left, r.bottom - r.top);
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), need,
                        nullptr, nullptr);
    return out;
}

// Child mode: report the work area a FRESH process sees, so the parent can prove
// the SPI_SETWORKAREA change is desktop-global and outlives the writer.
int RunChild() {
    EnableDpiAwareness();
    const RECT r = WorkArea();
    const std::wstring path = ExeDir() + L"\\spike_workarea_child.txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") == 0 && f != nullptr) {
        std::fprintf(f, "%ld,%ld,%ld,%ld\n", r.left, r.top, r.right, r.bottom);
        std::fclose(f);
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring(argv[1]) == L"--child") return RunChild();

    EnableDpiAwareness();
    std::printf("T-301 finder bar spike part 2 (pid=%lu)\n",
                static_cast<unsigned long>(GetCurrentProcessId()));

    const int screen_w = GetSystemMetrics(SM_CXSCREEN);
    const int screen_h = GetSystemMetrics(SM_CYSCREEN);
    // GetDeviceCaps(LOGPIXELSX) on the screen DC is the system DPI. It is exact
    // for the primary display once the process is per-monitor aware, and unlike
    // GetDpiForSystem it needs no _WIN32_WINNT gate in this standalone spike.
    const HDC screen_dc = GetDC(nullptr);
    const UINT dpi = static_cast<UINT>(GetDeviceCaps(screen_dc, LOGPIXELSX));
    ReleaseDC(nullptr, screen_dc);
    std::printf("screen=%dx%d system_dpi=%u\n", screen_w, screen_h, dpi);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    const int bar_px = MulDiv(28, static_cast<int>(dpi), 96);

    // ---- Q7: hit testing of a top strip --------------------------------
    std::printf("\nQ7 top strip hit testing (DPI aware)\n");
    std::printf("  bar height = 28 DIP = %d px at %u dpi\n", bar_px, dpi);

    struct Variant {
        const char* name;
        DWORD ex_style;
        bool layered_alpha;
    };
    const Variant variants[] = {
        {"plain TOPMOST", WS_EX_TOPMOST, false},
        {"+TRANSPARENT", WS_EX_TOPMOST | WS_EX_TRANSPARENT, false},
        {"+LAYERED|TRANSPARENT a=255", WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED, true},
    };
    for (const Variant& v : variants) {
        HWND strip = CreateWindowExW(v.ex_style, kClass, L"DockForge Spike Strip", WS_POPUP, 0, 0,
                                     screen_w, bar_px, nullptr, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
        if (strip == nullptr) {
            std::printf("  [%s] CreateWindowExW failed err=%lu\n", v.name,
                        static_cast<unsigned long>(GetLastError()));
            continue;
        }
        if (v.layered_alpha) {
            const BOOL ok = SetLayeredWindowAttributes(strip, 0, 255, LWA_ALPHA);
            std::printf("  [%s] SetLayeredWindowAttributes=%d err=%lu\n", v.name, ok ? 1 : 0,
                        static_cast<unsigned long>(GetLastError()));
        }
        SetWindowPos(strip, HWND_TOPMOST, 0, 0, screen_w, bar_px, SWP_SHOWWINDOW);
        PumpFor(120);
        const POINT centre{screen_w / 2, bar_px / 2};
        const POINT left{40, bar_px / 2};
        const POINT below{screen_w / 2, bar_px + 60};
        const HWND hit_centre = WindowFromPoint(centre);
        const HWND hit_left = WindowFromPoint(left);
        const HWND hit_below = WindowFromPoint(below);
        std::printf("  [%s] visible=%d centre_hit_strip=%d left_hit_strip=%d below_hit_strip=%d\n",
                    v.name, IsWindowVisible(strip) ? 1 : 0, hit_centre == strip ? 1 : 0,
                    hit_left == strip ? 1 : 0, hit_below == strip ? 1 : 0);
        DestroyWindow(strip);
    }

    // ---- Q8: work area reservation -------------------------------------
    std::printf("\nQ8 work area reservation\n");
    const RECT original = WorkArea();
    PrintRect("original work area", original);

    // (c) A window maximized BEFORE the change: does it move afterwards?
    HWND pre = CreateWindowExW(0, kClass, L"DockForge Spike PreMax", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 400, 300, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    ShowWindow(pre, SW_SHOWMAXIMIZED);
    PumpFor(200);
    RECT pre_rect{};
    GetWindowRect(pre, &pre_rect);
    PrintRect("pre-maximized window rect", pre_rect);

    // Emergency recovery: the original rectangle is on disk BEFORE the change, so
    // a crash here is repairable with one SystemParametersInfo call from any
    // script instead of a logout. tools/workarea_restore.ps1 reads this file.
    {
        const std::wstring backup = ExeDir() + L"\\workarea_original.txt";
        FILE* f = nullptr;
        if (_wfopen_s(&f, backup.c_str(), L"w") == 0 && f != nullptr) {
            std::fprintf(f, "%ld,%ld,%ld,%ld\n", original.left, original.top, original.right,
                         original.bottom);
            std::fclose(f);
        }
    }

    RECT reserved = original;
    reserved.top += bar_px;
    const bool set_ok = SetWorkArea(reserved);
    PumpFor(250);
    const RECT after_set = WorkArea();
    std::printf("  SPI_SETWORKAREA(top+=%d) -> %d\n", bar_px, set_ok ? 1 : 0);
    PrintRect("work area after set", after_set);

    GetWindowRect(pre, &pre_rect);
    PrintRect("pre-maximized window rect after change", pre_rect);

    // (b) A window maximized AFTER the change.
    HWND post = CreateWindowExW(0, kClass, L"DockForge Spike PostMax", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 400, 300, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    ShowWindow(post, SW_SHOWMAXIMIZED);
    PumpFor(200);
    RECT post_rect{};
    GetWindowRect(post, &post_rect);
    PrintRect("post-maximized window rect", post_rect);
    std::printf("  post-max top == reserved.top ? %d (delta=%ld)\n",
                post_rect.top == reserved.top ? 1 : 0, post_rect.top - reserved.top);

    // (d) Does the change outlive this process? Ask a fresh child.
    const std::wstring child_out = ExeDir() + L"\\spike_workarea_child.txt";
    DeleteFileW(child_out.c_str());
    std::wstring cmd = std::wstring(L"\"") + ExePath() + L"\" --child";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');
    if (CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                       &pi) != FALSE) {
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        FILE* f = nullptr;
        if (_wfopen_s(&f, child_out.c_str(), L"r") == 0 && f != nullptr) {
            char line[128] = {};
            if (std::fgets(line, sizeof(line), f) != nullptr) {
                std::printf("  child process sees work area = %s", line);
            }
            std::fclose(f);
        }
    }

    // (e) Restore and prove the restore.
    const bool restore_ok = SetWorkArea(original);
    PumpFor(250);
    const RECT restored = WorkArea();
    std::printf("  restore -> %d\n", restore_ok ? 1 : 0);
    PrintRect("work area after restore", restored);
    std::printf("  restored == original ? %d\n",
                (restored.left == original.left && restored.top == original.top &&
                 restored.right == original.right && restored.bottom == original.bottom)
                    ? 1
                    : 0);
    std::printf("  original top was %ld (taskbar %s)\n", original.top,
                original.top > 0 ? "at top" : "not at top");

    DestroyWindow(post);
    DestroyWindow(pre);

    std::printf("\nverdict written above; work area left as found.\n");
    return 0;
}
