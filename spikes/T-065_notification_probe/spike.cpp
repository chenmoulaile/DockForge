// DockForge T-065 spike: MinGW-w64 direct WinRT access to UserNotificationListener.
//
// Question (task prompt): can the dock talk to Windows.UI.Notifications.Management
// without C++/WinRT and without MSVC? Answer measured here:
//   1. RoGetActivationFactory via GetProcAddress("combase.dll") - MinGW ships no
//      import lib for it (checked: nm libole32.a / libruntimeobject.a).
//   2. NO hand-written GUIDs from memory: the factory is activated against
//      IID_IInspectable, and the real interface IIDs are DISCOVERED at runtime
//      via IInspectable::GetIids + QueryInterface + GetRuntimeClassName matching
//      (source of truth: the probe log from the .NET projection).
//   3. vtable order comes from the same probe log:
//      IUserNotificationListenerStatics: get_Current = slot 6
//      IUserNotificationListener: RequestAccessAsync=6, GetAccessStatus=7 (SYNC),
//        add_NotificationChanged=8, remove_NotificationChanged=9,
//        GetNotificationsAsync=10, GetNotification=11, ClearNotifications=12,
//        RemoveNotification=13
//      IVectorView<T>: First=6, get_Size=7, GetAt=8, IndexOf=9, GetMany=10
//      IAsyncInfo: get_Id=6, get_Status=7, get_ErrorCode=8, Cancel=9, Close=10
//      IAsyncOperation<T>: put_Completed=11, get_Completed=12, GetResults=13
//      IUserNotification (this OS build): get_Notification=6, get_AppInfo=7,
//        get_Id=8, get_CreationTime=9   (no AppId - removed from the contract)
//
// Build (Git Bash, repo root):
//   g++ -std=c++17 -O0 -g -o spikes/T-065_notification_probe/spike.exe \
//       spikes/T-065_notification_probe/spike.cpp -static -municode
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

// HSTRING is an opaque reference-counted handle; ABI-wise a pointer.
using HSTRING_T = void*;

//IID_IInspectable {AF86E2E0-B12D-4C6A-9C5A-D7AA65101E90} - stable, documented.
const IID kIidInspectable = {
    0xAF86E2E0, 0xB12D, 0x4C6A, {0x9C, 0x5A, 0xD7, 0xAA, 0x65, 0x10, 0x1E, 0x90}};

// AsyncStatus (Windows.Foundation), stable ABI values.
enum AsyncStatusT { kCanceled = 2, kCompleted = 1, kError = 3, kStarted = 0 };

// ---- minimal IInspectable (extends IUnknown with the 3 documented slots) ----
struct Inspectable : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetIids(ULONG* iid_count, IID** iids) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING_T* name) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTrustLevel(int* level) = 0;
};

struct AsyncInfo : public Inspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(unsigned int* id) = 0;          // 6
    virtual HRESULT STDMETHODCALLTYPE get_Status(int* status) = 0;           // 7
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT* code) = 0;      // 8
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;                          // 9
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;                           // 10
};

template <typename T>
struct AsyncOperation : public AsyncInfo {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void* handler) = 0;      // 11
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void** handler) = 0;     // 12
    virtual HRESULT STDMETHODCALLTYPE GetResults(T* results) = 0;            // 13
};

struct VectorView : public Inspectable {
    virtual HRESULT STDMETHODCALLTYPE First(void** it) = 0;                  // 6
    virtual HRESULT STDMETHODCALLTYPE get_Size(unsigned int* size) = 0;      // 7
    virtual HRESULT STDMETHODCALLTYPE GetAt(unsigned int index, void** item) = 0;  // 8
    virtual HRESULT STDMETHODCALLTYPE IndexOf(void* item, unsigned int* index,
                                              int* found) = 0;               // 9
    virtual HRESULT STDMETHODCALLTYPE GetMany(unsigned int start, unsigned int cap,
                                              void** items, unsigned int* got) = 0;  // 10
};

struct UserNotification : public Inspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Notification(void** n) = 0;        // 6
    virtual HRESULT STDMETHODCALLTYPE get_AppInfo(void** info) = 0;          // 7
    virtual HRESULT STDMETHODCALLTYPE get_Id(unsigned int* id) = 0;          // 8
    virtual HRESULT STDMETHODCALLTYPE get_CreationTime(void* time) = 0;      // 9
};

