// DockForge - logon-trigger autostart via Task Scheduler (T-060).
//
// Implementation notes inherited from the spike (spikes/t060_autostart):
//   * `schtasks /create /sc onlogon` returns "Access is denied" in a
//     non-elevated process (measured three ways); the COM path succeeds, so the
//     registration goes through ITaskService / ITaskFolder.
//   * RegisterTaskDefinition lives on ITaskFolder, not ITaskService (the MinGW
//     header has no such member on the service - the spike's first compile
//     proved it).
//   * IRegisteredTask::get_Xml (lower-case "ml") and ITaskFolder::GetTask (not
//     OpenTask) are the names in MinGW's taskschd.h.
#include "core/autostart.h"

#include "core/log.h"
#include "core/paths.h"

#include <windows.h>
#include <objbase.h>
#include <taskschd.h>

#include <memory>

namespace dockforge::core {
namespace {

constexpr char kLogModule[] = "autostart";

// S_OK means registered; FILE_NOT_FOUND / PATH_NOT_FOUND mean absent. Anything
// else is a real failure and gets logged with its HRESULT.
bool IsTaskMissing(HRESULT hr) {
    return hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
           hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
}

// RAII over SysAllocString; the spike passed BSTR(L"...") literals straight
// into calls, which leaks - a long-lived app must not.
class Bstr {
  public:
    explicit Bstr(const wchar_t* text) : raw_(SysAllocString(text)) {}
    ~Bstr() {
        if (raw_) SysFreeString(raw_);
    }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    BSTR get() const { return raw_; }

  private:
    BSTR raw_ = nullptr;
};

// Per-call apartment handling. The app thread is COM STA already (the dock's
// drop target needs it), but CoInitializeEx with the same mode is free and
// makes every method standalone; a changed-mode failure is reported, not
// swallowed.
struct ComScope {
    HRESULT hr = E_FAIL;
    bool owns = false;
    explicit ComScope() {
        hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (hr == RPC_E_CHANGED_MODE) {
            // Another apartment mode is active on this thread; COM is usable
            // all the same, but we did not add a reference.
            hr = S_OK;
        } else {
            owns = SUCCEEDED(hr);
        }
    }
    ~ComScope() {
        if (owns) CoUninitialize();
    }
};

std::wstring CurrentUser() {
    wchar_t buffer[128]{};
    DWORD size = ARRAYSIZE(buffer);
    if (!GetUserNameW(buffer, &size)) return {};
    return std::wstring(buffer, size > 0 ? size - 1 : 0);
}

// Writes the registered task's own XML (from get_Xml, true UTF-16) next to the
// config as an always-current audit copy. The repo-side audit copy under
// tools/autostart/ is derived from this by the probe.
void WriteAuditCopy(ITaskFolder* folder, const wchar_t* task_path) {
    IRegisteredTask* registered = nullptr;
    if (FAILED(folder->GetTask(Bstr(task_path).get(), &registered)) || !registered) {
        DF_WARN(kLogModule, "audit copy skipped: registered task not readable");
        return;
    }
    BSTR xml = nullptr;
    std::wstring text;
    if (SUCCEEDED(registered->get_Xml(&xml)) && xml != nullptr) {
        text.assign(xml, SysStringLen(xml));
        SysFreeString(xml);
    }
    registered->Release();
    if (text.empty()) {
        DF_WARN(kLogModule, "audit copy skipped: empty task XML");
        return;
    }

    const std::wstring dir = ConfigDir() + L"\\autostart";
    if (!EnsureDirectory(dir)) {
        DF_WARN(kLogModule, "audit copy skipped: cannot create %s",
                WideToUtf8(dir).c_str());
        return;
    }
    const std::wstring file = dir + L"\\DockForge.xml";
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        DF_WARN(kLogModule, "audit copy skipped: open failed 0x%08lX", GetLastError());
        return;
    }
    // UTF-16 with BOM, matching the XML declaration task XML always carries.
    const unsigned char bom[] = {0xFF, 0xFE};
    DWORD written = 0;
    WriteFile(handle, bom, sizeof(bom), &written, nullptr);
    WriteFile(handle, text.c_str(),
              static_cast<DWORD>(text.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(handle);
    DF_INFO(kLogModule, "audit copy written: %s", WideToUtf8(file).c_str());
}

}  // namespace

std::wstring AutostartTaskName() { return L"\\DockForge"; }

std::string AutostartXmlEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c; break;
        }
    }
    return out;
}

