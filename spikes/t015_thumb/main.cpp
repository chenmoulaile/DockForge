// DockForge spike T-015: does DwmRegisterThumbnail work for a preview popup?
//
// Four questions, each of which changes the design:
//   1. Does a thumbnail render into a plain window at all on this host?
//   2. Does it render into a WS_EX_NOREDIRECTIONBITMAP window (what the dock uses)?
//   3. What does a minimized source show - last frame, black, or nothing?
//   4. Do N thumbnails in one destination work, and what do they cost?
//
// The answers come from sampling the composited desktop, not from HRESULTs:
// DwmRegisterThumbnail returning S_OK says the registration was accepted, not
// that any pixels appeared.

#include <windows.h>
#include <dwmapi.h>
#include <shellscalingapi.h>

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

std::string ToUtf8(const wchar_t* text) {
    if (text == nullptr) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};
    std::string out(static_cast<std::size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), need, nullptr, nullptr);
    return out;
}

constexpr int kSourceW = 260;
constexpr int kSourceH = 160;
// Slot geometry in DIP. The destination window and every DWM rectangle are in
// physical pixels, so each is multiplied by the monitor scale once, here, and the
// resulting pixel values are used everywhere after that. Mixing the two units is
// how the first version of this spike sampled outside its own window and reported
// "no thumbnail" for a thumbnail that was drawing fine.
constexpr int kDestPadDip = 14;
constexpr int kMaxSlots = 8;

// Slot geometry is mutable so one build can answer "is the limit a count or an area"
// without a recompile per hypothesis.
int g_slot_w_dip = 200;
int g_slot_h_dip = 130;
int g_slot_gap_dip = 12;
int g_dest_x = -1;  // -1 = centre on the primary monitor
int g_dest_y = -1;  // -1 = 80px above the bottom edge

int g_scale_px = 1;  // 1, 2 or 3: DIP -> physical multiplier for this monitor

int Px(int dip) { return dip * g_scale_px; }

FILE* g_log = nullptr;

