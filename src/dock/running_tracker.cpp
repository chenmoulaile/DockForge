#include "dock/running_tracker.h"

#include "core/frame_clock.h"
#include "core/log.h"

#include <algorithm>
#include <string>

namespace dockforge::dock {

const char* ToString(TileRunState state) {
    switch (state) {
        case TileRunState::Idle:
            return "idle";
        case TileRunState::Running:
            return "running";
        case TileRunState::Foreground:
            return "foreground";
    }
    return "idle";
}

namespace {

// Compares two window snapshots for "the dock would draw the same thing".
//
// Only the fields the indicator reads are compared. Titles are deliberately left
// out: a browser rewriting its title on every navigation would otherwise repaint
// the dock, and the dot does not depend on the title.
bool SameVisibleSet(const std::vector<shell::AppWindow>& a,
                    const std::vector<shell::AppWindow>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].hwnd != b[i].hwnd) return false;
        if (a[i].app_pid != b[i].app_pid) return false;
    }
    return true;
}

}  // namespace

RunningTracker::RunningTracker(int interval_ms) { SetIntervalMs(interval_ms); }

void RunningTracker::SetIntervalMs(int interval_ms) {
    interval_ms_ = std::clamp(interval_ms, 50, 5000);
}

bool RunningTracker::Update(double delta_ms) {
    if (delta_ms > 0.0) {
        since_poll_ms_ += delta_ms;
    }

    bool changed = false;

    // Foreground is read every frame. It is one syscall and it is the state that
    // alt-tab changes, so sampling it on the poll interval would make the highlight
    // lag by up to 300ms.
    const HWND foreground = GetForegroundWindow();
    if (foreground != foreground_) {
        foreground_ = foreground;
        ++stats_.foreground_changes;
        changed = true;
    }

    next_poll_ms_ -= (delta_ms > 0.0 ? delta_ms : 0.0);
    if (next_poll_ms_ <= 0.0) {
        const std::int64_t start = core::QueryPerformanceTicks();
        std::vector<shell::AppWindow> fresh = shell::EnumerateAppWindows();
        const double elapsed =
            core::QpcToMilliseconds(core::QueryPerformanceTicks() - start);

        ++stats_.polls;
        stats_.last_poll_ms = elapsed;
        stats_.total_poll_ms += elapsed;
        stats_.max_poll_ms = std::max(stats_.max_poll_ms, elapsed);

        // A poll that returns the same windows does not force a repaint. On an idle
        // desktop this is every poll, which is why the dock costs 0.003% CPU rather
        // than repainting 3.3 times a second for nothing.
        if (!SameVisibleSet(windows_, fresh)) {
            windows_ = std::move(fresh);
            changed = true;
        }
        next_poll_ms_ = static_cast<double>(interval_ms_);
    }

    return changed;
}

TileRunState RunningTracker::StateFor(const shell::AppEntry& app) const {
    shell::AppWindow hit;
    if (!shell::FindAppWindow(app, windows_, hit)) return TileRunState::Idle;

    // A foreground *child* window (a dialog, a popup) still counts as the app being
    // in front. Without walking up, opening a modal dialog would drop the highlight
    // from the app that owns it.
    HWND probe = foreground_;
    for (int depth = 0; probe && depth < 8; ++depth) {
        if (probe == hit.hwnd) return TileRunState::Foreground;
        probe = GetAncestor(probe, GA_ROOTOWNER);
        if (probe == foreground_) break;  // GA_ROOTOWNER returned the same window
    }
    // GetAncestor(GA_ROOTOWNER) does not always land on our top-level window, so the
    // pid is the fallback: the foreground window belonging to the same process means
    // the same app is in front.
    if (foreground_) {
        DWORD fg_pid = 0;
        GetWindowThreadProcessId(foreground_, &fg_pid);
        if (fg_pid != 0 && (fg_pid == hit.app_pid || fg_pid == hit.pid)) {
            return TileRunState::Foreground;
        }
    }
    return TileRunState::Running;
}

void RunningTracker::ApplyToTiles(std::vector<DockTile>& tiles) const {
    for (auto& tile : tiles) {
        tile.run_state = StateFor(tile.app);
    }
}

}  // namespace dockforge::dock
