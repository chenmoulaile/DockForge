// DockForge - system stats sampler implementation (T-068, C-23).
#include "finderbar/system_stats.h"

#include <windows.h>

namespace dockforge::finderbar {

float ComputeCpuPercent(const SystemCounters& prev, const SystemCounters& cur) {
    // GetSystemTimes: kernel time INCLUDES idle time. Total elapsed CPU =
    // kernel + user; busy = total - idle. All deltas are unsigned-safe because
    // the comparison below rejects a backwards clock.
    const uint64_t kernel = cur.kernel_ticks > prev.kernel_ticks
                                ? cur.kernel_ticks - prev.kernel_ticks
                                : 0;
    const uint64_t user =
        cur.user_ticks > prev.user_ticks ? cur.user_ticks - prev.user_ticks : 0;
    const uint64_t idle =
        cur.idle_ticks > prev.idle_ticks ? cur.idle_ticks - prev.idle_ticks : 0;
    const uint64_t total = kernel + user;
    if (total == 0) return 0.0f;
    if (idle > total) return 0.0f;
    const float busy = static_cast<float>(total - idle);
    const float ratio = busy / static_cast<float>(total);
    if (ratio < 0.0f) return 0.0f;
    if (ratio > 1.0f) return 100.0f;
    return ratio * 100.0f;
}

float ComputeMemoryPercent(uint64_t used_bytes, uint64_t total_bytes) {
    if (total_bytes == 0) return 0.0f;
    const float ratio = static_cast<float>(used_bytes) / static_cast<float>(total_bytes);
    if (ratio < 0.0f) return 0.0f;
    if (ratio > 1.0f) return 100.0f;
    return ratio * 100.0f;
}

SystemCounters ReadSystemCounters() {
    SystemCounters out{};
    FILETIME idle{}, kernel{}, user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        const auto ticks = [](const FILETIME& ft) {
            return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        };
        out.idle_ticks = ticks(idle);
        out.kernel_ticks = ticks(kernel);
        out.user_ticks = ticks(user);
    }
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        out.memory_total_bytes = mem.ullTotalPhys;
        out.memory_used_bytes =
            mem.ullTotalPhys > mem.ullAvailPhys ? mem.ullTotalPhys - mem.ullAvailPhys : 0;
    }
    return out;
}

namespace {
SystemCounters (*s_counter_fn)() = nullptr;
}

void SystemStatsSampler::SetCounterSourceForTest(SystemCounters (*fn)()) {
    s_counter_fn = fn;
}

SystemStatsUpdated SystemStatsSampler::Update() {
    const SystemCounters cur = s_counter_fn ? s_counter_fn() : ReadSystemCounters();
    SystemStatsUpdated event{};
    event.cpu_percent = has_prev_ ? ComputeCpuPercent(prev_, cur) : 0.0f;
    event.memory_percent = ComputeMemoryPercent(cur.memory_used_bytes,
                                                cur.memory_total_bytes);
    event.memory_used_bytes = cur.memory_used_bytes;
    event.memory_total_bytes = cur.memory_total_bytes;
    prev_ = cur;
    has_prev_ = true;
    return event;
}

}  // namespace dockforge::finderbar
