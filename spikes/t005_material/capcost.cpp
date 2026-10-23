// spikes/t005_material/capcost.cpp - what does one screen readback actually cost?
//
// Part 3 measured BitBlt(GetDC(NULL)) at a flat ~78ms for every region size from
// 160x60 to 2560x480, and part 4 could not open DXGI Desktop Duplication
// (E_ACCESSDENIED). Before choosing a capture strategy the constant has to be
// explained, and two follow-up questions decide the design:
//
//   * Is the 78ms WALL time or CPU time? A blocking compositor round-trip can be
//     moved to a worker thread for free; a busy-wait cannot.
//   * Does WDA_EXCLUDEFROMCAPTURE also hide the window from PrintWindow of
//     itself? Part 2 proved exclusion is required to avoid feedback, but the
//     screenshot tool used for evidence is itself a capture, so if exclusion also
//     blocks self-capture there is no way to produce a screenshot of the plate.
//
// Sections:
//   A. BitBlt back to back            (wall + thread CPU time)
//   B. BitBlt paced at 100ms
//   C. GetPixel on the screen DC
//   D. PrintWindow of our own window
//   E. BitBlt from our own window DC
//   F. PrintWindow / BitBlt of our own window WITH WDA_EXCLUDEFROMCAPTURE set
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

namespace {

std::int64_t Qpc() {
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

double Ms(std::int64_t ticks, std::int64_t freq) {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(freq);
}

// Kernel + user time consumed by this thread, in milliseconds.
double CpuMs(HANDLE thread) {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetThreadTimes(thread, &creation, &exit, &kernel, &user)) return -1.0;
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return static_cast<double>(k.QuadPart + u.QuadPart) / 10000.0;
}

void Stats(const char* label, std::vector<double> v) {
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    std::printf("%-40s n=%3zu min=%8.3f p50=%8.3f p99=%8.3f max=%8.3f\n", label, v.size(),
                v.front(), v[v.size() / 2], v[static_cast<std::size_t>(v.size() * 0.99)],
                v.back());
}

constexpr wchar_t kClass[] = L"DockForgeSpikeCapCostWin";

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HBRUSH b = CreateSolidBrush(RGB(255, 0, 255));  // magenta: unmistakable
        FillRect(reinterpret_cast<HDC>(wp), &rc, b);
        DeleteObject(b);
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Center pixel of a top-down 32bpp DIB.
void ReadCenter(const void* bits, int w, int h, unsigned* out) {
    const auto* p = static_cast<const unsigned char*>(bits);
    const std::size_t off = (static_cast<std::size_t>(h / 2) * static_cast<std::size_t>(w) +
                             static_cast<std::size_t>(w / 2)) * 4u;
    std::memcpy(out, p + off, 4);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    (void)argc;
    (void)argv;
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);

