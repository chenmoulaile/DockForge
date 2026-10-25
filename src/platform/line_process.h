// DockForge - one child process spoken to over pipes, one UTF-8 line per message.
//
// Extracted from the T-068 media bridge (src/finderbar/media_sidecar.cpp) when
// T-076 needed the same thing for the notification sidecar. Two copies of the
// CreatePipe/SetHandleInformation/PeekNamedPipe dance would be two places for
// the same three bugs (inherited handles leaking, blocking reads on a live
// pipe, a half-line delivered as a full line), so the transport lives here and
// the bridges keep only their protocol.
//
// Lifecycle contract: Start() spawns with CREATE_NO_WINDOW and both std handles
// redirected; Stop() closes stdin first so a well-behaved child exits on EOF,
// waits 2s, then terminates. No child outlives its owner.
#pragma once

#include "platform/win_common.h"

#include <string>
#include <vector>

namespace dockforge::platform {

class LineProcess {
  public:
    LineProcess() = default;
    ~LineProcess();

    LineProcess(const LineProcess&) = delete;
    LineProcess& operator=(const LineProcess&) = delete;

    // Spawns `exe_path`. Returns false when the file is missing or
    // CreateProcessW failed - both are logged by the caller's module, never
    // silently turned into a working-looking process.
    bool Start(const std::wstring& exe_path);

    void Stop();

    bool running() const { return process_ != nullptr; }

    // Appends "\n" and flushes. False once the child is gone.
    bool SendLine(const std::string& line);

    // Non-blocking: every complete line currently buffered in the pipe. A
    // trailing partial line stays in the buffer for the next call.
    std::vector<std::string> DrainLines();

    // True once the child has exited (or was never started).
    bool ChildDied();

    // Process id of the running child, 0 otherwise. For log lines only.
    unsigned long pid() const { return pid_; }

  private:
    HANDLE out_read_ = nullptr;   // child stdout, our read end
    HANDLE in_write_ = nullptr;   // child stdin, our write end
    HANDLE process_ = nullptr;
    unsigned long pid_ = 0;
    std::string buffer_;
};

// "C:\dir\app.exe" -> "C:\dir\app.exe"; "" -> "<exe dir>\<fallback_name>".
// Returns an empty string when the module path cannot be read.
std::wstring SiblingExecutablePath(const std::wstring& explicit_path,
                                   const wchar_t* fallback_name);

}  // namespace dockforge::platform
