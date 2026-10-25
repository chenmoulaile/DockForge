// T-062: pure rotation decisions (log_rotation.cpp) - file naming, name
// parsing, size roll, expiry. All filesystem-free so the decision table is
// exhaustively assertable; the Logger wiring that CONSUMES these functions is
// exercised for real by tools/log_rotation_probe.ps1.
#include "catch_amalgamated.hpp"

#include "core/log_rotation.h"

#include <string>
#include <vector>

namespace cf = dockforge::core;

// ---------------------------------------------------------------------------
// Naming: primary daily files and same-day overflow sequences.
// ---------------------------------------------------------------------------

TEST_CASE("log_rotation: sequence 0 is the plain daily name", "[log_rotation]") {
    REQUIRE(cf::LogFileNameFor(2026, 10, 25, 0) == L"dockforge-20261025.log");
    REQUIRE(cf::LogFileNameFor(2026, 1, 3, 0) == L"dockforge-20260103.log");
}

TEST_CASE("log_rotation: overflow files carry the sequence", "[log_rotation]") {
    REQUIRE(cf::LogFileNameFor(2026, 10, 25, 1) == L"dockforge-20261025-1.log");
    REQUIRE(cf::LogFileNameFor(2026, 10, 25, 12) == L"dockforge-20261025-12.log");
    REQUIRE(cf::LogFileNameFor(2026, 10, 25, 999) == L"dockforge-20261025-999.log");
}

// ---------------------------------------------------------------------------
// Parsing: exactly the two produced shapes match; nothing else ever will.
// The prune path DELETES by this predicate, so the negative space matters more
// than the positive.
// ---------------------------------------------------------------------------

TEST_CASE("log_rotation: parse accepts both produced shapes", "[log_rotation]") {
    cf::LogFileDate date{};
    REQUIRE(cf::ParseLogFileName(L"dockforge-20261025.log", date));
    CHECK(date.year == 2026);
    CHECK(date.month == 10);
    CHECK(date.day == 25);
    CHECK(date.sequence == 0);

    REQUIRE(cf::ParseLogFileName(L"dockforge-20261025-7.log", date));
    CHECK(date.year == 2026);
    CHECK(date.month == 10);
    CHECK(date.day == 25);
    CHECK(date.sequence == 7);
}

TEST_CASE("log_rotation: parse round-trips every generated name", "[log_rotation]") {
    for (int seq = 0; seq <= 40; ++seq) {
        const std::wstring name = cf::LogFileNameFor(2026, 2, 28, seq);
        cf::LogFileDate date{};
        REQUIRE(cf::ParseLogFileName(name, date));
        CHECK(date.year == 2026);
        CHECK(date.month == 2);
        CHECK(date.day == 28);
        CHECK(date.sequence == seq);
    }
}

TEST_CASE("log_rotation: parse rejects non-log names", "[log_rotation]") {
    // The logs directory also holds crash markers and anything a user dropped
    // in; none of these may ever match, because a match means deletion.
    cf::LogFileDate date{};
    CHECK_FALSE(cf::ParseLogFileName(L"crash-dump.txt", date));
    CHECK_FALSE(cf::ParseLogFileName(L"taskbar-crash.txt", date));
    CHECK_FALSE(cf::ParseLogFileName(L"dockforge-20261025.dmp", date));
    CHECK_FALSE(cf::ParseLogFileName(L"", date));
    CHECK_FALSE(cf::ParseLogFileName(L"dockforge-.log", date));
    CHECK_FALSE(cf::ParseLogFileName(L"dockforge-2026.log", date));
    CHECK_FALSE(cf::ParseLogFileName(L"dockforge-20261025", date));
    CHECK_FALSE(cf::ParseLogFileName(L"dockforge-20261025.log.bak", date));
    CHECK_FALSE(cf::ParseLogFileName(L"DockerForge-20261025.log", date));
}

TEST_CASE("log_rotation: parse rejects malformed dates and sequences", "[log_rotation]") {
    cf::LogFileDate date{};
    CHECK_FALSE(cf::ParseLogFileName(L"dockforge-20261301.log", date));  // month 13
    CHECK_FALSE(cf::ParseLogFileName(L"dockforge-20260015.log", date));  // month 00
    CHECK_FALSE(cf::ParseLogFileName(L"dockforge-20261000.log", date));  // day 00
    CHECK_FALSE(cf::ParseLogFileName(L"dockforge-20261032.log", date));  // day 32
    CHECK_FALSE(cf::ParseLogFileName(L"dockforge-20261025-0.log", date));  // seq 0 not produced
    CHECK_FALSE(cf::ParseLogFileName(L"dockforge-20261025-a.log", date));  // non-digit seq
    CHECK_FALSE(cf::ParseLogFileName(L"dockforge-2026102.log", date));   // 7 digits
    CHECK_FALSE(cf::ParseLogFileName(L"dockforge-20261025a.log", date)); // 9th non-sep char
}

// ---------------------------------------------------------------------------
// Size roll: the pure threshold rule the Logger asks before every line.
// ---------------------------------------------------------------------------

TEST_CASE("log_rotation: size roll triggers only past the cap", "[log_rotation]") {
    const uint64_t mb = 1024ull * 1024ull;
    // Exactly at the cap: 10MB + 0 new bytes fits, no roll.
    CHECK_FALSE(cf::ShouldRollBySize(10 * mb, 0, 10 * mb));
    // One byte over: roll.
    CHECK(cf::ShouldRollBySize(10 * mb, 1, 10 * mb));
    // Comfortably under: no roll.
    CHECK_FALSE(cf::ShouldRollBySize(9 * mb, 512, 10 * mb));
    // The line itself is what pushes over the edge.
    CHECK(cf::ShouldRollBySize(10 * mb - 1, 2, 10 * mb));
}

