// DockForge - Core Audio hardware control implementation (T-068, C-23).
//
// ABI notes measured while writing the spike (spikes/T-068_hw_probe):
//  - MinGW's audiopolicy.h matches the official SDK signature set for
//    IAudioSessionEvents, INCLUDING the fact that OnStateChanged and
//    OnSessionDisconnected carry no EventContext parameter.
//  - SetMute with the current value returns S_FALSE, not S_OK - treated as
//    success everywhere below.
//  - The friendly-name property read can fail on locked-down stores; the name
//    is then empty and the panel falls back to a generic label.
#include "finderbar/audio_hardware.h"

#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <psapi.h>

#include <cstdio>

namespace dockforge::finderbar {

namespace {

const CLSID kClsidMMDeviceEnumerator = {
    0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};

std::wstring HrText(const wchar_t* what, HRESULT hr) {
    wchar_t buf[160];
    swprintf(buf, 160, L"%s失败 (0x%08lX)", what, static_cast<unsigned long>(hr));
    return buf;
}

// Single-instance callback owner (the panel creates exactly one AudioHardware
// per process; the COM notify objects below are file-locals that need to reach
// back into it).
AudioHardware* g_callback_owner = nullptr;

// Where the audio thread's notifications get posted. MEASURED (T-068 probe
// run dockforge-t068-21589703): OnNotify arrives on the AUDIO SERVICE's
// thread, not ours. Touching COM (GetEndpoint), the event bus or GDI repaint
// from that callback dead-locks the audio thread on some runs (one probe pass,
// one fail with the event never delivered). So the callback does exactly one
// thing - PostMessage - and the main thread owns the rest.
HWND g_notify_hwnd = nullptr;

// Registered message carrying one endpoint volume change. wparam bits: 1 =
// capture endpoint, 2 = muted. lparam: level * 10000, truncated.
const UINT kVolumeNotifyMsg = WM_APP + 0x31;

class EndpointNotify : public IAudioEndpointVolumeCallback {
  public:
    bool capture = false;
    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IAudioEndpointVolumeCallback)) {
            *out = static_cast<IAudioEndpointVolumeCallback*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) override;
};

EndpointNotify g_render_notify;
EndpointNotify g_capture_notify;

}  // namespace

UINT VolumeNotifyMessage() { return kVolumeNotifyMsg; }

// The whole audio-thread body: copy two fields into a PostMessage. No COM, no
// bus, no paint - those all run on the main thread from the panel's WndProc.
STDMETHODIMP EndpointNotify::OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) {
    if (!data || !g_notify_hwnd) return S_OK;
    const WPARAM flags = (capture ? 1u : 0u) | ((data->bMuted != FALSE) ? 2u : 0u);
    const LPARAM level10000 =
        static_cast<LPARAM>(data->fMasterVolume * 10000.0f);
    PostMessageW(g_notify_hwnd, kVolumeNotifyMsg, flags, level10000);
    return S_OK;
}

// ---- COM member block --------------------------------------------------------

struct AudioCom {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* render_device = nullptr;
    IMMDevice* capture_device = nullptr;
    IAudioEndpointVolume* render_volume = nullptr;
    IAudioEndpointVolume* capture_volume = nullptr;
    IAudioSessionManager2* session_manager = nullptr;
    std::wstring render_id;
    std::wstring capture_id;
};

// ---- AudioHardware ----------------------------------------------------------

AudioHardware::AudioHardware(
    std::function<void(const VolumeChanged&)> on_volume_changed)
    : on_volume_changed_(std::move(on_volume_changed)) {
    com_ = new AudioCom();
}

AudioHardware::~AudioHardware() {
    Shutdown();
    delete reinterpret_cast<AudioCom*>(com_);
    com_ = nullptr;
}

