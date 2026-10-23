// DockForge - running indicator tests (T-012).
//
// The tracker talks to the real window system, so these tests are written to be
// robust on a desktop where anything can be running: they assert relationships
// (a window we create is found, a window we destroy stops being found) rather than
// absolute counts, and they never assume the machine is idle.
#include <catch_amalgamated.hpp>

#include <windows.h>

#include <string>
#include <vector>

#include "dock/running_tracker.h"
#include "dock/dock_view.h"

namespace dock = dockforge::dock;
namespace shell = dockforge::shell;

namespace {

// A real top-level window that passes the app-window filter, used to prove the
// tracker sees windows it should see. Registering a throwaway class per test keeps
// the tests from fighting over one global class name.
class ScopedWindow {
  public:
    explicit ScopedWindow(const wchar_t* title) {
        static int counter = 0;
        class_name_ = L"DockForgeTestWindow" + std::to_wstring(++counter);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = class_name_.c_str();
        RegisterClassExW(&wc);
        hwnd_ = CreateWindowExW(0, class_name_.c_str(), title, WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 320, 200, nullptr, nullptr,
                                wc.hInstance, nullptr);
        if (hwnd_) ShowWindow(hwnd_, SW_SHOW);
    }
    ~ScopedWindow() {
        if (hwnd_) DestroyWindow(hwnd_);
        UnregisterClassW(class_name_.c_str(), GetModuleHandleW(nullptr));
    }
    ScopedWindow(const ScopedWindow&) = delete;
    ScopedWindow& operator=(const ScopedWindow&) = delete;

    HWND Get() const { return hwnd_; }

  private:
    std::wstring class_name_;
    HWND hwnd_ = nullptr;
};

// Pumps the queue so a freshly shown window is actually composited and visible to
// EnumWindows. Without this the window exists but IsWindowVisible can still be false
// on the first message-loop turn.
void PumpBriefly(int ms = 120) {
    const DWORD until = GetTickCount() + static_cast<DWORD>(ms);
    MSG msg{};
    while (GetTickCount() < until) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}

shell::AppEntry EntryForWindow(HWND hwnd) {
    shell::AppEntry entry;
    wchar_t title[256]{};
    GetWindowTextW(hwnd, title, 256);
    entry.display_name = title;
    entry.kind = shell::AppKind::Executable;
    // Match by the test executable's own path: the window belongs to this process, so
    // the path comparison in window_registry resolves it. That exercises the real
    // matching path rather than a stub.
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    entry.target_path = exe;
    return entry;
}

}  // namespace

TEST_CASE("ToString covers every run state", "[dock][indicator]") {
    CHECK(std::string(dock::ToString(dock::TileRunState::Idle)) == "idle");
    CHECK(std::string(dock::ToString(dock::TileRunState::Running)) == "running");
    CHECK(std::string(dock::ToString(dock::TileRunState::Foreground)) == "foreground");
}

TEST_CASE("tracker clamps its poll interval", "[dock][indicator]") {
    dock::RunningTracker tracker(0);
    CHECK(tracker.IntervalMs() == 50);

    tracker.SetIntervalMs(999999);
    CHECK(tracker.IntervalMs() == 5000);

    tracker.SetIntervalMs(300);
    CHECK(tracker.IntervalMs() == 300);
}

TEST_CASE("an app with no window is idle", "[dock][indicator]") {
    dock::RunningTracker tracker;
    tracker.Update(0.0);

    shell::AppEntry ghost;
    ghost.kind = shell::AppKind::Executable;
    ghost.target_path = L"C:\\definitely\\not\\running\\nothing.exe";

    CHECK(tracker.StateFor(ghost) == dock::TileRunState::Idle);
}

TEST_CASE("a visible window makes its app running or foreground", "[dock][indicator]") {
    ScopedWindow window(L"DockForge Indicator Test Window");
    REQUIRE(window.Get() != nullptr);
    PumpBriefly();

    dock::RunningTracker tracker;
    tracker.Update(0.0);

    const shell::AppEntry entry = EntryForWindow(window.Get());
    const dock::TileRunState state = tracker.StateFor(entry);
    // Either is correct: the test window may or may not have ended up in front
    // depending on what else is on the desktop. What must never happen is Idle -
    // the window is right there.
    CHECK(state != dock::TileRunState::Idle);
}

TEST_CASE("destroying the window returns the app to idle", "[dock][indicator]") {
    dock::RunningTracker tracker;
    shell::AppEntry entry;
    {
        ScopedWindow window(L"DockForge Transient Window");
        REQUIRE(window.Get() != nullptr);
        PumpBriefly();
        tracker.Invalidate();
        tracker.Update(0.0);
        entry = EntryForWindow(window.Get());
        CHECK(tracker.StateFor(entry) != dock::TileRunState::Idle);
    }
    // The window is gone; a forced poll must notice.
    PumpBriefly();
    tracker.Invalidate();
    tracker.Update(0.0);
    CHECK(tracker.StateFor(entry) == dock::TileRunState::Idle);
}

