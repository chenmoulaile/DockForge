// DockForge T-068 spike: Core Audio per-app + endpoint volume, MinGW-w64 direct.
//
// DoD questions answered by running this binary:
//   1. Do MinGW headers carry IAudioSessionManager2 / IAudioSessionEnumerator /
//      IAudioSessionControl2 / ISimpleAudioVolume / IAudioEndpointVolume?
//   2. Can an unpackaged console process enumerate audio sessions and read each
//      session's volume/mute/PID/name?
//   3. Can it read AND write the render-endpoint (master) volume and the
//      capture-endpoint (microphone) volume?
//   4. Do the notify callbacks (IAudioSessionControl::RegisterAudioSessionNotification
//      / IAudioEndpointVolume::RegisterControlChangeNotify) fire in this process?
//      (C-23 wants event-driven volume; if callbacks never arrive the panel polls.)
//
// Build (Git Bash, repo root):
//   g++ -std=c++17 -O0 -g -o spikes/T-068_hw_probe/audio_spike.exe \
//       spikes/T-068_hw_probe/audio_spike.cpp -lole32 -luuid
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

// Session control v2: the identity half (PID, grouping, session id).
// MinGW audiopolicy.h ships IAudioSessionControl2 (verified by the build itself).
const CLSID kCLSID_MMDeviceEnumerator = {
    0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const IID kIID_IMMDeviceEnumerator = {
    0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};

int g_notify_count = 0;

class SessionNotify : public IAudioSessionEvents {
  public:
    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IAudioSessionEvents)) {
            *out = static_cast<IAudioSessionEvents*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnSimpleVolumeChanged(float, BOOL, LPCGUID) override {
        ++g_notify_count;
        return S_OK;
    }
    STDMETHODIMP OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }
    // NOTE (measured against MinGW's audiopolicy.h, which matches the official
    // SDK): these two callbacks carry NO EventContext parameter.
    STDMETHODIMP OnStateChanged(AudioSessionState) override { return S_OK; }
    STDMETHODIMP OnSessionDisconnected(AudioSessionDisconnectReason) override { return S_OK; }
};

class EndpointNotify : public IAudioEndpointVolumeCallback {
  public:
    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IAudioEndpointVolumeCallback)) {
            *out = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override {
        ++g_notify_count;
        return S_OK;
    }
};

