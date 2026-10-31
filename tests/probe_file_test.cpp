// Regression cover for the 2026-10-26 taskbar incident.
//
// A probe run inherited DOCKFORGE_SETTINGS_CMD_FILE from the previous day. That
// file still held `set behavior.hide_taskbar 1`. Every probe channel dedups
// against "the last command I saw", and that memory started empty, so the stale
// line was executed as if freshly written - and the operator's taskbar
// disappeared. ProbeCommandGate::Prime is the fix, and these cases are what
// keeps it fixed.
#include "catch_amalgamated.hpp"

#include <string>

#include "core/probe_file.h"

using dockforge::core::ProbeCommandGate;
using dockforge::core::ProbeCommandOf;
using dockforge::core::ReadProbeFileContent;

TEST_CASE("probe gate: a primed file's existing command is inert", "[probe]") {
    ProbeCommandGate gate;
    gate.Prime("set behavior.hide_taskbar 1");

    // This is the exact incident: the same bytes the app started up with must not
    // execute on the first poll.
    CHECK_FALSE(gate.Accept("set behavior.hide_taskbar 1"));

    // A genuinely new command still gets through.
    CHECK(gate.Accept("set behavior.hide_taskbar 0"));
}

TEST_CASE("probe gate: priming an absent or empty file changes nothing",
          "[probe]") {
    ProbeCommandGate gate;
    gate.Prime("");
    CHECK(gate.Accept("show"));
    CHECK_FALSE(gate.Accept("show"));
    CHECK(gate.Accept("hide"));
}

TEST_CASE("probe gate: repeated identical content is delivered once", "[probe]") {
    ProbeCommandGate gate;
    gate.Prime("");
    CHECK(gate.Accept("state"));
    CHECK_FALSE(gate.Accept("state"));
    CHECK_FALSE(gate.Accept("state"));
    // The probe scripts defeat this on purpose by appending a unique token line,
    // which makes the whole content differ.
    CHECK(gate.Accept("state\n#12345"));
    CHECK(gate.Accept("state\n#12346"));
}

TEST_CASE("probe gate: empty content is never a command", "[probe]") {
    ProbeCommandGate gate;
    gate.Prime("");
    CHECK_FALSE(gate.Accept(""));
    CHECK(gate.Accept("show"));
}

TEST_CASE("probe gate: priming records the content it swallowed", "[probe]") {
    ProbeCommandGate gate;
    gate.Prime("set behavior.hide_taskbar 1");
    CHECK(gate.last_content() == "set behavior.hide_taskbar 1");
}

TEST_CASE("probe command: the token line is not part of the command", "[probe]") {
    CHECK(ProbeCommandOf("push Qoder|title|body\n#98765") == "push Qoder|title|body");
    CHECK(ProbeCommandOf("state\n#1") == "state");
    CHECK(ProbeCommandOf("show") == "show");
    // A trailing carriage return survives the file trim (it is inside the line)
    // and has to come off the command too.
    CHECK(ProbeCommandOf("state\r\n#1") == "state");
    CHECK(ProbeCommandOf("") == "");
}

TEST_CASE("probe command: bodies containing pipes and spaces survive", "[probe]") {
    // The push payload is app|title|body, and a title with a space must not be
    // truncated at the first space.
    const std::string line = "push Qoder|Build failed in 3 steps|see log\n#42";
    CHECK(ProbeCommandOf(line) == "push Qoder|Build failed in 3 steps|see log");
}

TEST_CASE("probe file: a missing file reads as empty", "[probe]") {
    CHECK(ReadProbeFileContent(L"").empty());
    CHECK(ReadProbeFileContent(L"Z:\\no\\such\\dockforge\\probe-file.txt").empty());
}
