// spikes/t005_material/dupl.cpp - desktop duplication cost spike (T-005, part 4).
//
// Part 3 measured that BitBlt(GetDC(NULL)) costs a FIXED ~78ms per call on this
// host, independent of the captured region size (160x60 and 2560x480 both ~78ms).
// That is a compositor round-trip, not pixel bandwidth, so BitBlt cannot feed a
// 60 FPS frosted plate no matter how small the Dock is.
//
// This spike measures the GPU path instead: DXGI Desktop Duplication
// (IDXGIOutput1::DuplicateOutput -> AcquireNextFrame -> copy to a D3D11 texture).
//
// Questions:
//   1. Does DuplicateOutput succeed on this host, and at what size?
//   2. What does AcquireNextFrame cost when the desktop is idle
//      (DXGI_ERROR_WAIT_TIMEOUT) versus when a frame is available?
//   3. Does our own WDA_EXCLUDEFROMCAPTURE window stay out of the duplicated frame?
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kClass[] = L"DockForgeSpikeDuplMarker";
constexpr int kMarkX = 400;
constexpr int kMarkY = 400;
constexpr int kMarkW = 200;
constexpr int kMarkH = 80;

std::int64_t Qpc() {
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

double Ms(std::int64_t ticks, std::int64_t freq) {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(freq);
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HBRUSH b = CreateSolidBrush(RGB(255, 0, 255));
        FillRect(reinterpret_cast<HDC>(wp), &rc, b);
        DeleteObject(b);
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

struct Duplicator {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> dupl;
    ComPtr<ID3D11Texture2D> staging;
    DXGI_OUTPUT_DESC desc{};
    UINT w = 0;
    UINT h = 0;
};

bool InitDuplicator(Duplicator& d) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, &d.device, nullptr, &d.context);
    std::printf("D3D11CreateDevice                     hr=0x%08lX\n",
                static_cast<unsigned long>(hr));
    if (FAILED(hr)) return false;

    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(d.device.As(&dxgi))) return false;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi->GetAdapter(&adapter))) return false;
    ComPtr<IDXGIOutput> output;
    // Output 0 on the default adapter; multi-monitor enumeration comes later.
    hr = adapter->EnumOutputs(0, &output);
    std::printf("IDXGIAdapter::EnumOutputs(0)          hr=0x%08lX\n",
                static_cast<unsigned long>(hr));
    if (FAILED(hr)) return false;

    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output.As(&output1))) return false;

    hr = output1->DuplicateOutput(d.device.Get(), &d.dupl);
    std::printf("IDXGIOutput1::DuplicateOutput         hr=0x%08lX %s\n",
                static_cast<unsigned long>(hr),
                hr == DXGI_ERROR_UNSUPPORTED   ? "(unsupported)"
                : hr == E_ACCESSDENIED         ? "(access denied: another duplicator?)"
                : hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE ? "(max sessions reached)"
                                                           : "");
    if (FAILED(hr)) return false;

    // DuplicateOutput hands back a DXGI_OUTDUPL_DESC; the monitor rect lives in the
    // OUTPUT desc, which has to be queried from IDXGIOutput separately.
    DXGI_OUTDUPL_DESC dupl_desc{};
    d.dupl->GetDesc(&dupl_desc);
    output->GetDesc(&d.desc);
    d.w = dupl_desc.ModeDesc.Width;
    d.h = dupl_desc.ModeDesc.Height;
    std::printf("output mode=%ux%u rotation=%d attached=%d\n", d.w, d.h,
                static_cast<int>(dupl_desc.Rotation), d.desc.AttachedToDesktop);

    // A CPU-readable staging copy of the frame. BGRA8 must match the duplication
    // format, which is always DXGI_FORMAT_B8G8R8A8_UNORM.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = d.w;
    td.Height = d.h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = d.device->CreateTexture2D(&td, nullptr, &d.staging);
    std::printf("CreateTexture2D(staging %ux%u)     hr=0x%08lX\n", d.w, d.h,
                static_cast<unsigned long>(hr));
    return SUCCEEDED(hr);
}