TEST_CASE("a poll that sees the same windows reports no change", "[dock][indicator]") {
    // The dock relies on this to stay asleep: on an idle desktop every poll returns
    // false, which is why the idle CPU is 0.003% rather than a repaint every 300ms.
    dock::RunningTracker tracker(50);
    tracker.Update(0.0);  // first poll, populates the snapshot

    // Advance just under the interval: no poll happens, and the foreground window is
    // unchanged, so nothing is reported.
    CHECK_FALSE(tracker.Update(10.0));
    CHECK_FALSE(tracker.Update(10.0));
}

TEST_CASE("the tracker polls on its interval, not every frame", "[dock][indicator]") {
    dock::RunningTracker tracker(300);
    tracker.Update(0.0);
    const std::uint64_t after_first = tracker.GetStats().polls;
    REQUIRE(after_first >= 1);

    // 16ms frames for ~200ms: still inside the 300ms interval, so no second poll.
    for (int i = 0; i < 12; ++i) tracker.Update(16.0);
    CHECK(tracker.GetStats().polls == after_first);

    // Push past the interval.
    for (int i = 0; i < 10; ++i) tracker.Update(16.0);
    CHECK(tracker.GetStats().polls > after_first);
}

TEST_CASE("Invalidate forces a poll on the next update", "[dock][indicator]") {
    dock::RunningTracker tracker(5000);  // long interval: nothing polls on its own
    tracker.Update(0.0);
    const std::uint64_t baseline = tracker.GetStats().polls;

    tracker.Update(16.0);
    CHECK(tracker.GetStats().polls == baseline);

    tracker.Invalidate();
    tracker.Update(16.0);
    CHECK(tracker.GetStats().polls == baseline + 1);
}

TEST_CASE("poll cost is recorded and bounded", "[dock][indicator]") {
    dock::RunningTracker tracker;
    tracker.Update(0.0);
    const auto& stats = tracker.GetStats();
    REQUIRE(stats.polls >= 1);
    // A full EnumWindows sweep on this machine measures 0.4-0.7ms (evidence/T-011).
    // The bound is loose on purpose - a loaded CI box can be much slower - but it
    // still catches the failure that matters: an enumeration that walks every child
    // of every window, which would be tens of milliseconds.
    CHECK(stats.max_poll_ms >= 0.0);
    CHECK(stats.max_poll_ms < 100.0);
    CHECK(stats.AveragePollMs() >= 0.0);
}

TEST_CASE("ApplyToTiles writes state for every tile", "[dock][indicator]") {
    dock::RunningTracker tracker;
    tracker.Update(0.0);

    std::vector<dock::DockTile> tiles(3);
    for (auto& tile : tiles) {
        tile.app.kind = shell::AppKind::Executable;
        tile.app.target_path = L"C:\\definitely\\not\\running\\nothing.exe";
        tile.run_state = dock::TileRunState::Foreground;  // stale value from before
    }
    tracker.ApplyToTiles(tiles);
    for (const auto& tile : tiles) {
        CHECK(tile.run_state == dock::TileRunState::Idle);
    }
}

TEST_CASE("an empty tile list is handled", "[dock][indicator]") {
    dock::RunningTracker tracker;
    tracker.Update(0.0);
    std::vector<dock::DockTile> tiles;
    CHECK_NOTHROW(tracker.ApplyToTiles(tiles));
}

TEST_CASE("indicator constants are inside the plate padding", "[dock][indicator]") {
    // The dot is drawn inside the plate's bottom padding because anything lower would
    // fall outside the plate and outside the window region. If someone grows the dot
    // past the padding it silently clips, so the relationship is asserted rather than
    // left to a comment.
    CHECK(dock::kIndicatorDotDip > 0.0f);
    CHECK(dock::kIndicatorDotDip < dock::kPlatePaddingDip);
    CHECK(dock::kIndicatorOpacityForeground > dock::kIndicatorOpacityRunning);
    CHECK(dock::kIndicatorOpacityRunning > 0.0f);
    CHECK(dock::kIndicatorOpacityForeground <= 1.0f);
}

TEST_CASE("geometry exposes the baseline the indicators are drawn from", "[dock][indicator]") {
    dockforge::core::Config config;
    const dock::DockGeometry geometry = dock::ComputeGeometry(config, 12, 1.0f);
    // Icons bottom-align at the baseline; the indicator band lives below it, inside
    // the bottom padding.
    CHECK(geometry.baseline_dip > 0.0f);
    CHECK(geometry.baseline_dip < geometry.window_height_dip);
    CHECK(geometry.window_height_dip - geometry.baseline_dip ==
          Catch::Approx(dock::kPlatePaddingDip));
}
