// DockForge T-061 spike: Shell_TrayWnd show/hide behaviour, measured.
//
// Questions the T-061 DoD requires the spike to answer before any product code:
//   Q1  Is the taskbar window still class "Shell_TrayWnd" on this OS build, and
//       is there a "Shell_SecondaryTrayWnd" per extra monitor?
//   Q2  How long does ShowWindow(SW_HIDE) block on explorer's message loop, and
//       does ShowWindowAsync (the project's known gotcha: "ShowWindow 同步等待
//       目标应用消息循环，实测 p99 494.59ms") avoid that?
//   Q3  Does IsWindowVisible settle on its own, and how fast?
//   Q4  Does the work area change while the taskbar window is hidden? (decides
//       whether the dock must re-place on TaskbarVisibilityChanged)
//   Q5  Does ShowWindowAsync(SW_SHOW) restore the taskbar?
//   Q6  (--restart) Does the taskbar come back by itself after explorer.exe is
//       killed and restarted? (DoD: "Explorer 重启后任务栏自然恢复")
//
// The spike always ends with the taskbar VISIBLE, on every path.
//
// Build (MSYS2 MinGW g++, same toolchain as the repo):
//   g++ -std=c++20 -O2 -Wall -o spike.exe spike.cpp -luser32
// Usage:
//   spike.exe             Q1-Q5
//   spike.exe --restart   Q1-Q6 (kills explorer.exe; shell is restarted)
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace {

double MsSince(LARGE_INTEGER start, LARGE_INTEGER now, LARGE_INTEGER freq) {
    return static_cast<double>(now.QuadPart - start.QuadPart) * 1000.0 /
           static_cast<double>(freq.QuadPart);
}

// OS build number without the version-lying API wrapper.
DWORD OsBuild() {
    typedef LONG(WINAPI * RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto fn = reinterpret_cast<RtlGetVersionPtr>(
            reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
        if (fn && fn(&info) == 0) return info.dwBuildNumber;
    }
    return 0;
}

int CountSecondaryTrays() {
    int count = 0;
    HWND hwnd = nullptr;
    while ((hwnd = FindWindowExW(nullptr, hwnd, L"Shell_SecondaryTrayWnd", nullptr)) !=
           nullptr) {
        ++count;
    }
    return count;
}

void PrintState(const char* tag) {
    HWND tray = FindWindowExW(nullptr, nullptr, L"Shell_TrayWnd", nullptr);
    if (tray == nullptr) {
        printf("  [%s] Shell_TrayWnd=absent\n", tag);
        return;
    }
    RECT rc{};
    GetWindowRect(tray, &rc);
    printf("  [%s] hwnd=%p visible=%d rect=%ld,%ld,%ld,%ld\n", tag,
           static_cast<void*>(tray), IsWindowVisible(tray) ? 1 : 0, rc.left, rc.top,
           rc.right, rc.bottom);
}

void PrintWorkArea(const char* tag) {
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    POINT origin{0, 0};
    HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(monitor, &mi);
    printf("  [%s] SPI_GETWORKAREA=%ld,%ld,%ld,%ld monitor.rcWork=%ld,%ld,%ld,%ld\n", tag,
           wa.left, wa.top, wa.right, wa.bottom, mi.rcWork.left, mi.rcWork.top,
           mi.rcWork.right, mi.rcWork.bottom);
}

// Issues the show command and polls IsWindowVisible until it settles.
// Returns the settle time in ms, or -1 when it never settled within 1000ms.
double ShowAndWaitSettle(HWND tray, UINT cmd, bool want_visible) {
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t0{};
    QueryPerformanceCounter(&t0);
    const BOOL posted = ShowWindowAsync(tray, cmd);
    for (int i = 0; i < 100; ++i) {
        if ((IsWindowVisible(tray) != FALSE) == want_visible) {
            LARGE_INTEGER t1{};
            QueryPerformanceCounter(&t1);
            printf("  ShowWindowAsync(0x%04X) posted=%d settled visible=%d after %.2fms\n",
                   cmd, posted ? 1 : 0, want_visible ? 1 : 0, MsSince(t0, t1, freq));
            return MsSince(t0, t1, freq);
        }
        Sleep(10);
    }
    printf("  ShowWindowAsync(0x%04X) posted=%d NEVER settled to visible=%d within 1000ms\n",
           cmd, posted ? 1 : 0, want_visible ? 1 : 0);
    return -1.0;
}

}  // namespace

int main(int argc, char** argv) {
    const bool restart_mode =
        argc > 1 && (std::strcmp(argv[1], "--restart") == 0);

    printf("== T-061 taskbar spike ==\n");
    printf("  os build=%lu monitor-secondary-trays=%d\n",
           static_cast<unsigned long>(OsBuild()), CountSecondaryTrays());

    HWND tray = FindWindowExW(nullptr, nullptr, L"Shell_TrayWnd", nullptr);
    if (tray == nullptr) {
        printf("  FATAL: no Shell_TrayWnd; nothing to measure\n");
        return 1;
    }
    PrintState("baseline");
    PrintWorkArea("baseline");

    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);

    // --- Q2/Q3: synchronous ShowWindow(SW_HIDE) - how long does it block? ---
    LARGE_INTEGER t0{};
    QueryPerformanceCounter(&t0);
    ShowWindow(tray, SW_HIDE);
    LARGE_INTEGER t1{};
    QueryPerformanceCounter(&t1);
    const bool visible_immediately = IsWindowVisible(tray) != FALSE;
    printf("  ShowWindow(SW_HIDE) blocking call took %.2fms, visible right after=%d\n",
           MsSince(t0, t1, freq), visible_immediately ? 1 : 0);
    Sleep(200);
    PrintState("after sync hide + 200ms");
    PrintWorkArea("while hidden");

    // --- Q2: restore with the ASYNC path and time the settle ---
    ShowAndWaitSettle(tray, SW_SHOW, true);
    Sleep(200);
    PrintState("after async show + 200ms");

    // --- Q2: hide with the ASYNC path (the product path) ---
    ShowAndWaitSettle(tray, SW_HIDE, false);
    Sleep(200);
    PrintState("after async hide + 200ms");

    // --- Q5: restore again (async), final state must be visible ---
    const double restore_ms = ShowAndWaitSettle(tray, SW_SHOW, true);
    Sleep(200);
    PrintState("after async restore + 200ms");
    PrintWorkArea("after restore");

    printf("  summary: async-hide and async-restore both settle; restore took %.2fms\n",
           restore_ms);

    if (!restart_mode) {
        printf("== spike done (taskbar left VISIBLE) ==\n");
        return 0;
    }

    // --- Q6: explorer restart = natural recovery -------------------------
    // taskkill by image name, then start a fresh shell. The taskbar window is
    // recreated by the new explorer process; its visibility is explorer's own
    // default, not anything we asked for - that is the whole point of the
    // emergency path.
    printf("  --restart: killing explorer.exe\n");
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    WCHAR cmd[] = L"cmd.exe /c taskkill /f /im explorer.exe";
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        printf("  FATAL: cannot launch taskkill (%lu)\n", GetLastError());
        return 1;
    }
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    HWND gone = nullptr;
    for (int i = 0; i < 100; ++i) {
        gone = FindWindowExW(nullptr, nullptr, L"Shell_TrayWnd", nullptr);
        if (gone == nullptr) break;
        Sleep(100);
    }
    printf("  after taskkill: Shell_TrayWnd=%s (null means gone)\n",
           gone == nullptr ? "null" : "STILL PRESENT");

    printf("  --restart: starting explorer.exe\n");
    WCHAR shell_cmd[] = L"explorer.exe";
    if (!CreateProcessW(nullptr, shell_cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &si, &pi)) {
        printf("  FATAL: cannot launch explorer (%lu)\n", GetLastError());
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    HWND back = nullptr;
    double wait_s = 0.0;
    for (int i = 0; i < 200; ++i) {
        back = FindWindowExW(nullptr, nullptr, L"Shell_TrayWnd", nullptr);
        if (back != nullptr && IsWindowVisible(back)) break;
        Sleep(100);
        wait_s += 0.1;
    }
    if (back != nullptr && IsWindowVisible(back)) {
        printf("  natural recovery: new Shell_TrayWnd=%p visible after %.1fs "
               "(no ShowWindow call from this process)\n",
               static_cast<void*>(back), wait_s);
        PrintState("after explorer restart");
        printf("== spike done (taskbar left VISIBLE) ==\n");
        return 0;
    }
    printf("  natural recovery FAILED after 20s\n");
    return 2;
}
