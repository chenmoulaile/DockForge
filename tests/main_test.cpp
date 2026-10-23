// Catch2 entry point (main() comes from catch_amalgamated.cpp).
//
// The shell tests call IShellLinkW, IShellItemImageFactory and WIC, all of which need
// an initialized COM apartment on the calling thread. Catch2 runs every test on the
// main thread, so one process-wide apartment is enough - and initializing it here
// keeps the individual test cases free of setup noise.
#include "catch_amalgamated.hpp"

#include <windows.h>
#include <objbase.h>

#include <string>

namespace {

// Points the config at a throwaway file before anything can read the real one.
//
// ConfigPath() caches on first use, so this has to run during static initialization -
// setting it inside a TEST_CASE would be too late for any test that ran earlier.
// The reason it exists at all: a test that exercises the order serializer called
// SaveConfig, which wrote `tiles: ["alpha.app"]` into the developer's real
// %APPDATA%\DockForge\config.json, and the next launch of the app came up with a
// single tile. Test isolation is not a nicety here; the test binary shares a profile
// with the product.
struct ConfigIsolation {
    ConfigIsolation() {
        wchar_t dir[MAX_PATH] = {};
        const DWORD written = GetTempPathW(MAX_PATH, dir);
        if (written == 0 || written >= MAX_PATH) return;
        const std::wstring path =
            std::wstring(dir, written) + L"dockforge-tests-" +
            std::to_wstring(GetCurrentProcessId()) + L"\\config.json";
        SetEnvironmentVariableW(L"DOCKFORGE_CONFIG_PATH", path.c_str());
    }
};

ConfigIsolation g_config_isolation;

struct ComApartment {
    HRESULT hr = E_FAIL;
    bool owns = false;

    ComApartment() {
        hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        // S_FALSE means the apartment already existed on this thread; either way it
        // is ours to balance with CoUninitialize.
        owns = SUCCEEDED(hr);
    }
    ~ComApartment() {
        if (owns) CoUninitialize();
    }
};

ComApartment g_com;

}  // namespace

TEST_CASE("COM apartment is available to the test process") {
    // Every shell test depends on this; failing here explains the cascade instead of
    // leaving a dozen unrelated-looking HRESULT failures.
    CHECK(SUCCEEDED(g_com.hr));
}
