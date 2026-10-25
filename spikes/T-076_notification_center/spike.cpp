// DockForge T-076 spike: can a MESSAGE-PUMPING process complete
// UserNotificationListener::GetNotificationsAsync?
//
// Why this spike exists (T-065 left exactly one open boundary):
//   spikes/T-065_notification_probe/spike.cpp measured that in a plain console
//   process the IAsyncOperation returned by GetNotificationsAsync stays in
//   AsyncStatus::Started forever - under RO_INIT_MULTITHREADED and
//   RO_INIT_SINGLETHREADED alike - while the .NET projection completes the same
//   call in ~1s. That spike blocked the thread with Sleep() and never pumped a
//   message queue, so it did NOT answer the question that actually matters for
//   DockForge: the dock is a GUI process with a live message loop.
//
// Questions measured here:
//   Q1  STA (RO_INIT_SINGLETHREADED) + a real window + GetMessage/DispatchMessage
//       pump, with an IAsyncOperationCompletedHandler installed via
//       put_Completed: does the handler fire, and does get_Status reach
//       Completed?  <-- decisive
//   Q2  Same process, same apartment, NO completed handler: does merely pumping
//       the queue move get_Status to Completed?  (distinguishes "needs a pump"
//       from "needs a handler")
//   Q3  With the result in hand: are id / creation time / AppUserModelId
//       readable through the frozen ABI order, and how many notifications exist?
//   Q4  Does ClearNotifications stay untouched (this spike must not mutate user
//       state) and does GetNotification(id) round-trip for one id?
//
// Build (Git Bash, repo root):
//   g++ -std=c++17 -O0 -g -o spikes/T-076_notification_center/spike.exe \
//       spikes/T-076_notification_center/spike.cpp -static -municode
//
// ABI order reused verbatim from the T-065 spike (measured there, not guessed):
//   IUserNotificationListenerStatics: get_Current = 6
//   IUserNotificationListener: RequestAccessAsync=6, GetAccessStatus=7 (SYNC),
//     add_NotificationChanged=8, remove_NotificationChanged=9,
//     GetNotificationsAsync=10, GetNotification=11, ClearNotifications=12,
//     RemoveNotification=13
//   IVectorView<T>: First=6, get_Size=7, GetAt=8, IndexOf=9, GetMany=10
//   IAsyncInfo: get_Id=6, get_Status=7, get_ErrorCode=8, Cancel=9, Close=10
//   IAsyncOperation<T>: put_Completed=11, get_Completed=12, GetResults=13
//   IUserNotification: get_Notification=6, get_AppInfo=7, get_Id=8,
//     get_CreationTime=9   (this OS build has no AppId on IUserNotification)
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

using HSTRING_T = void*;

const IID kIidInspectable = {
    0xAF86E2E0, 0xB12D, 0x4C6A, {0x9C, 0x5A, 0xD7, 0xAA, 0x65, 0x10, 0x1E, 0x90}};