TEST_CASE("log_rotation: empty file never rolls even for a huge line", "[log_rotation]") {
    // A single line longer than the cap must LAND somewhere; rolling on an
    // empty file would loop through endless empty sequence files.
    CHECK_FALSE(cf::ShouldRollBySize(0, 10'000'000, 10 * 1024ull * 1024ull));
}

TEST_CASE("log_rotation: cap 0 disables the size roll", "[log_rotation]") {
    CHECK_FALSE(cf::ShouldRollBySize(999'999, 999, 0));
}

// ---------------------------------------------------------------------------
// Expiry: ordinal arithmetic and the keep window.
// ---------------------------------------------------------------------------

TEST_CASE("log_rotation: day ordinals behave across month and year edges", "[log_rotation]") {
    const int64_t epoch = cf::DayOrdinal(1970, 1, 1);
    CHECK(epoch == 0);
    // 2024 is a leap year: Feb 28 -> Feb 29 is one day, Feb 29 -> Mar 1 is one.
    CHECK(cf::DayOrdinal(2024, 2, 29) - cf::DayOrdinal(2024, 2, 28) == 1);
    CHECK(cf::DayOrdinal(2024, 3, 1) - cf::DayOrdinal(2024, 2, 29) == 1);
    // Non-leap 2026 has no Feb 29; Mar 1 is 1 day after Feb 28.
    CHECK(cf::DayOrdinal(2026, 3, 1) - cf::DayOrdinal(2026, 2, 28) == 1);
    // Year edge.
    CHECK(cf::DayOrdinal(2027, 1, 1) - cf::DayOrdinal(2026, 12, 31) == 1);
    // Independent references agree: 2026-10-25 is 16 days after 2026-10-09.
    CHECK(cf::DayOrdinal(2026, 10, 25) - cf::DayOrdinal(2026, 10, 9) == 16);
}

TEST_CASE("log_rotation: expiry keeps exactly the window", "[log_rotation]") {
    std::vector<std::pair<std::wstring, cf::LogFileDate>> files;
    for (int day = 20; day <= 25; ++day) {
        cf::LogFileDate date{2026, 10, day, 0};
        files.emplace_back(cf::LogFileNameFor(2026, 10, day, 0), date);
    }
    // keep 3 days ending 2026-10-25: keep 23/24/25, drop 20/21/22.
    const std::vector<std::wstring> expired =
        cf::ExpiredLogFiles(files, 2026, 10, 25, 3);
    REQUIRE(expired.size() == 3);
    CHECK(expired[0] == L"dockforge-20261020.log");
    CHECK(expired[1] == L"dockforge-20261021.log");
    CHECK(expired[2] == L"dockforge-20261022.log");
}

TEST_CASE("log_rotation: expiry never deletes today even at keep_days=1", "[log_rotation]") {
    std::vector<std::pair<std::wstring, cf::LogFileDate>> files;
    files.emplace_back(L"dockforge-20261025.log", cf::LogFileDate{2026, 10, 25, 0});
    files.emplace_back(L"dockforge-20261024.log", cf::LogFileDate{2026, 10, 24, 0});
    const std::vector<std::wstring> expired =
        cf::ExpiredLogFiles(files, 2026, 10, 25, 1);
    REQUIRE(expired.size() == 1);
    CHECK(expired[0] == L"dockforge-20261024.log");
}

TEST_CASE("log_rotation: expiry covers overflow sequences of old days too", "[log_rotation]") {
    std::vector<std::pair<std::wstring, cf::LogFileDate>> files;
    files.emplace_back(L"dockforge-20261025-2.log", cf::LogFileDate{2026, 10, 25, 2});
    files.emplace_back(L"dockforge-20261001.log", cf::LogFileDate{2026, 10, 1, 0});
    files.emplace_back(L"dockforge-20261001-4.log", cf::LogFileDate{2026, 10, 1, 4});
    const std::vector<std::wstring> expired =
        cf::ExpiredLogFiles(files, 2026, 10, 25, 14);
    REQUIRE(expired.size() == 2);
    CHECK(expired[0] == L"dockforge-20261001.log");
    CHECK(expired[1] == L"dockforge-20261001-4.log");
}

TEST_CASE("log_rotation: keep_days below 1 deletes nothing", "[log_rotation]") {
    std::vector<std::pair<std::wstring, cf::LogFileDate>> files;
    files.emplace_back(L"dockforge-20201010.log", cf::LogFileDate{2020, 10, 10, 0});
    CHECK(cf::ExpiredLogFiles(files, 2026, 10, 25, 0).empty());
    CHECK(cf::ExpiredLogFiles(files, 2026, 10, 25, -5).empty());
}

TEST_CASE("log_rotation: 14-day default window boundary is exact", "[log_rotation]") {
    // The T-062 DoD number: today 2026-10-25, keep 14 -> oldest kept is
    // 2026-10-12 (today + 13 back), 2026-10-11 goes.
    std::vector<std::pair<std::wstring, cf::LogFileDate>> files;
    files.emplace_back(L"dockforge-20261012.log", cf::LogFileDate{2026, 10, 12, 0});
    files.emplace_back(L"dockforge-20261011.log", cf::LogFileDate{2026, 10, 11, 0});
    const std::vector<std::wstring> expired =
        cf::ExpiredLogFiles(files, 2026, 10, 25, 14);
    REQUIRE(expired.size() == 1);
    CHECK(expired[0] == L"dockforge-20261011.log");
}
