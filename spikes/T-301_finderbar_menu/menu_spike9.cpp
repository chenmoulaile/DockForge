// T-301 spike 9: can a self-drawn menu bar (Sublime Text) still be read?
//
// Sublime Text answers GetMenu() with a real HMENU that has 10 top-level items, but
// every top-level item is MFT_OWNERDRAW with an empty string and dwItemData == 0.
// The submenus underneath are fully readable (&New File, &Open File..., ...).
// So GetMenuStringW cannot name the top-level titles; this spike tries the other
// documented routes before we decide what the bar shows.
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <oleacc.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>

static FILE* g_log = nullptr;
static void L(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    va_end(ap);
    fflush(g_log);
}

// ---- MSAA -------------------------------------------------------------------
static void TryMsaa(HWND hwnd) {
    L("[msaa] target=%p", (void*)hwnd);
    for (long obj : {static_cast<long>(OBJID_MENU), static_cast<long>(OBJID_CLIENT),
                     static_cast<long>(OBJID_WINDOW)}) {
        IAccessible* acc = nullptr;
        HRESULT hr = AccessibleObjectFromWindow(hwnd, static_cast<DWORD>(obj), IID_IAccessible,
                                                reinterpret_cast<void**>(&acc));
        L("[msaa] objid=0x%lX hr=0x%08lX acc=%p", obj, static_cast<unsigned long>(hr), (void*)acc);
        if (FAILED(hr) || acc == nullptr) continue;
        VARIANT self;
        VariantInit(&self);
        self.vt = VT_I4;
        self.lVal = CHILDID_SELF;
        long count = 0;
        HRESULT chr = acc->get_accChildCount(&count);
        BSTR name = nullptr;
        acc->get_accName(self, &name);
        L("[msaa]   name=%ls childCount=%ld hr=0x%08lX",
          name ? name : L"(null)", count, static_cast<unsigned long>(chr));
        if (name) SysFreeString(name);
        for (long i = 1; i <= count && i <= 12; ++i) {
            VARIANT child;
            VariantInit(&child);
            child.vt = VT_I4;
            child.lVal = i;
            BSTR cname = nullptr;
            acc->get_accName(child, &cname);
            VARIANT role;
            VariantInit(&role);
            acc->get_accRole(child, &role);
            L("[msaa]   child[%ld] name=%ls role=%ld", i, cname ? cname : L"(null)",
              role.vt == VT_I4 ? role.lVal : -1);
            if (cname) SysFreeString(cname);
        }
        acc->Release();
    }
}

// ---- raw menu detail --------------------------------------------------------
static void DumpMenu(HWND hwnd) {
    HMENU bar = GetMenu(hwnd);
    L("[menu] GetMenu=%p", (void*)bar);
    if (!bar) return;
    const int n = GetMenuItemCount(bar);
    L("[menu] count=%d", n);
    for (int i = 0; i < n && i < 12; ++i) {
        MENUITEMINFOW mi{};
        mi.cbSize = sizeof(mi);
        mi.fMask = MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_FTYPE | MIIM_STRING | MIIM_DATA;
        wchar_t buf[256]{};
        mi.dwTypeData = buf;
        mi.cch = 256;
        const BOOL ok = GetMenuItemInfoW(bar, static_cast<UINT>(i), TRUE, &mi);
        L("[menu] top[%d] ok=%d fType=0x%X fState=0x%X wID=%u data=%p text='%ls'", i, ok ? 1 : 0,
          mi.fType, mi.fState, mi.wID, (void*)mi.dwItemData, buf);
        HMENU sub = GetSubMenu(bar, i);
        if (sub) L("[menu]   subcount=%d", GetMenuItemCount(sub));
    }
    // The window title sometimes carries the "current document" but not the menus.
    wchar_t title[256]{};
    GetWindowTextW(hwnd, title, 256);
    L("[menu] window title='%ls'", title);
}

int wmain(int argc, wchar_t** argv) {
    g_log = _wfopen(L"menu_spike9.log", L"w");
    if (!g_log) return 2;
    HWND target = nullptr;
    if (argc > 1) {
        unsigned long long raw = 0;
        swscanf_s(argv[1], L"%llx", &raw);
        target = reinterpret_cast<HWND>(static_cast<UINT_PTR>(raw));
    }
    L("target arg=%p", (void*)target);
    if (!target || !IsWindow(target)) {
        L("verdict: Q24_msaa_menu=SKIP (no valid hwnd)");
        fclose(g_log);
        return 3;
    }
    DumpMenu(target);
    TryMsaa(target);
    L("verdict: done");
    fclose(g_log);
    return 0;
}