// T-304 spike: probe the two brand-new control-centre write paths on THIS machine.
//
//  1. DDC/CI brightness (dxva2): GetPhysicalMonitorsFromHMONITOR ->
//     GetMonitorBrightness -> SetMonitorBrightness round-trip. The ask says the
//     whole repo has zero prior brightness/DDC code, so nothing is assumed: the
//     spike decides whether the panel row is a live slider or an in-panel hint.
//     MinGW ships the declarations (physicalmonitorenumerationapi.h +
//     highlevelmonitorconfigurationapi.h) and libdxva2.a, so the calls link
//     directly - the spike proves that linking path too.
//
//  2. Registry levers for 勿扰 / 夜览:
//     - 勿扰 proxy: PushNotifications\ToastNotificationEnabled and
//       Notifications\Settings\NOC_GLOBAL_SETTING_TOASTS_ENABLED (both are the
//       documented master switches behind the Settings > Notifications page).
//     - 夜览: CloudStore key default$windows.data.bluelightreduction.bluelightreductionstate
//       (the state blob public toggle tools flip) - dump its header bytes to see
//       whether the documented enable byte exists on this build.
//     Each write round-trips and reads back; a failed write is reported, not faked.
#include <windows.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <cstdio>

int main() {
    printf("=== T-304 DDC/CI + registry spike ===\n");

    // ---- 1. DDC/CI ------------------------------------------------------------
    {
        HMONITOR hmon = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
        DWORD count = 0;
        const BOOL n_ok = GetNumberOfPhysicalMonitorsFromHMONITOR(hmon, &count);
        printf("ddc: GetNumberOfPhysicalMonitorsFromHMONITOR ok=%d count=%lu gle=%lu\n", n_ok,
               count, GetLastError());
        if (n_ok && count > 0) {
            PHYSICAL_MONITOR* mons = new PHYSICAL_MONITOR[count]{};
            if (GetPhysicalMonitorsFromHMONITOR(hmon, count, mons)) {
                for (DWORD i = 0; i < count; ++i) {
                    wprintf(L"ddc: monitor[%lu] \"%s\" handle=%p\n", i,
                            mons[i].szPhysicalMonitorDescription, mons[i].hPhysicalMonitor);
                    DWORD mcaps = 0, mctypes = 0;
                    const BOOL cok =
                        GetMonitorCapabilities(mons[i].hPhysicalMonitor, &mcaps, &mctypes);
                    printf("  caps ok=%d caps=0x%08lX (brightness-cap bit 0x2=%d) temp=%lu gle=%lu\n",
                           cok, mcaps, (mcaps & MC_CAPS_BRIGHTNESS) ? 1 : 0, mctypes,
                           GetLastError());
                    DWORD lo = 0, cur = 0, hi = 0;
                    const BOOL gok = GetMonitorBrightness(mons[i].hPhysicalMonitor, &lo, &cur, &hi);
                    printf("  GetMonitorBrightness ok=%d lo=%lu cur=%lu hi=%lu gle=%lu\n", gok,
                           lo, cur, hi, GetLastError());
                    if (gok && hi > lo) {
                        // Round-trip: set current+10 (clamped), read back, restore.
                        const DWORD target = (cur + 10 < hi) ? cur + 10 : hi;
                        const BOOL sok = SetMonitorBrightness(mons[i].hPhysicalMonitor, target);
                        Sleep(400);
                        DWORD cur2 = 0;
                        GetMonitorBrightness(mons[i].hPhysicalMonitor, &lo, &cur2, &hi);
                        printf("  SetMonitorBrightness(%lu) ok=%d readback=%lu -> %s\n", target,
                               sok, cur2,
                               (sok && cur2 != cur)
                                   ? "LIVE WRITE CONFIRMED"
                                   : (sok ? "accepted, value unchanged (may be clamped)"
                                          : "FAILED"));
                        if (cur2 != cur) {
                            SetMonitorBrightness(mons[i].hPhysicalMonitor, cur);  // restore
                            Sleep(300);
                            DWORD cur3 = 0;
                            GetMonitorBrightness(mons[i].hPhysicalMonitor, &lo, &cur3, &hi);
                            printf("  restore to %lu -> %lu\n", cur, cur3);
                        }
                    }
                }
                DestroyPhysicalMonitors(count, mons);
            } else {
                printf("ddc: GetPhysicalMonitorsFromHMONITOR FAILED gle=%lu\n", GetLastError());
            }
            delete[] mons;
        }
    }

    // ---- 2. registry levers -----------------------------------------------------
    printf("\nregistry:\n");
    auto dump_dword = [](const wchar_t* label, HKEY root, const wchar_t* path,
                         const char* value) {
        HKEY k = nullptr;
        const LONG open_r = RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k);
        if (open_r != ERROR_SUCCESS) {
            wprintf(L"  %s: OPEN FAILED %ld (%s)\n", label, open_r, path);
            return;
        }
        DWORD v = 0, size = sizeof(v), type = 0;
        wchar_t wname[128];
        MultiByteToWideChar(CP_UTF8, 0, value, -1, wname, 128);
        const LONG r = RegQueryValueExW(k, wname, nullptr, &type, (LPBYTE)&v, &size);
        wprintf(L"  %s: read r=%ld type=%lu value=%lu\n", label, r, type,
                r == ERROR_SUCCESS && type == REG_DWORD ? v : 0xFFFFFFFFul);
        // Round-trip a benign write: write the SAME value back to prove KEY_SET_VALUE.
        if (r == ERROR_SUCCESS && type == REG_DWORD) {
            const LONG wr = RegSetValueExW(k, wname, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
            wprintf(L"  %s: same-value rewrite r=%ld -> %s\n", label, wr,
                    wr == ERROR_SUCCESS ? L"WRITE OK" : L"WRITE DENIED");
        }
        RegCloseKey(k);
    };
    dump_dword(L"toast-enabled", HKEY_CURRENT_USER,
               L"Software\\Microsoft\\Windows\\CurrentVersion\\PushNotifications",
               "ToastNotificationEnabled");
    dump_dword(L"NOC_GLOBAL_TOASTS", HKEY_CURRENT_USER,
               L"Software\\Microsoft\\Windows\\CurrentVersion\\Notifications\\Settings",
               "NOC_GLOBAL_SETTING_TOASTS_ENABLED");

    // ---- 3. night light CloudStore blob ----------------------------------------
    {
        const wchar_t* path =
            L"Software\\Microsoft\\Windows\\CurrentVersion\\CloudStore\\Store\\DefaultAccount"
            L"\\Current\\default$windows.data.bluelightreduction.bluelightreductionstate\\Current";
        HKEY k = nullptr;
        const LONG r = RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k);
        printf("  nightlight-state key: r=%ld\n", r);
        if (r == ERROR_SUCCESS) {
            DWORD type = 0, size = 0;
            const LONG q = RegQueryValueExW(k, L"Data", nullptr, &type, nullptr, &size);
            printf("  Data query: r=%ld type=%lu size=%lu\n", q, type, size);
            if (q == ERROR_SUCCESS && type == REG_BINARY && size > 32) {
                BYTE* blob = new BYTE[size];
                RegQueryValueExW(k, L"Data", nullptr, &type, blob, &size);
                printf("  Data head: ");
                for (DWORD i = 0; i < 40 && i < size; ++i) printf("%02X ", blob[i]);
                printf("\n  (documented enable byte: offset 0x12 = 0x0F off / 0x13 on)\n");
                delete[] blob;
            }
            RegCloseKey(k);
        }
    }
    {
        const wchar_t* path =
            L"Software\\Microsoft\\Windows\\CurrentVersion\\CloudStore\\Store\\DefaultAccount"
            L"\\Current\\default$windows.data.bluelightreduction.settings\\Current";
        HKEY k = nullptr;
        const LONG r = RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_QUERY_VALUE, &k);
        printf("  nightlight-settings key: r=%ld\n", r);
        if (r == ERROR_SUCCESS) RegCloseKey(k);
    }

    // ---- 4. WM_SETTINGCHANGE broadcast reachability ------------------------------
    {
        const LONG r = SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                                           (LPARAM)L"Notifications", SMTO_ABORTIFHUNG, 500,
                                           nullptr);
        printf("\nWM_SETTINGCHANGE(\"Notifications\") broadcast: r=%ld (nonzero=delivered)\n",
               r);
    }
    return 0;
}
