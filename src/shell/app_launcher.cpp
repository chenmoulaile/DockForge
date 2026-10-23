// DockForge - start an AppEntry (CONTRACTS.md C-08).
#include "shell/app_launcher.h"

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <vector>

#include "core/log.h"
#include "core/paths.h"

using Microsoft::WRL::ComPtr;

namespace dockforge::shell {
namespace {

// CreateProcessW wants a mutable command line, and the arguments come from a .lnk
// verbatim (they may contain quotes the user typed). Building one buffer and
// passing it through avoids both the const_cast and the quoting surprises of
// ShellExecuteEx's lpParameters.
std::wstring BuildCommandLine(const std::wstring& exe, const std::wstring& arguments) {
    std::wstring command = L"\"" + exe + L"\"";
    if (!arguments.empty()) {
        command += L' ';
        command += arguments;
    }
    return command;
}

LaunchResult LaunchByPath(const AppEntry& entry) {
    LaunchResult result;
    result.route = "CreateProcess";
    if (entry.target_path.empty()) {
        result.error = LaunchError::NotLaunchable;
        return result;
    }
    if (GetFileAttributesW(entry.target_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        result.error = LaunchError::NotFound;
        DF_DEBUG("shell", "launch: %s is gone", core::WideToUtf8(entry.target_path).c_str());
        return result;
    }

    std::wstring command = BuildCommandLine(entry.target_path, entry.arguments);
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    const wchar_t* workdir = entry.working_dir.empty() ? nullptr : entry.working_dir.c_str();

    // CREATE_UNICODE_ENVIRONMENT is implied by passing nullptr; the child inherits
    // our environment block, which is what a launcher should do.
    const BOOL ok = CreateProcessW(entry.target_path.c_str(), buffer.data(), nullptr, nullptr,
                                   FALSE, 0, nullptr, workdir, &startup, &info);
    if (!ok) {
        const DWORD last = GetLastError();
        DF_WARN("shell", "CreateProcess(%s) failed: %lu",
                core::WideToUtf8(entry.target_path).c_str(), last);
        result.error = LaunchError::ProcessFailed;
        return result;
    }

    result.process_id = info.dwProcessId;
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    result.error = LaunchError::None;
    return result;
}

LaunchResult LaunchPackaged(const AppEntry& entry) {
    LaunchResult result;
    result.route = "ActivateApplication";
    if (entry.aumid.empty()) {
        result.error = LaunchError::NotLaunchable;
        return result;
    }

    ComPtr<IApplicationActivationManager> manager;
    HRESULT hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr,
                                  CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager));
    if (FAILED(hr)) {
        DF_WARN("shell", "CoCreateInstance(ApplicationActivationManager): 0x%08lX",
                static_cast<unsigned long>(hr));
        result.error = LaunchError::ActivationFailed;
        return result;
    }

    DWORD pid = 0;
    hr = manager->ActivateApplication(entry.aumid.c_str(), nullptr, AO_NONE, &pid);
    if (FAILED(hr)) {
        DF_WARN("shell", "ActivateApplication(%s): 0x%08lX",
                core::WideToUtf8(entry.aumid).c_str(), static_cast<unsigned long>(hr));
        result.error = LaunchError::ActivationFailed;
        return result;
    }

    result.process_id = pid;
    result.error = LaunchError::None;
    return result;
}

LaunchResult LaunchShellNamespace(const AppEntry& entry) {
    LaunchResult result;
    result.route = "ShellExecuteEx";
    const std::wstring identity =
        !entry.namespace_parsing.empty() ? entry.namespace_parsing : entry.aumid;
    if (identity.empty()) {
        result.error = LaunchError::NotLaunchable;
        return result;
    }

    // SEE_MASK_NOASYNC keeps the shell from posting work to a message loop we may not
    // be pumping at that instant; SEE_MASK_NOCLOSEPROCESS is deliberately NOT set,
    // because the Dock does not want a process handle it has to close on every click.
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    info.lpVerb = L"open";
    const std::wstring target = ShellNamespacePath(identity);
    info.lpFile = target.c_str();
    info.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&info)) {
        const DWORD last = GetLastError();
        DF_WARN("shell", "ShellExecuteEx(%s) failed: %lu", core::WideToUtf8(target).c_str(), last);
        result.error = LaunchError::ActivationFailed;
        return result;
    }

    result.error = LaunchError::None;
    return result;
}

}  // namespace

const char* ToString(LaunchError error) {
    switch (error) {
        case LaunchError::None: return "none";
        case LaunchError::NotLaunchable: return "not-launchable";
        case LaunchError::NotFound: return "not-found";
        case LaunchError::ActivationFailed: return "activation-failed";
        case LaunchError::ProcessFailed: return "process-failed";
    }
    return "unknown";
}

// A folder is opened through the shell's default verb, not CreateProcess: the
// latter fails with ACCESS_DENIED on a directory. ShellExecuteEx with an empty verb
// runs the default action, which is exactly what double-clicking the folder does.
LaunchResult LaunchFolder(const AppEntry& entry) {
    LaunchResult result;
    result.route = "ShellExecuteEx";
    if (entry.target_path.empty()) {
        result.error = LaunchError::NotLaunchable;
        return result;
    }
    if (GetFileAttributesW(entry.target_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        result.error = LaunchError::NotFound;
        return result;
    }

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    info.lpVerb = nullptr;
    info.lpFile = entry.target_path.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        const DWORD last = GetLastError();
        DF_WARN("shell", "ShellExecuteEx(%s) failed: %lu",
                core::WideToUtf8(entry.target_path).c_str(), last);
        result.error = LaunchError::ProcessFailed;
        return result;
    }
    result.error = LaunchError::None;
    return result;
}
LaunchResult Launch(const AppEntry& entry) {
    switch (entry.kind) {
        case AppKind::Uwp:
            return LaunchPackaged(entry);
        case AppKind::ShellItem:
        case AppKind::NamespaceItem:
            // Both have no filesystem path; the shell resolves them by name.
            return LaunchShellNamespace(entry);
        case AppKind::Shortcut:
        case AppKind::Executable:
            return LaunchByPath(entry);
        case AppKind::Folder:
            return LaunchFolder(entry);
        case AppKind::Unknown:
            break;
    }
    LaunchResult result;
    result.error = LaunchError::NotLaunchable;
    return result;
}

}  // namespace dockforge::shell
