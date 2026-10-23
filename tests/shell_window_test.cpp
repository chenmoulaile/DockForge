// DockForge - window registry and click behaviour tests (T-011).
//
// These deliberately avoid asserting anything about the machine's real app set:
// a test that expects "Calculator is running" fails on any other desktop. What is
// pinned here is the logic that decides, which is where the bugs live.
#include <windows.h>

#include "shell/app_entry.h"
#include "shell/window_actions.h"
#include "shell/window_registry.h"

#include <catch_amalgamated.hpp>

namespace shell = dockforge::shell;

namespace {

shell::AppEntry MakeApp(shell::AppKind kind, std::wstring path, std::wstring aumid) {
    shell::AppEntry app;
    app.kind = kind;
    app.display_name = L"test app";
    app.target_path = std::move(path);
    app.aumid = std::move(aumid);
    return app;
}

shell::AppWindow MakeWindow(std::wstring exe, std::wstring aumid) {
    shell::AppWindow window;
    window.hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(1));
    window.pid = 100;
    window.app_pid = 100;
    window.exe_path = std::move(exe);
    window.aumid = std::move(aumid);
    return window;
}

}  // namespace

TEST_CASE("SameExecutable compares case-insensitively", "[shell][window]") {
    CHECK(shell::SameExecutable(L"C:\\Apps\\Thing.exe", L"c:\\apps\\thing.EXE"));
    CHECK(shell::SameExecutable(L"D:\\One\\a.exe", L"D:\\One\\a.exe"));
}

TEST_CASE("SameExecutable rejects empty and different paths", "[shell][window]") {
    CHECK_FALSE(shell::SameExecutable(L"", L""));
    CHECK_FALSE(shell::SameExecutable(L"C:\\a.exe", L""));
    CHECK_FALSE(shell::SameExecutable(L"", L"C:\\a.exe"));
    CHECK_FALSE(shell::SameExecutable(L"C:\\a.exe", L"C:\\b.exe"));
    // Trailing separators are not normalized away: a directory is not an executable,
    // and silently equating the two would match the wrong process.
    CHECK_FALSE(shell::SameExecutable(L"C:\\a.exe", L"C:\\a.exe\\"));
}

