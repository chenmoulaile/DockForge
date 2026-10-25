// DockForge T-067 spike: taskbar progress READ paths, measured.
//
// ADR-0024: ITaskbarList3::SetProgressValue/SetProgressState are WRITE-only
// APIs. Before any product code, this spike must prove at least one read path:
//
//   Q1 (self)    After Set* on our own window, does any window property
//                (SetProp storage) appear that carries the state? That would be
//                the "target-window proxy state" ADR-0024 prefers.
//   Q2 (setter/reader)  Cross-process: can a SECOND process EnumProps that
//                window and decode the same bytes?
//   Q3 (copy)    Real source: does an explorer copy dialog (IFileOperation,
//                the copy engine explorer itself uses) expose the same property
//                on its window tree - and do MSAA values work as the
//                white-list fallback (ADR-0024 degrade path)?
//
// vtable hooking is NOT probed: ADR-0024 rules it the same class of forbidden
// target-process memory modification as ADR-0022.
//
// Build (MSYS2 MinGW g++, same toolchain as the repo):
//   g++ -std=c++20 -O2 -Wall -municode -o spike.exe spike.cpp \
//       -lole32 -loleaut32 -loleacc -lshell32 -luser32 -lshlwapi
//
// Usage:
//   spike.exe self                     Q1
//   spike.exe setter [seconds=30]      Q2 writer side (prints hwnd, ramps states)
//   spike.exe reader <hwnd> [secs=20]  Q2 reader side (separate process)
//   spike.exe copy <src> <dst>         Q3 (make src big first, e.g. fsutil)

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <oleacc.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// shobjidl.h provides the TBPF_* flags and TBPFLAG; MinGW ships them.