struct Listener : public Inspectable {
    virtual HRESULT STDMETHODCALLTYPE RequestAccessAsync(
        AsyncOperation<int>** op) = 0;                                       // 6
    virtual HRESULT STDMETHODCALLTYPE GetAccessStatus(int* status) = 0;      // 7 SYNC
    virtual HRESULT STDMETHODCALLTYPE add_NotificationChanged(void* handler,
                                                              void* token) = 0;  // 8
    virtual HRESULT STDMETHODCALLTYPE remove_NotificationChanged(
        void* token) = 0;                                                    // 9
    virtual HRESULT STDMETHODCALLTYPE GetNotificationsAsync(
        unsigned int kinds, AsyncOperation<VectorView*>** op) = 0;           // 10
    virtual HRESULT STDMETHODCALLTYPE GetNotification(unsigned int id,
                                                      UserNotification** n) = 0;  // 11
    virtual HRESULT STDMETHODCALLTYPE ClearNotifications() = 0;              // 12
    virtual HRESULT STDMETHODCALLTYPE RemoveNotification(unsigned int id) = 0;    // 13
};

struct Statics : public Inspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Current(Listener** current) = 0;   // 6
};

// ---- combase.dll entry points, resolved dynamically ----
using RoInitializeFn = HRESULT(STDAPICALLTYPE*)(int);
// NOTE: the class id is an HSTRING, not a PCWSTR - passing a raw wide string
// returns E_INVALIDARG (measured in the first run of this spike).
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

// Format the IID without pulling in StringFromGUID2 (keeps imports to nothing).
std::wstring FormatIid(const IID& iid) {
    wchar_t buf[64];
    swprintf(buf, 64, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             static_cast<unsigned>(iid.Data1), iid.Data2, iid.Data3, iid.Data4[0],
             iid.Data4[1], iid.Data4[2], iid.Data4[3], iid.Data4[4], iid.Data4[5],
             iid.Data4[6], iid.Data4[7]);
    return buf;
}

// Finds the IID whose interface RuntimeClassName matches `name_suffix`
// ("...IUserNotificationListenerStatics"). Uses GetIids on `from`, then
// QueryInterface + GetRuntimeClassName per candidate. Returns S_OK on hit.
//
// MEASURED CAVEAT: activation factories return E_NOTIMPL (0x8000000E) from
// GetRuntimeClassName, so name matching is impossible there. The IID lists are
// printed instead and matched against the two IIDs cross-verified between this
// machine's GetIids output and the windows-rs generated bindings.
HRESULT ReportIids(Inspectable* from, const char* tag, const IID* expect, int expect_n) {
    ULONG count = 0;
    IID* ids = nullptr;
    HRESULT hr = from->GetIids(&count, &ids);
    printf("   [%s GetIids hr=0x%08lX count=%lu]\n", tag, hr, count);
    if (SUCCEEDED(hr)) {
        for (ULONG i = 0; i < count; ++i) {
            bool matched = false;
            for (int k = 0; k < expect_n; ++k) {
                if (ids[i] == expect[k]) { matched = true; break; }
            }
            printf("   [%s iid %lu = %ls%s\n", tag, i, FormatIid(ids[i]).c_str(),
                   matched ? "  <== expected interface]" : "]");
        }
        CoTaskMemFree(ids);
    }
    return hr;
}