bool ApplyAutostart(bool enable, AutostartBackend& backend,
                    const std::wstring& exe_path, std::string* detail) {
    const bool exists = backend.Exists();
    if (!enable && !exists) {
        if (detail != nullptr) *detail = "already off (task absent)";
        return true;
    }
    const bool ok = enable ? backend.Create(exe_path) : backend.Remove();
    if (detail != nullptr) {
        *detail = enable ? (ok ? "task registered (logon trigger)" : "register FAILED")
                         : (ok ? "task deleted" : "delete FAILED");
    }
    DF_INFO(kLogModule, "apply enable=%d exists_before=%d -> %s", enable ? 1 : 0,
            exists ? 1 : 0, ok ? "ok" : "failed");
    return ok;
}

bool TaskSchedulerAutostart::Exists() {
    ComScope com;
    if (FAILED(com.hr)) {
        DF_ERROR(kLogModule, "CoInitializeEx failed 0x%08lX",
                 static_cast<unsigned long>(com.hr));
        return false;
    }
    ITaskService* service = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITaskService, reinterpret_cast<void**>(&service));
    if (FAILED(hr)) {
        DF_ERROR(kLogModule, "CoCreateInstance(CLSID_TaskScheduler) failed 0x%08lX",
                 static_cast<unsigned long>(hr));
        return false;
    }
    bool exists = false;
    VARIANT no_arg{};
    no_arg.vt = VT_EMPTY;
    if (SUCCEEDED(service->Connect(no_arg, no_arg, no_arg, no_arg))) {
        ITaskFolder* root = nullptr;
        if (SUCCEEDED(service->GetFolder(Bstr(L"\\").get(), &root)) && root) {
            IRegisteredTask* task = nullptr;
            hr = root->GetTask(Bstr(AutostartTaskName().c_str()).get(), &task);
            if (SUCCEEDED(hr) && task) {
                task->Release();
                exists = true;
            } else if (!IsTaskMissing(hr)) {
                DF_WARN(kLogModule, "GetTask failed 0x%08lX", static_cast<unsigned long>(hr));
            }
            root->Release();
        }
    }
    service->Release();
    return exists;
}

bool TaskSchedulerAutostart::Create(const std::wstring& exe_path) {
    ComScope com;
    if (FAILED(com.hr)) {
        DF_ERROR(kLogModule, "CoInitializeEx failed 0x%08lX",
                 static_cast<unsigned long>(com.hr));
        return false;
    }
    ITaskService* service = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITaskService, reinterpret_cast<void**>(&service));
    if (FAILED(hr)) {
        DF_ERROR(kLogModule, "CoCreateInstance(CLSID_TaskScheduler) failed 0x%08lX",
                 static_cast<unsigned long>(hr));
        return false;
    }

    bool created = false;
    ITaskFolder* root = nullptr;
    ITaskDefinition* task = nullptr;
    VARIANT no_arg{};
    no_arg.vt = VT_EMPTY;
    VARIANT user{};
    user.vt = VT_EMPTY;

    do {
        hr = service->Connect(no_arg, no_arg, no_arg, no_arg);
        if (FAILED(hr)) {
            DF_ERROR(kLogModule, "Connect failed 0x%08lX", static_cast<unsigned long>(hr));
            break;
        }
        hr = service->NewTask(0, &task);
        if (FAILED(hr)) {
            DF_ERROR(kLogModule, "NewTask failed 0x%08lX", static_cast<unsigned long>(hr));
            break;
        }

        // Registration metadata: who wrote the task. Harmless on failure, so
        // these setters' HRESULTs are not fatal.
        IRegistrationInfo* info = nullptr;
        if (SUCCEEDED(task->get_RegistrationInfo(&info)) && info) {
            info->put_Author(Bstr(L"DockForge").get());
            info->put_Description(
                Bstr(L"Start DockForge at logon (configurable in settings)").get());
            info->Release();
        }

        // The logon trigger, scoped to the registering user. A user-less
        // LogonTrigger fires for every user's logon, which would start the app
        // for people who never enabled it.
        ITriggerCollection* triggers = nullptr;
        ITrigger* trigger = nullptr;
        if (FAILED(task->get_Triggers(&triggers)) || !triggers) {
            DF_ERROR(kLogModule, "get_Triggers failed");
            break;
        }
        hr = triggers->Create(TASK_TRIGGER_LOGON, &trigger);
        triggers->Release();
        if (FAILED(hr) || !trigger) {
            DF_ERROR(kLogModule, "LogonTrigger create failed 0x%08lX",
                     static_cast<unsigned long>(hr));
            break;
        }
        ILogonTrigger* logon = nullptr;
        if (FAILED(trigger->QueryInterface(IID_ILogonTrigger,
                                           reinterpret_cast<void**>(&logon))) ||
            !logon) {
            DF_ERROR(kLogModule, "ILogonTrigger QI failed");
            trigger->Release();
            break;
        }
        const std::wstring user_name = CurrentUser();
        if (!user_name.empty()) {
            logon->put_UserId(Bstr(user_name.c_str()).get());
        }
        logon->Release();
        trigger->Release();

        // The exec action: this binary, no arguments.
        IActionCollection* actions = nullptr;
        IAction* action = nullptr;
        if (FAILED(task->get_Actions(&actions)) || !actions) {
            DF_ERROR(kLogModule, "get_Actions failed");
            break;
        }
        hr = actions->Create(TASK_ACTION_EXEC, &action);
        actions->Release();
        if (FAILED(hr) || !action) {
            DF_ERROR(kLogModule, "ExecAction create failed 0x%08lX",
                     static_cast<unsigned long>(hr));
            break;
        }
        IExecAction* exec = nullptr;
        if (FAILED(action->QueryInterface(IID_IExecAction,
                                          reinterpret_cast<void**>(&exec))) ||
            !exec) {
            DF_ERROR(kLogModule, "IExecAction QI failed");
            action->Release();
            break;
        }
        exec->put_Path(Bstr(exe_path.c_str()).get());
        exec->Release();
        action->Release();

        hr = service->GetFolder(Bstr(L"\\").get(), &root);
        if (FAILED(hr) || !root) {
            DF_ERROR(kLogModule, "GetFolder(root) failed 0x%08lX",
                     static_cast<unsigned long>(hr));
            break;
        }

        // TASK_CREATE_OR_UPDATE re-registers over a stale task (moved binary);
        // TASK_LOGON_INTERACTIVE_TOKEN + LeastPrivilege (the default run level)
        // is what makes non-elevated registration legal (spike XML evidence).
        // The VARIANT borrows the string; ownership stays with the Bstr below,
        // which lives until after RegisterTaskDefinition returns.
        Bstr user_bstr(CurrentUser().c_str());
        user.vt = VT_BSTR;
        user.bstrVal = user_bstr.get();
        VARIANT empty{};
        empty.vt = VT_EMPTY;
        hr = root->RegisterTaskDefinition(
            Bstr(AutostartTaskName().c_str()).get(), task, TASK_CREATE_OR_UPDATE, user,
            empty, TASK_LOGON_INTERACTIVE_TOKEN, empty, nullptr);
        user.bstrVal = nullptr;  // borrowed; Bstr owns the allocation
        if (FAILED(hr)) {
            DF_ERROR(kLogModule, "RegisterTaskDefinition failed 0x%08lX",
                     static_cast<unsigned long>(hr));
            break;
        }
        WriteAuditCopy(root, AutostartTaskName().c_str());
        created = true;
    } while (false);

    if (task) task->Release();
    if (root) root->Release();
    service->Release();
    return created;
}