namespace {

// IFileOperation is declared in shobjidl.h; its CLSID/IID too.
constexpr CLSID kClsidFileOperation = {0x3ad05575, 0x8857, 0x4850,
                                       {0x92, 0x77, 0x1f, 0x86, 0x51, 0x87, 0x2e, 0x6b}};
constexpr IID kIidFileOperation = {0x947aab5f, 0x0a5c, 0x4c13,
                                   {0xb4, 0xd6, 0x4b, 0xf6, 0x43, 0x61, 0x2b, 0x8c}};
constexpr IID kIidIShellItem = {0x43826d1e, 0xe718, 0x42ee,
                                {0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe}};
constexpr IID kIidIAccessible = {0x618736e0, 0x3c3d, 0x11cf,
                                 {0x81, 0x0c, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

double NowMs() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

void PumpFor(int ms) {
    const double until = NowMs() + ms;
    MSG msg;
    while (NowMs() < until) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(30);
    }
}

bool Readable(const void* p) {
    if (p == nullptr) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) !=
           0;
}

struct PropRecord {
    std::wstring name;
    HANDLE value;
    std::vector<unsigned char> bytes;  // up to 32 bytes if readable
};

BOOL CALLBACK PropEnumProc(HWND hwnd, LPWSTR name, HANDLE value, ULONG_PTR lparam) {
    auto* out = reinterpret_cast<std::vector<PropRecord>*>(lparam);
    PropRecord rec;
    if (IS_INTRESOURCE(name)) {
        WCHAR atom_name[256] = L"";
        GlobalGetAtomNameW(static_cast<ATOM>(reinterpret_cast<UINT_PTR>(name)), atom_name, 256);
        rec.name = std::wstring(atom_name) + L" (atom)";
    } else {
        rec.name = name;
    }
    rec.value = value;
    const auto* raw = reinterpret_cast<const unsigned char*>(value);
    if (Readable(raw)) {
        for (int i = 0; i < 32; ++i) rec.bytes.push_back(raw[i]);
    }
    out->push_back(std::move(rec));
    return TRUE;
}

std::vector<PropRecord> Snapshot(HWND hwnd) {
    std::vector<PropRecord> props;
    EnumPropsExW(hwnd, PropEnumProc, reinterpret_cast<ULONG_PTR>(&props));
    return props;
}

void Dump(const char* tag, HWND hwnd) {
    const std::vector<PropRecord> props = Snapshot(hwnd);
    printf("[%s] hwnd=%p props=%zu\n", tag, static_cast<void*>(hwnd), props.size());
    for (const PropRecord& rec : props) {
        printf("  prop '%ls' handle=%p", rec.name.c_str(), rec.value);
        if (!rec.bytes.empty()) {
            printf(" bytes:");
            char hex[8];
            for (unsigned char b : rec.bytes) {
                snprintf(hex, sizeof(hex), " %02X", b);
                printf("%s", hex);
            }
            // Also as 4x uint64 in case the payload is (completed,total) pairs.
            if (rec.bytes.size() >= 16) {
                unsigned long long q[2];
                memcpy(q, rec.bytes.data(), 16);
                printf(" | q0=%llu q1=%llu", q[0], q[1]);
            }
        } else {
            printf(" (value not readable as pointer)");
        }
        printf("\n");
    }
    fflush(stdout);
}

// Diff against `before` by name: prints only added/changed properties.
void DumpNew(const char* tag, HWND hwnd, const std::vector<PropRecord>& before) {
    const std::vector<PropRecord> now = Snapshot(hwnd);
    printf("[%s] props=%zu\n", tag, now.size());
    for (const PropRecord& rec : now) {
        const PropRecord* old = nullptr;
        for (const PropRecord& b : before) {
            if (b.name == rec.name) old = &b;
        }
        if (old && old->value == rec.value && old->bytes == rec.bytes) continue;
        printf("  %s '%ls' handle=%p", old ? "CHANGED" : "NEW", rec.name.c_str(), rec.value);
        if (!rec.bytes.empty()) {
            printf(" bytes:");
            char hex[8];
            for (unsigned char b : rec.bytes) {
                snprintf(hex, sizeof(hex), " %02X", b);
                printf("%s", hex);
            }
            if (rec.bytes.size() >= 16) {
                unsigned long long q[2];
                memcpy(q, rec.bytes.data(), 16);
                printf(" | q0=%llu q1=%llu", q[0], q[1]);
            }
        } else {
            printf(" (value not readable as pointer in THIS process)");
        }
        printf("\n");
    }
    fflush(stdout);
}

ITaskbarList3* CreateTaskbarList3() {
    ITaskbarList3* tb = nullptr;
    // GUIDs from the SDK headers / libuuid, not hand-rolled: the first attempt
    // with hand-written constants returned E_NOINTERFACE, and a spike must not
    // doubt the platform because of a typo'd GUID.
    const CLSID& clsid = CLSID_TaskbarList;
    const IID& iid = IID_ITaskbarList3;
    HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, iid,
                                  reinterpret_cast<void**>(&tb));
    if (FAILED(hr) || !tb) {
        printf("FAIL CoCreateInstance(TaskbarList) hr=0x%08lX\n", hr);
        return nullptr;
    }
    hr = tb->HrInit();
    if (FAILED(hr)) {
        printf("FAIL HrInit hr=0x%08lX\n", hr);
        tb->Release();
        return nullptr;
    }
    return tb;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND MakeWindow(const wchar_t* title) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"T067SpikeWnd";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW, 64, 64, 320, 120,
                                nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    return hwnd;
}

int RunSelf() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        printf("FAIL CoInitialize\n");
        return 1;
    }
    HWND hwnd = MakeWindow(L"t067 self");
    PumpFor(150);

    ITaskbarList3* tb = CreateTaskbarList3();
    if (!tb) return 1;

    const std::vector<PropRecord> before = Snapshot(hwnd);
    printf("=== Q1: proxy state on our own window ===\n");
    Dump("baseline", hwnd);

    HRESULT hr1 = tb->SetProgressState(hwnd, TBPF_NORMAL);
    HRESULT hr2 = tb->SetProgressValue(hwnd, 37, 100);
    printf("SetProgressState(NORMAL) hr=0x%08lX  SetProgressValue(37,100) hr=0x%08lX\n", hr1, hr2);
    fflush(stdout);
    PumpFor(200);
    DumpNew("after NORMAL 37/100", hwnd, before);

    std::vector<PropRecord> prev = Snapshot(hwnd);
    tb->SetProgressValue(hwnd, 62, 100);
    PumpFor(200);
    DumpNew("after value 62/100 (state unchanged)", hwnd, prev);

    prev = Snapshot(hwnd);
    tb->SetProgressState(hwnd, TBPF_PAUSED);
    PumpFor(200);
    DumpNew("after TBPF_PAUSED", hwnd, prev);

    prev = Snapshot(hwnd);
    tb->SetProgressState(hwnd, TBPF_ERROR);
    PumpFor(200);
    DumpNew("after TBPF_ERROR", hwnd, prev);

    prev = Snapshot(hwnd);
    tb->SetProgressState(hwnd, TBPF_INDETERMINATE);
    PumpFor(200);
    DumpNew("after TBPF_INDETERMINATE", hwnd, prev);

    prev = Snapshot(hwnd);
    tb->SetProgressState(hwnd, TBPF_NOPROGRESS);
    PumpFor(200);
    DumpNew("after TBPF_NOPROGRESS", hwnd, prev);

    tb->Release();
    DestroyWindow(hwnd);
    CoUninitialize();
    return 0;
}

