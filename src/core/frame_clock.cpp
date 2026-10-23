// DockForge - frame time sampling implementation.
#include "core/frame_clock.h"

#include "platform/win_common.h"

#include <algorithm>
#include <cmath>

namespace dockforge::core {
namespace {

std::int64_t QpcFrequency() {
    static const std::int64_t frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value.QuadPart > 0 ? value.QuadPart : 1;
    }();
    return frequency;
}

}  // namespace

double QpcToMilliseconds(std::int64_t ticks) {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(QpcFrequency());
}

std::int64_t QueryPerformanceTicks() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

FrameClock::FrameClock(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {
    samples_.resize(capacity_, 0.0);
}

double FrameClock::Tick() {
    const std::int64_t now = QueryPerformanceTicks();
    const std::int64_t previous = last_tick_qpc_;
    last_tick_qpc_ = now;
    if (previous == 0) return 0.0;  // first tick only establishes the baseline

    const double elapsed = QpcToMilliseconds(now - previous);
    AddSample(elapsed);
    return elapsed;
}

void FrameClock::AddSample(double milliseconds) {
    samples_[next_] = milliseconds;
    next_ = (next_ + 1) % capacity_;
    if (count_ < capacity_) ++count_;
    last_ms_ = milliseconds;
}

double FrameClock::AverageMs() const {
    if (count_ == 0) return 0.0;
    double total = 0.0;
    for (std::size_t i = 0; i < count_; ++i) total += samples_[i];
    return total / static_cast<double>(count_);
}

double FrameClock::Percentile(double percentile) const {
    if (count_ == 0) return 0.0;
    const double clamped = std::clamp(percentile, 0.0, 100.0);

    std::vector<double> scratch(samples_.begin(), samples_.begin() + count_);
    // index of the requested percentile in a 0-based sorted array
    const std::size_t rank = static_cast<std::size_t>(
        std::ceil(clamped / 100.0 * static_cast<double>(count_)));
    const std::size_t index = rank == 0 ? 0 : std::min(rank - 1, count_ - 1);
    std::nth_element(scratch.begin(), scratch.begin() + index, scratch.end());
    return scratch[index];
}

double FrameClock::AverageFps() const {
    const double average = AverageMs();
    return average > 0.0 ? 1000.0 / average : 0.0;
}

void FrameClock::Reset() {
    std::fill(samples_.begin(), samples_.end(), 0.0);
    next_ = 0;
    count_ = 0;
    last_ms_ = 0.0;
    last_tick_qpc_ = 0;
}

}  // namespace dockforge::core