// IID_IUnknown {00000000-0000-0000-C000-000000000046} - spelled out to keep the
// spike free of a libuuid dependency.
const IID kIidUnknown = {0x00000000, 0x0000, 0x0000,
                         {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const IID kIidStatics = {0xFF6123CF, 0x4386, 0x4AA3,
                         {0xB7, 0x3D, 0xB8, 0x04, 0xE5, 0xB6, 0x3B, 0x23}};
const IID kIidListener = {0x62553E41, 0x8A06, 0x4CEF,
                          {0x82, 0x15, 0x60, 0x33, 0xA5, 0xBE, 0x4B, 0x03}};
// IID_IAgileObject {94EA2B94-E9CC-49E0-C0FF-EE64CA8F5B90} - a WinRT delegate must
// answer QI for this or the marshaller refuses to hand it across apartments.
const IID kIidAgileObject = {0x94EA2B94, 0xE9CC, 0x49E0,
                             {0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90}};

enum AsyncStatusT { kCanceled = 2, kCompleted = 1, kError = 3, kStarted = 0 };

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

template <typename T>
struct AsyncOperation : public AsyncInfo {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void** handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(T* results) = 0;
};

struct VectorView : public Inspectable {
    virtual HRESULT STDMETHODCALLTYPE First(void** it) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(unsigned int* size) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAt(unsigned int index, void** item) = 0;
    virtual HRESULT STDMETHODCALLTYPE IndexOf(void* item, unsigned int* index,
                                              int* found) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(unsigned int start, unsigned int cap,
                                              void** items, unsigned int* got) = 0;
};

struct UserNotification : public Inspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Notification(void** n) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AppInfo(void** info) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Id(unsigned int* id) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_CreationTime(void* time) = 0;
};

struct AppInfo : public Inspectable {
    virtual HRESULT STDMETHODCALLTYPE get_AppUserModelId(HSTRING_T* id) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DisplayInfo(void** info) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Id(HSTRING_T* id) = 0;
};

struct Listener : public Inspectable {
    virtual HRESULT STDMETHODCALLTYPE RequestAccessAsync(AsyncOperation<int>** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAccessStatus(int* status) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_NotificationChanged(void* handler,
                                                              void* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_NotificationChanged(void* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNotificationsAsync(
        unsigned int kinds, AsyncOperation<VectorView*>** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNotification(unsigned int id,
                                                      UserNotification** n) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearNotifications() = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveNotification(unsigned int id) = 0;
};

struct Statics : public Inspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Current(Listener** current) = 0;
};

// A minimal WinRT delegate. IAsyncOperationCompletedHandler<T> is
// IUnknown + Invoke at slot 3; the generic argument does not change the layout.
//
// MEASURED (first two runs of this spike): installing this handler and then
// pumping fastfails the process with 0xC0000409 right after put_Completed
// returns S_OK, even when QueryInterface answers exactly IUnknown+IAgileObject.
// The raw ABI expects a delegate that is also a full IInspectable (GetIids /
// GetRuntimeClassName / GetTrustLevel), which a 4-slot vtable cannot provide.
// The handler is therefore NOT on the critical path: the dock polls get_Status
// from its UI tick while the ordinary message loop runs (C-04b). It is kept
// behind a switch so the boundary stays documented and reproducible.
struct CompletedHandler : public IUnknown {
    LONG refs = 1;
    volatile LONG fired = 0;
    int last_status = -1;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        // Answer IUnknown and IAgileObject only. Reporting IAgileObject is what
        // tells WinRT "no marshaling needed" - and answering *everything* with
        // S_OK (the first version of this spike) makes the marshaler QI for
        // IMarshal, get this pointer back, and call slot 3 (Invoke) as
        // GetUnmarshalClass, which fastfails the process with 0xC0000409.
        if (IsEqualIID(riid, kIidUnknown) || IsEqualIID(riid, kIidAgileObject)) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refs));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG left = InterlockedDecrement(&refs);
        if (left == 0) delete this;
        return static_cast<ULONG>(left);
    }
    virtual HRESULT STDMETHODCALLTYPE Invoke(AsyncInfo* op, int status) = 0;
};

struct CompletedHandlerImpl : public CompletedHandler {
    HRESULT STDMETHODCALLTYPE Invoke(AsyncInfo* op, int status) override {
        last_status = status;
        InterlockedExchange(&fired, 1);
        printf("   [handler fired: status=%d]\n", status);
        return S_OK;
    }
};

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
        RoInitialize = reinterpret_cast<RoInitializeFn>(
            GetProcAddress(combase, "RoInitialize"));
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
        return RoInitialize && RoGetActivationFactory && RoUninitialize &&
               CreateString && GetStringRaw && DelString;
    }
};

Api g_api;

std::wstring HStr(HSTRING_T s) {
    if (!s) return L"<null>";
    unsigned int len = 0;
    const wchar_t* raw = g_api.GetStringRaw(s, &len);
    return raw ? std::wstring(raw, len) : L"<null>";
}

// The hidden window exists only to give this thread a real message queue, which
// is the condition the T-065 console spike never satisfied.
HWND g_pump_window = nullptr;

bool CreatePumpWindow(HINSTANCE inst) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = L"DockForgeT076SpikePump";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    g_pump_window = CreateWindowExW(0, wc.lpszClassName, L"pump", 0, 0, 0, 0, 0,
                                    HWND_MESSAGE, nullptr, inst, nullptr);
    return g_pump_window != nullptr;
}

