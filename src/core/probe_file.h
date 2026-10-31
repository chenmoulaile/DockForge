#pragma once

#include <string>

namespace dockforge::core {

// Reads a whole probe command file, trimmed of trailing whitespace, or an empty
// string when the file is absent, unreadable or blank.
//
// Full sharing, not a CRT fopen: the writer is another process rewriting the
// file between frames, and fopen denies the concurrent write (measured on the
// T-016 hover probe: "The process cannot access the file ... because it is
// being used by another process").
std::string ReadProbeFileContent(const std::wstring& path);

// The command part of a probe file: everything before the first newline.
//
// Probe scripts append a unique token line (`push ...\n#12345`) so that two
// identical commands in a row both arrive; the token is there to defeat dedup,
// not to be parsed as part of the command.
std::string ProbeCommandOf(const std::string& content);

// Decides whether a probe file's content is a command the app has not acted on
// yet.
//
// This exists because of a real incident: on 2026-10-26 a probe run inherited
// DOCKFORGE_SETTINGS_CMD_FILE from the previous day, that file still held
// `set behavior.hide_taskbar 1`, and because the "last command seen" memory
// starts empty at launch the app executed a day-old command as if it had just
// been written. It hid the operator's taskbar.
//
// The fix is to seed the memory with whatever the file already contains before
// the first poll (Prime). Probe scripts all write their first command *after*
// the app is up, so priming cannot swallow a live command.
class ProbeCommandGate {
public:
    // Seeds the gate with content the app has implicitly already seen. Call once
    // at startup with the file's existing content.
    void Prime(const std::string& existing_content);

    // True when `content` is new and should be executed; also records it as seen.
    // Empty content is never a command.
    bool Accept(const std::string& content);

    const std::string& last_content() const { return last_content_; }

private:
    std::string last_content_;
};

}  // namespace dockforge::core