void DumpEndpoint(IMMDeviceEnumerator* enums, EDataFlow flow, const char* tag,
                  EndpointNotify* notify) {
    IMMDevice* device = nullptr;
    HRESULT hr = enums->GetDefaultAudioEndpoint(flow, eConsole, &device);
    if (FAILED(hr)) {
        printf("%s. GetDefaultAudioEndpoint      : FAILED 0x%08lX\n", tag, hr);
        return;
    }
    IAudioEndpointVolume* vol = nullptr;
    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&vol));
    if (FAILED(hr) || !vol) {
        printf("%s. Activate(IAudioEndpointVolume): FAILED 0x%08lX\n", tag, hr);
        device->Release();
        return;
    }
    float level = -1.0f;
    BOOL muted = FALSE;
    hr = vol->GetMasterVolumeLevelScalar(&level);
    if (SUCCEEDED(hr)) hr = vol->GetMute(&muted);
    printf("%s. endpoint volume               : 0x%08lX level=%.3f muted=%d\n", tag, hr, level,
           muted ? 1 : 0);

    UINT ch = 0;
    if (SUCCEEDED(vol->GetChannelCount(&ch))) {
        printf("%s. endpoint channels             : %u\n", tag, ch);
    }
    // Probe the WRITE path with the value it already has: a no-op set proves the
    // permission and the ABI without changing the user's volume.
    hr = vol->SetMasterVolumeLevelScalar(level, &GUID_NULL);
    printf("%s. SetMasterVolumeLevelScalar     : 0x%08lX (no-op same value)\n", tag, hr);
    hr = vol->SetMute(muted, &GUID_NULL);
    printf("%s. SetMute(same)                 : 0x%08lX\n", tag, hr);

    if (notify) {
        hr = vol->RegisterControlChangeNotify(notify);
        printf("%s. RegisterControlChangeNotify    : 0x%08lX\n", tag, hr);
    }

    // Device friendly name through the property store (PKEY_Device_FriendlyName).
    IPropertyStore* store = nullptr;
    hr = device->OpenPropertyStore(STGM_READ, &store);
    if (SUCCEEDED(hr)) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv)) &&
            pv.vt == VT_LPWSTR && pv.pwszVal) {
            printf("%s. device name                   : %ls\n", tag, pv.pwszVal);
        }
        PropVariantClear(&pv);
        store->Release();
    }
    vol->Release();
    device->Release();
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("=== T-068 Core Audio spike (MinGW direct) ===\n");

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        printf("0. CoInitializeEx                  : FAILED 0x%08lX\n", hr);
        return 1;
    }

    IMMDeviceEnumerator* enums = nullptr;
    hr = CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                          kIID_IMMDeviceEnumerator, reinterpret_cast<void**>(&enums));
    if (FAILED(hr) || !enums) {
        printf("1. CoCreateInstance(MMDeviceEnum)  : FAILED 0x%08lX\n", hr);
        CoUninitialize();
        return 1;
    }
    printf("1. CoCreateInstance(MMDeviceEnum)  : OK (headers + import resolved)\n");

    EndpointNotify end_notify;
    DumpEndpoint(enums, eRender, "2", &end_notify);
    DumpEndpoint(enums, eCapture, "3", nullptr);

    // ---- per-app sessions on the default render endpoint -------------------
    printf("4. sessions (IAudioSessionManager2 on default render)\n");
    IMMDevice* render = nullptr;
    hr = enums->GetDefaultAudioEndpoint(eRender, eConsole, &render);
    if (SUCCEEDED(hr)) {
        IAudioSessionManager2* mgr2 = nullptr;
        hr = render->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&mgr2));
        if (SUCCEEDED(hr) && mgr2) {
            IAudioSessionEnumerator* sess_enum = nullptr;
            hr = mgr2->GetSessionEnumerator(&sess_enum);
            if (SUCCEEDED(hr) && sess_enum) {
                int count = -1;
                sess_enum->GetCount(&count);
                printf("   GetCount                        : %d sessions\n", count);
                SessionNotify session_notify;
                for (int i = 0; i < count; ++i) {
                    IAudioSessionControl* ctrl = nullptr;
                    if (FAILED(sess_enum->GetSession(i, &ctrl)) || !ctrl) continue;
                    IAudioSessionControl2* ctrl2 = nullptr;
                    if (SUCCEEDED(ctrl->QueryInterface(__uuidof(IAudioSessionControl2),
                                                       reinterpret_cast<void**>(&ctrl2)))) {
                        DWORD pid = 0;
                        ctrl2->GetProcessId(&pid);
                        LPWSTR sid = nullptr;
                        ctrl2->GetSessionIdentifier(&sid);
                        if (i == 0 && ctrl) {
                            HRESULT reg = ctrl->RegisterAudioSessionNotification(&session_notify);
                            printf("   [session 0 RegisterAudioSessionNotification hr=0x%08lX]\n", reg);
                        }
                        ISimpleAudioVolume* simple = nullptr;
                        if (SUCCEEDED(ctrl2->QueryInterface(
                                __uuidof(ISimpleAudioVolume),
                                reinterpret_cast<void**>(&simple)))) {
                            float vol = -1.0f;
                            BOOL mute = FALSE;
                            simple->GetMasterVolume(&vol);
                            simple->GetMute(&mute);
                            // no-op write proves the permission (same value back)
                            HRESULT setw = simple->SetMasterVolume(vol, &GUID_NULL);
                            printf("   session[%d] pid=%lu vol=%.3f mute=%d setw=0x%08lX\n",
                                   i, pid, vol, mute ? 1 : 0, setw);
                            simple->Release();
                        }
                        if (sid) CoTaskMemFree(sid);
                        ctrl2->Release();
                    }
                    ctrl->Release();
                }
                sess_enum->Release();
            } else {
                printf("   GetSessionEnumerator            : FAILED 0x%08lX\n", hr);
            }
            mgr2->Release();
        } else {
            printf("   Activate(IAudioSessionManager2) : FAILED 0x%08lX\n", hr);
        }
        render->Release();
    } else {
        printf("   GetDefaultAudioEndpoint(render) : FAILED 0x%08lX\n", hr);
    }

    // ---- do change callbacks arrive in this process? -----------------------
    printf("5. waiting 12s for any change callback (tweak system volume now)...\n");
    const DWORD wait_start = GetTickCount();
    MSG msg;
    while (GetTickCount() - wait_start < 12000) {
        // MTA: callbacks arrive on this thread via the COM window; pump anyway.
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(200);
    }
    printf("5. change callbacks received       : %d %s\n", g_notify_count,
           g_notify_count > 0 ? "(EVENT-DRIVEN OK)" : "(NONE - panel must poll)");

    enums->Release();
    CoUninitialize();
    printf("=== spike end ===\n");
    return 0;
}
