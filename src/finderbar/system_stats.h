// DockForge - system stats sampler: CPU % + memory (T-068, C-23).
//
// Contract constraints:
//  - GetSystemTimes + GlobalMemoryStatusEx, sampled every 2 seconds (0.5Hz
//    ceiling), NEVER on the render path (the panel tick owns the cadence).
//  - The CPU math is a pure function over two counter snapshots so a unit test
//    can pin it (idle time is INCLUDED in kernel time per GetSystemTimes docs;
//    forgetting that double-counts idle as busy).
#pragma once

#include "finderbar/hardware_types.h"

#include <cstdint>

namespace dockforge::finderbar {

// Raw counters of one sampling instant.
struct SystemCounters {
    uint64_t idle_ticks = 0;    // FILETIME ticks; included in kernel_ticks
    uint64_t kernel_ticks = 0;  // includes idle
    uint64_t user_ticks = 0;
    uint64_t memory_total_bytes = 0;
    uint64_t memory_used_bytes = 0;
};

// Pure CPU% between two snapshots. Returns the busy fraction of the total
// elapsed CPU time in percent. Degenerate cases (no time elapsed, counters
// went backwards e.g. after sleep) return 0 rather than NaN/garbage.
float ComputeCpuPercent(const SystemCounters& prev, const SystemCounters& cur);

// Pure memory percent helper (0..100), clamped.
float ComputeMemoryPercent(uint64_t used_bytes, uint64_t total_bytes);

// Reads the machine's current counters (GetSystemTimes +
// GlobalMemoryStatusEx). Wrapped so tests never call it directly - they inject
// snapshots instead.
SystemCounters ReadSystemCounters();

// Stateful sampler: Update() takes a new snapshot, computes the delta against
// the previous one, and returns the C-23 event. The first Update after
// construction (no baseline yet) returns zero CPU - one 2-second interval of
// "0%" on open is the documented, measured cost of not keeping a sampler warm
// forever.
class SystemStatsSampler {
  public:
    SystemStatsUpdated Update();
    // Test seam: samples from `fn` instead of the machine. Nullptr restores
    // the real reader.
    void SetCounterSourceForTest(SystemCounters (*fn)());

  private:
    SystemCounters prev_{};
    bool has_prev_ = false;
};

}  // namespace dockforge::finderbar
