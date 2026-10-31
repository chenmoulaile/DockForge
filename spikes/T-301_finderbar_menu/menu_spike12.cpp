// T-301 spike 12: can an owner-draw menu bar be located, named, or driven?
//
// spike 10/11 established the shape of the problem for Sublime Text:
//   GetMenu -> 10 items, all MFT_OWNERDRAW, all with empty text
//   MSAA OBJID_MENU -> 10 children, all ROLE_SYSTEM_MENUITEM, all with null name
//   UIA -> MenuBar "应用程序" with 10 MenuItem children, all with empty name
// while every submenu underneath is fully readable (&New File, &Open File...).
//
// Four routes are measured here. Each one either yields text, yields geometry, or
// yields a working native dropdown; the bar will use whichever survive:
//   R1) GetMenuItemRect        - do the top-level items have screen geometry?
//   R2) every MSAA property    - is the title hidden in accValue/accDescription?
//   R3) every UIA property     - is the title hidden in a pattern property?
//   R4) click the app's own bar- does a real #32768 popup open, and whose is it?
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

struct MenuWin { HWND hwnd; DWORD pid; RECT rect; };
static BOOL CALLBACK FindPopup(HWND h, LPARAM lp) {
    auto* out = reinterpret_cast<std::vector<MenuWin>*>(lp);
    wchar_t cls[64]{};
    GetClassNameW(h, cls, 64);
    if (wcscmp(cls, L"#32768") != 0) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    RECT r{};
    GetWindowRect(h, &r);
    out->push_back({h, pid, r});
    return TRUE;
}

static void R1_ItemRects(HWND hwnd) {
    HMENU bar = GetMenu(hwnd);
    L("[R1] GetMenu=%p", (void*)bar);
    if (!bar) return;
    const int n = GetMenuItemCount(bar);
    for (int i = 0; i < n && i < 12; ++i) {
        RECT r{};
        const BOOL ok = GetMenuItemRect(hwnd, bar, static_cast<UINT>(i), &r);
        L("[R1] item[%d] ok=%d rect=%ld,%ld,%ld,%ld w=%ld h=%ld", i, ok ? 1 : 0, r.left, r.top,
          r.right, r.bottom, r.right - r.left, r.bottom - r.top);
    }
}

static void R2_MsaaProps(HWND hwnd) {
    IAccessible* acc = nullptr;
    if (FAILED(AccessibleObjectFromWindow(hwnd, OBJID_MENU, IID_IAccessible,
                                         reinterpret_cast<void**>(&acc))) || !acc) {
        L("[R2] no OBJID_MENU accessible");
        return;
    }
    long count = 0;
    acc->get_accChildCount(&count);
    L("[R2] childCount=%ld", count);
    for (long i = 1; i <= count && i <= 12; ++i) {
        VARIANT ch;
        VariantInit(&ch);
        ch.vt = VT_I4;
        ch.lVal = i;
        BSTR name = nullptr, value = nullptr, desc = nullptr, help = nullptr, keys = nullptr;
        VARIANT role, state;
        VariantInit(&role);
        VariantInit(&state);
        acc->get_accName(ch, &name);
        acc->get_accValue(ch, &value);
        acc->get_accDescription(ch, &desc);
        acc->get_accHelp(ch, &help);
        acc->get_accKeyboardShortcut(ch, &keys);
        acc->get_accRole(ch, &role);
        acc->get_accState(ch, &state);
        L("[R2] child[%ld] name='%s' value='%s' desc='%s' help='%s' keys='%s' role=%ld state=0x%lX",
          i, name ? N(name).c_str() : "", value ? N(value).c_str() : "",
          desc ? N(desc).c_str() : "", help ? N(help).c_str() : "",
          keys ? N(keys).c_str() : "", role.vt == VT_I4 ? role.lVal : -1L,
          state.vt == VT_I4 ? state.lVal : 0L);
        if (name) SysFreeString(name);
        if (value) SysFreeString(value);
        if (desc) SysFreeString(desc);
        if (help) SysFreeString(help);
        if (keys) SysFreeString(keys);
    }
    acc->Release();
}

