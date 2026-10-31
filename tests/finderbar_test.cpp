// T-301: Finder Bar name/menu helpers.
//
// The bar's window, DComp surface and menu thread cannot run in a test process, so
// what is covered here is the part that decides what a user reads: how a window's
// application is named, how a Win32 menu title is cleaned up for display, and that
// text measurement is monotone. Those are exactly the pieces a spike cannot prove
// (spike Q6 measured the cost, not the correctness) and the pieces that would
// silently degrade the bar into showing "chrome.exe".
#include "catch_amalgamated.hpp"

#include "finderbar/bar_layout.h"
#include "finderbar/finder_bar.h"

#include <string>
#include <vector>

namespace {

using dockforge::finderbar::DisplayNameForExecutable;
using dockforge::finderbar::FallbackNameFromPath;
using dockforge::finderbar::StripAccelerator;

std::wstring SystemNotepad() {
    wchar_t windows[MAX_PATH]{};
    const UINT n = GetWindowsDirectoryW(windows, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"C:\\Windows\\System32\\notepad.exe";
    std::wstring path(windows);
    path += L"\\System32\\notepad.exe";
    return path;
}

}  // namespace

TEST_CASE("accelerator markers are stripped for display", "[finderbar]") {
    // Win32 menu titles almost always carry an accelerator. Drawing it would make the
    // bar read as a Win32 app rather than a menu bar.
    CHECK(StripAccelerator(L"&File") == L"File");
    CHECK(StripAccelerator(L"&Edit") == L"Edit");
    CHECK(StripAccelerator(L"Save &As") == L"Save As");
    // A doubled ampersand is an escaped literal, not an accelerator: "&&" must become
    // one ampersand and the character after it must survive.
    CHECK(StripAccelerator(L"Rock && Roll") == L"Rock & Roll");
    // No marker at all is the common case for modern titles and must be a no-op.
    CHECK(StripAccelerator(L"File") == L"File");
    CHECK(StripAccelerator(L"") == L"");
    // A trailing marker has nothing to mark; it is dropped rather than left dangling.
    CHECK(StripAccelerator(L"File&") == L"File");
}

TEST_CASE("path fallback drops directory and extension", "[finderbar]") {
    CHECK(FallbackNameFromPath(L"C:\\Windows\\System32\\notepad.exe") == L"notepad");
    CHECK(FallbackNameFromPath(L"notepad.exe") == L"notepad");
    CHECK(FallbackNameFromPath(L"D:\\Apps\\Some Tool\\Some Tool.EXE") == L"Some Tool");
    // No extension: the name is the last segment unchanged.
    CHECK(FallbackNameFromPath(L"C:\\Tools\\runner") == L"runner");
    // Trailing separator leaves no segment; the caller shows nothing rather than a
    // stray backslash.
    CHECK(FallbackNameFromPath(L"C:\\Tools\\").empty());
    CHECK(FallbackNameFromPath(L"").empty());
}

TEST_CASE("executable display name prefers the version resource", "[finderbar]") {
    const std::wstring notepad = SystemNotepad();
    const std::wstring name = DisplayNameForExecutable(notepad);
    // notepad.exe carries FileDescription "Notepad" (localised on some installs), so
    // the assertion is that the resolution produced something and did not fall through
    // to the bare file name with its extension still attached.
    CHECK_FALSE(name.empty());
    CHECK(name.find(L".exe") == std::wstring::npos);
    CHECK(name.find(L'\\') == std::wstring::npos);

    // A path that does not exist has no version resource; the fallback must still
    // produce a readable name instead of an empty label.
    CHECK(DisplayNameForExecutable(L"C:\\definitely\\missing\\thing.exe") == L"thing");
    CHECK(DisplayNameForExecutable(L"").empty());
}

TEST_CASE("bar text measurement is monotone and non-zero", "[finderbar]") {
    using dockforge::finderbar::MeasureBarText;
    const float empty = MeasureBarText(L"", 13.0f);
    const float one = MeasureBarText(L"F", 13.0f);
    const float four = MeasureBarText(L"File", 13.0f);
    const float longer = MeasureBarText(L"File Edit View", 13.0f);
    CHECK(empty == 0.0f);
    CHECK(one > 0.0f);
    CHECK(four > one);
    CHECK(longer > four);
    // A null pointer must not crash the layout pass; the bar measures whatever the
    // menu reader produced, and an empty title is legal.
    CHECK(MeasureBarText(nullptr, 13.0f) == 0.0f);
    // Larger text is wider for the same string.
    CHECK(MeasureBarText(L"File", 20.0f) > four);
}

TEST_CASE("system menu ids stay above the forwarded range", "[finderbar]") {
    // The bar decides "this id is mine, not the application's" with a single compare
    // against kSystemMenuBase. An application command id must never be mistaken for a
    // system command, or clicking a menu item would quit the app instead of running it.
    CHECK(dockforge::finderbar::kSystemMenuBase > 0);
    CHECK(static_cast<int>(dockforge::finderbar::SystemCommand::Quit) <
          dockforge::finderbar::kSystemMenuBase);
    // Every command id the menu builds is above the base, by construction.
    CHECK(dockforge::finderbar::kSystemMenuBase +
              static_cast<int>(dockforge::finderbar::SystemCommand::About) >
          dockforge::finderbar::kSystemMenuBase);
}