TEST_CASE("FindAppWindow matches a packaged app by AUMID", "[shell][window]") {
    const auto app = MakeApp(shell::AppKind::Uwp, L"", L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App");
    // A packaged app has no path; only the AUMID can identify it.
    const std::vector<shell::AppWindow> windows{
        MakeWindow(L"C:\\Windows\\System32\\ApplicationFrameHost.exe", L""),
        MakeWindow(L"C:\\Other\\thing.exe", L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"),
    };
    shell::AppWindow found;
    REQUIRE(shell::FindAppWindow(app, windows, found));
    CHECK(found.pid == 100);
}

TEST_CASE("FindAppWindow matches a Win32 app by executable path", "[shell][window]") {
    const auto app = MakeApp(shell::AppKind::Executable, L"C:\\Tools\\Editor.exe", L"");
    const std::vector<shell::AppWindow> windows{
        MakeWindow(L"C:\\Windows\\notepad.exe", L""),
        MakeWindow(L"c:\\tools\\editor.EXE", L""),
    };
    shell::AppWindow found;
    REQUIRE(shell::FindAppWindow(app, windows, found));
    CHECK(found.exe_path == L"c:\\tools\\editor.EXE");
}

TEST_CASE("FindAppWindow reports a miss when nothing matches", "[shell][window]") {
    const auto app = MakeApp(shell::AppKind::Executable, L"C:\\Tools\\Ghost.exe", L"");
    const std::vector<shell::AppWindow> windows{MakeWindow(L"C:\\Windows\\notepad.exe", L"")};
    shell::AppWindow found;
    CHECK_FALSE(shell::FindAppWindow(app, windows, found));
    CHECK(found.hwnd == nullptr);
}

TEST_CASE("FindAppWindow prefers the AUMID over a coincidental path match", "[shell][window]") {
    // ApplicationFrameHost.exe hosts every packaged app, so a path match on it would
    // make Calculator and Settings look like the same running app. The AUMID has to
    // win or the dock raises the wrong window.
    const auto app = MakeApp(shell::AppKind::Uwp, L"C:\\Windows\\System32\\ApplicationFrameHost.exe",
                             L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App");
    const std::vector<shell::AppWindow> windows{
        MakeWindow(L"C:\\Windows\\System32\\ApplicationFrameHost.exe", L"Microsoft.WindowsSettings_cw5n1h2txyewy!App"),
        MakeWindow(L"C:\\Windows\\System32\\ApplicationFrameHost.exe", L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"),
    };
    shell::AppWindow found;
    REQUIRE(shell::FindAppWindow(app, windows, found));
    CHECK(found.aumid == L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App");
}

TEST_CASE("EnumerateAppWindows returns plausible windows only", "[shell][window]") {
    // The filter itself is what matters, and it can be checked without assuming any
    // app is running: every window that comes back must pass the same predicates.
    //
    // Windows can disappear between the enumeration and the check - a test process
    // closing, an app shutting down - and a window that is gone is not a violation of
    // the filter. Skipping dead handles keeps this from flaking on a live desktop
    // (observed: 3 spurious failures while another test command was killing apps).
    const auto windows = shell::EnumerateAppWindows();
    for (const auto& window : windows) {
        if (IsWindow(window.hwnd) == FALSE) continue;
        CHECK(window.hwnd != nullptr);
        CHECK(window.pid != 0);
        CHECK(window.app_pid != 0);
        CHECK_FALSE(window.title.empty());
        CHECK_FALSE(window.class_name.empty());
        CHECK(IsWindowVisible(window.hwnd) != FALSE);
        CHECK(GetWindow(window.hwnd, GW_OWNER) == nullptr);
        CHECK((GetWindowLongPtrW(window.hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) == 0);
    }
}

TEST_CASE("EnumerateAppWindows skips a tool window", "[shell][window]") {
    // A WS_EX_TOOLWINDOW window with a title is exactly the shape that would slip
    // through a naive "visible and titled" filter, so it is the case worth pinning.
    HWND tool = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"DockForge tool window test",
                                WS_POPUP | WS_VISIBLE, 0, 0, 120, 60, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    REQUIRE(tool != nullptr);
    ShowWindow(tool, SW_SHOWNOACTIVATE);
    UpdateWindow(tool);

    bool found = false;
    for (const auto& window : shell::EnumerateAppWindows()) {
        if (window.hwnd == tool) found = true;
    }
    DestroyWindow(tool);
    CHECK_FALSE(found);
}

TEST_CASE("ToString covers every window action", "[shell][window]") {
    CHECK(std::string(shell::ToString(shell::WindowAction::None)) == "none");
    CHECK(std::string(shell::ToString(shell::WindowAction::Launched)) == "launched");
    CHECK(std::string(shell::ToString(shell::WindowAction::Minimized)) == "minimized");
    CHECK(std::string(shell::ToString(shell::WindowAction::Activated)) == "activated");
    CHECK(std::string(shell::ToString(shell::WindowAction::Failed)) == "failed");
}

TEST_CASE("ActivateWindow rejects a dead handle instead of crashing", "[shell][window]") {
    bool alt = false;
    CHECK_FALSE(shell::ActivateWindow(nullptr, alt));
    CHECK_FALSE(alt);
    // A handle that was never a window: IsWindow guards it before any Win32 call
    // that would fault on a bogus HWND.
    CHECK_FALSE(shell::ActivateWindow(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(0x1234)), alt));
}

TEST_CASE("ActivateOrMinimize reports a failed launch for an unresolvable entry", "[shell][window]") {
    // Not launchable and no window: the click has to report a failure rather than
    // claim success, otherwise a dock that silently does nothing looks alive.
    shell::AppEntry app;
    app.kind = shell::AppKind::Unknown;
    app.display_name = L"nothing";
    const shell::ActionResult result = shell::ActivateOrMinimize(app, {});
    CHECK(result.action == shell::WindowAction::Failed);
    CHECK(std::string(result.route) == "launch");
    CHECK(result.process_id == 0);
}