// Waits for an IAsyncOperation to leave Started. Returns final status.
//
// MEASURED (T-065): in this console process the operation NEVER completes -
// status stays Started under RO_INIT_MULTITHREADED and RO_INIT_SINGLETHREADED
// alike, while the same call from the .NET projection (notif_probe.exe, STA +
// AsTask bridging) completes in ~1s with 51 notifications. The dock does not
// need the async path (the C-25 adapters poll window titles), so this boundary
// is recorded, not fixed here.
int WaitAsync(AsyncInfo* op, unsigned int timeout_ms) {
    int status = kStarted;
    unsigned int waited = 0;
    while (SUCCEEDED(op->get_Status(&status)) && status == kStarted &&
           waited < timeout_ms) {
        Sleep(500);
        waited += 500;
        printf("   [waiting async... %u ms status=%d]\n", waited, status);
    }
    return status;
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered: a hang must still show progress
    printf("=== T-065 MinGW direct WinRT spike ===\n");

    if (!g_api.Load()) {
        printf("1. LoadLibrary(combase.dll)          : FAILED (GetLastError=%lu)\n",
               GetLastError());
        return 1;
    }
    printf("1. LoadLibrary(combase.dll)          : OK (RoInitialize + RoGetActivationFactory + string helpers resolved)\n");

    HRESULT hr = g_api.RoInitialize(1 /* RO_INIT_MULTITHREADED */);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE) {
        printf("2. RoInitialize                      : FAILED 0x%08lX\n", hr);
        return 1;
    }
    printf("2. RoInitialize(SINGLETHREADED)     : 0x%08lX\n", hr);

    // Activate against IID_IInspectable: the factory always implements it, so no
    // guessed GUIDs are needed anywhere in this spike. The class id must be an
    // HSTRING (WindowsCreateString), not a bare wide string.
    HSTRING_T class_id = nullptr;
    hr = g_api.CreateString(L"Windows.UI.Notifications.Management.UserNotificationListener",
                            static_cast<unsigned int>(wcslen(
                                L"Windows.UI.Notifications.Management.UserNotificationListener")),
                            &class_id);
    if (FAILED(hr)) {
        printf("3. WindowsCreateString(class id)     : FAILED 0x%08lX\n", hr);
        return 1;
    }
    Inspectable* factory = nullptr;
    hr = g_api.RoGetActivationFactory(class_id, kIidInspectable,
                                      reinterpret_cast<void**>(&factory));
    g_api.DelString(class_id);
    if (FAILED(hr)) {
        printf("3. RoGetActivationFactory            : FAILED 0x%08lX\n", hr);
        return 1;
    }

    HSTRING_T cls = nullptr;
    factory->GetRuntimeClassName(&cls);
    printf("3. RoGetActivationFactory            : OK, class=%ls\n", HStr(cls).c_str());
    g_api.DelString(cls);

    // Discover IUserNotificationListenerStatics on the factory.
    // IID sources, cross-verified:
    //  (a) THIS run: factory->GetIids returned exactly
    //      {FF6123CF-4386-4AA3-B73D-B804E5B63B23} after IMultiQI;
    //  (b) windows-rs generated bindings list the same value for
    //      IUserNotificationListenerStatics (fetched during the spike).
    const IID kIidStatics = {0xFF6123CF, 0x4386, 0x4AA3,
                             {0xB7, 0x3D, 0xB8, 0x04, 0xE5, 0xB6, 0x3B, 0x23}};
    const IID kIidListener = {0x62553E41, 0x8A06, 0x4CEF,
                              {0x82, 0x15, 0x60, 0x33, 0xA5, 0xBE, 0x4B, 0x03}};
    printf("4. IIDs in use (GetIids + windows-rs cross-verified)\n");
    ReportIids(factory, "factory", &kIidStatics, 1);

    Statics* statics = nullptr;
    hr = factory->QueryInterface(kIidStatics, reinterpret_cast<void**>(&statics));
    if (FAILED(hr)) {
        printf("5. QI(IUserNotificationListenerStatics) : FAILED 0x%08lX\n", hr);
        return 1;
    }

    Listener* listener = nullptr;
    hr = statics->get_Current(&listener);
    if (FAILED(hr) || !listener) {
        printf("6. get_Current                       : FAILED 0x%08lX\n", hr);
        return 1;
    }
    printf("6. get_Current                       : OK (listener instance)\n");
    ReportIids(listener, "listener", &kIidListener, 1);

    // GetAccessStatus is SYNCHRONOUS - no async plumbing needed for the check.
    int access = -1;
    hr = listener->GetAccessStatus(&access);
    printf("7. GetAccessStatus()                 : 0x%08lX status=%d (0=Unspecified 1=Allowed 2=Denied 4=DeniedBySystem)\n",
           hr, access);
    if (FAILED(hr)) return 1;

    // GetNotificationsAsync(Toast=1) -> IVectorView<UserNotification>
    AsyncOperation<VectorView*>* op = nullptr;
    hr = listener->GetNotificationsAsync(1, &op);
    if (FAILED(hr)) {
        printf("8. GetNotificationsAsync(Toast)      : FAILED 0x%08lX\n", hr);
        return 1;
    }
    const int st = WaitAsync(op, 10000);
    if (st != kCompleted) {
        printf("8. GetNotificationsAsync(Toast)      : status=%d (not completed)\n", st);
        return 1;
    }
    VectorView* view = nullptr;
    hr = op->GetResults(&view);
    if (FAILED(hr) || !view) {
        printf("9. op->GetResults()                  : FAILED 0x%08lX\n", hr);
        return 1;
    }
    unsigned int count = 0;
    view->get_Size(&count);
    printf("9. notifications visible             : %u\n", count);

    // Read ids through the discovered ABI order (get_Id = slot 8 on this build).
    unsigned int printed = 0;
    for (unsigned int i = 0; i < count && printed < 10; ++i) {
        UserNotification* n = nullptr;
        if (FAILED(view->GetAt(i, reinterpret_cast<void**>(&n))) || !n) continue;
        unsigned int id = 0;
        if (SUCCEEDED(n->get_Id(&id))) {
            printf("   notification[%u] id=%u\n", i, id);
            ++printed;
        }
        n->Release();
    }
    printf("   (read %u of %u via IUserNotification::get_Id, slot 8)\n", printed, count);

    view->Release();
    op->Release();
    listener->Release();
    statics->Release();
    factory->Release();
    g_api.RoUninitialize();
    printf("=== spike end: MinGW direct WinRT VIABLE ===\n");
    return 0;
}
