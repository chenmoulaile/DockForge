// DockForge tests - window thumbnail preview (T-015, CONTRACTS.md C-17).
#include "catch_amalgamated.hpp"

#include "dock/hover_timer.h"
#include "shell/thumbnail_host.h"
#include "shell/window_list.h"

#include <windows.h>

using namespace dockforge;

// ---------------------------------------------------------------------------
// dock::HoverTimer - the 400ms dwell gate (C-17a)
// ---------------------------------------------------------------------------

TEST_CASE("HoverTimer fires once after exactly the dwell", "[preview][hover]") {
    dock::HoverTimer timer;
    // 400ms of hover, fed in 16ms frames: fires on the frame that crosses it, and
    // only that frame.
    bool fired = false;
    int fired_at = -1;
    for (int frame = 1; frame <= 60; ++frame) {
        if (timer.Update(3, 16.0f)) {
            // A second fire would mean the timer never latched.
            REQUIRE_FALSE(fired);
            fired = true;
            fired_at = frame;
        }
    }
    REQUIRE(fired);
    // Frame 25 is the first frame with elapsed_ms_ == 400 (24 prior frames plus
    // this one), so the crossing happens on frame 26.
    REQUIRE(fired_at == 26);
}

TEST_CASE("HoverTimer does not fire below the dwell", "[preview][hover]") {
    dock::HoverTimer timer;
    // The first Update arms the timer and does not count, so ElapsedMs is still
    // 0 after it.
    REQUIRE_FALSE(timer.Update(3, 16.0f));
    REQUIRE(timer.ElapsedMs() == Catch::Approx(0.0f));
    // 24 counted frames reach 384ms, still short of the dwell.
    for (int frame = 0; frame < 24; ++frame) {
        REQUIRE_FALSE(timer.Update(3, 16.0f));
    }
    REQUIRE(timer.ElapsedMs() == Catch::Approx(384.0f));
    // The 25th counted frame reaches 400ms.
    REQUIRE(timer.Update(3, 16.0f));
    REQUIRE(timer.ElapsedMs() == Catch::Approx(400.0f));
}

TEST_CASE("HoverTimer re-arms on tile change", "[preview][hover]") {
    dock::HoverTimer timer;
    // 350ms on tile 1, then move to tile 2: the dwell restarts, so a preview for
    // tile 2 must not appear at 400ms of total hover.
    for (int frame = 0; frame < 21; ++frame) {
        REQUIRE_FALSE(timer.Update(1, 16.0f));
    }
    // This frame re-arms; it does not count toward the new dwell.
    REQUIRE_FALSE(timer.Update(2, 16.0f));
    // 24 counted frames = 384ms on tile 2: still short.
    for (int frame = 0; frame < 24; ++frame) {
        REQUIRE_FALSE(timer.Update(2, 16.0f));
    }
    // And the 25th reaches 400ms.
    REQUIRE(timer.Update(2, 16.0f));
}

TEST_CASE("HoverTimer never fires for a pointer that left", "[preview][hover]") {
    dock::HoverTimer timer;
    for (int frame = 0; frame < 10; ++frame) {
        REQUIRE_FALSE(timer.Update(0, 16.0f));
    }
    REQUIRE_FALSE(timer.Update(-1, 16.0f));
    for (int frame = 0; frame < 100; ++frame) {
        REQUIRE_FALSE(timer.Update(-1, 16.0f));
    }
}

TEST_CASE("HoverTimer reset discards partial dwell", "[preview][hover]") {
    dock::HoverTimer timer;
    // 23 counted frames = 368ms of dwell, then the pointer leaves and comes
    // back, so the dwell must restart from zero rather than continue.
    REQUIRE_FALSE(timer.Update(5, 16.0f));
    for (int frame = 0; frame < 23; ++frame) {
        REQUIRE_FALSE(timer.Update(5, 16.0f));
    }
    timer.Reset();
    // The first Update after the reset re-arms without counting, so 24 counted
    // frames is 384ms: the discarded 368ms is gone, so this is 384ms of dwell,
    // not 752ms.
    REQUIRE_FALSE(timer.Update(5, 16.0f));
    for (int frame = 0; frame < 24; ++frame) {
        REQUIRE_FALSE(timer.Update(5, 16.0f));
    }
    // The 26th counted frame after the reset reaches 416ms, past the dwell.
    REQUIRE(timer.Update(5, 16.0f));
}

TEST_CASE("HoverTimer reports its tile and progress", "[preview][hover]") {
    dock::HoverTimer timer;
    REQUIRE(timer.Index() == -1);
    REQUIRE_FALSE(timer.Armed());
    REQUIRE(timer.Update(2, 16.0f) == false);
    REQUIRE(timer.Index() == 2);
    REQUIRE(timer.Armed());
    for (int frame = 0; frame < 25; ++frame) timer.Update(2, 16.0f);
    // The first Update armed the timer without counting, so 25 more frames is
    // exactly the dwell.
    REQUIRE(timer.ElapsedMs() == Catch::Approx(400.0f));
    REQUIRE(timer.Fired());
    timer.Reset();
    REQUIRE(timer.Index() == -1);
    REQUIRE_FALSE(timer.Armed());
    REQUIRE_FALSE(timer.Fired());
}

