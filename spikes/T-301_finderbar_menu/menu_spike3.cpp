// DockForge T-301 spike, part 3: the three remaining unknowns before the Finder
// Bar can be written as product code.
//
// Q9.  Can we pop up ANOTHER process's HMENU with TrackPopupMenuEx?
//      Parts 1 and 2 proved we can read a foreign menu with GetMenuItemInfo and
//      that PostMessage(WM_COMMAND) reaches the owner. The cheap way to render a
//      menu is therefore to hand the real HMENU to TrackPopupMenuEx with
//      TPM_RETURNCMD and forward whatever id it returns - no self-drawn menu, no
//      per-item font/checkbox/shortcut re-implementation, and accelerator text,
//      check marks and radio bullets come out right for free. If that does not
//      work the bar has to draw its own menu, which is a much larger and much
//      less faithful job, so this is the question that sets the size of T-301.
//      The popup is dismissed from a timer with EndMenu() so the spike does not
//      hang waiting for a human.
//
// Q10. What does a per-frame GetCursorPos poll cost? Auto-hide needs to know when
//      the pointer reaches the top edge. A trigger strip cannot be used: part 2
//      measured that a strip that reacts to the mouse also eats clicks, and one
//      that does not react (WS_EX_TRANSPARENT) never sees WM_MOUSEMOVE either.
//      Polling the cursor from the existing 60 Hz frame loop avoids both, but
//      only if the call is cheap enough to run every frame.
//
// Q11. What does one full foreground-menu snapshot cost? The bar re-reads the
//      foreground window's menu whenever the foreground changes; that has to fit
//      in the 100ms DoD budget with room to spare.
//
// Build:
//   g++ -std=c++17 -O2 -municode -o spikes/T-301_finderbar_menu/menu_spike3.exe \
//       spikes/T-301_finderbar_menu/menu_spike3.cpp -luser32
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kClass[] = L"DockForgeSpike3Window";

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER) {
        // Closes the TrackPopupMenu loop the same way a real cancel would.
        EndMenu();
        return 0;
    }
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

