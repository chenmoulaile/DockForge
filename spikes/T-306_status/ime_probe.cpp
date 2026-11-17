// T-306 spike: verify WM_IME_CONTROL IMC_GETCONVERSIONMODE / IMC_SETCONVERSIONMODE
// work cross-process on this machine (Win11, codepage 936, Chinese IME present).
// imm.h (MinGW) lacks the conversion-mode constants; immdev.h (Windows SDK,
// tpn/winsdk-10 mirror of Microsoft headers, line 994) pins SETCONVERSIONMODE=0x0002;
// GETCONVERSIONMODE=0x0001 follows the GET/SET alternation (GETSENTENCEMODE=0x0003
// before SETSENTENCEMODE=0x0004, GETOPENSTATUS=0x0005 before SETOPENSTATUS=0x0006).
#include <windows.h>
#include <imm.h>
#include <cstdio>

#ifndef IMC_GETCONVERSIONMODE
#define IMC_GETCONVERSIONMODE 0x0001
#endif
#ifndef IMC_SETCONVERSIONMODE
#define IMC_SETCONVERSIONMODE 0x0002
#endif

int main() {
    HWND fg = GetForegroundWindow();
    wchar_t title[128]{};
    GetWindowTextW(fg, title, 128);
    wprintf(L"foreground: %s hwnd=%p\n", title, fg);
    HWND ime = ImmGetDefaultIMEWnd(fg);
    wprintf(L"default IME window: %p\n", ime);
    if (!ime) return 1;
    DWORD_PTR mode = 0, open = 0, sent_mode = 0;
    DWORD r1 = SendMessageTimeoutW(ime, WM_IME_CONTROL, IMC_GETCONVERSIONMODE, 0,
                                   SMTO_ABORTIFHUNG, 500, &mode);
    DWORD r2 = SendMessageTimeoutW(ime, WM_IME_CONTROL, 0x0003 /*GETSENTENCEMODE*/, 0,
                                   SMTO_ABORTIFHUNG, 500, &sent_mode);
    DWORD r3 = SendMessageTimeoutW(ime, WM_IME_CONTROL, 0x0005 /*GETOPENSTATUS*/, 0,
                                   SMTO_ABORTIFHUNG, 500, &open);
    wprintf(L"GETCONVERSIONMODE sent=%lu mode=0x%llX (native=%d)\n", r1,
            static_cast<unsigned long long>(mode), static_cast<int>(mode & 1));
    wprintf(L"GETSENTENCEMODE(0x0003) sent=%lu mode=0x%llX\n", r2,
            static_cast<unsigned long long>(sent_mode));
    wprintf(L"GETOPENSTATUS(0x0005) sent=%lu open=%lld\n", r3,
            static_cast<long long>(open));

    // Toggle: flip IME_CMODE_NATIVE (0x1), keep every other bit, then read back.
    unsigned target = static_cast<unsigned>(mode) ^ 0x1u;
    DWORD_PTR set_result = 0;
    DWORD rs = SendMessageTimeoutW(ime, WM_IME_CONTROL, IMC_SETCONVERSIONMODE,
                                   static_cast<LPARAM>(target), SMTO_ABORTIFHUNG, 500,
                                   &set_result);
    DWORD_PTR mode2 = 0;
    SendMessageTimeoutW(ime, WM_IME_CONTROL, IMC_GETCONVERSIONMODE, 0, SMTO_ABORTIFHUNG,
                        500, &mode2);
    wprintf(L"SETCONVERSIONMODE sent=%lu result=%lld target=0x%X -> mode2=0x%llX (%s)\n",
            rs, static_cast<long long>(set_result), target,
            static_cast<unsigned long long>(mode2),
            (static_cast<unsigned>(mode2) & 1u) == (target & 1u) ? L"FLIP VERIFIED"
                                                                 : L"NOT APPLIED");
    // Restore the original mode so the spike leaves no side effect.
    SendMessageTimeoutW(ime, WM_IME_CONTROL, IMC_SETCONVERSIONMODE,
                        static_cast<LPARAM>(mode), SMTO_ABORTIFHUNG, 500, &set_result);
    return 0;
}