bool TaskSchedulerAutostart::Remove() {
    ComScope com;
    if (FAILED(com.hr)) return false;
    ITaskService* service = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITaskService, reinterpret_cast<void**>(&service));
    if (FAILED(hr)) {
        DF_ERROR(kLogModule, "CoCreateInstance(CLSID_TaskScheduler) failed 0x%08lX",
                 static_cast<unsigned long>(hr));
        return false;
    }
    bool removed = false;
    VARIANT no_arg{};
    no_arg.vt = VT_EMPTY;
    if (SUCCEEDED(service->Connect(no_arg, no_arg, no_arg, no_arg))) {
        ITaskFolder* root = nullptr;
        if (SUCCEEDED(service->GetFolder(Bstr(L"\\").get(), &root)) && root) {
            hr = root->DeleteTask(Bstr(AutostartTaskName().c_str()).get(), 0);
            if (SUCCEEDED(hr)) {
                removed = true;
            } else if (IsTaskMissing(hr)) {
                // Someone deleted it out from under the app; the requested end
                // state is reached either way.
                removed = true;
                DF_INFO(kLogModule, "task already absent at delete");
            } else {
                DF_ERROR(kLogModule, "DeleteTask failed 0x%08lX",
                         static_cast<unsigned long>(hr));
            }
            root->Release();
        }
    }
    service->Release();
    return removed;
}

bool ApplyAutostartSystem(bool enable, std::string* detail) {
    TaskSchedulerAutostart backend;
    wchar_t exe[MAX_PATH]{};
    const DWORD written = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    if (written == 0 || written >= ARRAYSIZE(exe)) {
        if (detail != nullptr) *detail = "GetModuleFileNameW failed";
        DF_ERROR(kLogModule, "cannot resolve own executable path");
        return false;
    }
    return ApplyAutostart(enable, backend, std::wstring(exe, written), detail);
}

bool AutostartTaskExists() {
    TaskSchedulerAutostart backend;
    return backend.Exists();
}

}  // namespace dockforge::core