// ---------------------------------------------------------------------------
// shell::ThumbnailHost - slot constants and geometry (C-17c, C-17e)
// ---------------------------------------------------------------------------

TEST_CASE("Thumbnail slot constants match the measured limits", "[preview][slots]") {
    // These are not arbitrary: 110x70 + gap 8 rendered 8/8 three times in a row
    // on this machine, while every larger size lost slots (C-17c).
    REQUIRE(shell::kMaxThumbnails == 8);
    REQUIRE(shell::kSlotWidthDip == Catch::Approx(110.0f));
    REQUIRE(shell::kSlotHeightDip == Catch::Approx(70.0f));
    REQUIRE(shell::kSlotGapDip == Catch::Approx(8.0f));
    // One registration per tick: a cold registration costs up to 609ms, so more
    // than one would blow the frame budget (C-17d).
    REQUIRE(shell::kRegistrationsPerTick == 1);
}

TEST_CASE("ThumbnailHost with no destination queues instead of dropping", "[preview][slots]") {
    shell::ThumbnailHost host;
    std::vector<HWND> sources;
    // Values are only used as identity; no window is created in a unit test, so
    // this proves the queueing path, not the DWM path. SetSources with no
    // destination bound keeps them pending.
    sources.push_back(reinterpret_cast<HWND>(1));
    sources.push_back(reinterpret_cast<HWND>(2));

    REQUIRE(host.SetSources(sources) == 0);
    REQUIRE(host.PendingCount() == 2);
    REQUIRE(host.LiveCount() == 0);
    // Pumping without a destination must not fabricate a registration, and must
    // not consume the queue either: the fake HWNDs are not real windows, so
    // IsWindow would reject them if it reached that check.
    REQUIRE(host.PumpPending() == 0);
    REQUIRE(host.PendingCount() == 2);
}

TEST_CASE("ThumbnailHost caps the source list at the render limit", "[preview][slots]") {
    shell::ThumbnailHost host;
    std::vector<HWND> sources;
    for (int i = 0; i < 20; ++i) {
        sources.push_back(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(i + 1)));
    }
    host.SetSources(sources);
    // 20 sources must not become 20 pending registrations: the measured ceiling
    // is 8 slots per destination (C-17c), and over-queueing would spend frames on
    // registrations that cannot be shown.
    REQUIRE(host.PendingCount() == shell::kMaxThumbnails);
}

TEST_CASE("ThumbnailHost release clears slots and pending", "[preview][slots]") {
    shell::ThumbnailHost host;
    std::vector<HWND> sources;
    for (int i = 0; i < 3; ++i) {
        sources.push_back(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(i + 1)));
    }
    host.SetSources(sources);
    REQUIRE(host.PendingCount() == 3);
    host.ReleaseAll();
    REQUIRE(host.PendingCount() == 0);
    REQUIRE(host.LiveCount() == 0);
    REQUIRE(host.Slots().empty());
}

TEST_CASE("ThumbnailHost ignores null sources", "[preview][slots]") {
    shell::ThumbnailHost host;
    std::vector<HWND> sources;
    sources.push_back(nullptr);
    sources.push_back(reinterpret_cast<HWND>(7));
    host.SetSources(sources);
    REQUIRE(host.PendingCount() == 1);
}

TEST_CASE("ThumbnailHost SetDestination releases the previous bind", "[preview][slots]") {
    shell::ThumbnailHost host;
    std::vector<HWND> sources;
    sources.push_back(reinterpret_cast<HWND>(1));
    host.SetSources(sources);
    // Binding a new destination must not carry registrations across: they were
    // made against the old window's redirection surface.
    host.SetDestination(reinterpret_cast<HWND>(99));
    REQUIRE(host.Destination() == reinterpret_cast<HWND>(99));
    REQUIRE(host.PendingCount() == 0);
    REQUIRE(host.LiveCount() == 0);
}

// ---------------------------------------------------------------------------
// shell::WindowsForEntry - the per-tile window list (C-17h)
// ---------------------------------------------------------------------------

namespace {

shell::AppWindow MakeWindow(HWND hwnd, const wchar_t* exe, const wchar_t* aumid) {
    shell::AppWindow w;
    w.hwnd = hwnd;
    w.pid = 1;
    w.app_pid = 1;
    w.exe_path = exe ? exe : L"";
    w.aumid = aumid ? aumid : L"";
    w.title = L"window";
    w.class_name = L"Chrome_WidgetWin_1";
    return w;
}

}  // namespace