bool AudioHardware::Initialize() {
    if (initialized_) return true;
    auto* c = reinterpret_cast<AudioCom*>(com_);
    HRESULT hr = CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&c->enumerator));
    if (FAILED(hr) || !c->enumerator) return false;

    // Default endpoints. A machine without a microphone still has a render
    // endpoint; missing capture is not fatal, the panel shows the mic row as
    // unavailable.
    c->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &c->render_device);
    c->enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &c->capture_device);

    if (c->render_device) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(c->render_device->GetId(&id)) && id) {
            c->render_id = id;
            CoTaskMemFree(id);
        }
        c->render_device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(&c->render_volume));
    }
    if (c->capture_device) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(c->capture_device->GetId(&id)) && id) {
            c->capture_id = id;
            CoTaskMemFree(id);
        }
        c->capture_device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(&c->capture_volume));
    }
    if (!c->render_volume) return false;  // no audio at all -> unavailable

    if (c->render_device) {
        c->render_device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(&c->session_manager));
    }

    // Register change notifications (event-driven per C-23). The globals carry
    // the callbacks back into this instance - one AudioHardware per process is
    // the panel's lifetime model. Per-SESSION notifications are deliberately
    // not registered (v1): a dying session leaves an outstanding registration
    // that must be chased through the exact control pointer. Endpoint volume
    // stays event-driven; session volumes refresh on the panel's 0.5Hz tick
    // (recorded in PROGRESS.md).
    g_callback_owner = this;
    g_render_notify.capture = false;
    g_capture_notify.capture = true;
    c->render_volume->RegisterControlChangeNotify(&g_render_notify);
    if (c->capture_volume) {
        c->capture_volume->RegisterControlChangeNotify(&g_capture_notify);
    }

    initialized_ = true;
    return true;
}

void AudioHardware::Shutdown() {
    if (!initialized_) return;
    auto* c = reinterpret_cast<AudioCom*>(com_);
    // Detach the sink FIRST: after the unregister calls below, a notification
    // racing in from the audio thread must find no window to post to.
    g_notify_hwnd = nullptr;
    if (c->render_volume) c->render_volume->UnregisterControlChangeNotify(&g_render_notify);
    if (c->capture_volume) {
        c->capture_volume->UnregisterControlChangeNotify(&g_capture_notify);
    }
    g_callback_owner = nullptr;
    #define DF_RELEASE(p) \
        do { if ((p)) { (p)->Release(); (p) = nullptr; } } while (0)
    DF_RELEASE(c->session_manager);
    DF_RELEASE(c->capture_volume);
    DF_RELEASE(c->render_volume);
    DF_RELEASE(c->capture_device);
    DF_RELEASE(c->render_device);
    DF_RELEASE(c->enumerator);
    #undef DF_RELEASE
    initialized_ = false;
}

AudioEndpointState AudioHardware::GetEndpoint(bool capture) const {
    AudioEndpointState out;
    auto* c = reinterpret_cast<AudioCom*>(com_);
    IAudioEndpointVolume* vol = capture ? c->capture_volume : c->render_volume;
    IMMDevice* dev = capture ? c->capture_device : c->render_device;
    if (!vol || !dev) return out;
    if (FAILED(vol->GetMasterVolumeLevelScalar(&out.level))) return out;
    BOOL muted = FALSE;
    if (SUCCEEDED(vol->GetMute(&muted))) out.muted = muted != FALSE;
    // Friendly name (best effort).
    IPropertyStore* store = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &store)) && store) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv)) &&
            pv.vt == VT_LPWSTR && pv.pwszVal) {
            out.name = pv.pwszVal;
        }
        PropVariantClear(&pv);
        store->Release();
    }
    out.present = true;
    return out;
}

HardwareControlError AudioHardware::SetEndpointLevel(bool capture, float level) {
    auto* c = reinterpret_cast<AudioCom*>(com_);
    IAudioEndpointVolume* vol = capture ? c->capture_volume : c->render_volume;
    if (!vol || !initialized_) {
        return HardwareControlError::Failed(
            capture ? L"麦克风" : L"主音量", L"音频设备不可用", E_FAIL);
    }
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    const HRESULT hr = vol->SetMasterVolumeLevelScalar(level, &GUID_NULL);
    if (FAILED(hr)) {
        return HardwareControlError::Failed(
            capture ? L"麦克风" : L"主音量", HrText(L"设置音量", hr), hr);
    }
    return HardwareControlError::None();
}

