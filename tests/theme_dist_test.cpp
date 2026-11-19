// DockForge T-085 tests: community theme index parsing, pack extraction,
// SHA256 verification and the install/uninstall orchestration.
//
// Isolation: every test that touches the filesystem points DOCKFORGE_THEMES_DIR
// at a fresh %TEMP% directory (paths.h documents the override alongside
// DOCKFORGE_CONFIG_PATH), so no test can touch the real profile. The config
// tests here use ParseConfig/SerializeConfig directly - no file IO.
#include <catch_amalgamated.hpp>

#include "core/config.h"
#include "core/paths.h"
#include "core/theme.h"
#include "core/theme_index.h"
#include "core/theme_install.h"
#include "platform/http_client.h"
#include "settings/settings_model.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace core = dockforge::core;
namespace platform = dockforge::platform;
namespace settings = dockforge::settings;

namespace {

// ---------------------------------------------------------------------------
// Minimal STORE-only zip writer (no compression): the pack format is a plain
// zip, and the installer's unzip path must be exercised against real bytes
// produced here rather than a fixture file that could drift from the writer.
unsigned long Crc32(const unsigned char* data, std::size_t len) {
    static unsigned long table[256];
    static bool table_ready = false;
    if (!table_ready) {
        for (unsigned long i = 0; i < 256; ++i) {
            unsigned long c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
        table_ready = true;
    }
    unsigned long crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

void PutU16(std::string& out, unsigned v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
}

void PutU32(std::string& out, unsigned long v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

struct ZipEntry {
    std::string name;  // exactly as written into the archive (may use '\\')
    std::string data;
};

// Builds a store-only zip: every entry stored uncompressed with correct
// CRC32/offsets, central directory and EOCD - the shape Compress-Archive
// produces (deflate), just without the deflate so the test owns all bytes.
std::string BuildZip(const std::vector<ZipEntry>& entries) {
    std::string out;
    struct Record {
        std::string name;
        unsigned long crc = 0;
        unsigned long size = 0;
        unsigned long offset = 0;
    };
    std::vector<Record> records;
    for (const ZipEntry& entry : entries) {
        Record rec;
        rec.name = entry.name;
        rec.size = static_cast<unsigned long>(entry.data.size());
        rec.crc = Crc32(reinterpret_cast<const unsigned char*>(entry.data.data()),
                        entry.data.size());
        rec.offset = static_cast<unsigned long>(out.size());
        out += "PK\x03\x04";
        PutU16(out, 20);      // version needed
        PutU16(out, 0);       // flags
        PutU16(out, 0);       // method: store
        PutU16(out, 0);       // mod time
        PutU16(out, 0);       // mod date
        PutU32(out, rec.crc);
        PutU32(out, rec.size);   // compressed
        PutU32(out, rec.size);   // uncompressed
        PutU16(out, static_cast<unsigned>(entry.name.size()));
        PutU16(out, 0);       // extra len
        out += entry.name;
        out += entry.data;
        records.push_back(rec);
    }
    const unsigned long central_offset = static_cast<unsigned long>(out.size());
    for (const Record& rec : records) {
        out += "PK\x01\x02";
        PutU16(out, 20);  // version made by
        PutU16(out, 20);  // version needed
        PutU16(out, 0);
        PutU16(out, 0);
        PutU16(out, 0);
        PutU16(out, 0);
        PutU32(out, rec.crc);
        PutU32(out, rec.size);
        PutU32(out, rec.size);
        PutU16(out, static_cast<unsigned>(rec.name.size()));
        PutU16(out, 0);  // extra
        PutU16(out, 0);  // comment
        PutU16(out, 0);  // disk
        PutU16(out, 0);  // internal attrs
        PutU32(out, 0);  // external attrs
        PutU32(out, rec.offset);
        out += rec.name;
    }
    const unsigned long central_size =
        static_cast<unsigned long>(out.size()) - central_offset;
    out += "PK\x05\x06";
    PutU16(out, 0);
    PutU16(out, 0);
    PutU16(out, static_cast<unsigned>(records.size()));
    PutU16(out, static_cast<unsigned>(records.size()));
    PutU32(out, central_size);
    PutU32(out, central_offset);
    PutU16(out, 0);
    return out;
}

// A C-02-conformant pack builder used by many cases below.
std::string BuildGoodPack(const char* theme_name = "mono") {
    std::string theme_json = std::string("{ \"name\": \"") + theme_name +
                             "\", \"author\": \"dockforge\", \"version\": \"1.0.0\", "
                             "\"overrides\": { \"appearance\": { \"corner_radius\": 12 } } }";
    return BuildZip({
        {"theme.json", theme_json},
        {"preview.png", std::string("\x89PNG fake preview bytes")},
        {"icons\\sample.png", std::string("fake icon bytes")},  // PS 5.1 separator
    });
}

// A scratch themes dir per test process; each SECTION re-points the env var so
// tests never share state and never write into the real profile.
std::wstring MakeScratchThemesDir(const char* tag) {
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    static unsigned counter = 0;
    const std::wstring dir =
        std::wstring(temp) + L"dockforge-t085-test-" + std::wstring(tag, tag + std::strlen(tag)) +
        L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(counter++);
    SetEnvironmentVariableW(L"DOCKFORGE_THEMES_DIR", dir.c_str());
    REQUIRE(core::ThemesDir() == dir);
    return dir;
}

}  // namespace

// ---------------------------------------------------------------- index parse

TEST_CASE("theme index parser accepts a valid document", "[t085]") {
    const std::string index = R"({
        "schema": 1,
        "updated": "2026-11-20",
        "themes": [ {
            "name": "mono", "author": "dockforge", "version": "1.0.0",
            "preview_url": "https://example.com/mono.png",
            "download_url": "https://example.com/mono.dockforge-theme",
            "sha256": "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
            "size_bytes": 1204
        } ]
    })";
    std::vector<core::ThemeIndexEntry> entries;
    std::string error;
    REQUIRE(core::ParseThemeIndex(index, entries, &error));
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].name == "mono");
    CHECK(entries[0].author == "dockforge");
    CHECK(entries[0].version == "1.0.0");
    CHECK(entries[0].size_bytes == 1204);
    // sha256 normalizes to lowercase for the installer comparison.
    CHECK(entries[0].sha256 ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("theme index parser rejects malformed documents", "[t085]") {
    std::vector<core::ThemeIndexEntry> entries;
    std::string error;

    CHECK_FALSE(core::ParseThemeIndex("not json", entries, &error));
    CHECK_FALSE(core::ParseThemeIndex(R"({"schema": 2, "themes": []})", entries, &error));
    CHECK_FALSE(core::ParseThemeIndex(R"({"schema": 1})", entries, &error));
    CHECK_FALSE(core::ParseThemeIndex(R"({"schema": 1, "themes": {}})", entries, &error));

    // An EMPTY array is valid - the browser just shows no rows.
    CHECK(core::ParseThemeIndex(R"({"schema": 1, "themes": []})", entries, &error));
    CHECK(entries.empty());
}

TEST_CASE("theme index parser rejects bad entries wholesale", "[t085]") {
    std::vector<core::ThemeIndexEntry> entries;
    std::string error;
    const auto parse = [&](const std::string& one_entry) {
        return core::ParseThemeIndex(
            std::string(R"({"schema": 1, "themes": [)") + one_entry + "]}", entries, &error);
    };
    const auto make = [](const std::string& name, const std::string& url,
                         const std::string& sha) {
        return std::string(R"({"name": ")") + name + R"(", "download_url": ")" + url +
                           R"(", "sha256": ")" + sha + "\"}";
    };
    const std::string sha_ok(64, 'a');

    // Name rules: traversal, built-ins, uppercase, empty, overlong.
    CHECK_FALSE(parse(make("../evil", "https://x/y", sha_ok)));
    CHECK_FALSE(parse(make("default", "https://x/y", sha_ok)));
    CHECK_FALSE(parse(make("dark", "https://x/y", sha_ok)));
    CHECK_FALSE(parse(make("Upper", "https://x/y", sha_ok)));
    CHECK_FALSE(parse(make("", "https://x/y", sha_ok)));
    CHECK_FALSE(parse(make(std::string(65, 'x'), "https://x/y", sha_ok)));
    // download_url must be http(s).
    CHECK_FALSE(parse(make("ok", "ftp://x/y", sha_ok)));
    CHECK_FALSE(parse(make("ok", "", sha_ok)));
    // sha256 must be exactly 64 hex chars.
    CHECK_FALSE(parse(make("ok", "https://x/y", std::string(63, 'a'))));
    CHECK_FALSE(parse(make("ok", "https://x/y", std::string(64, 'g'))));
    // Duplicate names are ambiguous for the browser.
    const std::string dup = std::string(R"({"schema": 1, "themes": [)") +
                           make("ok", "https://x/1", sha_ok) + "," +
                           make("ok", "https://x/2", sha_ok) + "]}";
    CHECK_FALSE(core::ParseThemeIndex(dup, entries, &error));
}

TEST_CASE("IsValidThemeName rules", "[t085]") {
    CHECK(core::IsValidThemeName("mono"));
    CHECK(core::IsValidThemeName("my_theme-2.test"));
    CHECK_FALSE(core::IsValidThemeName(""));
    CHECK_FALSE(core::IsValidThemeName("a/b"));
    CHECK_FALSE(core::IsValidThemeName("a\\b"));
    CHECK_FALSE(core::IsValidThemeName("a b"));
    CHECK_FALSE(core::IsValidThemeName(".."));
    CHECK_FALSE(core::IsValidThemeName("default"));
    CHECK_FALSE(core::IsValidThemeName("dark"));
}

// --------------------------------------------------------------------- sha256

TEST_CASE("SHA256 known vectors via bcrypt", "[t085]") {
    std::string hex;
    std::string error;
    REQUIRE(core::Sha256Hex("abc", hex, error));
    CHECK(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE(core::Sha256Hex("", hex, error));
    CHECK(hex == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    // 1MB input crosses the chunked-hash path.
    const std::string big(1 << 20, 'z');
    REQUIRE(core::Sha256Hex(big, hex, error));
    CHECK(hex.size() == 64);
}

// ---------------------------------------------------------------- extraction

TEST_CASE("pack extraction lands theme.json/preview/icons with backslash entries",
          "[t085]") {
    const std::wstring dir = MakeScratchThemesDir("extract");
    REQUIRE(core::EnsureDirectory(dir));
    std::string error;
    const std::string pack = BuildGoodPack();
    INFO("extract error: " << error);
    REQUIRE(core::ExtractThemePackBytes(pack, dir, error));
    // theme.json at the root...
    const std::wstring json_path = dir + L"\\theme.json";
    CHECK(GetFileAttributesW(json_path.c_str()) != INVALID_FILE_ATTRIBUTES);
    // ...icons\sample.png under the directory the BACKSLASH entry implies.
    const std::wstring icon_path = dir + L"\\icons\\sample.png";
    CHECK(GetFileAttributesW(icon_path.c_str()) != INVALID_FILE_ATTRIBUTES);
    HANDLE h = CreateFileW(icon_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    char buffer[32] = {};
    DWORD got = 0;
    ReadFile(h, buffer, sizeof(buffer), &got, nullptr);
    CloseHandle(h);
    CHECK(std::string(buffer, got) == "fake icon bytes");
}

TEST_CASE("pack extraction rejects traversal entries", "[t085]") {
    const std::wstring dir = MakeScratchThemesDir("traversal");
    REQUIRE(core::EnsureDirectory(dir));
    std::string error;
    const std::string pack = BuildZip({{"theme.json", "{}"},
                                       {"evil\\..\\..\\escaped.txt", "boom"}});
    CHECK_FALSE(core::ExtractThemePackBytes(pack, dir, error));
    CHECK(error.find("escapes its root") != std::string::npos);
    // Nothing escaped the destination.
    const std::wstring escaped = dir + L"\\..\\escaped.txt";
    CHECK(GetFileAttributesW(escaped.c_str()) == INVALID_FILE_ATTRIBUTES);
}

TEST_CASE("pack extraction rejects a non-zip body", "[t085]") {
    const std::wstring dir = MakeScratchThemesDir("notzip");
    REQUIRE(core::EnsureDirectory(dir));
    std::string error;
    CHECK_FALSE(core::ExtractThemePackBytes("this is not a zip at all", dir, error));
    CHECK(error.find("not a readable") != std::string::npos);
}

// ---------------------------------------------------------------- validation

TEST_CASE("pack validation enforces C-02 executable ban", "[t085]") {
    const std::wstring dir = MakeScratchThemesDir("validate");
    REQUIRE(core::EnsureDirectory(dir));

    // A good pack passes.
    std::string error;
    REQUIRE(core::ExtractThemePackBytes(BuildGoodPack(), dir, error));
    REQUIRE(core::ValidateThemePackDir(dir, error));

    // An executable payload is refused and names the offender.
    const std::string pack =
        BuildZip({{"theme.json", "{}"}, {"bin\\helper.exe", "MZ fake exe"}});
    REQUIRE(core::ExtractThemePackBytes(pack, dir, error));
    CHECK_FALSE(core::ValidateThemePackDir(dir, error));
    CHECK(error.find("helper.exe") != std::string::npos);
}

TEST_CASE("pack validation allows theme assets but not scripts", "[t085]") {
    const std::wstring dir = MakeScratchThemesDir("assets");
    REQUIRE(core::EnsureDirectory(dir));
    std::string error;
    const std::string pack = BuildZip({
        {"theme.json", "{}"},
        {"preview.png", "png"},
        {"calendar.json", "{}"},          // T-207 对接点
        {"fonts\\Display.ttf", "font"},   // fonts are themeable assets
        {"icons\\a.png", "png"},
    });
    REQUIRE(core::ExtractThemePackBytes(pack, dir, error));
    REQUIRE(core::ValidateThemePackDir(dir, error));

    const std::string bad = BuildZip({{"theme.json", "{}"}, {"run.ps1", "evil"}});
    REQUIRE(core::ExtractThemePackBytes(bad, dir, error));
    CHECK_FALSE(core::ValidateThemePackDir(dir, error));
    CHECK(error.find("run.ps1") != std::string::npos);
}

TEST_CASE("pack validation requires theme.json", "[t085]") {
    const std::wstring dir = MakeScratchThemesDir("nojson");
    REQUIRE(core::EnsureDirectory(dir));
    std::string error;
    REQUIRE(core::ExtractThemePackBytes(BuildZip({{"preview.png", "png"}}), dir, error));
    CHECK_FALSE(core::ValidateThemePackDir(dir, error));
    CHECK(error.find("theme.json") != std::string::npos);
}

// ------------------------------------------------------------------- install

TEST_CASE("install lands a pack and ListThemeDetails sees it", "[t085]") {
    const std::wstring dir = MakeScratchThemesDir("install");
    SetEnvironmentVariableW(L"DOCKFORGE_THEMES_DIR", dir.c_str());
    REQUIRE(core::EnsureDirectory(dir));

    const std::string pack = BuildGoodPack("mono");
    std::string sha;
    std::string sha_error;
    REQUIRE(core::Sha256Hex(pack, sha, sha_error));

    const core::ThemeInstallResult result =
        core::InstallThemePackBytes(pack, sha, "mono");
    REQUIRE(result.ok);
    CHECK(result.error.empty());
    // Installed where the contract says (C-03): <ThemesDir>\mono\theme.json.
    CHECK(GetFileAttributesW((dir + L"\\mono\\theme.json").c_str()) !=
          INVALID_FILE_ATTRIBUTES);

    bool found = false;
    for (const core::ThemeDetails& details : core::ListThemeDetails()) {
        if (details.name == "mono") {
            found = true;
            CHECK_FALSE(details.builtin);
            CHECK(details.author == "dockforge");
            CHECK(details.version == "1.0.0");
        }
    }
    CHECK(found);
    // And the normal combo list (T-052) picks it up too.
    const std::vector<std::string> names = core::ListThemes();
    CHECK(std::find(names.begin(), names.end(), "mono") != names.end());
}

TEST_CASE("install refuses a sha256 mismatch without writing anything", "[t085]") {
    const std::wstring dir = MakeScratchThemesDir("shafail");
    SetEnvironmentVariableW(L"DOCKFORGE_THEMES_DIR", dir.c_str());
    REQUIRE(core::EnsureDirectory(dir));

    const std::string wrong(64, '0');
    const core::ThemeInstallResult result =
        core::InstallThemePackBytes(BuildGoodPack("mono"), wrong, "mono");
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("sha256 mismatch") != std::string::npos);
    // Nothing installed, no staging leftovers.
    CHECK(GetFileAttributesW((dir + L"\\mono").c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesW((dir + L"\\mono.staging-1").c_str()) == INVALID_FILE_ATTRIBUTES);
    bool saw_staging = false;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name != L"." && name != L".." &&
                name.find(L"staging") != std::wstring::npos) {
                saw_staging = true;
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    CHECK_FALSE(saw_staging);
}

TEST_CASE("install refuses executable content and bad names", "[t085]") {
    const std::wstring dir = MakeScratchThemesDir("refuse");
    SetEnvironmentVariableW(L"DOCKFORGE_THEMES_DIR", dir.c_str());
    REQUIRE(core::EnsureDirectory(dir));

    const std::string pack =
        BuildZip({{"theme.json", "{}"}, {"payload.exe", "MZ"}});
    std::string sha;
    std::string sha_error;
    REQUIRE(core::Sha256Hex(pack, sha, sha_error));

    const core::ThemeInstallResult result = core::InstallThemePackBytes(pack, sha, "evil");
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("exe") != std::string::npos);

    // Names the validator refuses: built-ins and traversal shapes.
    const core::ThemeInstallResult builtin =
        core::InstallThemePackBytes(pack, sha, "default");
    CHECK_FALSE(builtin.ok);
    const core::ThemeInstallResult traversal =
        core::InstallThemePackBytes(pack, sha, "..\\evil");
    CHECK_FALSE(traversal.ok);
}

TEST_CASE("reinstall replaces an existing community pack", "[t085]") {
    const std::wstring dir = MakeScratchThemesDir("replace");
    SetEnvironmentVariableW(L"DOCKFORGE_THEMES_DIR", dir.c_str());
    REQUIRE(core::EnsureDirectory(dir));

    const std::string pack = BuildGoodPack("mono");
    std::string sha;
    std::string sha_error;
    REQUIRE(core::Sha256Hex(pack, sha, sha_error));
    REQUIRE(core::InstallThemePackBytes(pack, sha, "mono").ok);
    // Second install of the same pack: the directory is replaced, not merged
    // or duplicated, and no staging tree survives.
    REQUIRE(core::InstallThemePackBytes(pack, sha, "mono").ok);
    CHECK(GetFileAttributesW((dir + L"\\mono\\theme.json").c_str()) !=
          INVALID_FILE_ATTRIBUTES);
    WIN32_FIND_DATAW fd{};
    bool saw_staging = false;
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name != L"." && name != L".." &&
                name.find(L"staging") != std::wstring::npos) {
                saw_staging = true;
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    CHECK_FALSE(saw_staging);
}

TEST_CASE("uninstall deletes a pack and refuses built-ins", "[t085]") {
    const std::wstring dir = MakeScratchThemesDir("uninstall");
    SetEnvironmentVariableW(L"DOCKFORGE_THEMES_DIR", dir.c_str());
    REQUIRE(core::EnsureDirectory(dir));

    const std::string pack = BuildGoodPack("mono");
    std::string sha;
    std::string sha_error;
    REQUIRE(core::Sha256Hex(pack, sha, sha_error));
    REQUIRE(core::InstallThemePackBytes(pack, sha, "mono").ok);

    std::string error;
    std::string builtin_error;
    CHECK_FALSE(core::UninstallTheme("default", builtin_error));
    CHECK(core::UninstallTheme("mono", error));
    CHECK(GetFileAttributesW((dir + L"\\mono").c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK_FALSE(core::UninstallTheme("mono", error));
    CHECK(error.find("not installed") != std::string::npos);
}

// ------------------------------------------------------------------- config

TEST_CASE("config roundtrip keeps themes.index_url and stays quiet on default",
          "[t085]") {
    core::Config config;
    // The default URL appears in no default file (quiet-file rule).
    CHECK(std::string(core::DefaultThemeIndexUrl()) ==
          core::Config{}.themes.index_url);
    CHECK(core::SerializeConfig(core::Config{}).find("\"themes\"") == std::string::npos);

    // A custom URL parses, survives a save/load roundtrip, and is echoed.
    core::Config custom;
    custom.themes.index_url = "https://my.example.org/themes/index.json";
    const std::string text = core::SerializeConfig(custom);
    CHECK(text.find("themes") != std::string::npos);
    core::Config parsed;
    std::string error;
    REQUIRE(core::ParseConfig(text, parsed, &error));
    CHECK(parsed.themes.index_url == "https://my.example.org/themes/index.json");

    // An empty value in the file reads as the default.
    core::Config empty;
    REQUIRE(core::ParseConfig(R"({"themes": {"index_url": ""}})", empty, &error));
    CHECK(empty.themes.index_url == core::DefaultThemeIndexUrl());

    // A >256-byte URL is truncated (clamp), and the rewrite flag is set.
    core::Config long_url;
    long_url.themes.index_url = "https://example.com/" + std::string(300, 'a');
    const std::string long_text = core::SerializeConfig(long_url);
    core::Config long_parsed;
    REQUIRE(core::ParseConfig(long_text, long_parsed, &error));
    CHECK(long_parsed.themes.index_url.size() == 256);
    CHECK(long_parsed.needs_rewrite);
}

// ------------------------------------------------------------------ fetch

TEST_CASE("HttpGetBytes rejects a URL without a scheme in bounded time", "[t085]") {
    // No network: the URL parser is the unit under test here; the probe
    // exercises the real transport end to end.
    const platform::HttpBytesResult result = platform::HttpGetBytes("not a url", 3000);
    CHECK_FALSE(result.ok);
    CHECK(result.error == ERROR_INVALID_PARAMETER);
    CHECK(result.status == 0);
}

// ------------------------------------------------- settings model plumbing

TEST_CASE("settings model accepts and trims themes.index_url", "[t085]") {
    CHECK(dockforge::settings::SettingsModel::KnowsField("themes.index_url"));
    dockforge::settings::SettingsModel model;
    model.Record("themes.index_url", "  https://example.org/index.json  ");
    core::Config config;
    std::vector<std::string> changed;
    std::vector<std::string> rejected;
    CHECK(model.Apply(config, &changed, &rejected) == 1);
    CHECK(changed.size() == 1);
    CHECK(config.themes.index_url == "https://example.org/index.json");
    // Echo returns the stored value.
    CHECK(dockforge::settings::SettingsModel::CurrentValueText(config, "themes.index_url") ==
          "https://example.org/index.json");
}