// One acquire cycle. Returns the wait result so callers can tell "no new frame"
// (normal when the desktop is idle) from "new frame".
HRESULT AcquireOnce(Duplicator& d, bool copy) {
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = d.dupl->AcquireNextFrame(0, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return hr;
    if (FAILED(hr)) return hr;

    if (copy) {
        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(resource.As(&tex))) d.context->CopyResource(d.staging.Get(), tex.Get());
    }
    d.dupl->ReleaseFrame();
    return hr;
}

// Reads one pixel out of the staging texture. Used to prove the duplicated frame
// is real desktop content and that our excluded marker window is absent from it.
bool ReadStagingPixel(Duplicator& d, int x, int y, BYTE* rgba) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(d.context->Map(d.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    const auto* row = static_cast<const BYTE*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
    const BYTE* px = row + static_cast<std::size_t>(x) * 4;
    rgba[0] = px[2];  // R
    rgba[1] = px[1];  // G
    rgba[2] = px[0];  // B
    rgba[3] = px[3];
    d.context->Unmap(d.staging.Get(), 0);
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    int frames = 200;
    bool exclude_marker = false;
    bool with_copy = true;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg.rfind(L"--frames=", 0) == 0) frames = _wtoi(arg.c_str() + 9);
        if (arg == L"--exclude-marker") exclude_marker = true;
        if (arg == L"--no-copy") with_copy = false;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HINSTANCE inst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    HWND marker = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, L"marker", WS_POPUP,
                                  kMarkX, kMarkY, kMarkW, kMarkH, nullptr, nullptr, inst, nullptr);
    if (marker) {
        if (exclude_marker) {
            const BOOL ok = SetWindowDisplayAffinity(marker, WDA_EXCLUDEFROMCAPTURE);
            std::printf("marker SetWindowDisplayAffinity       %s\n", ok ? "OK" : "FAIL");
        }
        ShowWindow(marker, SW_SHOWNOACTIVATE);
        UpdateWindow(marker);
    }

    Duplicator d;
    if (!InitDuplicator(d)) {
        std::printf("duplication init failed\n");
        return 2;
    }
    Sleep(300);

    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);

    // Phase 1: acquire cost when the desktop is idle. Most calls return
    // DXGI_ERROR_WAIT_TIMEOUT immediately; those are the cheap path.
    int timeouts = 0;
    int acquired = 0;
    double acquire_sum = 0.0;
    double copy_sum = 0.0;
    double worst = 0.0;
    for (int i = 0; i < frames; ++i) {
        const std::int64_t t0 = Qpc();
        const HRESULT hr = AcquireOnce(d, with_copy);
        const double cost = Ms(Qpc() - t0, f.QuadPart);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            ++timeouts;
        } else if (SUCCEEDED(hr)) {
            ++acquired;
        }
        acquire_sum += cost;
        worst = (std::max)(worst, cost);
        (void)copy_sum;
        Sleep(8);  // roughly a frame, so the loop samples a realistic cadence
    }

    std::printf("frames=%d timeouts=%d acquired=%d\n", frames, timeouts, acquired);
    std::printf("acquire%s ms: avg=%.3f max=%.3f\n", with_copy ? "+copy" : "",
                acquire_sum / frames, worst);

    // Phase 2: is the marker window present in the duplicated frame? Force one
    // fresh acquisition by nudging the marker, then read its centre pixel.
    if (marker) {
        InvalidateRect(marker, nullptr, TRUE);
        UpdateWindow(marker);
        Sleep(120);
        if (SUCCEEDED(AcquireOnce(d, true))) {
            BYTE px[4]{};
            if (ReadStagingPixel(d, kMarkX + kMarkW / 2, kMarkY + kMarkH / 2, px)) {
                std::printf("duplicated frame at marker centre: rgb=(%d,%d,%d)\n", px[0], px[1],
                            px[2]);
                std::printf("marker %s in duplicated frame\n",
                            (px[0] > 200 && px[2] > 200 && px[1] < 60) ? "PRESENT" : "ABSENT");
            }
        }
    }

    if (marker) DestroyWindow(marker);
    return 0;
}