int RunSetter(int seconds) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    HWND hwnd = MakeWindow(L"t067 setter");
    ITaskbarList3* tb = CreateTaskbarList3();
    if (!tb) return 1;
    printf("SETTER hwnd=%lu pid=%lu seconds=%d\n", static_cast<unsigned long>(
              reinterpret_cast<UINT_PTR>(hwnd)),
           GetCurrentProcessId(), seconds);
    printf("SETTER state script: ramp 5..100 NORMAL, then PAUSED 2s, ERROR 2s, "
           "INDETERMINATE 2s, NOPROGRESS 2s, repeat\n");
    fflush(stdout);
    const double until = NowMs() + seconds * 1000.0;
    while (NowMs() < until) {
        for (int v = 5; v <= 100; v += 5) {
            tb->SetProgressState(hwnd, TBPF_NORMAL);
            tb->SetProgressValue(hwnd, static_cast<ULONGLONG>(v), 100);
            printf("SET value %d/100\n", v);
            fflush(stdout);
            PumpFor(400);
            if (NowMs() >= until) break;
        }
        tb->SetProgressState(hwnd, TBPF_PAUSED);
        printf("SET state PAUSED\n");
        fflush(stdout);
        PumpFor(2000);
        tb->SetProgressState(hwnd, TBPF_ERROR);
        printf("SET state ERROR\n");
        fflush(stdout);
        PumpFor(2000);
        tb->SetProgressState(hwnd, TBPF_INDETERMINATE);
        printf("SET state INDETERMINATE\n");
        fflush(stdout);
        PumpFor(2000);
        tb->SetProgressState(hwnd, TBPF_NOPROGRESS);
        printf("SET state NOPROGRESS\n");
        fflush(stdout);
        PumpFor(2000);
    }
    tb->Release();
    DestroyWindow(hwnd);
    CoUninitialize();
    return 0;
}

int RunReader(unsigned long hwnd_value, int seconds) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    HWND hwnd = reinterpret_cast<HWND>(static_cast<UINT_PTR>(hwnd_value));
    printf("READER target hwnd=%lu (foreign process) seconds=%d\n", hwnd_value, seconds);
    const double until = NowMs() + seconds * 1000.0;
    std::vector<PropRecord> prev;
    int rounds = 0;
    while (NowMs() < until) {
        if (!IsWindow(hwnd)) {
            printf("READER target window gone at round %d\n", rounds);
            break;
        }
        const std::vector<PropRecord> now = Snapshot(hwnd);
        bool same = now.size() == prev.size();
        if (same) {
            for (size_t i = 0; i < now.size(); ++i) {
                if (now[i].name != prev[i].name || now[i].value != prev[i].value ||
                    now[i].bytes != prev[i].bytes) {
                    same = false;
                    break;
                }
            }
        }
        if (!same || rounds == 0) {
            char tag[64];
            snprintf(tag, sizeof(tag), "reader round %d", rounds);
            Dump(tag, hwnd);
            prev = now;
        }
        ++rounds;
        PumpFor(400);
    }
    printf("READER finished after %d rounds\n", rounds);
    CoUninitialize();
    return 0;
}

