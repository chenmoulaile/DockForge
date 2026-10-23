// Tests for src/shell/app_resolver.* (CONTRACTS.md C-08).
//
// The pure helpers are tested without COM. The COM paths are tested only for their
// failure behaviour: a unit test must not depend on which apps this machine happens
// to have installed, so "not found" and "not a shortcut" are the assertions, not
// "Cursor resolved to C:\...".
#include "catch_amalgamated.hpp"

#include <windows.h>

#include <string>

#include "shell/app_entry.h"
#include "shell/app_launcher.h"
#include "shell/app_resolver.h"

using namespace dockforge::shell;

TEST_CASE("ClassifyParsingName separates the five AppsFolder shapes") {
    // Real parsing names captured from this machine's AppsFolder (evidence/T-006).
    CHECK(ClassifyParsingName(L"C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\"
                             L"AutoHotkey Dash.lnk") == ParsingKind::ShortcutPath);
    CHECK(ClassifyParsingName(L"C:\\msys64\\mingw64.exe") == ParsingKind::FilePath);
    CHECK(ClassifyParsingName(L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App") ==
          ParsingKind::PackagedAppId);
    CHECK(ClassifyParsingName(L"Anysphere.Cursor") == ParsingKind::DesktopAppId);
    CHECK(ClassifyParsingName(L"Chrome") == ParsingKind::DesktopAppId);
    CHECK(ClassifyParsingName(L"{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\cmd.exe") ==
          ParsingKind::NamespacePath);
    CHECK(ClassifyParsingName(L"") == ParsingKind::Unknown);
}

TEST_CASE("LooksLikeAppId accepts bare desktop ids but not paths") {
    CHECK(LooksLikeAppId(L"Anysphere.Cursor"));
    CHECK(LooksLikeAppId(L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"));
    CHECK_FALSE(LooksLikeAppId(L"C:\\app.exe"));
    CHECK_FALSE(LooksLikeAppId(L"dir\\app.exe"));
    CHECK_FALSE(LooksLikeAppId(L""));
}

TEST_CASE("ResolveParsingName builds entries for both id shapes without a path") {
    AppEntry uwp;
    REQUIRE(ResolveParsingName(L"Microsoft.Paint_8wekyb3d8bbwe!App", L"Paint", uwp) ==
            ResolveError::None);
    CHECK(uwp.kind == AppKind::Uwp);
    CHECK(uwp.display_name == L"Paint");
    CHECK(uwp.launchable());
    CHECK(IconSourceFor(uwp) == L"shell:AppsFolder\\Microsoft.Paint_8wekyb3d8bbwe!App");

    AppEntry desktop;
    REQUIRE(ResolveParsingName(L"Anysphere.Cursor", L"Cursor", desktop) ==
            ResolveError::None);
    CHECK(desktop.kind == AppKind::ShellItem);
    CHECK(desktop.display_name == L"Cursor");
    CHECK(desktop.aumid == L"Anysphere.Cursor");
    CHECK(desktop.target_path.empty());
    CHECK(desktop.launchable());
    CHECK(IconSourceFor(desktop) == L"shell:AppsFolder\\Anysphere.Cursor");
}

TEST_CASE("MakeShellItemEntry falls back to the app id as label") {
    const AppEntry entry = MakeShellItemEntry(L"", L"com.qoder.app");
    CHECK(entry.kind == AppKind::ShellItem);
    CHECK(entry.display_name == L"com.qoder.app");
    CHECK(entry.launchable());
}

TEST_CASE("ResolveParsingName reports failure for unresolvable paths") {
    AppEntry entry;
    CHECK(ResolveParsingName(L"C:\\DockForge\\nope.lnk", L"nope", entry) ==
          ResolveError::NotFound);
    CHECK(ResolveParsingName(L"C:\\DockForge\\nope.exe", L"nope", entry) ==
          ResolveError::NotFound);
    CHECK(ResolveParsingName(L"", L"", entry) == ResolveError::EmptyPath);
}

TEST_CASE("MakeNamespaceEntry keeps the identity and qualifies the icon source") {
    const std::wstring parsing = L"{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\cmd.exe";
    const AppEntry entry = MakeNamespaceEntry(L"\u547D\u4EE4\u63D0\u793A\u7B26", parsing);
    CHECK(entry.kind == AppKind::NamespaceItem);
    CHECK(entry.display_name == L"\u547D\u4EE4\u63D0\u793A\u7B26");
    CHECK(entry.namespace_parsing == parsing);
    CHECK(entry.aumid.empty());
    CHECK(entry.target_path.empty());
    CHECK(entry.launchable());
    // SHCreateItemFromParsingName rejects the bare form, so the icon source must
    // carry the shell:AppsFolder prefix.
    CHECK(entry.icon_source == L"shell:AppsFolder\\" + parsing);
    CHECK(IconSourceFor(entry) == L"shell:AppsFolder\\" + parsing);
}

TEST_CASE("ShellNamespacePath does not double-qualify") {
    const std::wstring bare = L"Anysphere.Cursor";
    const std::wstring qualified = L"shell:AppsFolder\\Anysphere.Cursor";
    CHECK(ShellNamespacePath(bare) == qualified);
    CHECK(ShellNamespacePath(qualified) == qualified);
    CHECK(ShellNamespacePath(L"") .empty());
}

TEST_CASE("ResolveParsingName builds a namespace entry without touching the filesystem") {
    const std::wstring parsing = L"{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\cleanmgr.exe";
    AppEntry entry;
    REQUIRE(ResolveParsingName(parsing, L"\u78C1\u76D8\u6E05\u7406", entry) == ResolveError::None);
    CHECK(entry.kind == AppKind::NamespaceItem);
    CHECK(entry.namespace_parsing == parsing);
    CHECK(entry.launchable());
}

TEST_CASE("Launch rejects an entry with no identity") {
    AppEntry empty;
    const LaunchResult result = Launch(empty);
    CHECK_FALSE(result.ok());
    CHECK(result.error == LaunchError::NotLaunchable);
    CHECK(result.process_id == 0);

    AppEntry no_target;
    no_target.kind = AppKind::Executable;
    CHECK(Launch(no_target).error == LaunchError::NotLaunchable);

    AppEntry no_id;
    no_id.kind = AppKind::ShellItem;
    CHECK(Launch(no_id).error == LaunchError::NotLaunchable);
}

TEST_CASE("Launch reports NotFound for a path that does not exist") {
    AppEntry entry;
    entry.kind = AppKind::Executable;
    entry.target_path = L"C:\\DockForge\\definitely-not-here.exe";
    const LaunchResult result = Launch(entry);
    CHECK_FALSE(result.ok());
    CHECK(result.error == LaunchError::NotFound);
    CHECK(std::string(result.route) == "CreateProcess");
}

TEST_CASE("ToString covers every launch error") {
    CHECK(std::string(ToString(LaunchError::None)) == "none");
    CHECK(std::string(ToString(LaunchError::NotLaunchable)) == "not-launchable");
    CHECK(std::string(ToString(LaunchError::NotFound)) == "not-found");
    CHECK(std::string(ToString(LaunchError::ActivationFailed)) == "activation-failed");
    CHECK(std::string(ToString(LaunchError::ProcessFailed)) == "process-failed");
}

TEST_CASE("HasShortcutExtension is case insensitive") {
    CHECK(HasShortcutExtension(L"a.lnk"));
    CHECK(HasShortcutExtension(L"A.LNK"));
    CHECK(HasShortcutExtension(L"dir\\thing.LnK"));
    CHECK_FALSE(HasShortcutExtension(L"a.exe"));
    CHECK_FALSE(HasShortcutExtension(L"lnk"));
    CHECK_FALSE(HasShortcutExtension(L""));
}

TEST_CASE("LooksLikeAumid requires family name and application id") {
    CHECK(LooksLikeAumid(L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"));
    CHECK(LooksLikeAumid(L"MSTeams_8wekyb3d8bbwe!MSTeams"));
    // Missing the underscore-separated publisher hash.
    CHECK_FALSE(LooksLikeAumid(L"Microsoft.WindowsCalculator!App"));
    // Missing the application id.
    CHECK_FALSE(LooksLikeAumid(L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!"));
    CHECK_FALSE(LooksLikeAumid(L"!App"));
    CHECK_FALSE(LooksLikeAumid(L""));
    // A filesystem path is not an AUMID even if it contains a bang.
    CHECK_FALSE(LooksLikeAumid(L"C:\\dir_with_underscore\\a!b.exe"));
}

TEST_CASE("TitleFromPath strips directory and extension") {
    CHECK(TitleFromPath(L"C:\\Windows\\notepad.exe") == L"notepad");
    CHECK(TitleFromPath(L"notepad.exe") == L"notepad");
    CHECK(TitleFromPath(L"C:\\no-extension") == L"no-extension");
    CHECK(TitleFromPath(L"") == L"");
    // A leading dot means a hidden file, not an extension separator.
    CHECK(TitleFromPath(L"C:\\.hidden") == L".hidden");
}

TEST_CASE("DirectoryPart drops the file name") {
    CHECK(DirectoryPart(L"C:\\Windows\\notepad.exe") == L"C:\\Windows");
    CHECK(DirectoryPart(L"notepad.exe") == L"");
    CHECK(DirectoryPart(L"C:\\") == L"");
}

TEST_CASE("IconSourceFor prefers the explicit source, then icon path, then target") {
    AppEntry entry;
    entry.kind = AppKind::Executable;
    entry.target_path = L"C:\\app.exe";
    CHECK(IconSourceFor(entry) == L"C:\\app.exe");

    entry.icon_path = L"C:\\icons.dll";
    CHECK(IconSourceFor(entry) == L"C:\\icons.dll");

    entry.icon_source = L"C:\\override.ico";
    CHECK(IconSourceFor(entry) == L"C:\\override.ico");

    AppEntry uwp;
    uwp.kind = AppKind::Uwp;
    uwp.aumid = L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App";
    CHECK(IconSourceFor(uwp) ==
          L"shell:AppsFolder\\Microsoft.WindowsCalculator_8wekyb3d8bbwe!App");
}

TEST_CASE("ExpandEnvironment resolves %VAR% and leaves plain paths alone") {
    CHECK(ExpandEnvironment(L"C:\\Windows\\notepad.exe") == L"C:\\Windows\\notepad.exe");
    CHECK(ExpandEnvironment(L"") == L"");

    wchar_t windir[MAX_PATH]{};
    REQUIRE(GetEnvironmentVariableW(L"windir", windir, MAX_PATH) > 0);
    CHECK(ExpandEnvironment(L"%windir%\\system32\\control.exe") ==
          std::wstring(windir) + L"\\system32\\control.exe");
    // An unknown variable is left verbatim rather than collapsed to nothing.
    CHECK(ExpandEnvironment(L"%DOCKFORGE_NO_SUCH_VAR%\\x.exe") ==
          L"%DOCKFORGE_NO_SUCH_VAR%\\x.exe");
}

TEST_CASE("ResolveShortcut rejects non-shortcuts and missing files") {
    AppEntry entry;
    CHECK(ResolveShortcut(L"", entry) == ResolveError::EmptyPath);
    CHECK(ResolveShortcut(L"C:\\Windows\\notepad.exe", entry) ==
          ResolveError::NotAShortcut);
    CHECK(ResolveShortcut(L"C:\\DockForge\\no-such-file.lnk", entry) ==
          ResolveError::NotFound);
    // A rejected resolve must not leave a half-filled entry behind.
    CHECK(entry.kind == AppKind::Unknown);
    CHECK(entry.target_path.empty());
}

TEST_CASE("ResolvePath rejects a missing executable") {
    AppEntry entry;
    CHECK(ResolvePath(L"", entry) == ResolveError::EmptyPath);
    CHECK(ResolvePath(L"C:\\DockForge\\no-such-app.exe", entry) == ResolveError::NotFound);
    CHECK(entry.kind == AppKind::Unknown);
}

TEST_CASE("ResolvePath accepts a real executable") {
    wchar_t windir[MAX_PATH]{};
    REQUIRE(GetEnvironmentVariableW(L"windir", windir, MAX_PATH) > 0);
    const std::wstring notepad = std::wstring(windir) + L"\\system32\\notepad.exe";
    REQUIRE(GetFileAttributesW(notepad.c_str()) != INVALID_FILE_ATTRIBUTES);

    AppEntry entry;
    REQUIRE(ResolvePath(notepad, entry) == ResolveError::None);
    CHECK(entry.kind == AppKind::Executable);
    CHECK(entry.display_name == L"notepad");
    CHECK(entry.target_path == notepad);
    CHECK(entry.working_dir == std::wstring(windir) + L"\\system32");
    CHECK(entry.launchable());
}

TEST_CASE("ResolveAumid rejects malformed ids before touching the shell") {
    AppEntry entry;
    CHECK(ResolveAumid(L"", entry) == ResolveError::InvalidAumid);
    CHECK(ResolveAumid(L"no-bang-here", entry) == ResolveError::InvalidAumid);
    CHECK(ResolveAumid(L"Microsoft.NoSuchPackage_8wekyb3d8bbwe!App", entry) ==
          ResolveError::InvalidAumid);
}

TEST_CASE("MakeUwpEntry fills icon source and falls back to the AUMID as label") {
    const AppEntry entry = MakeUwpEntry(L"", L"Microsoft.Paint_8wekyb3d8bbwe!App");
    CHECK(entry.kind == AppKind::Uwp);
    CHECK(entry.display_name == L"Microsoft.Paint_8wekyb3d8bbwe!App");
    CHECK(entry.icon_source == L"shell:AppsFolder\\Microsoft.Paint_8wekyb3d8bbwe!App");
    CHECK(entry.launchable());

    const AppEntry named = MakeUwpEntry(L"Paint", L"Microsoft.Paint_8wekyb3d8bbwe!App");
    CHECK(named.display_name == L"Paint");
}