double NowMs() {
    static LARGE_INTEGER freq{};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

// ---- child: a window with a menu, so the parent has a foreign menu to play with
int RunChild() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, 1001, L"&Open\tCtrl+O");
    AppendMenuW(file, MF_STRING, 1002, L"&Save");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING | MF_CHECKED, 1003, L"&Word Wrap");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");

    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, 2001, L"Cu&t");
    AppendMenuW(edit, MF_STRING, 2002, L"&Copy");
    AppendMenuW(edit, MF_STRING | MF_GRAYED, 2003, L"&Paste");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), L"&Edit");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, 3001, L"&Zoom In");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");

    HWND w = CreateWindowExW(WS_EX_TOOLWINDOW, kClass, L"DockForge Spike3 Child", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 420, 260, nullptr, bar,
                             GetModuleHandleW(nullptr), nullptr);
    if (w == nullptr) return 2;
    ShowWindow(w, SW_SHOWNOACTIVATE);

    // Report the HWND so the parent does not have to guess, then idle.
    const std::wstring out = ExeDir() + L"\\spike3_child.txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, out.c_str(), L"w") == 0 && f != nullptr) {
        std::fprintf(f, "%lld\n", static_cast<long long>(reinterpret_cast<intptr_t>(w)));
        std::fclose(f);
    }

    MSG msg{};
    const DWORD until = GetTickCount() + 20000;
    while (GetTickCount() < until) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != FALSE) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring(argv[1]) == L"--child") return RunChild();

    std::printf("T-301 finder bar spike part 3 (pid=%lu)\n",
                static_cast<unsigned long>(GetCurrentProcessId()));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    HWND own = CreateWindowExW(WS_EX_TOPMOST, kClass, L"DockForge Spike3 Host", WS_POPUP, 0, 0,
                               200, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    // ---- spawn the child that owns a real menu -------------------------
    std::printf("\nQ9 pop up a FOREIGN process menu\n");
    const std::wstring child_out = ExeDir() + L"\\spike3_child.txt";
    DeleteFileW(child_out.c_str());
    std::wstring cmd = std::wstring(L"\"") + ExePath() + L"\" --child";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                       &pi) == FALSE) {
        std::printf("  child spawn failed err=%lu\n", static_cast<unsigned long>(GetLastError()));
        return 1;
    }
    CloseHandle(pi.hThread);
    PumpFor(600);

    HWND child = nullptr;
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, child_out.c_str(), L"r") == 0 && f != nullptr) {
            long long value = 0;
            if (std::fscanf(f, "%lld", &value) == 1) {
                child = reinterpret_cast<HWND>(static_cast<intptr_t>(value));
            }
            std::fclose(f);
        }
    }
    std::printf("  child hwnd=%p valid=%d\n", static_cast<void*>(child),
                IsWindow(child) ? 1 : 0);

    HMENU foreign = child != nullptr ? GetMenu(child) : nullptr;
    std::printf("  GetMenu(child)=%p\n", static_cast<void*>(foreign));
    if (foreign != nullptr) {
        const int count = GetMenuItemCount(foreign);
        std::printf("  items=%d\n", count);
        HMENU sub = GetSubMenu(foreign, 0);
        std::printf("  GetSubMenu(0)=%p sub_count=%d\n", static_cast<void*>(sub),
                    sub != nullptr ? GetMenuItemCount(sub) : -1);

        // The decisive call: hand the foreign HMENU to our own menu loop.
        SetTimer(own, 1, 350, nullptr);
        const double t0 = NowMs();
        const UINT id = TrackPopupMenuEx(sub, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN |
                                                  TPM_TOPALIGN,
                                         200, 200, own, nullptr);
        const double elapsed = NowMs() - t0;
        KillTimer(own, 1);
        std::printf("  TrackPopupMenuEx(foreign sub) -> id=%u elapsed_ms=%.1f err=%lu\n", id,
                    elapsed, static_cast<unsigned long>(GetLastError()));

        // Second variant: the whole bar, so the caller can see the top level too.
        SetTimer(own, 2, 350, nullptr);
        const UINT id2 = TrackPopupMenuEx(foreign, TPM_RETURNCMD | TPM_NONOTIFY, 200, 200, own,
                                          nullptr);
        KillTimer(own, 2);
        std::printf("  TrackPopupMenuEx(foreign bar) -> id=%u err=%lu\n", id2,
                    static_cast<unsigned long>(GetLastError()));
    }
    PumpFor(200);
    if (child != nullptr) PostMessageW(child, WM_CLOSE, 0, 0);
    WaitForSingleObject(pi.hProcess, 2000);
    CloseHandle(pi.hProcess);

    // ---- Q10: cursor poll cost -----------------------------------------
    std::printf("\nQ10 GetCursorPos poll cost\n");
    {
        POINT p{};
        const int kIterations = 10000;
        const double t0 = NowMs();
        int ok = 0;
        for (int i = 0; i < kIterations; ++i) {
            if (GetCursorPos(&p) != FALSE) ++ok;
        }
        const double elapsed = NowMs() - t0;
        std::printf("  %d calls in %.2fms -> %.4fms/call ok=%d last=%ld,%ld\n", kIterations,
                    elapsed, elapsed / kIterations, ok, p.x, p.y);
        std::printf("  at 60fps that is %.3f%% of a 16.7ms frame\n",
                    (elapsed / kIterations) / 16.7 * 100.0);
    }

    // ---- Q11: full foreground snapshot cost ----------------------------
    std::printf("\nQ11 foreground snapshot cost\n");
    {
        const int kRounds = 200;
        double worst = 0.0;
        double total = 0.0;
        for (int i = 0; i < kRounds; ++i) {
            const double t0 = NowMs();
            const HWND fg = GetForegroundWindow();
            const HMENU menu = fg != nullptr ? GetMenu(fg) : nullptr;
            const int count = menu != nullptr ? GetMenuItemCount(menu) : 0;
            wchar_t text[128] = {};
            for (int j = 0; j < count && j < 8; ++j) {
                MENUITEMINFOW info{};
                info.cbSize = sizeof(info);
                info.fMask = MIIM_STRING | MIIM_FTYPE;
                info.dwTypeData = text;
                info.cch = 128;
                GetMenuItemInfoW(menu, static_cast<UINT>(j), TRUE, &info);
            }
            const double elapsed = NowMs() - t0;
            total += elapsed;
            if (elapsed > worst) worst = elapsed;
        }
        std::printf("  %d rounds avg_ms=%.4f worst_ms=%.4f\n", kRounds, total / kRounds, worst);
    }

    DestroyWindow(own);
    std::printf("\nverdict: see numbers above.\n");
    return 0;
}