static void DumpUiaProps(IUIAutomationElement* e, const char* tag) {
    BSTR n = nullptr, help = nullptr, item = nullptr, accel = nullptr, cls = nullptr, aid = nullptr;
    e->get_CurrentName(&n);
    e->get_CurrentHelpText(&help);
    e->get_CurrentItemStatus(&item);
    e->get_CurrentAcceleratorKey(&accel);
    e->get_CurrentClassName(&cls);
    e->get_CurrentAutomationId(&aid);
    L("[R3] %s name='%s' help='%s' itemStatus='%s' accel='%s' class='%s' id='%s'",
      tag, n ? N(n).c_str() : "", help ? N(help).c_str() : "", item ? N(item).c_str() : "",
      accel ? N(accel).c_str() : "", cls ? N(cls).c_str() : "", aid ? N(aid).c_str() : "");
    if (n) SysFreeString(n);
    if (help) SysFreeString(help);
    if (item) SysFreeString(item);
    if (accel) SysFreeString(accel);
    if (cls) SysFreeString(cls);
    if (aid) SysFreeString(aid);
    // LegacyIAccessible pattern often carries the MSAA name even when CurrentName is empty.
    IUIAutomationLegacyIAccessiblePattern* legacy = nullptr;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, __uuidof(IUIAutomationLegacyIAccessiblePattern), reinterpret_cast<void**>(&legacy))) && legacy) {
        BSTR ln = nullptr, lv = nullptr, ld = nullptr, lk = nullptr;
        legacy->get_CurrentName(&ln);
        legacy->get_CurrentValue(&lv);
        legacy->get_CurrentDescription(&ld);
        legacy->get_CurrentKeyboardShortcut(&lk);
        L("[R3] %s legacy name='%s' value='%s' desc='%s' keys='%s'", tag,
          ln ? N(ln).c_str() : "", lv ? N(lv).c_str() : "", ld ? N(ld).c_str() : "",
          lk ? N(lk).c_str() : "");
        if (ln) SysFreeString(ln);
        if (lv) SysFreeString(lv);
        if (ld) SysFreeString(ld);
        if (lk) SysFreeString(lk);
        legacy->Release();
    }
}

static void R3_UiaProps(IUIAutomation* uia, HWND hwnd) {
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
        int bc = 0;
        bars->get_Length(&bc);
        for (int b = 0; b < bc; ++b) {
            IUIAutomationElement* bar = nullptr;
            if (FAILED(bars->GetElement(b, &bar)) || !bar) continue;
            char tag[64];
            snprintf(tag, sizeof(tag), "bar[%d]", b);
            DumpUiaProps(bar, tag);
            IUIAutomationTreeWalker* walker = nullptr;
            uia->get_RawViewWalker(&walker);
            if (walker) {
                IUIAutomationElement* kid = nullptr;
                walker->GetFirstChildElement(bar, &kid);
                int idx = 0;
                while (kid && idx < 12) {
                    snprintf(tag, sizeof(tag), "  bar[%d].kid[%d]", b, idx);
                    DumpUiaProps(kid, tag);
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

static void R4_ClickOwnBar(HWND hwnd) {
    HMENU bar = GetMenu(hwnd);
    if (!bar) return;
    RECT r{};
    if (!GetMenuItemRect(hwnd, bar, 0, &r)) {
        L("[R4] no item rect, cannot click");
        return;
    }
    const int cx = (r.left + r.right) / 2;
    const int cy = (r.top + r.bottom) / 2;
    L("[R4] clicking the app's own item 0 at %d,%d", cx, cy);
    SetCursorPos(cx, cy);
    Sleep(60);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    Sleep(40);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    Sleep(250);
    std::vector<MenuWin> pops;
    EnumWindows(&FindPopup, reinterpret_cast<LPARAM>(&pops));
    L("[R4] popups after click=%zu", pops.size());
    for (const auto& p : pops) {
        L("[R4]   popup=%p pid=%lu rect=%ld,%ld,%ld,%ld", (void*)p.hwnd, p.pid, p.rect.left,
          p.rect.top, p.rect.right, p.rect.bottom);
    }
    // Close whatever we opened.
    if (!pops.empty()) {
        PostMessageW(hwnd, WM_CANCELMODE, 0, 0);
        Sleep(80);
        keybd_event(VK_ESCAPE, 0, 0, 0);
        keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
        Sleep(120);
    }
}

int wmain(int argc, wchar_t** argv) {
    g_log = _wfopen(L"menu_spike12.log", L"w");
    if (!g_log) return 2;
    HWND target = nullptr;
    if (argc > 1) {
        unsigned long long raw = 0;
        swscanf_s(argv[1], L"%llx", &raw);
        target = reinterpret_cast<HWND>(static_cast<UINT_PTR>(raw));
    }
    L("target=%p", (void*)target);
    if (!target || !IsWindow(target)) { L("verdict: SKIP"); fclose(g_log); return 3; }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IUIAutomation* uia = nullptr;
    CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                     reinterpret_cast<void**>(&uia));
    R1_ItemRects(target);
    R2_MsaaProps(target);
    R3_UiaProps(uia, target);
    R4_ClickOwnBar(target);
    L("verdict: done");
    if (uia) uia->Release();
    CoUninitialize();
    fclose(g_log);
    return 0;
}