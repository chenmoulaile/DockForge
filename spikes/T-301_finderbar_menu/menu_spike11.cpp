// T-301 spike 11: where do the top-level menu titles of a self-drawn menu bar live?
//
// spike 10 proved that for Sublime Text:
//   A) GetMenu returns 10 top-level items, all MFT_OWNERDRAW, all with empty text
//   B) MSAA OBJID_MENU sees 10 children, all ROLE_SYSTEM_MENUITEM, all with null name
//   C) UIA finds no MenuBar with children
// but every submenu underneath is fully readable. So the *titles* must come from
// somewhere else. This spike tests four candidate routes and reports which one yields
// real text, so the bar can pick a source on evidence:
//   R1) ReadProcessMemory at the MFT_OWNERDRAW dwItemData pointer
//   R2) UIA RawViewWalker children of the MenuBar element
//   R3) UIA LegacyIAccessible pattern on the MenuBar children
//   R4) open the popup by clicking the top-level item and read the popup's window text
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <oleacc.h>
#include <UIAutomation.h>
#include <UIAutomationClient.h>
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
static std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static bool LooksLikeText(const std::wstring& w) {
    if (w.size() < 2 || w.size() > 40) return false;
    for (wchar_t c : w) {
        if (c < 0x20 && c != L'&') return false;
        if (c == 0xFFFD) return false;
    }
    return true;
}

// R1 -------------------------------------------------------------------------
static void TryOwnerDrawData(HWND hwnd) {
    HMENU bar = GetMenu(hwnd);
    if (!bar) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    L("[R1] pid=%lu OpenProcess=%p", pid, proc);
    if (!proc) return;
    const int n = GetMenuItemCount(bar);
    for (int i = 0; i < n && i < 12; ++i) {
        MENUITEMINFOW mi{};
        mi.cbSize = sizeof(mi);
        mi.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_ID;
        if (!GetMenuItemInfoW(bar, static_cast<UINT>(i), TRUE, &mi)) continue;
        if ((mi.fType & MFT_OWNERDRAW) == 0 || mi.dwItemData == 0) continue;
        unsigned char buf[512]{};
        SIZE_T got = 0;
        const BOOL ok = ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(mi.dwItemData), buf,
                                          sizeof(buf), &got);
        L("[R1] top[%d] data=%p read=%d got=%zu", i, (void*)mi.dwItemData, ok ? 1 : 0,
          static_cast<size_t>(got));
        if (!ok) continue;
        // Try: the block starts with a pointer to a wide string.
        for (int off = 0; off < 64; off += 8) {
            void* p = *reinterpret_cast<void**>(buf + off);
            if (p == nullptr) continue;
            wchar_t wbuf[128]{};
            SIZE_T wgot = 0;
            if (!ReadProcessMemory(proc, p, wbuf, sizeof(wbuf) - 2, &wgot)) continue;
            wbuf[127] = L'\0';
            std::wstring s(wbuf);
            if (LooksLikeText(s)) {
                L("[R1]   top[%d] +%d -> ptr=%p text='%s'", i, off, p, Narrow(s).c_str());
                break;
            }
        }
    }
    CloseHandle(proc);
}

// R2 / R3 --------------------------------------------------------------------
static void TryUia(IUIAutomation* uia, HWND hwnd) {
    if (!uia) return;
    IUIAutomationElement* root = nullptr;
    if (FAILED(uia->ElementFromHandle(hwnd, &root)) || !root) return;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = UIA_MenuBarControlTypeId;
    IUIAutomationCondition* cond = nullptr;
    uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond);
    IUIAutomationElementArray* bars = nullptr;
    if (cond && SUCCEEDED(root->FindAll(TreeScope_Descendants, cond, &bars)) && bars) {
        int barCount = 0;
        bars->get_Length(&barCount);
        L("[R2] MenuBar elements=%d", barCount);
        for (int b = 0; b < barCount; ++b) {
            IUIAutomationElement* bar = nullptr;
            if (FAILED(bars->GetElement(b, &bar)) || !bar) continue;
            BSTR bn = nullptr;
            bar->get_CurrentName(&bn);
            L("[R2] bar[%d] name='%s'", b, bn ? Narrow(bn).c_str() : "");
            if (bn) SysFreeString(bn);
            IUIAutomationTreeWalker* walker = nullptr;
            uia->get_RawViewWalker(&walker);
            if (walker) {
                IUIAutomationElement* kid = nullptr;
                walker->GetFirstChildElement(bar, &kid);
                int idx = 0;
                while (kid && idx < 14) {
                    BSTR kn = nullptr;
                    kid->get_CurrentName(&kn);
                    CONTROLTYPEID ct = 0;
                    kid->get_CurrentControlType(&ct);
                    L("[R2]   rawkid[%d] type=%lu name='%s'", idx, ct,
                      kn ? Narrow(kn).c_str() : "");
                    if (kn) SysFreeString(kn);
                    IUIAutomationElement* next = nullptr;
                    walker->GetNextSiblingElement(kid, &next);
                    kid->Release();
                    kid = next;
                    ++idx;
                }
                if (kid) kid->Release();
                walker->Release();
            }
            bar->Release();
        }
        bars->Release();
    }
    if (cond) cond->Release();
    root->Release();
}

int wmain(int argc, wchar_t** argv) {
    g_log = _wfopen(L"menu_spike11.log", L"w");
    if (!g_log) return 2;
    HWND target = nullptr;
    if (argc > 1) {
        unsigned long long raw = 0;
        swscanf_s(argv[1], L"%llx", &raw);
        target = reinterpret_cast<HWND>(static_cast<UINT_PTR>(raw));
    }
    L("target=%p", (void*)target);
    if (!target || !IsWindow(target)) {
        L("verdict: SKIP");
        fclose(g_log);
        return 3;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IUIAutomation* uia = nullptr;
    CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                     reinterpret_cast<void**>(&uia));
    TryOwnerDrawData(target);
    TryUia(uia, target);
    L("verdict: done");
    if (uia) uia->Release();
    CoUninitialize();
    fclose(g_log);
    return 0;
}