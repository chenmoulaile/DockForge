// Catch2 entry point (main() comes from catch_amalgamated.cpp).
//
// The shell tests call IShellLinkW, IShellItemImageFactory and WIC, all of which need
// an initialized COM apartment on the calling thread. Catch2 runs every test on the
// main thread, so one process-wide apartment is enough - and initializing it here
// keeps the individual test cases free of setup noise.
#include "catch_amalgamated.hpp"

#include <windows.h>
#include <objbase.h>

namespace {

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
