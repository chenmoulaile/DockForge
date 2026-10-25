// DockForge T-069 spike: RegisterHotKey + MOD_NOREPEAT, conflict error codes,
// injected keystrokes reaching WM_HOTKEY, and the CPU cost of a 10Hz
// GetCursorPos poll loop.
//
// DoD questions answered by running this binary:
//   Q1. Does MinGW-w64 compile RegisterHotKey with MOD_NOREPEAT (0x4000) and
//       register it successfully from an unpackaged console process?
//   Q2. Does a second registration of the SAME combination fail with
//       GetLastError() == ERROR_HOTKEY_ALREADY_REGISTERED (1409)?
//   Q3. Do injected keystrokes (keybd_event, the same API the T-061 taskbar
//       probe used) trigger WM_HOTKEY in the registering process? This decides
//       whether the T-069 probe can drive the real app end to end.
//   Q4. What does a GetCursorPos + 10Hz timer loop cost in CPU, measured with
//       GetProcessTimes on this process? (C-24 allows exactly this shape; the
//       spike must show the number, not assert it is small.)
//
// Build (Git Bash, repo root):
//   g++ -std=c++17 -O2 -o spikes/T-069_hotkey_probe/hotkey_spike.exe \
//       spikes/T-069_hotkey_probe/hotkey_spike.cpp -luser32
#include <windows.h>

#include <cstdio>

namespace {

constexpr int kSpikeHotkeyId = 0x534B;  // 'SK' - spike
constexpr UINT kComboMods = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
constexpr UINT kComboVk = 'K';

int g_hotkey_count = 0;

void PrintCpu(const char* tag, FILETIME creation, FILETIME exit_, FILETIME kernel,
              FILETIME user) {
    FILETIME k1 = kernel, u1 = user;
    FILETIME k0, u0, dummy;
    GetProcessTimes(GetCurrentProcess(), &dummy, &dummy, &k0, &u0);
    (void)creation;
    (void)exit_;
    ULARGE_INTEGER uk0{}, uu0{}, uk1{}, uu1{};
    uk0.LowPart = k0.dwLowDateTime; uk0.HighPart = k0.dwHighDateTime;
    uu0.LowPart = u0.dwLowDateTime; uu0.HighPart = u0.dwHighDateTime;
    uk1.LowPart = k1.dwLowDateTime; uk1.HighPart = k1.dwHighDateTime;
    uu1.LowPart = u1.dwLowDateTime; uu1.HighPart = u1.dwHighDateTime;
    printf("  [%s] kernel=%llu us user=%llu us\n", tag,
           static_cast<unsigned long long>(uk0.QuadPart - uk1.QuadPart) * 10ULL,
           static_cast<unsigned long long>(uu0.QuadPart - uu1.QuadPart) * 10ULL);
}

}  // namespace

