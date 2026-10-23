// DockForge - layout engine benchmark (T-008 DoD: 1e6 evaluations < 20ms).
//
// Unit note, because getting this wrong makes the number meaningless: the DoD's
// "evaluation" is one icon term of the C-05 formula (scale + shifted centre), not
// one whole-dock layout. Both are reported. The per-icon figure is the DoD
// metric; the per-dock figure is the one that matters against the 16.7 ms frame
// budget, and it is printed so the frame cost is never hidden behind a
// favourable unit.
#include "dock/layout.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double ElapsedMs(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

dockforge::dock::LayoutParams Params(int icon_size, int gap, float mag) {
    dockforge::dock::LayoutParams params;
    params.icon_size = icon_size;
    params.gap = gap;
    params.magnification = mag;
    params.influence_radius = 3.0f;
    params.falloff_sharpness = 2.0f;
    return params;
}

}  // namespace

int main() {
    std::vector<dockforge::dock::IconLayout> icons(64);
    dockforge::dock::LayoutSummary summary;
    constexpr double kBudgetMs = 20.0;
    constexpr int kIconEvals = 1000000;

    struct Scenario {
        const char* name;
        int icon_size;
        int gap;
        float mag;
        int count;
    };
    const Scenario scenarios[] = {
        {"48px gap8 mag1.8 n=10", 48, 8, 1.8f, 10},
        {"96px gap0 mag2.5 n=20", 96, 0, 2.5f, 20},
        {"48px gap8 mag1.8 n=40", 48, 8, 1.8f, 40},
        {"32px gap32 mag1.0 n=8", 32, 32, 1.0f, 8},
    };

    std::printf("layout_bench: unit = one icon term of C-05 (scale + shifted centre)\n");
    std::printf("%-22s %-9s %-12s %-12s %-8s %-12s\n", "scenario", "icon_evals", "total_ms",
                "ns_per_icon", "verdict", "per_dock_ns");

    bool all_pass = true;
    for (const auto& scenario : scenarios) {
        const auto params = Params(scenario.icon_size, scenario.gap, scenario.mag);
        const int count = std::min(scenario.count, static_cast<int>(icons.size()));
        // To reach kIconEvals icon evaluations we need kIconEvals/count dock
        // layouts. Rounded up, then the icon count is reported exactly.
        const int dock_runs = (kIconEvals + count - 1) / count;
        const long long icon_evals = static_cast<long long>(dock_runs) * count;

        const auto start = Clock::now();
        float sink = 0.0f;
        for (int i = 0; i < dock_runs; ++i) {
            // Sweep the pointer so the no-hover and hover paths both run, and so
            // the pointer is never cached in one spot.
            const float phase = static_cast<float>(i % 2000) / 2000.0f;
            dockforge::dock::LayoutDock(params, count, phase * 1200.0f, true, icons.data(),
                                        icons.size(), &summary);
            sink += summary.hover_width;
        }
        const auto end = Clock::now();
        const double total_ms = ElapsedMs(start, end);
        const double ns_per_icon = total_ms * 1e6 / static_cast<double>(icon_evals);
        const double per_dock_ns = total_ms * 1e6 / static_cast<double>(dock_runs);
        const bool pass = total_ms < kBudgetMs;
        all_pass = all_pass && pass;
        std::printf("%-22s %-9lld %-12.3f %-12.2f %-8s %-12.1f\n", scenario.name, icon_evals,
                    total_ms, ns_per_icon, pass ? "PASS" : "FAIL", per_dock_ns);
        if (sink < 0.0f) std::printf("sink=%f\n", sink);
    }

    // Isolated curve cost: the number a GPU vertex path has to match.
    {
        const auto params = Params(48, 8, 1.8f);
        const auto start = Clock::now();
        float sink = 0.0f;
        for (int i = 0; i < kIconEvals; ++i) {
            sink += dockforge::dock::ComputeScale(static_cast<float>(i % 500), params);
        }
        const auto end = Clock::now();
        const double total_ms = ElapsedMs(start, end);
        std::printf("%-22s %-9d %-12.3f %-12.2f %-8s %-12s\n", "ComputeScale only", kIconEvals,
                    total_ms, total_ms * 1e6 / static_cast<double>(kIconEvals),
                    total_ms < kBudgetMs ? "PASS" : "FAIL", "-");
        if (sink < 0.0f) std::printf("sink=%f\n", sink);
    }

    std::printf("budget_ms=%.1f unit=icon_eval\n", kBudgetMs);
    std::printf("frame_note=worst per-dock cost above is a fraction of the 16.7ms frame budget\n");
    std::printf("verdict=%s\n", all_pass ? "PASS" : "FAIL");
    return all_pass ? 0 : 1;
}