// Reports the faulting code, address and owning module before the process dies.
// Without this a 0xC0000005/0xC0000409 in a spike is just a number.
LONG WINAPI VehHandler(EXCEPTION_POINTERS* info) {
    wchar_t module_path[MAX_PATH] = L"<unknown>";
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(info->ExceptionRecord->ExceptionAddress),
                           &mod) &&
        mod) {
        GetModuleFileNameW(mod, module_path, MAX_PATH);
    }
    printf("   [FAULT code=0x%08lX address=%p module=%ls]\n",
           info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress,
           module_path);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Pumps the queue for `budget_ms`, returning early when `done()` says stop.
template <typename Done>
void PumpFor(unsigned int budget_ms, Done done) {
    const ULONGLONG deadline = GetTickCount64() + budget_ms;
    int rounds = 0;
    while (!done() && GetTickCount64() < deadline) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (++rounds % 50 == 0) printf("   [pump round %d]\n", rounds);
        Sleep(10);
    }
}

// Reads the status once; returns false if the call itself failed.
bool Status(AsyncInfo* op, int* out) {
    return op && SUCCEEDED(op->get_Status(out));
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    AddVectoredExceptionHandler(1, VehHandler);
    printf("=== T-076 spike: GetNotificationsAsync under a message pump ===\n");

    if (!g_api.Load()) {
        printf("1. LoadLibrary(combase.dll) : FAILED (GetLastError=%lu)\n", GetLastError());
        return 1;
    }
    printf("1. combase.dll              : OK\n");

    // RO_INIT_SINGLETHREADED == 0. STA is what a Win32 UI thread uses and is the
    // condition the dock runs in.
    HRESULT hr = g_api.RoInitialize(0);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE) {
        printf("2. RoInitialize(STA)        : FAILED 0x%08lX\n", hr);
        return 1;
    }
    printf("2. RoInitialize(STA)        : 0x%08lX\n", hr);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!CreatePumpWindow(inst)) {
        printf("3. message-queue window     : FAILED (GetLastError=%lu)\n", GetLastError());
        return 1;
    }
    printf("3. message-queue window     : OK (this thread now owns a message queue)\n");

    HSTRING_T class_id = nullptr;
    const wchar_t* kClass = L"Windows.UI.Notifications.Management.UserNotificationListener";
    hr = g_api.CreateString(kClass, static_cast<unsigned int>(wcslen(kClass)), &class_id);
    if (FAILED(hr)) {
        printf("4. WindowsCreateString      : FAILED 0x%08lX\n", hr);
        return 1;
    }
    Inspectable* factory = nullptr;
    hr = g_api.RoGetActivationFactory(class_id, kIidInspectable,
                                      reinterpret_cast<void**>(&factory));
    g_api.DelString(class_id);
    if (FAILED(hr)) {
        printf("4. RoGetActivationFactory   : FAILED 0x%08lX\n", hr);
        return 1;
    }
    printf("4. RoGetActivationFactory   : OK\n");

    Statics* statics = nullptr;
    hr = factory->QueryInterface(kIidStatics, reinterpret_cast<void**>(&statics));
    if (FAILED(hr)) {
        printf("5. QI(Statics)              : FAILED 0x%08lX\n", hr);
        return 1;
    }
    Listener* listener = nullptr;
    hr = statics->get_Current(&listener);
    if (FAILED(hr) || !listener) {
        printf("5. get_Current              : FAILED 0x%08lX\n", hr);
        return 1;
    }
    int access = -1;
    listener->GetAccessStatus(&access);
    printf("5. GetAccessStatus          : %d (0=Unspecified 1=Allowed 2=Denied 4=DeniedBySystem)\n",
           access);

    // ---- Q1 (decisive): STA + pump, no handler ----
    // This is exactly what the dock will do: the UI thread owns the message loop,
    // a tick polls get_Status, and nothing marshals across apartments.
    printf("Q1 STA + pump, no handler installed\n");
    AsyncOperation<VectorView*>* op1 = nullptr;
    hr = listener->GetNotificationsAsync(1 /* Toast */, &op1);
    if (FAILED(hr) || !op1) {
        printf("   Q1 call                 : FAILED 0x%08lX\n", hr);
        return 1;
    }
    int st1 = kStarted;
    PumpFor(8000, [&] {
        Status(op1, &st1);
        return st1 != kStarted;
    });
    Status(op1, &st1);
    const bool q1_ok = (st1 == kCompleted);
    printf("   Q1 result               : status=%d => %s\n", st1, q1_ok ? "PASS" : "FAIL");

    VectorView* view = nullptr;
    if (q1_ok) {
        hr = op1->GetResults(&view);
        printf("   GetResults              : 0x%08lX view=%p\n", hr, (void*)view);
    }

    // ---- Q2: the same call with put_Completed ----
    // Measured to fastfail 0xC0000409 after put_Completed returns S_OK: a
    // 4-slot delegate is not a WinRT delegate (no IInspectable). Kept as a
    // reproducible boundary record; skipped unless explicitly requested.
    printf("Q2 STA + pump + put_Completed handler\n");
    if (GetEnvironmentVariableW(L"DOCKFORGE_T076_TRY_HANDLER", nullptr, 0) == 0) {
        printf("   Q2 result               : SKIPPED (set DOCKFORGE_T076_TRY_HANDLER=1 to reproduce the fastfail)\n");
    } else {
        AsyncOperation<VectorView*>* op2 = nullptr;
        hr = listener->GetNotificationsAsync(1, &op2);
        if (FAILED(hr) || !op2) {
            printf("   Q2 call                 : FAILED 0x%08lX\n", hr);
        } else {
            CompletedHandlerImpl* handler = new CompletedHandlerImpl();
            hr = op2->put_Completed(handler);
            printf("   put_Completed           : 0x%08lX\n", hr);
            int st2 = kStarted;
            PumpFor(8000, [&] {
                Status(op2, &st2);
                return handler->fired != 0 || st2 != kStarted;
            });
            Status(op2, &st2);
            printf("   Q2 result               : fired=%ld status=%d => %s\n", handler->fired,
                   st2, st2 == kCompleted ? "PASS" : "FAIL");
            handler->Release();
            op2->Release();
        }
    }

    // ---- Q3: read the payload through the frozen ABI ----
    if (!view) {
        printf("Q3 payload                  : SKIPPED (no IVectorView)\n");
        printf("Q4 GetNotification round-trip: SKIPPED\n");
        listener->Release();
        statics->Release();
        factory->Release();
        g_api.RoUninitialize();
        printf("=== spike end: async NOT usable in-process ===\n");
        return q1_ok ? 0 : 2;
    }
    unsigned int count = 0;
    view->get_Size(&count);
    printf("Q3 payload                  : %u notifications\n", count);
    unsigned int read = 0;
    unsigned int first_id = 0;
    for (unsigned int i = 0; i < count && read < 8; ++i) {
        UserNotification* n = nullptr;
        if (FAILED(view->GetAt(i, reinterpret_cast<void**>(&n))) || !n) continue;
        unsigned int id = 0;
        void* appinfo_raw = nullptr;
        std::wstring aumid = L"<none>";
        if (SUCCEEDED(n->get_Id(&id))) {
            if (first_id == 0) first_id = id;
            if (SUCCEEDED(n->get_AppInfo(&appinfo_raw)) && appinfo_raw) {
                auto* ai = static_cast<AppInfo*>(appinfo_raw);
                HSTRING_T aumid_h = nullptr;
                if (SUCCEEDED(ai->get_AppUserModelId(&aumid_h)) && aumid_h) {
                    aumid = HStr(aumid_h);
                    g_api.DelString(aumid_h);
                }
                ai->Release();
            }
            printf("   [%u] id=%-6u aumid=%ls\n", i, id, aumid.c_str());
            ++read;
        }
        n->Release();
    }
    printf("   read %u of %u (id / AppUserModelId via slots 8 / 7)\n", read, count);

    // ---- Q4: GetNotification(id) round-trip, and prove nothing was mutated ----
    if (first_id != 0) {
        UserNotification* one = nullptr;
        hr = listener->GetNotification(first_id, &one);
        printf("Q4 GetNotification(%u)      : hr=0x%08lX ptr=%p => %s\n", first_id, hr,
               (void*)one, (one ? "PASS" : "FAIL"));
        if (one) one->Release();
    }
    printf("Q4 ClearNotifications       : not called (this spike must not mutate user state)\n");

    view->Release();
    op1->Release();
    listener->Release();
    statics->Release();
    factory->Release();
    g_api.RoUninitialize();
    printf("=== spike end: Q1=%s Q2=%s ===\n", q1_ok ? "PASS" : "FAIL",
           "see log");
    return q1_ok ? 0 : 2;
}
