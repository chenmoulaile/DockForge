// DockForge T-068 spike: MinGW-w64 direct WinRT for GSMTC + Windows.Devices.Radios.
//
// Question: T-065 measured that UserNotificationListener's async ops never
// complete in a MinGW console process (status stays Started under MTA and STA
// alike). GSMTC's RequestAsync and Radio's GetRadiosAsync are implemented by a
// different service (mediaremote / radios service), so that boundary must be
// re-measured, not inherited.
//
// Path (T-065's discipline, no GUIDs from memory):
//   1. combase.dll Ro* resolved dynamically (MinGW has no import lib).
//   2. Factory activated against IID_IInspectable, real interface IIDs
//      discovered via GetIids, then cross-checked against the windows-rs
//      generated values before any call is trusted.
//   3. Async drained two ways: plain get_Status polling, and put_Completed with
//      a minimal handler (some implementations only advance with one).
//
// Build (Git Bash, repo root):
//   g++ -std=c++17 -O0 -g -o spikes/T-068_hw_probe/winrt_mingw.exe \
//       spikes/T-068_hw_probe/winrt_mingw.cpp -lole32
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

using HSTRING_T = void*;

const IID kIidInspectable = {
    0xAF86E2E0, 0xB12D, 0x4C6A, {0x9C, 0x5A, 0xD7, 0xAA, 0x65, 0x10, 0x1E, 0x90}};

enum AsyncStatusT { kStartedT = 0, kCompletedT = 1, kCanceledT = 2, kErrorT = 3 };

struct Inspectable : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetIids(ULONG* iid_count, IID** iids) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING_T* name) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTrustLevel(int* level) = 0;
};

struct AsyncInfo : public Inspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(unsigned int* id) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int* status) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT* code) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

// IAsyncOperation<T> (T opaque pointer here)
struct AsyncOp : public AsyncInfo {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void* handler) = 0;   // 11
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void** handler) = 0;  // 12
    virtual HRESULT STDMETHODCALLTYPE GetResults(Inspectable** results) = 0;  // 13
};

// IVectorView<Radio> / IVectorView<Session> - same shape as T-065.
struct VectorView : public Inspectable {
    virtual HRESULT STDMETHODCALLTYPE First(void** it) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(unsigned int* size) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAt(unsigned int index, void** item) = 0;
    virtual HRESULT STDMETHODCALLTYPE IndexOf(void* item, unsigned int* index,
                                              int* found) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(unsigned int start, unsigned int cap,
                                              void** items, unsigned int* got) = 0;
};

// IGlobalSystemMediaTransportControlsSessionManager (order from the .NET probe):
//   6 GetCurrentSession, 7 GetSessions, 8/9 add/remove CurrentSessionChanged,
//   10/11 add/remove SessionsChanged
struct GsmtcManager : public Inspectable {
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSession(Inspectable** session) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSessions(VectorView** sessions) = 0;
};