// ---- Q3: explorer copy dialog (driven externally by Shell.Application) -------

void DumpMsaaRecursive(IAccessible* acc, int depth, int max_depth, int* found) {
    if (!acc || depth > max_depth) return;
    VARIANT child_id;
    child_id.vt = VT_I4;
    child_id.lVal = CHILDID_SELF;
    VARIANT role;
    BSTR val = nullptr, name = nullptr;
    VariantInit(&role);
    acc->get_accRole(child_id, &role);
    acc->get_accValue(child_id, &val);
    acc->get_accName(child_id, &name);
    const bool is_progress = role.vt == VT_I4 && role.lVal == ROLE_SYSTEM_PROGRESSBAR;
    if (is_progress) {
        printf("  msaa depth=%d PROGRESSBAR name='%ls' value='%ls'\n", depth, name ? name : L"",
               val ? val : L"");
        ++*found;
    }
    VariantClear(&role);
    SysFreeString(val);
    SysFreeString(name);

    long count = 0;
    acc->get_accChildCount(&count);
    if (count <= 0 || count > 512) return;
    VARIANT* children = static_cast<VARIANT*>(CoTaskMemAlloc(sizeof(VARIANT) * count));
    long got = 0;
    if (children && AccessibleChildren(acc, 0, count, children, &got) == S_OK && got > 0) {
        for (long i = 0; i < got; ++i) {
            if (children[i].vt != VT_DISPATCH || !children[i].pdispVal) continue;
            IAccessible* ca = nullptr;
            if (SUCCEEDED(children[i].pdispVal->QueryInterface(
                    kIidIAccessible, reinterpret_cast<void**>(&ca)))) {
                DumpMsaaRecursive(ca, depth + 1, max_depth, found);
                ca->Release();
            }
        }
        for (long i = 0; i < got; ++i) VariantClear(&children[i]);
    }
    if (children) CoTaskMemFree(children);
}

int DumpMsaa(HWND hwnd) {
    // DirectUI content exposes its accessibility tree on the embedded
    // "DirectUIHWND" child, not on the top frame - probe EVERY window in the
    // tree (measured earlier: top-frame OBJID_CLIENT yields nothing).
    int found = 0;
    IAccessible* acc = nullptr;
    VARIANT child_id;
    child_id.vt = VT_I4;
    child_id.lVal = CHILDID_SELF;
    HRESULT hr = AccessibleObjectFromWindow(hwnd, OBJID_WINDOW, kIidIAccessible,
                                            reinterpret_cast<void**>(&acc));
    if (SUCCEEDED(hr) && acc) {
        DumpMsaaRecursive(acc, 0, 6, &found);
        acc->Release();
    }
    HWND child = nullptr;
    while ((child = FindWindowExW(hwnd, child, nullptr, nullptr)) != nullptr) {
        found += DumpMsaa(child);
    }
    fflush(stdout);
    return found;
}

struct WatchCtx {
    HWND found;
    // When set, also report every NEW top-level window (any process) so the
    // probe cannot miss the dialog because of a wrong title/class guess.
    std::vector<HWND>* baseline;
    std::vector<std::pair<HWND, WCHAR*>>* news;
};

