// DockForge - logon-trigger autostart via Task Scheduler (T-060).
//
// The task lives in the scheduler root folder, created through COM ITaskService
// (spike: spikes/t060_autostart - `schtasks /create /sc onlogon` is denied in a
// non-elevated process, the COM path is not). The registry Run key is banned by
// the T-060 DoD.
#pragma once

#include <string>

namespace dockforge::core {

// "\DockForge" - the exact path inside the scheduler root folder. Visible in
// Task Scheduler's GUI under this name; DeleteTask takes the same string.
std::wstring AutostartTaskName();

// XML-escapes a value embedded into a task definition (Command paths, names).
// Only the five predefined entities; attributes we build never carry the raw
// characters this would double-escape.
std::string AutostartXmlEscape(const std::string& text);

// The decision layer sits over an injectable backend so the rules below are
// unit-testable without touching the real scheduler (tests use a fake; the
// COM backend is exercised for real by tools/autostart_probe.ps1):
//
//   enable  + missing -> Create with the current exe path
//   enable  + present -> Create again, so a moved binary re-registers its path
//   disable + present -> Remove
//   disable + missing -> no backend call, reported as a no-op success
//
// Returns false only when a required action failed; `*detail` (when non-null)
// gets a one-line human-readable action report either way.
struct AutostartBackend {
    virtual bool Exists() = 0;
    virtual bool Create(const std::wstring& exe_path) = 0;
    virtual bool Remove() = 0;
    virtual ~AutostartBackend() = default;
};

bool ApplyAutostart(bool enable, AutostartBackend& backend,
                    const std::wstring& exe_path, std::string* detail);

// The real backend. Each method balances its own CoInitializeEx/CoUninitialize
// so it can be called from any apartment state the caller happens to be in.
class TaskSchedulerAutostart : public AutostartBackend {
  public:
    bool Exists() override;
    bool Create(const std::wstring& exe_path) override;
    bool Remove() override;
};

// One-shot helpers over TaskSchedulerAutostart, for callers that never hold a
// backend instance (main wiring, the probe exe).
bool ApplyAutostartSystem(bool enable, std::string* detail);
bool AutostartTaskExists();

}  // namespace dockforge::core
