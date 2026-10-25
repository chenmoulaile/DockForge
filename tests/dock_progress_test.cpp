// DockForge - taskbar progress tests (T-067).
//
// Two layers, both pure enough to test without a live copy dialog:
//   * the mapping helpers - TBPF_* flag -> state machine, "56%" -> 0.56,
//     C-22 geometry constants and colours;
//   * the ProgressTracker lifecycle against an injected reader - readings
//     appear, advance, go indeterminate, and drop when the window dies.
//
// NOT covered here (honest boundaries, covered by the probe instead):
//   * the paint pass itself (needs a D2D device context; no Paint-level test
//     precedent exists in this suite) including the badge-over-progress
//     priority and the indeterminate pulse - exercised by
//     tools/progress_probe.ps1 on the live dock.
//   * CopyDialogMsaaReader against a real dialog - that is the spike and the
//     probe's job (spikes/T-067_progress_probe/, evidence/T-067/).
#include <catch_amalgamated.hpp>

#include <windows.h>

#include <cmath>
#include <string>
#include <vector>

#include "dock/dock_view.h"
#include "shell/app_entry.h"
#include "shell/taskbar_progress.h"

namespace dock = dockforge::dock;
namespace render = dockforge::render;
namespace shell = dockforge::shell;

namespace {

using shell::ProgressState;

// A real top-level window: the tracker's cleanup path keys on IsWindow, so a
// fake HWND would never exercise the destroy branch.
class ScopedWindow {
  public:
    ScopedWindow() {
        static int counter = 0;
        class_name_ = L"DockForgeProgressTest" + std::to_wstring(++counter);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = class_name_.c_str();
        RegisterClassExW(&wc);
        hwnd_ = CreateWindowExW(0, class_name_.c_str(), L"progress test", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 200, 100, nullptr, nullptr,
                                wc.hInstance, nullptr);
    }
    ~ScopedWindow() {
        if (hwnd_) DestroyWindow(hwnd_);
        UnregisterClassW(class_name_.c_str(), GetModuleHandleW(nullptr));
    }
    ScopedWindow(const ScopedWindow&) = delete;
    ScopedWindow& operator=(const ScopedWindow&) = delete;
    HWND Get() const { return hwnd_; }
    // Destroys the window but leaves the class registered, so the tracker can
    // observe the dead HWND without the destructor double-destroying it.
    void DestroyEarly() {
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

  private:
    std::wstring class_name_;
    HWND hwnd_ = nullptr;
};

class FakeReader : public shell::IDialogProgressReader {
  public:
    std::vector<shell::ProgressReading> readings;
    std::vector<shell::ProgressReading> ScanDialogs() override { return readings; }
};

// Drives the tracker past one poll interval. The tracker starts "due", so the
// first Update scans immediately; after that each call needs 500ms of delta.
bool Poll(shell::ProgressTracker& tracker) { return tracker.Update(501.0); }

}  // namespace

// ---- TBPF_* state machine (DoD: 正常/暂停/错误/不确定 四态映射) ----------------

TEST_CASE("progress flags map to the four DoD states", "[progress]") {
    using S = ProgressState;
    CHECK(shell::ProgressStateFromFlags(0x0) == S::NoProgress);
    CHECK(shell::ProgressStateFromFlags(0x1) == S::Normal);         // TBPF_NORMAL
    CHECK(shell::ProgressStateFromFlags(0x2) == S::Indeterminate);  // TBPF_INDETERMINATE
    CHECK(shell::ProgressStateFromFlags(0x4) == S::Error);          // TBPF_ERROR
    CHECK(shell::ProgressStateFromFlags(0x8) == S::Paused);         // TBPF_PAUSED
}

TEST_CASE("progress flag combinations resolve to the most severe state", "[progress]") {
    using S = ProgressState;
    CHECK(shell::ProgressStateFromFlags(0x1 | 0x8) == S::Paused);
    CHECK(shell::ProgressStateFromFlags(0x2 | 0x8) == S::Paused);
    CHECK(shell::ProgressStateFromFlags(0x1 | 0x2) == S::Indeterminate);
    CHECK(shell::ProgressStateFromFlags(0x4 | 0x8) == S::Error);
}

// ---- value parsing -----------------------------------------------------------

TEST_CASE("progress percent parsing", "[progress]") {
    CHECK(shell::ParseProgressPercent(L"56%") == Catch::Approx(0.56f));
    CHECK(shell::ParseProgressPercent(L"0%") == Catch::Approx(0.0f));
    CHECK(shell::ParseProgressPercent(L"100%") == Catch::Approx(1.0f));
    CHECK(shell::ParseProgressPercent(L"7%") == Catch::Approx(0.07f));
}

TEST_CASE("progress percent parsing clamps and rejects", "[progress]") {
    CHECK(shell::ParseProgressPercent(L"105%") == Catch::Approx(1.0f));
    CHECK(shell::ParseProgressPercent(L"abc") < 0.0f);
    CHECK(shell::ParseProgressPercent(L"") < 0.0f);
    CHECK(shell::ParseProgressPercent(nullptr) < 0.0f);
}

TEST_CASE("progress percent parsing skips localized prefixes", "[progress]") {
    // Explorer answers "56%"; a future source answering "剩余 56%" or
    // "56% of 2 items" still yields 56.
    CHECK(shell::ParseProgressPercent(L"剩余 56%") == Catch::Approx(0.56f));
    CHECK(shell::ParseProgressPercent(L"56% of 2 items") == Catch::Approx(0.56f));
}

// ---- C-22 geometry contract --------------------------------------------------

TEST_CASE("progress bar C-22 constants are the contract values", "[progress][c22]") {
    // C-22: 进度条高度 4px@100%DPI（= 4 DIP），圆角 2px。断言而非注释，
    // same discipline as kIndicatorDotDip < kPlatePaddingDip.
    CHECK(dock::kProgressBarHeightDip == 4.0f);
    CHECK(dock::kProgressBarCornerDip == 2.0f);
}

TEST_CASE("progress geometry sits inside the icon's bottom edge", "[progress]") {
    const render::RectF slot{10.0f, 20.0f, 58.0f, 68.0f};  // 48x48 DIP
    const dock::ProgressGeometry pg = dock::ComputeProgressGeometry(slot, 1.0f);
    CHECK(pg.track.Height() == Catch::Approx(dock::kProgressBarHeightDip));
    // Bottom inside edge: the track's bottom is `inset` above the icon's.
    CHECK(pg.track.bottom == Catch::Approx(slot.bottom - dock::kProgressBarInsetDip));
    CHECK(pg.track.top == Catch::Approx(pg.track.bottom - dock::kProgressBarHeightDip));
    // Full fraction fills the track.
    CHECK(pg.bar.Width() == Catch::Approx(pg.track.Width()));
}

TEST_CASE("progress geometry bar width follows the fraction", "[progress]") {
    const render::RectF slot{0.0f, 0.0f, 48.0f, 48.0f};
    const dock::ProgressGeometry half = dock::ComputeProgressGeometry(slot, 0.5f);
    const dock::ProgressGeometry full = dock::ComputeProgressGeometry(slot, 1.0f);
    CHECK(half.bar.Width() == Catch::Approx(full.bar.Width() * 0.5f));
    CHECK(half.bar.left == full.bar.left);
    CHECK(half.bar.top == full.bar.top);
    CHECK(half.bar.bottom == full.bar.bottom);
}

TEST_CASE("progress geometry clamps out-of-range fractions", "[progress]") {
    const render::RectF slot{0.0f, 0.0f, 48.0f, 48.0f};
    const dock::ProgressGeometry over = dock::ComputeProgressGeometry(slot, 1.5f);
    const dock::ProgressGeometry under = dock::ComputeProgressGeometry(slot, -0.25f);
    CHECK(over.bar.Width() == Catch::Approx(dock::ComputeProgressGeometry(slot, 1.0f).bar.Width()));
    CHECK(under.bar.Width() == Catch::Approx(0.0f));
}

TEST_CASE("progress colours: error and paused are the C-22 literals", "[progress][c22]") {
    const render::ColorF error = dock::ProgressOverlayColor(ProgressState::Error);
    CHECK(error.r == Catch::Approx(0xE8 / 255.0f));
    CHECK(error.g == Catch::Approx(0x11 / 255.0f));
    CHECK(error.b == Catch::Approx(0x23 / 255.0f));
    const render::ColorF paused = dock::ProgressOverlayColor(ProgressState::Paused);
    CHECK(paused.r == Catch::Approx(1.0f));
    CHECK(paused.g == Catch::Approx(0xB9 / 255.0f));
    CHECK(paused.b == Catch::Approx(0.0f));
}

TEST_CASE("progress colours: normal and indeterminate share the accent", "[progress]") {
    const render::ColorF normal = dock::ProgressOverlayColor(ProgressState::Normal);
    const render::ColorF indet = dock::ProgressOverlayColor(ProgressState::Indeterminate);
    CHECK(normal.r == Catch::Approx(indet.r));
    CHECK(normal.g == Catch::Approx(indet.g));
    CHECK(normal.b == Catch::Approx(indet.b));
}

TEST_CASE("dock tile progress defaults to no-progress", "[progress]") {
    dock::DockTile tile;
    CHECK(tile.progress_state == ProgressState::NoProgress);
    CHECK(tile.progress_fraction == Catch::Approx(0.0f));
}

// ---- tracker lifecycle -------------------------------------------------------

TEST_CASE("tracker surfaces a reading and throttles between polls", "[progress]") {
    ScopedWindow window;
    FakeReader reader;
    shell::ProgressTracker tracker(500, nullptr, &reader);

    shell::ProgressReading reading;
    reading.hwnd = window.Get();
    reading.fraction = 0.3f;
    reading.target_app_id = L"explorer.exe";
    reader.readings = {reading};

    // First Update is "due" (starts that way) so a copy already running when
    // the dock starts shows on the first frame.
    CHECK(tracker.Update(0.0) == true);
    const shell::ProgressTracker::Snapshot snap = tracker.StateFor(L"explorer.exe");
    CHECK(snap.state == ProgressState::Normal);
    CHECK(snap.fraction == Catch::Approx(0.3f));

    // Under the interval: no scan, no change.
    CHECK(tracker.Update(100.0) == false);
    CHECK(tracker.StateFor(L"explorer.exe").fraction == Catch::Approx(0.3f));

    // Past the interval with an unchanged reading: polled, no visible change.
    CHECK(tracker.Update(501.0) == false);
    CHECK(tracker.StateFor(L"explorer.exe").fraction == Catch::Approx(0.3f));
}

TEST_CASE("tracker advances with the reading", "[progress]") {
    ScopedWindow window;
    FakeReader reader;
    shell::ProgressTracker tracker(500, nullptr, &reader);

    shell::ProgressReading reading;
    reading.hwnd = window.Get();
    reading.fraction = 0.3f;
    reading.target_app_id = L"explorer.exe";
    reader.readings = {reading};
    tracker.Update(0.0);

    reading.fraction = 0.6f;
    reader.readings = {reading};
    CHECK(tracker.Update(501.0) == true);
    CHECK(tracker.StateFor(L"explorer.exe").fraction == Catch::Approx(0.6f));
}

TEST_CASE("tracker maps an unparseable value to indeterminate", "[progress]") {
    ScopedWindow window;
    FakeReader reader;
    shell::ProgressTracker tracker(500, nullptr, &reader);

    shell::ProgressReading reading;
    reading.hwnd = window.Get();
    reading.fraction = -1.0f;  // "dialog exists, no percentage came back"
    reading.target_app_id = L"explorer.exe";
    reader.readings = {reading};
    tracker.Update(0.0);
    const shell::ProgressTracker::Snapshot snap = tracker.StateFor(L"explorer.exe");
    CHECK(snap.state == ProgressState::Indeterminate);
}

TEST_CASE("tracker drops state when the target window is destroyed", "[progress]") {
    ScopedWindow window;
    FakeReader reader;
    shell::ProgressTracker tracker(500, nullptr, &reader);

    shell::ProgressReading reading;
    reading.hwnd = window.Get();
    reading.fraction = 0.4f;
    reading.target_app_id = L"explorer.exe";
    reader.readings = {reading};
    tracker.Update(0.0);
    REQUIRE(tracker.StateFor(L"explorer.exe").state == ProgressState::Normal);

    // The copy finished: the dialog left the scan AND the HWND is gone.
    const HWND dead = window.Get();
    window.DestroyEarly();
    REQUIRE(!IsWindow(dead));
    reader.readings.clear();
    CHECK(tracker.Update(501.0) == true);
    CHECK(tracker.StateFor(L"explorer.exe").state == ProgressState::NoProgress);
    CHECK(tracker.GetStats().dropped >= 1);
}

TEST_CASE("tracker reports the window that left the scan as dropped even if alive",
          "[progress]") {
    // A live HWND that stopped being reported (MSAA hiccup) is just as gone to
    // the overlay: the scan is the source of truth.
    ScopedWindow window;
    FakeReader reader;
    shell::ProgressTracker tracker(500, nullptr, &reader);

    shell::ProgressReading reading;
    reading.hwnd = window.Get();
    reading.fraction = 0.4f;
    reading.target_app_id = L"explorer.exe";
    reader.readings = {reading};
    tracker.Update(0.0);
    reader.readings.clear();
    CHECK(tracker.Update(501.0) == true);
    CHECK(tracker.StateFor(L"explorer.exe").state == ProgressState::NoProgress);
}

TEST_CASE("tracker fires a clearing change when progress ends", "[progress]") {
    ScopedWindow window;
    FakeReader reader;
    shell::ProgressTracker tracker(500, nullptr, &reader);

    shell::ProgressReading reading;
    reading.hwnd = window.Get();
    reading.fraction = 0.4f;
    reading.target_app_id = L"explorer.exe";
    reader.readings = {reading};

    std::vector<std::pair<ProgressState, float>> events;
    tracker.set_changed([&](const std::wstring& id, ProgressState state, float fraction) {
        REQUIRE(id == L"explorer.exe");
        events.push_back({state, fraction});
    });
    tracker.Update(0.0);
    reader.readings.clear();
    tracker.Update(501.0);

    bool saw_clear = false;
    for (const auto& [state, fraction] : events) {
        if (state == ProgressState::NoProgress) saw_clear = true;
    }
    CHECK(saw_clear);
}

TEST_CASE("tracker answers the largest fraction among same-app windows", "[progress]") {
    ScopedWindow a, b;
    FakeReader reader;
    shell::ProgressTracker tracker(500, nullptr, &reader);

    shell::ProgressReading first;
    first.hwnd = a.Get();
    first.fraction = 0.2f;
    first.target_app_id = L"explorer.exe";
    shell::ProgressReading second;
    second.hwnd = b.Get();
    second.fraction = 0.8f;
    second.target_app_id = L"explorer.exe";
    reader.readings = {first, second};
    tracker.Update(0.0);
    CHECK(tracker.StateFor(L"explorer.exe").fraction == Catch::Approx(0.8f));
}

TEST_CASE("tracker matches a tile entry by lowercase executable basename", "[progress]") {
    ScopedWindow window;
    FakeReader reader;
    shell::ProgressTracker tracker(500, nullptr, &reader);

    shell::ProgressReading reading;
    reading.hwnd = window.Get();
    reading.fraction = 0.5f;
    reading.target_app_id = L"explorer.exe";
    reader.readings = {reading};
    tracker.Update(0.0);

    shell::AppEntry app;
    app.target_path = L"C:\\Windows\\EXPLORER.EXE";  // case must not matter
    const shell::ProgressTracker::Snapshot snap = tracker.StateForEntry(app);
    CHECK(snap.state == ProgressState::Normal);
    CHECK(snap.fraction == Catch::Approx(0.5f));

    shell::AppEntry other;
    other.target_path = L"C:\\Program Files\\Other\\other.exe";
    CHECK(tracker.StateForEntry(other).state == ProgressState::NoProgress);
}

TEST_CASE("tracker poll interval clamps to 250..5000", "[progress]") {
    shell::ProgressTracker tracker(500, nullptr, nullptr);
    tracker.SetPollIntervalMs(10);
    CHECK(tracker.poll_interval_ms() == 250);
    tracker.SetPollIntervalMs(99999);
    CHECK(tracker.poll_interval_ms() == 5000);
    tracker.SetPollIntervalMs(1000);
    CHECK(tracker.poll_interval_ms() == 1000);
}

TEST_CASE("white list ships the explorer copy dialog", "[progress]") {
    // The spike-verified entry: class "OperationStatusWindow" (one word, empty
    // title), credited to the File Explorer tile by its AUMID - the tile
    // resolves as a ShellItem with an empty target_path, so an exe-name id
    // would never match (measured during the T-067 probe bring-up).
    REQUIRE(std::size(shell::kProgressWhiteList) >= 1);
    bool has_explorer_copy = false;
    for (const shell::ProgressWhiteListEntry& entry : shell::kProgressWhiteList) {
        if (wcscmp(entry.window_class, L"OperationStatusWindow") == 0 &&
            _wcsicmp(entry.target_app_id, L"Microsoft.Windows.Explorer") == 0) {
            has_explorer_copy = true;
        }
    }
    CHECK(has_explorer_copy);
}

TEST_CASE("tracker key matching is case-insensitive", "[progress]") {
    // The white list spells the AUMID mixed-case; StateFor must still find it
    // whatever case the caller uses.
    ScopedWindow window;
    FakeReader reader;
    shell::ProgressTracker tracker(500, nullptr, &reader);

    shell::ProgressReading reading;
    reading.hwnd = window.Get();
    reading.fraction = 0.42f;
    reading.target_app_id = L"Microsoft.Windows.Explorer";
    reader.readings = {reading};
    tracker.Update(0.0);
    CHECK(tracker.StateFor(L"microsoft.windows.explorer").state == ProgressState::Normal);
    CHECK(tracker.StateFor(L"MICROSOFT.WINDOWS.EXPLORER").fraction == Catch::Approx(0.42f));
}