HardwareControlError AudioHardware::SetEndpointMute(bool capture, bool mute) {
    auto* c = reinterpret_cast<AudioCom*>(com_);
    IAudioEndpointVolume* vol = capture ? c->capture_volume : c->render_volume;
    if (!vol || !initialized_) {
        return HardwareControlError::Failed(
            capture ? L"麦克风" : L"主音量", L"音频设备不可用", E_FAIL);
    }
    const HRESULT hr = vol->SetMute(mute ? TRUE : FALSE, &GUID_NULL);
    if (FAILED(hr)) {  // S_FALSE (no change) is a success
        return HardwareControlError::Failed(
            capture ? L"麦克风" : L"主音量", HrText(L"设置静音", hr), hr);
    }
    return HardwareControlError::None();
}

std::vector<AudioSessionInfo> AudioHardware::EnumerateSessions() const {
    RefreshSessions();
    return sessions_;
}

void AudioHardware::RefreshSessions() const {
    sessions_.clear();
    auto* c = reinterpret_cast<AudioCom*>(com_);
    if (!initialized_ || !c->session_manager) return;
    IAudioSessionEnumerator* ses = nullptr;
    if (FAILED(c->session_manager->GetSessionEnumerator(&ses)) || !ses) return;
    int count = 0;
    ses->GetCount(&count);
    for (int i = 0; i < count; ++i) {
        IAudioSessionControl* ctrl = nullptr;
        if (FAILED(ses->GetSession(i, &ctrl)) || !ctrl) continue;
        IAudioSessionControl2* ctrl2 = nullptr;
        if (SUCCEEDED(ctrl->QueryInterface(__uuidof(IAudioSessionControl2),
                                           reinterpret_cast<void**>(&ctrl2))) &&
            ctrl2) {
            AudioSessionInfo state;
            ctrl2->GetProcessId(&state.pid);
            state.name = SessionDisplayName(state.pid);
            LPWSTR sid = nullptr;
            if (SUCCEEDED(ctrl2->GetSessionIdentifier(&sid)) && sid) {
                state.session_id = sid;
                CoTaskMemFree(sid);
            }
            ISimpleAudioVolume* simple = nullptr;
            if (SUCCEEDED(ctrl2->QueryInterface(
                    __uuidof(ISimpleAudioVolume),
                    reinterpret_cast<void**>(&simple)))) {
                simple->GetMasterVolume(&state.level);
                BOOL mute = FALSE;
                simple->GetMute(&mute);
                state.muted = mute != FALSE;
                simple->Release();
            }
            if (!state.session_id.empty()) sessions_.push_back(std::move(state));
            ctrl2->Release();
        }
        ctrl->Release();
    }
    ses->Release();
}

// Finds the session control whose identifier matches `session_id` and runs
// `fn(ctrl2)` on it. Helper shared by the two session mutators.
template <typename Fn>
static HRESULT ForEachSessionUntilMatch(IAudioSessionManager2* manager,
                                        const std::wstring& session_id, Fn fn) {
    IAudioSessionEnumerator* ses = nullptr;
    if (FAILED(manager->GetSessionEnumerator(&ses)) || !ses) return E_FAIL;
    HRESULT hr = E_FAIL;
    for (int i = 0;; ++i) {
        IAudioSessionControl* ctrl = nullptr;
        if (FAILED(ses->GetSession(i, &ctrl)) || !ctrl) break;
        IAudioSessionControl2* ctrl2 = nullptr;
        if (SUCCEEDED(ctrl->QueryInterface(__uuidof(IAudioSessionControl2),
                                           reinterpret_cast<void**>(&ctrl2))) &&
            ctrl2) {
            LPWSTR sid = nullptr;
            if (SUCCEEDED(ctrl2->GetSessionIdentifier(&sid)) && sid) {
                const bool match = session_id == sid;
                CoTaskMemFree(sid);
                if (match) {
                    hr = fn(ctrl2);
                    ctrl2->Release();
                    ctrl->Release();
                    break;
                }
            }
            ctrl2->Release();
        }
        ctrl->Release();
    }
    ses->Release();
    return hr;
}

