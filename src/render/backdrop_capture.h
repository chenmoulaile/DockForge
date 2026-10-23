// DockForge - desktop backdrop capture on a worker thread (T-005).
//
// The spike measured one BitBlt of the composited desktop at a flat ~78ms for
// every region size from 160x60 to 2560x480, while consuming only 0.5% of that as
// CPU time. It is a blocking compositor round-trip, not bandwidth: reading our own
// window DC takes 0.35ms. That number rules out capturing on the UI thread inside a
// 16.7ms frame, and the near-zero CPU share means a worker thread costs nothing.
//
// The worker owns the screen DC and the DIB. The UI thread asks for a copy only
// when the published generation moved, so a steady 60 FPS renderer performs zero
// pixel copies on most frames and a slow capture delays the frost but never stalls
// a frame. Frames are copied out under the lock rather than handed out as raw
// pointers: a pointer into a double buffer can be recycled mid-read, and that class
// of bug shows up as a one-frame tear that is nearly impossible to reproduce.
#pragma once

#include "platform/win_common.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace dockforge::render {

class BackdropCapture {
  public:
    BackdropCapture() = default;
    ~BackdropCapture();

    BackdropCapture(const BackdropCapture&) = delete;
    BackdropCapture& operator=(const BackdropCapture&) = delete;

    // Starts the worker. `hwnd` must already have WDA_EXCLUDEFROMCAPTURE set, or
    // the worker will photograph our own window and feed the blur its own output.
    // `interval_ms` below ~120ms buys nothing: the compositor round-trip itself is
    // ~78ms and the desktop behind a Dock changes slowly.
    bool Start(HWND hwnd, int x, int y, int width_px, int height_px, int interval_ms = 200);
    void Stop();

    // Moves and resizes the capture region, in physical screen pixels. Safe to call
    // from the UI thread while running; the worker allocates the new DIB itself.
    // The origin is explicit because BitBlt reads the whole desktop: the worker
    // cannot derive where the window is without touching window state off-thread.
    void SetRegion(int x, int y, int width_px, int height_px);

    // Copies the newest frame into `out` if its generation is newer than
    // `last_generation`. Returns false when nothing new is available, which is the
    // common case at 60 FPS with a 200ms capture interval.
    bool AcquireIfNew(std::uint64_t last_generation, std::vector<std::uint8_t>& out, int& width,
                      int& height, std::uint64_t& generation) const;

    // True once at least one capture succeeded.
    bool HasFrame() const;

    struct Stats {
        std::uint64_t attempts = 0;
        std::uint64_t successes = 0;
        double last_ms = 0.0;
        double average_ms = 0.0;
    };
    Stats GetStats() const;

  private:
    void Run();
    bool EnsureSurfaces();
    void ReleaseSurfaces();

    HWND hwnd_ = nullptr;
    // Region requested by the UI thread. Atomic because the worker re-reads it
    // every iteration to pick up a move or resize without a lock on the hot path.
    std::atomic<int> x_{0};
    std::atomic<int> y_{0};
    std::atomic<int> width_px_{0};
    std::atomic<int> height_px_{0};
    int interval_ms_ = 200;

    // Worker-private GDI objects.
    HDC screen_dc_ = nullptr;
    HDC mem_dc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ old_obj_ = nullptr;
    void* bits_ = nullptr;
    int allocated_w_ = 0;
    int allocated_h_ = 0;

    mutable std::mutex mutex_;
    std::vector<std::uint8_t> published_;
    int published_w_ = 0;
    int published_h_ = 0;
    std::uint64_t generation_ = 0;
    bool has_frame_ = false;

    std::condition_variable wake_;
    std::mutex wake_mutex_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> attempts_{0};
    std::atomic<std::uint64_t> successes_{0};
    std::atomic<double> last_ms_{0.0};
    std::atomic<double> total_ms_{0.0};
    std::thread worker_;
};

}  // namespace dockforge::render
