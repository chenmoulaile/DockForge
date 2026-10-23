// DockForge - desktop backdrop capture worker (T-005).
#include "render/backdrop_capture.h"

#include "core/log.h"

#include <algorithm>
#include <chrono>

namespace dockforge::render {
namespace {

std::int64_t Qpc() {
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

double Ms(std::int64_t ticks) {
    static const double freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    return static_cast<double>(ticks) * 1000.0 / freq;
}

}  // namespace

BackdropCapture::~BackdropCapture() { Stop(); }

bool BackdropCapture::Start(HWND hwnd, int x, int y, int width_px, int height_px,
                            int interval_ms) {
    if (running_.load()) return true;
    if (!hwnd || width_px <= 0 || height_px <= 0) return false;

    hwnd_ = hwnd;
    x_.store(x);
    y_.store(y);
    width_px_.store(width_px);
    height_px_.store(height_px);
    interval_ms_ = std::clamp(interval_ms, 50, 2000);

    running_.store(true);
    worker_ = std::thread([this] { Run(); });
    DF_INFO("render", "backdrop capture started: %dx%d @%d,%d every %dms", width_px, height_px, x,
            y, interval_ms_);
    return true;
}

void BackdropCapture::Stop() {
    if (!running_.exchange(false)) {
        if (worker_.joinable()) worker_.join();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();

    // GDI objects are created on the worker, so they must be released after it
    // stops. Deleting a DIB still selected into a DC leaks both.
    ReleaseSurfaces();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        published_.clear();
        published_w_ = 0;
        published_h_ = 0;
        has_frame_ = false;
    }
    DF_INFO("render", "backdrop capture stopped: attempts=%llu successes=%llu",
            static_cast<unsigned long long>(attempts_.load()),
            static_cast<unsigned long long>(successes_.load()));
}

void BackdropCapture::SetRegion(int x, int y, int width_px, int height_px) {
    if (width_px <= 0 || height_px <= 0) return;
    x_.store(x);
    y_.store(y);
    width_px_.store(width_px);
    height_px_.store(height_px);
    // Wake the worker so a resize is picked up immediately instead of after the
    // current sleep interval; a stale-size capture would be drawn stretched.
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
    }
    wake_.notify_all();
}

bool BackdropCapture::AcquireIfNew(std::uint64_t last_generation,
                                   std::vector<std::uint8_t>& out, int& width, int& height,
                                   std::uint64_t& generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_frame_ || generation_ <= last_generation) return false;
    out = published_;
    width = published_w_;
    height = published_h_;
    generation = generation_;
    return true;
}

bool BackdropCapture::HasFrame() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return has_frame_;
}

BackdropCapture::Stats BackdropCapture::GetStats() const {
    Stats stats;
    stats.attempts = attempts_.load();
    stats.successes = successes_.load();
    stats.last_ms = last_ms_.load();
    const std::uint64_t successes = stats.successes;
    if (successes > 0) {
        stats.average_ms = total_ms_.load() / static_cast<double>(successes);
    }
    return stats;
}

void BackdropCapture::ReleaseSurfaces() {
    if (mem_dc_) {
        if (old_obj_) SelectObject(mem_dc_, old_obj_);
        DeleteDC(mem_dc_);
        mem_dc_ = nullptr;
        old_obj_ = nullptr;
    }
    if (dib_) {
        DeleteObject(dib_);
        dib_ = nullptr;
    }
    bits_ = nullptr;
    allocated_w_ = 0;
    allocated_h_ = 0;
    if (screen_dc_) {
        ReleaseDC(nullptr, screen_dc_);
        screen_dc_ = nullptr;
    }
}

bool BackdropCapture::EnsureSurfaces() {
    const int w = width_px_.load();
    const int h = height_px_.load();
    if (w <= 0 || h <= 0) return false;
    if (bits_ && allocated_w_ == w && allocated_h_ == h) return true;

    ReleaseSurfaces();

    screen_dc_ = GetDC(nullptr);
    if (!screen_dc_) return false;

    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih);
    bih.biWidth = w;
    bih.biHeight = -h;  // top-down: row 0 is the top of the screen
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    dib_ = CreateDIBSection(screen_dc_, reinterpret_cast<BITMAPINFO*>(&bih), DIB_RGB_COLORS,
                            &bits_, nullptr, 0);
    if (!dib_) {
        DF_WARN("render", "CreateDIBSection(%dx%d) failed: %lu", w, h, GetLastError());
        ReleaseSurfaces();
        return false;
    }
    mem_dc_ = CreateCompatibleDC(screen_dc_);
    if (!mem_dc_) {
        ReleaseSurfaces();
        return false;
    }
    old_obj_ = SelectObject(mem_dc_, dib_);
    allocated_w_ = w;
    allocated_h_ = h;
    return true;
}

void BackdropCapture::Run() {
    // The screen DC and the DIB live for the worker's lifetime; recreating them per
    // capture costs a kernel handle pair every 200ms for no benefit.
    if (!EnsureSurfaces()) {
        DF_WARN("render", "backdrop capture could not allocate surfaces; worker exits");
        running_.store(false);
        return;
    }

    while (running_.load()) {
        // A resize invalidates the DIB; the loop re-checks the requested size every
        // iteration so a move or resize is never more than one interval late.
        const int want_w = width_px_.load();
        const int want_h = height_px_.load();
        if (want_w != allocated_w_ || want_h != allocated_h_) {
            if (!EnsureSurfaces()) {
                DF_WARN("render", "backdrop capture lost its surfaces; retrying next tick");
                std::unique_lock<std::mutex> lock(wake_mutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(interval_ms_),
                               [this] { return !running_.load(); });
                continue;
            }
        }

        const int cx = x_.load();
        const int cy = y_.load();
        const std::int64_t t0 = Qpc();
        attempts_.fetch_add(1);
        const BOOL ok = BitBlt(mem_dc_, 0, 0, allocated_w_, allocated_h_, screen_dc_, cx, cy,
                               SRCCOPY);
        const double elapsed = Ms(Qpc() - t0);
        last_ms_.store(elapsed);

        if (ok && bits_) {
            total_ms_.store(total_ms_.load() + elapsed);
            successes_.fetch_add(1);
            const std::size_t bytes = static_cast<std::size_t>(allocated_w_) *
                                      static_cast<std::size_t>(allocated_h_) * 4u;
            std::lock_guard<std::mutex> lock(mutex_);
            published_.assign(static_cast<const std::uint8_t*>(bits_),
                              static_cast<const std::uint8_t*>(bits_) + bytes);
            published_w_ = allocated_w_;
            published_h_ = allocated_h_;
            ++generation_;
            has_frame_ = true;
        } else {
            // Keep the previous frame: a failed readback is not a reason to make the
            // plate flash. The next tick usually succeeds.
            DF_DEBUG("render", "backdrop BitBlt failed: %lu", GetLastError());
        }

        std::unique_lock<std::mutex> lock(wake_mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(interval_ms_),
                       [this] { return !running_.load(); });
    }

    ReleaseSurfaces();
}

}  // namespace dockforge::render