void Log(const char* format, ...) {
    if (g_log == nullptr) return;
    va_list args;
    va_start(args, format);
    std::vfprintf(g_log, format, args);
    va_end(args);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

COLORREF ColourFor(int index) {
    const int r = 40 + (index * 47) % 190;
    const int g = 60 + (index * 71) % 170;
    const int b = 200 - (index * 23) % 150;
    return RGB(r, g, b);
}

LRESULT CALLBACK SourceProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const auto colour = static_cast<COLORREF>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        HBRUSH brush = CreateSolidBrush(colour);
        FillRect(dc, &rc, brush);
        DeleteObject(brush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK DestProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        // A near-black fill so a rendered thumbnail is unmistakable: the sources
        // are saturated colours, the fill is not.
        HBRUSH brush = CreateSolidBrush(RGB(8, 9, 11));
        FillRect(dc, &rc, brush);
        DeleteObject(brush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

COLORREF SampleScreen(int x, int y) {
    HDC screen = GetDC(nullptr);
    if (screen == nullptr) return CLR_INVALID;
    const COLORREF colour = GetPixel(screen, x, y);
    ReleaseDC(nullptr, screen);
    return colour;
}

std::vector<HWND> g_repaint_targets;

void PumpFor(int milliseconds) {
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(milliseconds);
    for (;;) {
        for (HWND target : g_repaint_targets) InvalidateRect(target, nullptr, TRUE);
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (GetTickCount64() >= end) break;
        Sleep(5);
    }
}

struct Options {
    std::wstring mode = L"run";
    std::wstring dest = L"plain";
    int count = 3;
    int minimize = -1;
    int hold_ms = 2500;
    std::wstring source_title;
    std::wstring out;
    int reverse = 0;   // 1 = register the source list back to front
    int repaint = 0;   // 1 = keep invalidating the sources so DWM sees fresh frames
    int vertical = 0;  // 1 = stack slots in a column instead of a row
    int topmost = 1;   // 0 = drop WS_EX_TOPMOST from the destination
    int pad = 0;       // extra DIP of empty window on each side, slots unchanged
    int pump_ms = 0;   // ms to pump between registrations
};

struct Session {
    HINSTANCE instance = nullptr;
    std::vector<HWND> sources;
    std::vector<COLORREF> colours;
    HWND dest = nullptr;
    RECT dest_rect{};
    std::vector<HTHUMBNAIL> thumbs;
};

bool RegisterClasses(Session& session) {
    WNDCLASSEXW src{};
    src.cbSize = sizeof(src);
    src.style = CS_HREDRAW | CS_VREDRAW;
    src.lpfnWndProc = &SourceProc;
    src.hInstance = session.instance;
    src.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    src.hbrBackground = nullptr;
    src.lpszClassName = L"SpikeT015Source";
    if (RegisterClassExW(&src) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log("register source class failed: %lu", GetLastError());
        return false;
    }

    WNDCLASSEXW dst{};
    dst.cbSize = sizeof(dst);
    dst.style = CS_HREDRAW | CS_VREDRAW;
    dst.lpfnWndProc = &DestProc;
    dst.hInstance = session.instance;
    dst.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    dst.hbrBackground = nullptr;
    dst.lpszClassName = L"SpikeT015Dest";
    if (RegisterClassExW(&dst) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log("register dest class failed: %lu", GetLastError());
        return false;
    }
    return true;
}

void CreateSources(Session& session, int count) {
    const int screen_w = GetSystemMetrics(SM_CXSCREEN);
    for (int i = 0; i < count; ++i) {
        const COLORREF colour = ColourFor(i);
        const int x = 40 + (i % 4) * (kSourceW + 16);
        const int y = 60 + (i / 4) * (kSourceH + 16);
        HWND hwnd = CreateWindowExW(0, L"SpikeT015Source", L"spike source",
                                    WS_POPUP | WS_VISIBLE, x, y, kSourceW, kSourceH,
                                    nullptr, nullptr, session.instance, nullptr);
        if (hwnd == nullptr) {
            Log("source %d create failed: %lu", i, GetLastError());
            continue;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, static_cast<LONG_PTR>(colour));
        SetWindowTextW(hwnd, (L"spike source " + std::to_wstring(i)).c_str());
        UpdateWindow(hwnd);
        session.sources.push_back(hwnd);
        session.colours.push_back(colour);
    }
    (void)screen_w;
    Log("sources=%zu", session.sources.size());
}

bool g_vertical = false;

void SlotRect(int index, RECT& out) {
    if (g_vertical) {
        out.left = Px(kDestPadDip);
        out.top = Px(kDestPadDip + index * (g_slot_h_dip + g_slot_gap_dip));
        out.right = out.left + Px(g_slot_w_dip);
        out.bottom = out.top + Px(g_slot_h_dip);
        return;
    }
    out.left = Px(kDestPadDip + index * (g_slot_w_dip + g_slot_gap_dip));
    out.top = Px(kDestPadDip);
    out.right = out.left + Px(g_slot_w_dip);
    out.bottom = out.top + Px(g_slot_h_dip);
}

void CreateDestination(Session& session, const Options& options, int slot_count) {
    g_vertical = options.vertical != 0;
    const int span = slot_count * (g_vertical ? g_slot_h_dip : g_slot_w_dip) +
                     (slot_count - 1) * g_slot_gap_dip;
    const int width_dip = (g_vertical ? kDestPadDip * 2 + g_slot_w_dip : kDestPadDip * 2 + span) +
                          options.pad * 2;
    const int height_dip = (g_vertical ? kDestPadDip * 2 + span : kDestPadDip * 2 + g_slot_h_dip) +
                           options.pad * 2;
    // CreateWindowExW takes physical pixels for a per-monitor-DPI-aware process.
    const int width = Px(width_dip);
    const int height = Px(height_dip);
    const int x = g_dest_x >= 0 ? g_dest_x : (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = g_dest_y >= 0 ? g_dest_y : GetSystemMetrics(SM_CYSCREEN) - height - 80;

    DWORD ex_style = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (options.topmost != 0) ex_style |= WS_EX_TOPMOST;
    if (options.dest == L"noredir") {
        ex_style |= WS_EX_NOREDIRECTIONBITMAP;
    }
    session.dest = CreateWindowExW(ex_style, L"SpikeT015Dest", L"spike dest",
                                   WS_POPUP | WS_VISIBLE, x, y, width, height, nullptr,
                                   nullptr, session.instance, nullptr);
    if (session.dest == nullptr) {
        Log("dest create failed: %lu", GetLastError());
        return;
    }
    SetWindowPos(session.dest, options.topmost != 0 ? HWND_TOPMOST : HWND_TOP, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateWindow(session.dest);
    GetWindowRect(session.dest, &session.dest_rect);
    RECT client{};
    GetClientRect(session.dest, &client);
    Log("client rect=%ld,%ld..%ld,%ld", client.left, client.top, client.right, client.bottom);
    Log("dest=%s rect=%ld,%ld %dx%d dip=%dx%d scale=%d slot=%dx%d gap=%d", ToUtf8(options.dest.c_str()).c_str(),
        session.dest_rect.left, session.dest_rect.top, width, height, width_dip, height_dip,
        g_scale_px, g_slot_w_dip, g_slot_h_dip, g_slot_gap_dip);
}

void RegisterThumbnails(Session& session, int slot_count, bool reverse = false, int pump_ms = 0) {
    for (int step = 0; step < slot_count && step < static_cast<int>(session.sources.size()); ++step) {
        const int i = reverse ? (slot_count - 1 - step) : step;
        // Diagnose the source before trusting the thumbnail: a source that is cloaked
        // (another virtual desktop), hidden, or zero-sized registers fine and renders
        // nothing, and the "no thumbnail" verdict then blames DWM for a window problem.
        RECT src_rect{};
        GetWindowRect(session.sources[i], &src_rect);
        int cloaked = 0;
        DwmGetWindowAttribute(session.sources[i], DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        const COLORREF src_mid = SampleScreen((src_rect.left + src_rect.right) / 2,
                                              (src_rect.top + src_rect.bottom) / 2);
        Log("source %d rect=%ld,%ld..%ld,%ld visible=%d iconic=%d cloaked=%d screen_mid=(%u,%u,%u)",
            i, src_rect.left, src_rect.top, src_rect.right, src_rect.bottom,
            IsWindowVisible(session.sources[i]) ? 1 : 0,
            IsIconic(session.sources[i]) ? 1 : 0, cloaked, GetRValue(src_mid),
            GetGValue(src_mid), GetBValue(src_mid));

        HTHUMBNAIL thumb = nullptr;
        const HRESULT hr = DwmRegisterThumbnail(session.dest, session.sources[i], &thumb);
        if (FAILED(hr)) {
            Log("register %d hr=0x%08lX", i, static_cast<unsigned long>(hr));
            continue;
        }
        SIZE source_size{};
        const HRESULT qhr = DwmQueryThumbnailSourceSize(thumb, &source_size);

        // DWM rectangle is in the destination window's client coordinates, which for a
        // per-monitor-DPI-aware process are physical pixels. DIP here would put every
        // slot at half position on a 200% monitor.
        DWM_THUMBNAIL_PROPERTIES props{};
        props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY |
                        DWM_TNP_OPACITY;
        SlotRect(i, props.rcDestination);
        props.opacity = 255;
        props.fVisible = TRUE;
        props.fSourceClientAreaOnly = FALSE;
        const HRESULT uhr = DwmUpdateThumbnailProperties(thumb, &props);
        Log("register %d hr=0x%08lX query=0x%08lX size=%ldx%ld update=0x%08lX dest=%ld,%ld..%ld,%ld",
            i, static_cast<unsigned long>(hr), static_cast<unsigned long>(qhr), source_size.cx,
            source_size.cy, static_cast<unsigned long>(uhr), props.rcDestination.left,
            props.rcDestination.top, props.rcDestination.right, props.rcDestination.bottom);
        if (FAILED(uhr)) Log("WARN update %d failed hr=0x%08lX", i, static_cast<unsigned long>(uhr));
        if (pump_ms > 0) PumpFor(pump_ms);
        session.thumbs.push_back(thumb);
    }
    Log("thumbs=%zu", session.thumbs.size());
}

// Walk the destination row and report where each source colour actually landed.
// A single centre sample cannot tell "not drawn" from "drawn somewhere else", and the
// first two runs of this spike confused the two: slot 1 read as NO_THUMBNAIL while its
// colour was present 150px to the left of where the rect asked for it.
// One BitBlt for the whole row. Per-pixel GetPixel on a screen DC costs ~1ms on this
// host, so the first version of this scan took 77s for a 2576px row and every run hit
// the harness timeout before printing a single run.
void ScanRow(Session& session, const char* label) {
    const int cy = session.dest_rect.top + Px(kDestPadDip + g_slot_h_dip / 2);
    Log("--- scan %s y=%d ---", label, cy);
    const int scan_w = static_cast<int>(session.dest_rect.right - session.dest_rect.left);
    if (scan_w <= 0) return;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = scan_w;
    bi.bmiHeader.biHeight = -1;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    if (screen == nullptr) return;
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (mem == nullptr || dib == nullptr || bits == nullptr) {
        if (dib != nullptr) DeleteObject(dib);
        if (mem != nullptr) DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ old = SelectObject(mem, dib);
    const BOOL blit = BitBlt(mem, 0, 0, scan_w, 1, screen, session.dest_rect.left, cy, SRCCOPY);
    SelectObject(mem, old);
    ReleaseDC(nullptr, screen);
    if (!blit) {
        DeleteObject(dib);
        DeleteDC(mem);
        Log("scan blit failed: %lu", GetLastError());
        return;
    }
    const auto* row = static_cast<const std::uint8_t*>(bits);

    int run_start = session.dest_rect.left;
    int run_index = -2;
    for (int x = session.dest_rect.left; x <= session.dest_rect.right; ++x) {
        int which = -1;
        if (x < session.dest_rect.right) {
            const std::uint8_t* px = row + static_cast<std::size_t>(x - session.dest_rect.left) * 4;
            const COLORREF got = RGB(px[2], px[1], px[0]);
            for (std::size_t i = 0; i < session.colours.size(); ++i) {
                const COLORREF want = session.colours[i];
                const int delta =
                    std::abs(static_cast<int>(GetRValue(got)) - static_cast<int>(GetRValue(want))) +
                    std::abs(static_cast<int>(GetGValue(got)) - static_cast<int>(GetGValue(want))) +
                    std::abs(static_cast<int>(GetBValue(got)) - static_cast<int>(GetBValue(want)));
                if (delta <= 24) {
                    which = static_cast<int>(i);
                    break;
                }
            }
        }
        if (which != run_index) {
            if (run_index >= 0) {
                Log("run source=%d x=%d..%d width=%d", run_index, run_start, x - 1, x - run_start);
            }
            run_start = x;
            run_index = which;
        }
    }
    DeleteObject(dib);
    DeleteDC(mem);
}

// Full-window scan, one BitBlt per row. A single-row scan cannot distinguish "slot not
// rendered" from "slot rendered at a different y", and that ambiguity is exactly what
// made the count limit look like a DWM bug for four runs.
void ScanBox(Session& session, const char* label) {
    Log("--- box %s ---", label);
    const int w = static_cast<int>(session.dest_rect.right - session.dest_rect.left);
    const int h = static_cast<int>(session.dest_rect.bottom - session.dest_rect.top);
    if (w <= 0 || h <= 0) return;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    if (screen == nullptr) return;
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (mem == nullptr || dib == nullptr || bits == nullptr) {
        if (dib != nullptr) DeleteObject(dib);
        if (mem != nullptr) DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ old = SelectObject(mem, dib);
    BitBlt(mem, 0, 0, w, h, screen, session.dest_rect.left, session.dest_rect.top, SRCCOPY);
    SelectObject(mem, old);
    ReleaseDC(nullptr, screen);

    const auto* px = static_cast<const std::uint8_t*>(bits);
    for (std::size_t i = 0; i < session.colours.size(); ++i) {
        const COLORREF want = session.colours[i];
        int minx = 1 << 30, miny = 1 << 30, maxx = -1, maxy = -1, hits = 0;
        for (int y = 0; y < h; ++y) {
            const std::uint8_t* row = px + static_cast<std::size_t>(y) * w * 4;
            for (int x = 0; x < w; ++x) {
                const std::uint8_t* q = row + static_cast<std::size_t>(x) * 4;
                const int delta = std::abs(static_cast<int>(q[2]) - static_cast<int>(GetRValue(want))) +
                                  std::abs(static_cast<int>(q[1]) - static_cast<int>(GetGValue(want))) +
                                  std::abs(static_cast<int>(q[0]) - static_cast<int>(GetBValue(want)));
                if (delta <= 24) {
                    ++hits;
                    if (x < minx) minx = x;
                    if (x > maxx) maxx = x;
                    if (y < miny) miny = y;
                    if (y > maxy) maxy = y;
                }
            }
        }
        if (hits == 0) {
            Log("box source=%zu ABSENT", i);
        } else {
            Log("box source=%zu x=%d..%d y=%d..%d size=%dx%d hits=%d", i, minx, maxx, miny, maxy,
                maxx - minx + 1, maxy - miny + 1, hits);
        }
    }
    DeleteObject(dib);
    DeleteDC(mem);
}
// Dump the actual colours across one row so a "missing" verdict can be checked against
// what is really on screen instead of against the matcher's opinion of it.
void DumpRow(Session& session, const char* label) {
    const int cy = session.dest_rect.top + Px(kDestPadDip + g_slot_h_dip / 2);
    Log("--- dump %s y=%d ---", label, cy);
    const int scan_w = static_cast<int>(session.dest_rect.right - session.dest_rect.left);
    if (scan_w <= 0) return;
    HDC screen = GetDC(nullptr);
    if (screen == nullptr) return;
    int last = -1;
    int start = 0;
    for (int x = 0; x < scan_w; ++x) {
        const COLORREF c = GetPixel(screen, session.dest_rect.left + x, cy);
        const int packed = (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c);
        if (packed != last) {
            if (last >= 0 && x - start >= 6) {
                Log("  x=%d..%d w=%d rgb=(%d,%d,%d)", start, x - 1, x - start, (last >> 16) & 0xFF,
                    (last >> 8) & 0xFF, last & 0xFF);
            }
            start = x;
            last = packed;
        }
    }
    if (scan_w - start >= 6) {
        Log("  x=%d..%d w=%d rgb=(%d,%d,%d)", start, scan_w - 1, scan_w - start, (last >> 16) & 0xFF,
            (last >> 8) & 0xFF, last & 0xFF);
    }
    ReleaseDC(nullptr, screen);
}
void SampleSlots(Session& session, const char* label) {
    Log("--- sample %s scale=%d ---", label, g_scale_px);
    for (std::size_t i = 0; i < session.thumbs.size(); ++i) {
        // Three points across the slot, not one: a thumbnail drawn at a shifted rect
        // reads as "missing" from the centre alone, and a centre-only verdict sent me
        // chasing a DWM limit when the last slot was simply offset.
        const int slot_left = Px(kDestPadDip + static_cast<int>(i) * (g_slot_w_dip + g_slot_gap_dip));
        const int slot_w = Px(g_slot_w_dip);
        const int cy = session.dest_rect.top + Px(kDestPadDip + g_slot_h_dip / 2);
        const COLORREF want = session.colours[i];
        int best_delta = 1 << 30;
        int best_x = 0;
        for (int frac = 1; frac <= 7; ++frac) {
            const int cx = session.dest_rect.left + slot_left + slot_w * frac / 8;
            const COLORREF got = SampleScreen(cx, cy);
            const int delta = std::abs(static_cast<int>(GetRValue(got)) - static_cast<int>(GetRValue(want))) +
                              std::abs(static_cast<int>(GetGValue(got)) - static_cast<int>(GetGValue(want))) +
                              std::abs(static_cast<int>(GetBValue(got)) - static_cast<int>(GetBValue(want)));
            if (delta < best_delta) {
                best_delta = delta;
                best_x = cx;
            }
        }
        const int centre_x = session.dest_rect.left + slot_left + slot_w / 2;
        const COLORREF centre = SampleScreen(centre_x, cy);
        const bool match = best_delta <= 24;
        Log("slot %zu centre=(%d,%d) got=(%u,%u,%u) best_delta=%d at_x=%d want=(%u,%u,%u) verdict=%s",
            i, centre_x, cy, GetRValue(centre), GetGValue(centre), GetBValue(centre), best_delta,
            best_x, GetRValue(want), GetGValue(want), GetBValue(want),
            match ? "THUMBNAIL_VISIBLE" : "NO_THUMBNAIL");
    }
}

int RunThumbnailSpike(Session& session, const Options& options) {
    if (!RegisterClasses(session)) return 2;

    if (options.mode == L"uwp") {
        HWND source = FindWindowW(nullptr, options.source_title.c_str());
        if (source == nullptr) {
            Log("source window not found: %s", ToUtf8(options.source_title.c_str()).c_str());
            return 3;
        }
        wchar_t cls[128]{};
        GetClassNameW(source, cls, static_cast<int>(std::size(cls)));
        Log("source hwnd=%p class=%s title=%s", reinterpret_cast<void*>(source), ToUtf8(cls).c_str(),
            ToUtf8(options.source_title.c_str()).c_str());
        session.sources.push_back(source);
        session.colours.push_back(RGB(255, 0, 255));
    } else {
        CreateSources(session, options.count);
    }
    if (session.sources.empty()) return 4;

    if (options.repaint != 0) g_repaint_targets = session.sources;
    const int slot_count = static_cast<int>(session.sources.size());
    CreateDestination(session, options, slot_count);
    if (session.dest == nullptr) return 5;

    const ULONGLONG start = GetTickCount64();
    RegisterThumbnails(session, slot_count, options.reverse != 0, options.pump_ms);
    Log("register_cost_ms=%llu", GetTickCount64() - start);

    // Time series, not a single sample: a slot that is merely late looks identical to
    // one that never renders, and the difference decides whether the fix is patience
    // or a hard limit.
    // Force the destination to repaint after registration. If the missing slots come
    // back, DWM was only compositing the region it considered dirty - a stale-surface
    // problem, not a thumbnail limit.
    InvalidateRect(session.dest, nullptr, TRUE);
    UpdateWindow(session.dest);

    PumpFor(300);
    ScanRow(session, "t300");
    ScanBox(session, "t300");

    // Re-register every thumbnail from scratch. If the missing slots appear now, the
    // earlier absence was a race in registration order, not a compositor limit - and
    // the two demand completely different fixes.
    for (HTHUMBNAIL thumb : session.thumbs) DwmUnregisterThumbnail(thumb);
    session.thumbs.clear();
    PumpFor(150);
    RegisterThumbnails(session, slot_count, false, options.pump_ms);
    PumpFor(600);
    ScanBox(session, "after_reregister");
    if (options.hold_ms > 0) {
        PumpFor(700);
        ScanRow(session, "t1000");
        PumpFor(1000);
        ScanRow(session, "t2000");
        PumpFor(2000);
        ScanRow(session, "t4000");
    }
    SampleSlots(session, "before_minimize");

    if (options.minimize >= 0 && options.minimize < static_cast<int>(session.sources.size())) {
        Log("minimizing source %d", options.minimize);
        ShowWindow(session.sources[options.minimize], SW_MINIMIZE);
        PumpFor(700);
        SampleSlots(session, "after_minimize");
    }

    PumpFor(options.hold_ms);
    SampleSlots(session, "before_teardown");

    const int gui_before = static_cast<int>(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS));
    const int user_before = static_cast<int>(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS));
    for (HTHUMBNAIL thumb : session.thumbs) {
        DwmUnregisterThumbnail(thumb);
    }
    session.thumbs.clear();
    const int gui_after = static_cast<int>(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS));
    const int user_after = static_cast<int>(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS));
    Log("gui_objects %d -> %d, user_objects %d -> %d", gui_before, gui_after, user_before, user_after);

    // Register/unregister in a loop: a per-call leak shows up here as handle growth.
    const int gui_loop_before = static_cast<int>(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS));
    for (int round = 0; round < 40; ++round) {
        HTHUMBNAIL thumb = nullptr;
        if (SUCCEEDED(DwmRegisterThumbnail(session.dest, session.sources[0], &thumb))) {
            DwmUnregisterThumbnail(thumb);
        }
    }
    const int gui_loop_after = static_cast<int>(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS));
    Log("gui_objects after 40 register/unregister: %d -> %d", gui_loop_before, gui_loop_after);
    return 0;
}

Options ParseOptions(int argc, wchar_t** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const std::size_t eq = arg.find(L'=');
        const std::wstring key = eq == std::wstring::npos ? arg : arg.substr(0, eq);
        const std::wstring value = eq == std::wstring::npos ? L"" : arg.substr(eq + 1);
        if (key == L"--mode") options.mode = value;
        else if (key == L"--dest") options.dest = value;
        else if (key == L"--count") options.count = _wtoi(value.c_str());
        else if (key == L"--minimize") options.minimize = _wtoi(value.c_str());
        else if (key == L"--hold-ms") options.hold_ms = _wtoi(value.c_str());
        else if (key == L"--source-title") options.source_title = value;
        else if (key == L"--out") options.out = value;
        else if (key == L"--slot-w") g_slot_w_dip = _wtoi(value.c_str());
        else if (key == L"--slot-h") g_slot_h_dip = _wtoi(value.c_str());
        else if (key == L"--gap") g_slot_gap_dip = _wtoi(value.c_str());
        else if (key == L"--x") g_dest_x = _wtoi(value.c_str());
        else if (key == L"--y") g_dest_y = _wtoi(value.c_str());
        else if (key == L"--reverse") options.reverse = _wtoi(value.c_str());
        else if (key == L"--repaint") options.repaint = _wtoi(value.c_str());
        else if (key == L"--vertical") options.vertical = _wtoi(value.c_str());
        else if (key == L"--topmost") options.topmost = _wtoi(value.c_str());
        else if (key == L"--pad") options.pad = _wtoi(value.c_str());
        else if (key == L"--pump-ms") options.pump_ms = _wtoi(value.c_str());
    }
    if (options.count < 1) options.count = 1;
    if (options.count > kMaxSlots) options.count = kMaxSlots;
    if (g_slot_w_dip < 16) g_slot_w_dip = 16;
    if (g_slot_h_dip < 16) g_slot_h_dip = 16;
    if (g_slot_gap_dip < 0) g_slot_gap_dip = 0;
    return options;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const Options options = ParseOptions(argc, argv);

    // One scale for the whole spike: the host is single-monitor, and every rectangle
    // here is derived from it. A multi-monitor spike would have to carry the scale
    // per window instead.
    const UINT dpi = GetDpiForSystem();
    g_scale_px = static_cast<int>(std::lround(dpi / 96.0));
    if (g_scale_px < 1) g_scale_px = 1;

    if (!options.out.empty()) {
        _wfopen_s(&g_log, options.out.c_str(), L"wb");
    }
    if (g_log == nullptr) g_log = stdout;

    Session session;
    session.instance = GetModuleHandleW(nullptr);

    const int code = RunThumbnailSpike(session, options);
    Log("exit code=%d", code);
    if (g_log != stdout) std::fclose(g_log);
    return code;
}
