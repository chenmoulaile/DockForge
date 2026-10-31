// T-301 spike 13: dump the owner-draw menu item payload, and does the real bar react?
//
// spike 12 measured that Sublime Text's top-level menu items are MFT_OWNERDRAW with an
// empty title, but GetMenuItemRect still reports exact screen geometry for each item
// (item 0 at 0,23,28,42 ... item 9 at 385,23,420,42). Two questions remain:
//   Q25) Is the title recoverable from the MFT_OWNERDRAW dwItemData block? spike 11
//        only probed 8-byte-aligned wide-string pointers and found nothing, so this
//        time the raw bytes are dumped and both wide and narrow strings are searched
//        at every byte offset.
//   Q26) If the bar forwards a click to the real menu bar rect while the target owns
//        the foreground, does the application's own popup open?
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

static FILE* g_log = nullptr;
static void L(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    va_end(ap);
    fflush(g_log);
}
static std::string N(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static bool Printable(const std::wstring& w) {
    if (w.size() < 2 || w.size() > 48) return false;
    for (wchar_t c : w) {
        if (c < 0x20 && c != L'&') return false;
        if (c > 0xFFFD) return false;
    }
    return true;
}

static void HexDump(const unsigned char* b, int n, int per_line = 16) {
    for (int i = 0; i < n; i += per_line) {
        char line[200]{};
        int off = 0;
        off += snprintf(line + off, sizeof(line) - off, "      %04X ", i);
        for (int j = 0; j < per_line && i + j < n; ++j)
            off += snprintf(line + off, sizeof(line) - off, "%02X ", b[i + j]);
        L("%s", line);
    }
}

static void Q25(HWND hwnd) {
    HMENU bar = GetMenu(hwnd);
    if (!bar) { L("[Q25] no menu"); return; }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) { L("[Q25] OpenProcess failed %lu", GetLastError()); return; }
    const int n = GetMenuItemCount(bar);
    for (int i = 0; i < n && i < 3; ++i) {
        MENUITEMINFOW mi{};
        mi.cbSize = sizeof(mi);
        mi.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_ID;
        if (!GetMenuItemInfoW(bar, static_cast<UINT>(i), TRUE, &mi)) continue;
        L("[Q25] top[%d] fType=0x%X data=%p", i, mi.fType, (void*)mi.dwItemData);
        if (mi.dwItemData == 0) continue;
        unsigned char buf[128]{};
        SIZE_T got = 0;
        if (!ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(mi.dwItemData), buf, sizeof(buf),
                               &got)) {
            L("[Q25]   ReadProcessMemory failed %lu", GetLastError());
            continue;
        }
        L("[Q25]   got=%zu bytes", static_cast<size_t>(got));
        HexDump(buf, 96);
        // Search every byte offset for a plausible wide or narrow string.
        for (int off = 0; off + 4 < 128; ++off) {
            const wchar_t* w = reinterpret_cast<const wchar_t*>(buf + off);
            std::wstring ws;
            for (int k = 0; k < 32 && off + k * 2 + 1 < 128; ++k) {
                const wchar_t c = w[k];
                if (c == 0) break;
                ws.push_back(c);
            }
            if (Printable(ws)) {
                L("[Q25]   WIDE  +%d -> '%s'", off, N(ws).c_str());
            }
            std::string ns;
            for (int k = 0; k < 32 && off + k < 128; ++k) {
                const unsigned char c = buf[off + k];
                if (c == 0) break;
                if (c < 0x20 || c > 0x7E) { ns.clear(); break; }
                ns.push_back(static_cast<char>(c));
            }
            if (ns.size() >= 3 && ns.size() <= 32) {
                L("[Q25]   ASCII +%d -> '%s'", off, ns.c_str());
            }
        }
    }
    CloseHandle(proc);
}

static void Q26(HWND hwnd) {
    HMENU bar = GetMenu(hwnd);
    if (!bar) return;
    RECT r{};
    if (!GetMenuItemRect(hwnd, bar, 0, &r)) { L("[Q26] no rect"); return; }
    L("[Q26] item0 rect=%ld,%ld,%ld,%ld", r.left, r.top, r.right, r.bottom);
    const BOOL fg = SetForegroundWindow(hwnd);
    L("[Q26] SetForegroundWindow=%d err=%lu", fg ? 1 : 0, GetLastError());
    Sleep(220);
    L("[Q26] foreground now=%p target=%p", (void*)GetForegroundWindow(), (void*)hwnd);
    const int cx = (r.left + r.right) / 2;
    const int cy = (r.top + r.bottom) / 2;
    L("[Q26] clicking %d,%d", cx, cy);
    SetCursorPos(cx, cy);
    Sleep(70);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    Sleep(50);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    Sleep(350);
    struct Found { HWND h; DWORD pid; RECT r; };
    std::vector<Found> pops;
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            wchar_t cls[64]{};
            GetClassNameW(h, cls, 64);
            if (wcscmp(cls, L"#32768") != 0) return TRUE;
            if (!IsWindowVisible(h)) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            RECT rr{};
            GetWindowRect(h, &rr);
            reinterpret_cast<std::vector<Found>*>(lp)->push_back({h, pid, rr});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&pops));
    L("[Q26] #32768 popups=%zu", pops.size());
    for (const auto& p : pops) {
        L("[Q26]   popup=%p pid=%lu rect=%ld,%ld,%ld,%ld", (void*)p.h, p.pid, p.r.left, p.r.top,
          p.r.right, p.r.bottom);
    }
    if (!pops.empty()) {
        keybd_event(VK_ESCAPE, 0, 0, 0);
        keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
        Sleep(150);
        L("[Q26] dismissed=%d", pops.empty() ? 0 : 1);
    }
}

int wmain(int argc, wchar_t** argv) {
    g_log = _wfopen(L"menu_spike13.log", L"w");
    if (!g_log) return 2;
    HWND target = nullptr;
    if (argc > 1) {
        unsigned long long raw = 0;
        swscanf_s(argv[1], L"%llx", &raw);
        target = reinterpret_cast<HWND>(static_cast<UINT_PTR>(raw));
    }
    L("target=%p", (void*)target);
    if (!target || !IsWindow(target)) { L("verdict: SKIP"); fclose(g_log); return 3; }
    Q25(target);
    Q26(target);
    L("verdict: done");
    fclose(g_log);
    return 0;
}