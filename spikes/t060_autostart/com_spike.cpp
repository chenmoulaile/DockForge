// T-060 spike: is COM ITaskService usable from MinGW-w64 for a logon-trigger
// autostart task, in a NON-ELEVATED process?
//
// The schtasks.exe /sc onlogon path was measured first (see findings.md): every
// variant returns "Access is denied" (rc=1) in a non-elevated shell. This
// program exercises the COM path with the same trigger so the spike compares
// both paths against reality, not assumptions.
//
// Steps are printed with raw HRESULTs. At the end it registers the task and,
// if that succeeded, deletes it again so the machine is left clean. Run:
//   g++ -std=c++20 -O0 -o com_spike.exe com_spike.cpp -lole32 -loleaut32 -luuid -ltaskschd
#include <windows.h>
#include <taskschd.h>
#include <objbase.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {

const char* HrName(HRESULT hr) {
    switch (hr) {
        case S_OK: return "S_OK";
        case E_ACCESSDENIED: return "E_ACCESSDENIED";
        case E_INVALIDARG: return "E_INVALIDARG";
        case E_NOTIMPL: return "E_NOTIMPL";
        case HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS): return "ALREADY_EXISTS";
        default: return nullptr;
    }
}

void Report(const char* step, HRESULT hr) {
    if (const char* known = HrName(hr)) {
        std::printf("  %-34s hr=0x%08lX (%s)\n", step, static_cast<unsigned long>(hr), known);
    } else {
        std::printf("  %-34s hr=0x%08lX\n", step, static_cast<unsigned long>(hr));
    }
}

}  // namespace

int main(int argc, char** argv) {
    // `keep` skips the cleanup delete so an external schtasks /query /xml can
    // export the registered definition (then a schtasks /create /xml round trip
    // can be measured against it).
    const bool keep = argc > 1 && std::strcmp(argv[1], "keep") == 0;
    std::printf("== COM ITaskService spike (T-060) ==\n");

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        Report("CoInitializeEx", hr);
        return 1;
    }
    ITaskService* service = nullptr;
    hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ITaskService, reinterpret_cast<void**>(&service));
    Report("CoCreateInstance(CLSID_TaskScheduler)", hr);
    if (FAILED(hr)) {
        CoUninitialize();
        return 1;
    }

    VARIANT no_server{}, no_domain{}, no_user{}, no_pass{};
    no_server.vt = VT_EMPTY;
    no_domain.vt = VT_EMPTY;
    no_user.vt = VT_EMPTY;
    no_pass.vt = VT_EMPTY;
    hr = service->Connect(no_server, no_domain, no_user, no_pass);
    Report("Connect(local, current user)", hr);

    ITaskDefinition* task = nullptr;
    if (SUCCEEDED(hr)) hr = service->NewTask(0, &task);
    Report("NewTask(0)", hr);

    if (SUCCEEDED(hr)) {
        IRegistrationInfo* info = nullptr;
        if (SUCCEEDED(task->get_RegistrationInfo(&info)) && info) {
            info->put_Author(BSTR(L"DockForge spike"));
            info->Release();
        }
        ITriggerCollection* triggers = nullptr;
        if (SUCCEEDED(task->get_Triggers(&triggers)) && triggers) {
            ITrigger* trigger = nullptr;
            if (SUCCEEDED(triggers->Create(TASK_TRIGGER_LOGON, &trigger)) && trigger) {
                ILogonTrigger* logon = nullptr;
                if (SUCCEEDED(trigger->QueryInterface(IID_ILogonTrigger,
                                                      reinterpret_cast<void**>(&logon))) &&
                    logon) {
                    wchar_t user[128]{};
                    DWORD n = ARRAYSIZE(user);
                    GetUserNameW(user, &n);
                    logon->put_UserId(BSTR(user));  // this user's logon only
                    std::printf("  trigger=LogonTrigger user=%ls\n", user);
                    logon->Release();
                }
                trigger->Release();
            }
            triggers->Release();
        }
        IActionCollection* actions = nullptr;
        if (SUCCEEDED(task->get_Actions(&actions)) && actions) {
            IAction* action = nullptr;
            if (SUCCEEDED(actions->Create(TASK_ACTION_EXEC, &action)) && action) {
                IExecAction* exec = nullptr;
                if (SUCCEEDED(action->QueryInterface(IID_IExecAction,
                                                     reinterpret_cast<void**>(&exec))) &&
                    exec) {
                    exec->put_Path(BSTR(L"C:\\Windows\\System32\\notepad.exe"));
                    exec->Release();
                }
                action->Release();
            }
            actions->Release();
        }
    }

    const wchar_t* kPath = L"\\DockForge_Spike_T060_COM";
    ITaskFolder* root = nullptr;
    if (SUCCEEDED(hr)) {
        hr = service->GetFolder(BSTR(L"\\"), &root);
        Report("GetFolder(root)", hr);
    }
    if (SUCCEEDED(hr)) {
        VARIANT user{};
        user.vt = VT_BSTR;
        wchar_t buf[128]{};
        DWORD n = ARRAYSIZE(buf);
        GetUserNameW(buf, &n);
        user.bstrVal = SysAllocString(buf);
        VARIANT empty{};
        empty.vt = VT_EMPTY;
        hr = root->RegisterTaskDefinition(BSTR(kPath), task, TASK_CREATE_OR_UPDATE,
                                          user, empty, TASK_LOGON_INTERACTIVE_TOKEN,
                                          empty, nullptr);
        VariantClear(&user);
        Report("RegisterTaskDefinition(root, logon)", hr);
    }

    const bool registered = SUCCEEDED(hr);

    if (registered) {
        IRegisteredTask* check = nullptr;
        HRESULT qhr = root->GetTask(BSTR(kPath), &check);
        Report("OpenTask(verify)", qhr);
        if (SUCCEEDED(qhr) && check) {
            BSTR xml = nullptr;
            check->get_Xml(&xml);
            std::printf("  xml head: %.80ls\n", xml ? xml : L"(none)");
            if (xml) SysFreeString(xml);
            check->Release();
        }
    }

    if (task) task->Release();
    if (root) {
        HRESULT dhr = S_OK;
        if (keep) {
            std::printf("  keep: leaving task registered for external export\n");
        } else {
            dhr = root->DeleteTask(BSTR(kPath), 0);
            Report("DeleteTask(cleanup)", dhr);
        }
        root->Release();
    }

    CoUninitialize();
    std::printf("== result: %s ==\n", registered ? "COM PATH WORKS" : "COM PATH DENIED");
    return registered ? 0 : 2;
}
