// DockForge - frame time sampling (PERFORMANCE.md budgets).
//
// Stores the last N frame durations in a fixed ring buffer; percentile queries
// copy the live window into a scratch vector and use nth_element. Allocation
// happens only in the query, never per frame.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dockforge::core {

class FrameClock {
  public:
    // 1000 samples covers a 16s window at 60 FPS, enough for a stable p99.
    explicit FrameClock(std::size_t capacity = 1000);

    // Records one frame. Returns the frame duration in milliseconds.
    double Tick();
    // Records an externally measured duration (used by benchmarks).
    void AddSample(double milliseconds);

    std::size_t SampleCount() const { return count_; }
    double LastMs() const { return last_ms_; }
    double AverageMs() const;
    // percentile in [0,100]; returns 0 when no samples exist.
    double Percentile(double percentile) const;
    // Frames per second implied by the average frame time.
    double AverageFps() const;
    void Reset();

  private:
    std::vector<double> samples_;
    std::size_t capacity_;
    std::size_t next_ = 0;
    std::size_t count_ = 0;
    double last_ms_ = 0.0;
    std::int64_t last_tick_qpc_ = 0;
};

// Converts QPC ticks to milliseconds using the machine's frequency once.
double QpcToMilliseconds(std::int64_t ticks);
std::int64_t QueryPerformanceTicks();

}  // namespace dockforge::core