int main() {
    printf("T-069 hotkey spike (pid=%lu)\n", GetCurrentProcessId());

    // ---- Q1: register with MOD_NOREPEAT ----
    SetLastError(0);
    const BOOL ok1 = RegisterHotKey(nullptr, kSpikeHotkeyId, kComboMods, kComboVk);
    const DWORD err1 = GetLastError();
    printf("Q1 RegisterHotKey(Ctrl+Alt+K|MOD_NOREPEAT) -> %d err=%lu (expect ok=1 err=0)\n",
           ok1 ? 1 : 0, err1);

    // ---- Q2: same combination again must fail with 1409 ----
    SetLastError(0);
    const BOOL ok2 = RegisterHotKey(nullptr, kSpikeHotkeyId + 1, kComboMods, kComboVk);
    const DWORD err2 = GetLastError();
    printf("Q2 duplicate register -> %d err=%lu (expect ok=0 err=1409)\n", ok2 ? 1 : 0, err2);

    // ---- Q3: inject the keystrokes, pump for WM_HOTKEY ----
    // keybd_event order mirrors tools/taskbar_probe.ps1 (T-061 part C), which
    // already proved injected input reaches RegisterHotKey hotkeys; this
    // re-proves it for the MOD_NOREPEAT registration.
    keybd_event(VK_CONTROL, 0, 0, 0);
    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event('K', 0, 0, 0);
    keybd_event('K', 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);

    // ---- Q4: 10Hz GetCursorPos loop, CPU measured over 5 seconds ----
    FILETIME f_creation, f_exit, f_kernel0, f_user0;
    GetProcessTimes(GetCurrentProcess(), &f_creation, &f_exit, &f_kernel0, &f_user0);
    ULARGE_INTEGER uk0{}, uu0{};
    uk0.LowPart = f_kernel0.dwLowDateTime; uk0.HighPart = f_kernel0.dwHighDateTime;
    uu0.LowPart = f_user0.dwLowDateTime; uu0.HighPart = f_user0.dwHighDateTime;

    const ULONGLONG t0 = GetTickCount64();
    int polls = 0;
    POINT last{};
    while (GetTickCount64() - t0 < 5000) {
        // 100ms cadence = 10Hz, the C-24 ceiling. A waitable timer would be
        // cheaper than Sleep, but the dock integrates the poll into its frame
        // tick, which is MsgWaitForMultipleObjects-driven; Sleep is a fair
        // upper bound for the cost of the loop body itself.
        POINT pt{};
        GetCursorPos(&pt);
        if (pt.x != last.x || pt.y != last.y) last = pt;
        ++polls;
        MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_HOTKEY) {
                printf("Q3 WM_HOTKEY id=%Ix mods=%Ix vk=%Ix\n",
                       static_cast<size_t>(msg.wParam),
                       static_cast<size_t>(LOWORD(msg.lParam)),
                       static_cast<size_t>(HIWORD(msg.lParam)));
                ++g_hotkey_count;
            }
            if (msg.message == WM_QUIT) return 0;
        }
    }
    FILETIME f_kernel1, f_user1;
    GetProcessTimes(GetCurrentProcess(), &f_creation, &f_exit, &f_kernel1, &f_user1);
    ULARGE_INTEGER uk1{}, uu1{};
    uk1.LowPart = f_kernel1.dwLowDateTime; uk1.HighPart = f_kernel1.dwHighDateTime;
    uu1.LowPart = f_user1.dwLowDateTime; uu1.HighPart = f_user1.dwHighDateTime;
    const double cpu_us =
        static_cast<double>((uk1.QuadPart - uk0.QuadPart) + (uu1.QuadPart - uu0.QuadPart)) * 10.0;
    printf("Q4 polls=%d over %.1fs -> process CPU %.0f us total = %.3f%% of one core\n",
           polls, static_cast<double>(GetTickCount64() - t0) / 1000.0, cpu_us,
           cpu_us / (5000.0 * 1000.0) * 100.0);

    UnregisterHotKey(nullptr, kSpikeHotkeyId);
    if (!ok2) UnregisterHotKey(nullptr, kSpikeHotkeyId + 1);

    // ---- Q5: unregistered combination is free again ----
    SetLastError(0);
    const BOOL ok5 = RegisterHotKey(nullptr, kSpikeHotkeyId + 2, kComboMods, kComboVk);
    const DWORD err5 = GetLastError();
    printf("Q5 re-register after unregister -> %d err=%lu (expect ok=1: no residue)\n",
           ok5 ? 1 : 0, err5);
    if (ok5) UnregisterHotKey(nullptr, kSpikeHotkeyId + 2);

    printf("verdict: Q1=%s Q2=%s Q3=%s\n", ok1 ? "PASS" : "FAIL",
           (!ok2 && err2 == 1409) ? "PASS" : "FAIL", g_hotkey_count > 0 ? "PASS" : "FAIL");
    PrintCpu("final", f_creation, f_exit, f_kernel1, f_user1);
    return 0;
}