HardwareControlError AudioHardware::SetSessionLevel(const std::wstring& session_id,
                                                    float level) {
    auto* c = reinterpret_cast<AudioCom*>(com_);
    if (!initialized_ || !c->session_manager) {
        return HardwareControlError::Failed(L"程序音量", L"音频会话不可用", E_FAIL);
    }
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    const HRESULT hr = ForEachSessionUntilMatch(
        c->session_manager, session_id, [level](IAudioSessionControl2* ctrl2) {
            ISimpleAudioVolume* simple = nullptr;
            HRESULT inner = E_FAIL;
            if (SUCCEEDED(ctrl2->QueryInterface(__uuidof(ISimpleAudioVolume),
                                                reinterpret_cast<void**>(&simple)))) {
                inner = simple->SetMasterVolume(level, &GUID_NULL);
                simple->Release();
            }
            return inner;
        });
    if (FAILED(hr)) {
        return HardwareControlError::Failed(L"程序音量", HrText(L"设置程序音量", hr), hr);
    }
    return HardwareControlError::None();
}

HardwareControlError AudioHardware::SetSessionMute(const std::wstring& session_id,
                                                   bool mute) {
    auto* c = reinterpret_cast<AudioCom*>(com_);
    if (!initialized_ || !c->session_manager) {
        return HardwareControlError::Failed(L"程序音量", L"音频会话不可用", E_FAIL);
    }
    const HRESULT hr = ForEachSessionUntilMatch(
        c->session_manager, session_id, [mute](IAudioSessionControl2* ctrl2) {
            ISimpleAudioVolume* simple = nullptr;
            HRESULT inner = E_FAIL;
            if (SUCCEEDED(ctrl2->QueryInterface(__uuidof(ISimpleAudioVolume),
                                                reinterpret_cast<void**>(&simple)))) {
                inner = simple->SetMute(mute ? TRUE : FALSE, &GUID_NULL);
                simple->Release();
            }
            return inner;
        });
    if (FAILED(hr)) {
        return HardwareControlError::Failed(L"程序音量", HrText(L"设置程序静音", hr), hr);
    }
    return HardwareControlError::None();
}

void AudioHardware::PublishVolume(bool capture_endpoint, const std::wstring& session_id,
                                  float level, bool muted) {
    auto* c = reinterpret_cast<AudioCom*>(com_);
    VolumeChanged event;
    event.device_id = capture_endpoint ? c->capture_id : c->render_id;
    event.session_id = session_id;
    event.level = level;
    event.muted = muted;
    if (on_volume_changed_) on_volume_changed_(event);
}

void AudioHardware::PublishVolumeCallback(const VolumeChanged& event) {
    if (on_volume_changed_) on_volume_changed_(event);
}

void AudioHardware::SetEventSink(HWND hwnd) {
    // Reached through the public surface from the integration; the audio
    // thread's notify objects read this pointer as their only cross-thread
    // state (plain assignment, set once before events can flow, cleared on
    // shutdown before the interfaces go away).
    g_notify_hwnd = hwnd;
}

void AudioHardware::DispatchQueuedVolume(bool capture_endpoint, float level,
                                         bool muted) {
    auto* c = reinterpret_cast<AudioCom*>(com_);
    VolumeChanged event;
    event.device_id = capture_endpoint ? c->capture_id : c->render_id;
    event.level = level;
    event.muted = muted;
    if (on_volume_changed_) on_volume_changed_(event);
}

std::wstring SessionDisplayName(DWORD pid) {
    if (pid == 0) return L"系统声音";
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return L"进程 " + std::to_wstring(pid);
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::wstring name;
    if (GetProcessImageFileNameW(process, path, size) && size > 0) {
        std::wstring full(path, size);
        const std::size_t slash = full.find_last_of(L"\\/");
        name = slash == std::wstring::npos ? full : full.substr(slash + 1);
    }
    CloseHandle(process);
    if (name.empty()) return L"进程 " + std::to_wstring(pid);
    // .exe suffix off - shorter rows in the panel.
    const std::wstring kExe = L".exe";
    if (name.size() > kExe.size() &&
        _wcsicmp(name.c_str() + name.size() - kExe.size(), kExe.c_str()) == 0) {
        name.resize(name.size() - kExe.size());
    }
    return name;
}

std::wstring FormatLevelPercent(float level) {
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    wchar_t buf[16];
    swprintf(buf, 16, L"%d%%", static_cast<int>(level * 100.0f + 0.5f));
    return buf;
}

}  // namespace dockforge::finderbar