    const int w = 320;
    const int h = 120;
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih);
    bih.biWidth = w;
    bih.biHeight = -h;
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen0 = GetDC(nullptr);
    HBITMAP dib = CreateDIBSection(screen0, reinterpret_cast<BITMAPINFO*>(&bih), DIB_RGB_COLORS,
                                   &bits, nullptr, 0);
    HDC mem = CreateCompatibleDC(screen0);
    HGDIOBJ old = SelectObject(mem, dib);
    std::printf("capture region=%dx%d pixels=%d\n\n", w, h, w * h);

    // A: back to back, separating wall clock from CPU time. If CPU time is a small
    // fraction of wall time the readback blocks, and a worker thread costs nothing.
    std::vector<double> a;
    const double cpu0 = CpuMs(GetCurrentThread());
    const std::int64_t wall0 = Qpc();
    for (int i = 0; i < 40; ++i) {
        const std::int64_t t0 = Qpc();
        BitBlt(mem, 0, 0, w, h, screen0, 400, 400, SRCCOPY);
        a.push_back(Ms(Qpc() - t0, f.QuadPart));
    }
    const std::int64_t wall1 = Qpc();
    const double cpu1 = CpuMs(GetCurrentThread());
    Stats("A BitBlt back-to-back", a);
    const double wall_total = Ms(wall1 - wall0, f.QuadPart);
    std::printf("   A cpu=%.3fms wall=%.3fms -> cpu/wall=%.1f%%\n\n", cpu1 - cpu0, wall_total,
                (cpu1 - cpu0) * 100.0 / (wall_total > 0.0 ? wall_total : 1.0));

    // B: paced at 100ms, which is above any plausible per-frame throttle.
    std::vector<double> b;
    for (int i = 0; i < 20; ++i) {
        Sleep(100);
        const std::int64_t t0 = Qpc();
        BitBlt(mem, 0, 0, w, h, screen0, 400, 400, SRCCOPY);
        b.push_back(Ms(Qpc() - t0, f.QuadPart));
    }
    Stats("B BitBlt paced 100ms", b);

    // C: single-pixel reads straight off the screen DC.
    std::vector<double> c;
    for (int i = 0; i < 40; ++i) {
        const std::int64_t t0 = Qpc();
        GetPixel(screen0, 400 + i, 400);
        c.push_back(Ms(Qpc() - t0, f.QuadPart));
    }
    Stats("C GetPixel(screen DC)", c);

    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    // D/E/F all use a window we own and paint magenta, so "captured" and "not
    // captured" are unambiguous in the sampled pixel.
    HWND win = CreateWindowExW(0, kClass, L"capcost", WS_POPUP, 500, 500, w, h, nullptr, nullptr,
                               inst, nullptr);
    if (win) {
        ShowWindow(win, SW_SHOWNOACTIVATE);
        UpdateWindow(win);
        Sleep(300);

        std::vector<double> d;
        for (int i = 0; i < 40; ++i) {
            const std::int64_t t0 = Qpc();
            PrintWindow(win, mem, 0);
            d.push_back(Ms(Qpc() - t0, f.QuadPart));
        }
        Stats("D PrintWindow(own window)", d);

        HDC wdc = GetDC(win);
        std::vector<double> e;
        for (int i = 0; i < 40; ++i) {
            const std::int64_t t0 = Qpc();
            BitBlt(mem, 0, 0, w, h, wdc, 0, 0, SRCCOPY);
            e.push_back(Ms(Qpc() - t0, f.QuadPart));
        }
        ReleaseDC(win, wdc);
        Stats("E BitBlt(own window DC)", e);

        // F: with capture exclusion on, can we still capture our own window? The
        // evidence screenshots depend on the answer.
        const BOOL affinity = SetWindowDisplayAffinity(win, WDA_EXCLUDEFROMCAPTURE);
        std::printf("\nSetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) = %s\n",
                    affinity ? "OK" : "FAIL");
        Sleep(300);

        unsigned px = 0;
        memset(bits, 0, static_cast<std::size_t>(w) * h * 4);
        const BOOL pw = PrintWindow(win, mem, PW_RENDERFULLCONTENT);
        ReadCenter(bits, w, h, &px);
        std::printf("F PrintWindow(PW_RENDERFULLCONTENT) ok=%d center=%u,%u,%u\n", pw ? 1 : 0,
                    px & 0xFF, (px >> 8) & 0xFF, (px >> 16) & 0xFF);

        memset(bits, 0, static_cast<std::size_t>(w) * h * 4);
        HDC wdc2 = GetDC(win);
        const BOOL bb = BitBlt(mem, 0, 0, w, h, wdc2, 0, 0, SRCCOPY);
        ReleaseDC(win, wdc2);
        ReadCenter(bits, w, h, &px);
        std::printf("F BitBlt(own window DC)          ok=%d center=%u,%u,%u\n", bb ? 1 : 0,
                    px & 0xFF, (px >> 8) & 0xFF, (px >> 16) & 0xFF);

        memset(bits, 0, static_cast<std::size_t>(w) * h * 4);
        BitBlt(mem, 0, 0, w, h, screen0, 500, 500, SRCCOPY);
        ReadCenter(bits, w, h, &px);
        std::printf("F BitBlt(screen DC)              center=%u,%u,%u\n", px & 0xFF,
                    (px >> 8) & 0xFF, (px >> 16) & 0xFF);

        SetWindowDisplayAffinity(win, WDA_NONE);
        DestroyWindow(win);
    }

    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(dib);
    ReleaseDC(nullptr, screen0);
    return 0;
}