BOOL CALLBACK FindCopyDialog(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<WatchCtx*>(lp);
    if (ctx->baseline) {
        bool known = false;
        for (HWND b : *ctx->baseline) {
            if (b == hwnd) {
                known = true;
                break;
            }
        }
        if (!known && IsWindowVisible(hwnd)) {
            WCHAR* title = static_cast<WCHAR*>(calloc(160, sizeof(WCHAR)));
            GetWindowTextW(hwnd, title, 160);
            ctx->news->push_back({hwnd, title});
        }
    }
    WCHAR cls_check[128] = L"";
    GetClassNameW(hwnd, cls_check, 128);
    if (wcscmp(cls_check, L"OperationStatusWindow") == 0) {
        // Explorer's copy-engine progress dialog (measured: the class exists,
        // the title is EMPTY, so a title match alone never fires).
        ctx->found = hwnd;
        return FALSE;
    }
    if (!IsWindowVisible(hwnd)) return TRUE;
    // Skip our own console/tool windows.
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    WCHAR title[160] = L"";
    if (GetWindowTextW(hwnd, title, 160) == 0) return TRUE;
    if (wcsstr(title, L"复制") || wcsstr(title, L"Copying") || wcsstr(title, L"移动") ||
        wcsstr(title, L"Moving")) {
        ctx->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

BOOL CALLBACK CollectVisible(HWND hwnd, LPARAM lp) {
    auto* out = reinterpret_cast<std::vector<HWND>*>(lp);
    if (IsWindowVisible(hwnd)) out->push_back(hwnd);
    return TRUE;
}

int RunWatch(int seconds) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    printf("=== Q3: watch explorer copy dialog (any process), %d s ===\n", seconds);
    printf("start the copy NOW (e.g. PowerShell Shell.Application CopyHere of a big file)\n");
    fflush(stdout);
    // Baseline of visible top-level windows, so newly created ones stand out.
    std::vector<HWND> baseline;
    EnumWindows(CollectVisible, reinterpret_cast<LPARAM>(&baseline));
    const double until = NowMs() + seconds * 1000.0;
    HWND seen = nullptr;
    int rounds = 0, msaa_reports = 0;
    while (NowMs() < until) {
        PumpFor(300);
        ++rounds;
        WatchCtx ctx{nullptr, &baseline, nullptr};
        std::vector<std::pair<HWND, WCHAR*>> news;
        ctx.news = &news;
        EnumWindows(FindCopyDialog, reinterpret_cast<LPARAM>(&ctx));
        for (auto& [wh, title] : news) {
            DWORD pid = 0;
            GetWindowThreadProcessId(wh, &pid);
            WCHAR cls[128] = L"";
            GetClassNameW(wh, cls, 128);
            printf("NEW window hwnd=%p class='%ls' title='%ls' pid=%lu\n",
                   static_cast<void*>(wh), cls, title, pid);
            baseline.push_back(wh);
            free(title);
        }
        if (!ctx.found) {
            if (rounds % 10 == 0) printf("round %d: no copy dialog\n", rounds);
            continue;
        }
        if (ctx.found != seen) {
            seen = ctx.found;
            DWORD pid = 0;
            GetWindowThreadProcessId(seen, &pid);
            WCHAR cls[128] = L"";
            GetClassNameW(seen, cls, 128);
            printf("round %d: dialog hwnd=%p class='%ls' pid=%lu\n", rounds,
                   static_cast<void*>(seen), cls, pid);
            // Full window-property dump of the dialog AND its child window tree:
            // if the copy engine stashes proxy state anywhere on its HWNDs, this
            // is where it shows. Foreign process: expect "not readable" handles.
            Dump("dialog", seen);
            HWND child = nullptr;
            while ((child = FindWindowExW(seen, child, nullptr, nullptr)) != nullptr) {
                Dump("dialog-child", child);
                HWND grand = nullptr;
                while ((grand = FindWindowExW(child, grand, nullptr, nullptr)) != nullptr) {
                    Dump("dialog-grandchild", grand);
                }
            }
        }
        msaa_reports += DumpMsaa(ctx.found);
    }
    printf("watch done: %d rounds, %d msaa progress reports\n", rounds, msaa_reports);
    CoUninitialize();
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && wcscmp(argv[1], L"self") == 0) return RunSelf();
    if (argc >= 2 && wcscmp(argv[1], L"setter") == 0) {
        int seconds = argc >= 3 ? _wtoi(argv[2]) : 30;
        return RunSetter(seconds);
    }
    if (argc >= 3 && wcscmp(argv[1], L"reader") == 0) {
        unsigned long hwnd = wcstoul(argv[2], nullptr, 10);
        int seconds = argc >= 4 ? _wtoi(argv[3]) : 20;
        return RunReader(hwnd, seconds);
    }
    if (argc >= 2 && wcscmp(argv[1], L"watch") == 0) {
        int seconds = argc >= 3 ? _wtoi(argv[2]) : 45;
        return RunWatch(seconds);
    }
    printf("usage: spike.exe self | setter [secs] | reader <hwnd> [secs] | watch [secs]\n");
    return 2;
}
