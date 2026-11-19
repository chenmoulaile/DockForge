// DockForge T-085 probe: the theme distribution chain through the production
// modules, against a loopback HTTP origin (the PS1 driver serves the index and
// the packs; see theme_dist_probe.ps1).
//
// Parts (each prints "Pn PASS/FAIL"; exit code 0 iff all pass):
//   P1  fetch the remote index from the origin and parse it
//   P2  download the pack, verify SHA256, install it; it shows up in
//       ListThemes()/ListThemeDetails() at DOCKFORGE_THEMES_DIR
//   P3  the installed pack loads through the T-052 loader (LoadThemeInto)
//       and applies its overrides
//   P4  a pack whose bytes do not match the advertised SHA256 is refused and
//       the previously installed theme is untouched
//   P5  a pack carrying executable content passes the SHA gate but is refused
//       by the C-02 content validation, nothing is installed
//   P6  UninstallTheme removes the pack and the list no longer shows it
//
// Build: part of dockforge_tests' sibling targets (tools/*_probe.cpp pattern);
// linked against dockforge_lib so it exercises the real code paths.
#include "core/config.h"
#include "core/paths.h"
#include "core/theme.h"
#include "core/theme_index.h"
#include "core/theme_install.h"
#include "platform/http_client.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace core = dockforge::core;
namespace platform = dockforge::platform;

namespace {

int g_failures = 0;

void Report(const char* part, bool ok, const std::string& detail) {
    std::printf("%s %s %s\n", part, ok ? "PASS" : "FAIL", detail.c_str());
    if (!ok) ++g_failures;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: theme_dist_probe.exe <port>\n");
        return 2;
    }
    const int port = _wtoi(argv[1]);
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    std::printf("themes_dir=%ls\n", core::ThemesDir().c_str());

    // P1: index fetch + parse.
    {
        std::vector<core::ThemeIndexEntry> entries;
        std::string error;
        const bool ok = core::FetchThemeIndex(base + "/theme-index/index.json", entries,
                                              &error);
        Report("P1", ok && entries.size() == 1 && entries[0].name == "mono",
               ok ? ("entries=" + std::to_string(entries.size()) + " name=" +
                     (entries.empty() ? "?" : entries[0].name) + " sha=" +
                     (entries.empty() ? "?" : entries[0].sha256.substr(0, 12)))
                  : error);
        if (!ok || entries.empty()) return 1;
        const core::ThemeIndexEntry entry = entries[0];

        // P2: download + SHA256 + install.
        const platform::HttpBytesResult downloaded =
            dockforge::platform::HttpGetBytes(entry.download_url, 15000);
        bool p2 = downloaded.ok;
        std::string p2_detail = "status=" + std::to_string(downloaded.status) +
                                " bytes=" + std::to_string(downloaded.body.size());
        if (p2) {
            const core::ThemeInstallResult result =
                core::InstallThemePackBytes(downloaded.body, entry.sha256, entry.name);
            p2 = result.ok;
            p2_detail = result.ok
                            ? p2_detail + " installed; theme.json present=" +
                                  (GetFileAttributesW((core::ThemesDir() + L"\\mono\\theme.json")
                                                          .c_str()) != INVALID_FILE_ATTRIBUTES
                                         ? "1"
                                         : "0")
                            : result.error;
        }
        bool listed = false;
        for (const std::string& name : core::ListThemes()) {
            if (name == "mono") listed = true;
        }
        Report("P2", p2 && listed, p2_detail + " listed=" + (listed ? "1" : "0"));

        // P3: the T-052 loader applies the pack's overrides.
        core::Config config;
        std::string theme_error;
        const bool loaded = core::LoadThemeInto("mono", config, &theme_error);
        Report("P3", loaded && config.appearance.corner_radius == 12,
               loaded ? ("resolved=" + theme_error + " corner_radius=" +
                         std::to_string(config.appearance.corner_radius))
                      : theme_error);

        // P4: tampered bytes against the advertised sha -> refused, original
        // install untouched.
        const platform::HttpBytesResult tampered =
            dockforge::platform::HttpGetBytes(base + "/theme-index/packs/other.dockforge-theme",
                                              15000);
        bool p4 = tampered.ok;
        if (p4) {
            const core::ThemeInstallResult result =
                core::InstallThemePackBytes(tampered.body, entry.sha256, "mono");
            p4 = !result.ok && result.error.find("sha256 mismatch") != std::string::npos;
            Report("P4", p4, result.ok ? "INCORRECTLY INSTALLED" : result.error);
        } else {
            Report("P4", false, "tampered download failed");
        }

        // P5: executable content passes the SHA gate but the C-02 validator
        // refuses it.
        const platform::HttpBytesResult exec_pack =
            dockforge::platform::HttpGetBytes(base + "/theme-index/packs/exec.dockforge-theme",
                                              15000);
        bool p5 = exec_pack.ok;
        if (p5) {
            std::string sha;
            std::string sha_error;
            p5 = core::Sha256Hex(exec_pack.body, sha, sha_error);
            if (p5) {
                const core::ThemeInstallResult result =
                    core::InstallThemePackBytes(exec_pack.body, sha, "execpack");
                p5 = !result.ok && result.error.find("exe") != std::string::npos;
                Report("P5", p5, result.ok ? "INCORRECTLY INSTALLED" : result.error);
            } else {
                Report("P5", false, sha_error);
            }
        } else {
            Report("P5", false, "exec pack download failed");
        }

        // P6: uninstall.
        std::string uninstall_error;
        bool p6 = core::UninstallTheme("mono", uninstall_error);
        bool still_listed = false;
        for (const std::string& name : core::ListThemes()) {
            if (name == "mono") still_listed = true;
        }
        p6 = p6 && !still_listed &&
             GetFileAttributesW((core::ThemesDir() + L"\\mono").c_str()) ==
                 INVALID_FILE_ATTRIBUTES;
        Report("P6", p6, p6 ? "uninstalled, list clean" : uninstall_error);
    }

    if (g_failures > 0) {
        std::printf("PROBE FAIL (%d part(s))\n", g_failures);
        return 1;
    }
    std::printf("PROBE ALL PASS\n");
    return 0;
}