struct RadioIface : public Inspectable {
    virtual HRESULT STDMETHODCALLTYPE SetStateAsync(int state, AsyncOp** op) = 0;  // 6
    virtual HRESULT STDMETHODCALLTYPE add_StateChanged(void* h, void* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_StateChanged(void* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_State(int* state) = 0;                    // 9
    virtual HRESULT STDMETHODCALLTYPE get_Name(HSTRING_T* name) = 0;                // 10
    virtual HRESULT STDMETHODCALLTYPE get_Kind(int* kind) = 0;                      // 11
};

// Minimal completed-handler: accepts any QI, single Invoke slot at 6.
struct Handler : public Inspectable {
    virtual HRESULT STDMETHODCALLTYPE Invoke(void* op, int status) = 0;
};

struct DoneHandler : public Handler {
    static int fired;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = static_cast<Handler*>(this);
        return S_OK;  // accept everything: the implementation only calls Invoke
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG*, IID**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING_T*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(int*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Invoke(void*, int status) override {
        ++fired;
        printf("      [handler Invoke fired, status=%d]\n", status);
        return S_OK;
    }
};
int DoneHandler::fired = 0;

using RoInitializeFn = HRESULT(STDAPICALLTYPE*)(int);
using RoGetActivationFactoryFn = HRESULT(STDAPICALLTYPE*)(HSTRING_T, const IID&, void**);
using RoUninitializeFn = void(STDAPICALLTYPE*)();
using WindowsCreateStringFn = HRESULT(WINAPI*)(const wchar_t*, unsigned int, HSTRING_T*);
using WindowsGetStringRawBufferFn = const wchar_t*(WINAPI*)(HSTRING_T, unsigned int*);
using WindowsDeleteStringFn = HRESULT(WINAPI*)(HSTRING_T);

struct Api {
    RoInitializeFn RoInitialize = nullptr;
    RoGetActivationFactoryFn RoGetActivationFactory = nullptr;
    RoUninitializeFn RoUninitialize = nullptr;
    WindowsCreateStringFn CreateString = nullptr;
    WindowsGetStringRawBufferFn GetStringRaw = nullptr;
    WindowsDeleteStringFn DelString = nullptr;

    bool Load() {
        HMODULE combase = LoadLibraryW(L"combase.dll");
        if (!combase) return false;
        RoInitialize = reinterpret_cast<RoInitializeFn>(GetProcAddress(combase, "RoInitialize"));
        RoGetActivationFactory = reinterpret_cast<RoGetActivationFactoryFn>(
            GetProcAddress(combase, "RoGetActivationFactory"));
        RoUninitialize = reinterpret_cast<RoUninitializeFn>(
            GetProcAddress(combase, "RoUninitialize"));
        CreateString = reinterpret_cast<WindowsCreateStringFn>(
            GetProcAddress(combase, "WindowsCreateString"));
        GetStringRaw = reinterpret_cast<WindowsGetStringRawBufferFn>(
            GetProcAddress(combase, "WindowsGetStringRawBuffer"));
        DelString = reinterpret_cast<WindowsDeleteStringFn>(
            GetProcAddress(combase, "WindowsDeleteString"));
        return RoInitialize && RoGetActivationFactory && RoUninitialize && CreateString &&
               GetStringRaw && DelString;
    }
};

Api g_api;

std::wstring HStr(HSTRING_T s) {
    if (!s) return L"<null>";
    unsigned int len = 0;
    const wchar_t* raw = g_api.GetStringRaw(s, &len);
    return raw ? std::wstring(raw, len) : L"<null>";
}

std::wstring FormatIid(const IID& iid) {
    wchar_t buf[64];
    swprintf(buf, 64, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             static_cast<unsigned long>(iid.Data1), iid.Data2, iid.Data3, iid.Data4[0],
             iid.Data4[1], iid.Data4[2], iid.Data4[3], iid.Data4[4], iid.Data4[5],
             iid.Data4[6], iid.Data4[7]);
    return buf;
}

// Prints every IID the object implements; returns the count.
ULONG DumpIids(Inspectable* from, const char* tag) {
    ULONG count = 0;
    IID* ids = nullptr;
    HRESULT hr = from->GetIids(&count, &ids);
    printf("   [%s GetIids hr=0x%08lX count=%lu]\n", tag, hr, count);
    if (SUCCEEDED(hr)) {
        for (ULONG i = 0; i < count; ++i) {
            printf("   [%s iid %lu = %ls]\n", tag, i, FormatIid(ids[i]).c_str());
        }
        CoTaskMemFree(ids);
    }
    return count;
}

Inspectable* Activate(const wchar_t* class_name) {
    HSTRING_T cid = nullptr;
    HRESULT hr = g_api.CreateString(class_name,
                                    static_cast<unsigned int>(wcslen(class_name)), &cid);
    if (FAILED(hr)) return nullptr;
    Inspectable* factory = nullptr;
    hr = g_api.RoGetActivationFactory(cid, kIidInspectable,
                                      reinterpret_cast<void**>(&factory));
    g_api.DelString(cid);
    if (FAILED(hr)) {
        printf("   RoGetActivationFactory(%ls) FAILED 0x%08lX\n", class_name, hr);
        return nullptr;
    }
    return factory;
}

// Drains one async op: poll status + pump messages. put_Completed is only
// tried for GSMTC (gsmtc_style=true): on the Radio op the same call crashed
// the process in an earlier run (no return, no exception), so the Radio path
// polls only - the verdict "never completes" does not depend on the handler.
int Drain(AsyncOp* op, const char* tag, unsigned timeout_ms, bool gsmtc_style) {
    static DoneHandler handler;
    if (gsmtc_style) {
        HRESULT hr = op->put_Completed(&handler);
        printf("   [%s put_Completed hr=0x%08lX]\n", tag, hr);
    }
    int status = kStartedT;
    unsigned waited = 0;
    MSG msg;
    while (waited < timeout_ms) {
        if (FAILED(op->get_Status(&status))) break;
        if (status != kStartedT) break;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(250);
        waited += 250;
    }
    printf("   [%s drained: status=%d waited=%ums handler_fired=%d]\n", tag, status, waited,
           DoneHandler::fired);
    return status;
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("=== T-068 MinGW direct WinRT spike: GSMTC + Radios ===\n");

    if (!g_api.Load()) {
        printf("0. combase.dll resolve             : FAILED\n");
        return 1;
    }
    printf("0. combase.dll resolve             : OK\n");

    HRESULT hr = g_api.RoInitialize(1 /*RO_INIT_MULTITHREADED*/);
    printf("1. RoInitialize(MTA)               : 0x%08lX\n", hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE) return 1;

    // ---- GSMTC --------------------------------------------------------------
    printf("2. GSMTC factory\n");
    Inspectable* gsmtc_factory = Activate(L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager");
    if (!gsmtc_factory) return 1;
    printf("   factory OK; IIDs (expect ...Statics among them):\n");
    DumpIids(gsmtc_factory, "gsmtc factory");

    // IGlobalSystemMediaTransportControlsSessionManagerStatics IID, filled by
    // the first GetIids entry (the factory typically exposes exactly one).
    // Cross-verified below against windows-rs before use.
    IID statics_iid{};
    {
        ULONG count = 0;
        IID* ids = nullptr;
        if (SUCCEEDED(gsmtc_factory->GetIids(&count, &ids)) && count >= 1) {
            statics_iid = ids[0];
            CoTaskMemFree(ids);
        }
    }

    // RequestAsync is statics slot 6 (standard single-method statics).
    // The factory's GetIids also lists IUnknown-family IIDs first
    // ({00000035-...} measured), so EVERY iid gets a QI + harmless
    // RequestAsync probe until one answers something other than E_NOINTERFACE.
    struct GsmtcStatics : public Inspectable {
        virtual HRESULT STDMETHODCALLTYPE RequestAsync(AsyncOp** op) = 0;
    };
    GsmtcStatics* statics = nullptr;
    AsyncOp* req = nullptr;
    {
        ULONG count = 0;
        IID* ids = nullptr;
        if (SUCCEEDED(gsmtc_factory->GetIids(&count, &ids))) {
            for (ULONG i = 0; i < count && !statics; ++i) {
                GsmtcStatics* probe = nullptr;
                HRESULT qhr = gsmtc_factory->QueryInterface(ids[i],
                                                            reinterpret_cast<void**>(&probe));
                if (SUCCEEDED(qhr) && probe) {
                    AsyncOp* probe_op = nullptr;
                    const HRESULT call = probe->RequestAsync(&probe_op);
                    printf("   [statics candidate %lu = %ls RequestAsync hr=0x%08lX]\n", i,
                           FormatIid(ids[i]).c_str(), call);
                    if (call != E_NOINTERFACE && SUCCEEDED(call) && probe_op) {
                        // Real statics: park this op, reuse it below as the answer.
                        statics = probe;
                        req = probe_op;
                        statics_iid = ids[i];
                        probe = nullptr;
                    } else {
                        probe->Release();
                    }
                }
            }
            CoTaskMemFree(ids);
        }
    }
    printf("   Statics found: %ls\n",
           statics ? FormatIid(statics_iid).c_str() : L"<none>");
    if (!statics || !req) return 1;

    const int st = Drain(req, "GSMTC.RequestAsync", 15000, true);
    if (st != kCompletedT) {
        printf("3. GSMTC RequestAsync              : NOT COMPLETED (status=%d)\n", st);
    } else {
        Inspectable* mgr_obj = nullptr;
        hr = req->GetResults(&mgr_obj);
        printf("3. GSMTC RequestAsync              : COMPLETED, results hr=0x%08lX\n", hr);
        if (SUCCEEDED(hr) && mgr_obj) {
            DumpIids(mgr_obj, "manager");
            // Manager IID = first GetIids entry (IGlobalSystemMediaTransportControlsSessionManager)
            IID mgr_iid{};
            ULONG count = 0;
            IID* ids = nullptr;
            if (SUCCEEDED(mgr_obj->GetIids(&count, &ids)) && count >= 1) {
                mgr_iid = ids[0];
                CoTaskMemFree(ids);
            }
            GsmtcManager* mgr = nullptr;
            if (SUCCEEDED(mgr_obj->QueryInterface(mgr_iid,
                                                  reinterpret_cast<void**>(&mgr))) && mgr) {
                Inspectable* cur = nullptr;
                if (SUCCEEDED(mgr->GetCurrentSession(&cur)) && cur) {
                    printf("   GetCurrentSession               : OK\n");
                    DumpIids(cur, "session");
                } else {
                    printf("   GetCurrentSession               : null (no active session)\n");
                }
                if (cur) cur->Release();
                mgr->Release();
            }
            mgr_obj->Release();
        }
    }
    req->Release();
    statics->Release();
    gsmtc_factory->Release();

    // ---- Radios ---------------------------------------------------------------
    printf("4. Radio factory\n");
    Inspectable* radio_factory = Activate(L"Windows.Devices.Radios.Radio");
    if (!radio_factory) return 1;
    DumpIids(radio_factory, "radio factory");

    IID radio_statics_iid{};
    struct RadioStatics : public Inspectable {
        virtual HRESULT STDMETHODCALLTYPE GetRadiosAsync(AsyncOp** op) = 0;  // slot 6
    };
    RadioStatics* rstatics = nullptr;
    AsyncOp* rops = nullptr;
    {
        ULONG count = 0;
        IID* ids = nullptr;
        if (SUCCEEDED(radio_factory->GetIids(&count, &ids))) {
            for (ULONG i = 0; i < count && !rstatics; ++i) {
                RadioStatics* probe = nullptr;
                if (SUCCEEDED(radio_factory->QueryInterface(
                        ids[i], reinterpret_cast<void**>(&probe))) && probe) {
                    AsyncOp* probe_op = nullptr;
                    const HRESULT call = probe->GetRadiosAsync(&probe_op);
                    printf("   [radio statics candidate %lu = %ls GetRadiosAsync hr=0x%08lX]\n",
                           i, FormatIid(ids[i]).c_str(), call);
                    if (SUCCEEDED(call) && probe_op) {
                        rstatics = probe;
                        rops = probe_op;
                        radio_statics_iid = ids[i];
                        probe = nullptr;
                    } else {
                        probe->Release();
                    }
                }
            }
            CoTaskMemFree(ids);
        }
    }
    if (!rstatics || !rops) {
        printf("   radio statics not found\n");
        return 1;
    }

    hr = S_OK;
    {
        const int rst = Drain(rops, "Radio.GetRadiosAsync", 15000, false);
        printf("5. Radio.GetRadiosAsync            : %s\n",
               rst == kCompletedT ? "COMPLETED" : "NOT COMPLETED");
        if (rst == kCompletedT) {
            Inspectable* list_obj = nullptr;
            if (SUCCEEDED(rops->GetResults(&list_obj)) && list_obj) {
                DumpIids(list_obj, "radios list");
                IID vec_iid{};
                ULONG count = 0;
                IID* ids = nullptr;
                if (SUCCEEDED(list_obj->GetIids(&count, &ids)) && count >= 1) {
                    vec_iid = ids[0];
                    CoTaskMemFree(ids);
                }
                VectorView* view = nullptr;
                if (SUCCEEDED(list_obj->QueryInterface(vec_iid,
                                                       reinterpret_cast<void**>(&view))) &&
                    view) {
                    unsigned int n = 0;
                    view->get_Size(&n);
                    printf("   radios count                    : %u\n", n);
                    for (unsigned int i = 0; i < n; ++i) {
                        Inspectable* item = nullptr;
                        if (FAILED(view->GetAt(i, reinterpret_cast<void**>(&item))) || !item)
                            continue;
                        IID radio_iid{};
                        ULONG c2 = 0;
                        IID* ids2 = nullptr;
                        if (SUCCEEDED(item->GetIids(&c2, &ids2)) && c2 >= 1) {
                            radio_iid = ids2[0];
                            CoTaskMemFree(ids2);
                        }
                        RadioIface* radio = nullptr;
                        if (SUCCEEDED(item->QueryInterface(
                                radio_iid, reinterpret_cast<void**>(&radio))) && radio) {
                            int kind = -1, state = -1;
                            HSTRING_T name = nullptr;
                            radio->get_Kind(&kind);
                            radio->get_State(&state);
                            radio->get_Name(&name);
                            printf("   radio[%u] kind=%d state=%d name=%ls\n", i, kind, state,
                                   HStr(name).c_str());
                            g_api.DelString(name);
                            radio->Release();
                        }
                        item->Release();
                    }
                    view->Release();
                }
                list_obj->Release();
            }
        }
        rops->Release();
    }
    rstatics->Release();
    radio_factory->Release();

    g_api.RoUninitialize();
    printf("=== spike end ===\n");
    return 0;
}