TEST_CASE("WindowsForEntry matches an exe entry by path identity", "[preview][windows]") {
    shell::AppEntry app;
    app.kind = shell::AppKind::Shortcut;
    app.display_name = L"Editor";
    app.target_path = L"C:\\Tools\\editor.exe";

    std::vector<shell::AppWindow> windows;
    windows.push_back(MakeWindow(reinterpret_cast<HWND>(1), L"C:\\Tools\\editor.exe", nullptr));
    windows.push_back(MakeWindow(reinterpret_cast<HWND>(2), L"C:\\Tools\\other.exe", nullptr));

    const auto matches = shell::WindowsForEntry(app, windows);
    REQUIRE(matches.size() == 1);
    REQUIRE(matches.front().hwnd == reinterpret_cast<HWND>(1));
}

TEST_CASE("WindowsForEntry returns every window of one app", "[preview][windows]") {
    // The whole point of the list: one tile, several windows (C-14h).
    shell::AppEntry app;
    app.kind = shell::AppKind::Executable;
    app.display_name = L"Editor";
    app.target_path = L"C:\\Tools\\editor.exe";

    std::vector<shell::AppWindow> windows;
    windows.push_back(MakeWindow(reinterpret_cast<HWND>(1), L"C:\\Tools\\editor.exe", nullptr));
    windows.push_back(MakeWindow(reinterpret_cast<HWND>(2), L"C:\\Tools\\editor.exe", nullptr));
    windows.push_back(MakeWindow(reinterpret_cast<HWND>(3), L"C:\\Tools\\editor.exe", nullptr));
    windows.push_back(MakeWindow(reinterpret_cast<HWND>(4), L"C:\\Tools\\editor.exe", nullptr));

    const auto matches = shell::WindowsForEntry(app, windows);
    REQUIRE(matches.size() == 4);
    // Enumeration order is preserved, and it is z-order (MRU first).
    REQUIRE(matches[0].hwnd == reinterpret_cast<HWND>(1));
    REQUIRE(matches[3].hwnd == reinterpret_cast<HWND>(4));
}

TEST_CASE("WindowsForEntry matches a packaged app by AUMID only", "[preview][windows]") {
    // Every packaged window is hosted by ApplicationFrameHost.exe, so the path is
    // shared and would make two apps look identical (C-13b). An entry that
    // declares an AUMID must match by AUMID alone.
    shell::AppEntry app;
    app.kind = shell::AppKind::Uwp;
    app.display_name = L"Calculator";
    app.aumid = L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App";

    std::vector<shell::AppWindow> windows;
    // Same frame-host executable path, different app.
    windows.push_back(MakeWindow(reinterpret_cast<HWND>(1),
                                 L"C:\\Windows\\System32\\ApplicationFrameHost.exe",
                                 L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"));
    windows.push_back(MakeWindow(reinterpret_cast<HWND>(2),
                                 L"C:\\Windows\\System32\\ApplicationFrameHost.exe",
                                 L"Microsoft.WindowsTerminal_8wekyb3d8bbwe!App"));

    const auto matches = shell::WindowsForEntry(app, windows);
    REQUIRE(matches.size() == 1);
    REQUIRE(matches.front().hwnd == reinterpret_cast<HWND>(1));
    // The frame host HWND is the thumbnail source (C-17g).
    REQUIRE(matches.front().pid != 0);
}

TEST_CASE("WindowsForEntry is case insensitive", "[preview][windows]") {
    shell::AppEntry app;
    app.kind = shell::AppKind::Executable;
    app.display_name = L"Editor";
    app.target_path = L"C:\\Tools\\Editor.exe";

    std::vector<shell::AppWindow> windows;
    windows.push_back(MakeWindow(reinterpret_cast<HWND>(1), L"c:\\tools\\EDITOR.EXE", nullptr));

    const auto matches = shell::WindowsForEntry(app, windows);
    REQUIRE(matches.size() == 1);
}

TEST_CASE("WindowsForEntry returns nothing for an entry with no identity", "[preview][windows]") {
    shell::AppEntry app;
    app.kind = shell::AppKind::Unknown;
    // No AUMID, no namespace, no path, no display name.
    std::vector<shell::AppWindow> windows;
    windows.push_back(MakeWindow(reinterpret_cast<HWND>(1), L"C:\\Tools\\editor.exe", nullptr));
    REQUIRE(shell::WindowsForEntry(app, windows).empty());
}

TEST_CASE("WindowsForEntry returns nothing when the app is not running", "[preview][windows]") {
    shell::AppEntry app;
    app.kind = shell::AppKind::Executable;
    app.display_name = L"Editor";
    app.target_path = L"C:\\Tools\\editor.exe";

    std::vector<shell::AppWindow> windows;
    windows.push_back(MakeWindow(reinterpret_cast<HWND>(1), L"C:\\Tools\\other.exe", nullptr));
    REQUIRE(shell::WindowsForEntry(app, windows).empty());
}

TEST_CASE("WindowsForEntry ignores windows with no identity at all", "[preview][windows]") {
    shell::AppEntry app;
    app.kind = shell::AppKind::Executable;
    app.display_name = L"Editor";
    app.target_path = L"C:\\Tools\\editor.exe";

    std::vector<shell::AppWindow> windows;
    windows.push_back(MakeWindow(reinterpret_cast<HWND>(1), nullptr, nullptr));
    REQUIRE(shell::WindowsForEntry(app, windows).empty());
}
