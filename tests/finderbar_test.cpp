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

using dockforge::finderbar::DecodeOwnerDrawnTitle;
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

// ------------------------------------------------- owner-drawn menu titles

// The byte layout comes from spikes/T-301_finderbar_menu/menu_spike13.cpp, which dumped
// Sublime Text's dwItemData: "&File\0&Edit\0&Selection\0F&ind\0&View\0&Goto\0&Tools\0
// &Project\0Prefere&nces\0&Help\0" as UTF-16LE. Decoding is a pure function, so the rules
// that matter - stop at NUL, refuse garbage - are testable without a live process.

namespace {

std::vector<std::uint8_t> Utf16Bytes(const std::wstring& text) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size() * 2);
    for (const wchar_t ch : text) {
        const auto value = static_cast<std::uint16_t>(ch);
        bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    }
    return bytes;
}

}  // namespace

TEST_CASE("An owner-drawn title decodes up to its terminator", "[finderbar][menu]") {
    // The real buffer holds every title back to back; item i's pointer lands on the start
    // of title i, so the decoder must stop at the first NUL and ignore the rest.
    //
    // Built with an explicit length on purpose: a std::wstring constructed from a literal
    // stops at the first NUL, which would silently hand the decoder a single unterminated
    // title and test the opposite of what this case claims.
    const wchar_t raw[] = L"&File\0&Edit\0&Selection\0F&ind\0";
    const std::wstring buffer(raw, std::size(raw) - 1);
    const std::vector<std::uint8_t> bytes = Utf16Bytes(buffer);
    CHECK(DecodeOwnerDrawnTitle(bytes.data(), bytes.size()) == L"&File");

    // A pointer landing mid-array decodes that title, which is what makes the single read
    // sufficient for every item.
    const std::size_t second = 6 * 2;  // "&File\0" is 6 code units
    CHECK(DecodeOwnerDrawnTitle(bytes.data() + second, bytes.size() - second) == L"&Edit");
}

TEST_CASE("An owner-drawn title with no terminator is rejected", "[finderbar][menu]") {
    // Without a NUL the read ran off the end of a real title into whatever follows. The
    // fallback is the application name, which is honest; a truncated guess is not.
    const std::vector<std::uint8_t> bytes = Utf16Bytes(L"File Edit View");
    CHECK(DecodeOwnerDrawnTitle(bytes.data(), bytes.size()).empty());
}

TEST_CASE("Control characters reject the buffer", "[finderbar][menu]") {
    // A misaligned pointer reads the high byte of one character as the low byte of the
    // next. Those produce control codes, and a menu title never contains one.
    std::vector<std::uint8_t> bytes = Utf16Bytes(L"Fi");
    bytes.push_back(0x01);
    bytes.push_back(0x00);
    bytes.push_back(0x00);
    bytes.push_back(0x00);
    CHECK(DecodeOwnerDrawnTitle(bytes.data(), bytes.size()).empty());
}

TEST_CASE("Degenerate owner-draw buffers are rejected", "[finderbar][menu]") {
    CHECK(DecodeOwnerDrawnTitle(nullptr, 0).empty());
    const std::vector<std::uint8_t> one{0x26};
    CHECK(DecodeOwnerDrawnTitle(one.data(), one.size()).empty());  // half a code unit
    const std::vector<std::uint8_t> immediate_nul{0x00, 0x00, 0x41, 0x00};
    CHECK(DecodeOwnerDrawnTitle(immediate_nul.data(), immediate_nul.size()).empty());
}

TEST_CASE("A surrogate code unit rejects the buffer", "[finderbar][menu]") {
    std::vector<std::uint8_t> bytes = Utf16Bytes(L"F");
    bytes.push_back(0x00);
    bytes.push_back(0xD8);  // 0xD800, a lone high surrogate
    bytes.push_back(0x00);
    bytes.push_back(0x00);
    CHECK(DecodeOwnerDrawnTitle(bytes.data(), bytes.size()).empty());
}