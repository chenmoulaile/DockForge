// T-301 spike 10: which menu source actually works across the desktop?
//
// Sublime Text proves that GetMenu() can return a real HMENU whose top-level items are
// MFT_OWNERDRAW with an empty string, so the bar cannot name them from the menu API.
// This spike surveys every visible top-level window and asks three independent sources
// for menu titles, so the choice of source is made on data, not on a guess:
//   A) GetMenu + GetMenuStringW          (classic Win32 menu bar)
//   B) MSAA OBJID_MENU child accName     (accessibility bridge)
//   C) UI Automation MenuBar descendants (modern accessibility bridge)
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <oleacc.h>
#include <UIAutomation.h>
#include <cstdio>
#include <initializer_list>
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

static std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// A) classic menu bar
static int CountMenuText(HWND hwnd, std::vector<std::wstring>& out) {
    HMENU bar = GetMenu(hwnd);
    if (!bar) return -1;
    const int n = GetMenuItemCount(bar);
    for (int i = 0; i < n; ++i) {
        wchar_t buf[128]{};
        const int copied = GetMenuStringW(bar, static_cast<UINT>(i), buf, 128, MF_BYPOSITION);
        if (copied > 0) out.push_back(buf);
    }
    return n;
}

// B) MSAA
static int CountMsaa(HWND hwnd, std::vector<std::wstring>& out) {
    IAccessible* acc = nullptr;
    if (FAILED(AccessibleObjectFromWindow(hwnd, OBJID_MENU, IID_IAccessible,
                                         reinterpret_cast<void**>(&acc))) || !acc) {
        return -1;
    }
    long count = 0;
    acc->get_accChildCount(&count);
    for (long i = 1; i <= count; ++i) {
        VARIANT ch;
        VariantInit(&ch);
        ch.vt = VT_I4;
        ch.lVal = i;
        BSTR name = nullptr;
        if (SUCCEEDED(acc->get_accName(ch, &name)) && name && *name) {
            out.push_back(name);
        }
        if (name) SysFreeString(name);
    }
    acc->Release();
    return static_cast<int>(count);
}

// C) UIA
static int CountUia(HWND hwnd, std::vector<std::wstring>& out, IUIAutomation* uia) {
    if (!uia) return -1;
    IUIAutomationElement* root = nullptr;
    if (FAILED(uia->ElementFromHandle(hwnd, &root)) || !root) return -1;
    IUIAutomationCondition* cond = nullptr;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = UIA_MenuBarControlTypeId;
    uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond);
    IUIAutomationElementArray* bars = nullptr;
    int items = -1;
    if (cond && SUCCEEDED(root->FindAll(TreeScope_Descendants, cond, &bars)) && bars) {
        int barCount = 0;
        bars->get_Length(&barCount);
        items = 0;
        for (int b = 0; b < barCount; ++b) {
            IUIAutomationElement* bar = nullptr;
            if (FAILED(bars->GetElement(b, &bar)) || !bar) continue;
            IUIAutomationElementArray* kids = nullptr;
            IUIAutomationCondition* any = nullptr;
            uia->CreateTrueCondition(&any);
            if (any && SUCCEEDED(bar->FindAll(TreeScope_Children, any, &kids)) && kids) {
                int kc = 0;
                kids->get_Length(&kc);
                for (int k = 0; k < kc; ++k) {
                    IUIAutomationElement* kid = nullptr;
                    if (FAILED(kids->GetElement(k, &kid)) || !kid) continue;
                    BSTR nm = nullptr;
                    if (SUCCEEDED(kid->get_CurrentName(&nm)) && nm && *nm) out.push_back(nm);
                    if (nm) SysFreeString(nm);
                    kid->Release();
                }
                kids->Release();
            }
            if (any) any->Release();
            bar->Release();
        }
        bars->Release();
    }
    if (cond) cond->Release();
    root->Release();
    return items;
}

struct Found { HWND hwnd; std::wstring cls; std::wstring proc; };

static BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp) {
    auto* out = reinterpret_cast<std::vector<Found>*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    const LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;
    wchar_t cls[128]{};
    GetClassNameW(hwnd, cls, 128);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    wchar_t proc[MAX_PATH]{};
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        DWORD n = MAX_PATH;
        QueryFullProcessImageNameW(h, 0, proc, &n);
        CloseHandle(h);
    }
    const wchar_t* base = wcsrchr(proc, L'\\');
    out->push_back({hwnd, cls, base ? base + 1 : proc});
    return TRUE;
}

int wmain() {
    g_log = _wfopen(L"menu_spike10.log", L"w");
    if (!g_log) return 2;
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    L("CoInitializeEx=0x%08lX", static_cast<unsigned long>(co));

    IUIAutomation* uia = nullptr;
    HRESULT uiaHr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                     __uuidof(IUIAutomation), reinterpret_cast<void**>(&uia));
    L("CoCreateInstance(CUIAutomation)=0x%08lX uia=%p", static_cast<unsigned long>(uiaHr), (void*)uia);

    std::vector<Found> windows;
    EnumWindows(&EnumProc, reinterpret_cast<LPARAM>(&windows));
    L("visible top-level windows=%zu", windows.size());

    int withMenu = 0, withMenuText = 0, withMsaa = 0, withMsaaText = 0, withUia = 0, withUiaText = 0;
    for (const auto& w : windows) {
        std::vector<std::wstring> a, b, c;
        const int na = CountMenuText(w.hwnd, a);
        const int nb = CountMsaa(w.hwnd, b);
        const int nc = CountUia(w.hwnd, c, uia);
        if (na > 0) ++withMenu;
        if (!a.empty()) ++withMenuText;
        if (nb > 0) ++withMsaa;
        if (!b.empty()) ++withMsaaText;
        if (nc > 0) ++withUia;
        if (!c.empty()) ++withUiaText;
        if (na <= 0 && nb <= 0 && nc <= 0) continue;
        L("[%p] %s (%s)", (void*)w.hwnd, Narrow(w.cls).c_str(), Narrow(w.proc).c_str());
        L("   A GetMenu items=%d titled=%zu %s", na, a.size(), a.empty() ? "" : Narrow(a[0]).c_str());
        L("   B MSAA    items=%d named=%zu", nb, b.size());
        for (size_t i = 0; i < b.size() && i < 6; ++i) L("        msaa[%zu]=%s", i, Narrow(b[i]).c_str());
        L("   C UIA     items=%d named=%zu", nc, c.size());
        for (size_t i = 0; i < c.size() && i < 6; ++i) L("        uia[%zu]=%s", i, Narrow(c[i]).c_str());
    }
    L("summary: A_items=%d A_titled=%d B_items=%d B_named=%d C_items=%d C_named=%d",
      withMenu, withMenuText, withMsaa, withMsaaText, withUia, withUiaText);
    L("verdict: survey complete");
    if (uia) uia->Release();
    CoUninitialize();
    fclose(g_log);
    return 0;
}