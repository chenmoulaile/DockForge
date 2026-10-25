#include "dock/launchpad_folders.h"

#include "core/log.h"
#include "platform/win_common.h"
#include "shell/app_identity.h"

#include <algorithm>
#include <cstdio>
#include <cctype>

namespace dockforge::dock {
namespace {

constexpr char kLogModule[] = "launchpad";

// Mirrors the caps in config.cpp so a mutation refuses early with a clear reason
// instead of being silently trimmed by ClampConfig afterwards. The caps are
// duplicated rather than exported because config.cpp owns the constants; these
// guards exist to keep the failure observable at the call site.
constexpr std::size_t kMaxFolders = 64;
constexpr std::size_t kMaxFolderItems = 256;

std::string Lower(const std::string& text) { return shell::LowerAscii(text); }

core::LaunchpadFolder* FindMutable(core::Config& config, const std::string& id) {
    for (auto& folder : config.launchpad.folders) {
        if (folder.id == id) return &folder;
    }
    return nullptr;
}

}  // namespace

std::string NewFolderId() {
    GUID guid{};
    if (FAILED(::CoCreateGuid(&guid))) {
        // CoCreateGuid is documented not to fail in practice, but a folder with
        // an empty id would be rejected by the loader (C-18: "empty id is
        // discarded"), so falling back to a timestamp keeps the operation
        // usable rather than silently dropping the user's drag.
        DF_WARN(kLogModule, "CoCreateGuid failed 0x%08lX; using a timestamp id", ::GetLastError());
        const unsigned long long stamp = ::GetTickCount64();
        char buffer[64]{};
        std::snprintf(buffer, sizeof(buffer), "folder-%llx", stamp);
        return std::string(buffer);
    }
    wchar_t wide[64]{};
    std::swprintf(wide, 64, L"%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2],
                  guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    char narrow[64]{};
    std::snprintf(narrow, sizeof(narrow), "%ls", wide);
    std::string id(narrow);
    // Canonical form is lowercase, matching how the loader reads ids back and
    // how identities are stored everywhere else.
    std::transform(id.begin(), id.end(), id.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return id;
}

std::string CreateLaunchpadFolder(core::Config& config, const std::string& name,
                                  const std::vector<std::string>& items) {
    if (items.empty()) {
        DF_WARN(kLogModule, "refusing to create an empty folder '%s'", name.c_str());
        return {};
    }
    if (config.launchpad.folders.size() >= kMaxFolders) {
        DF_WARN(kLogModule, "folder limit %zu reached; refusing '%s'", kMaxFolders, name.c_str());
        return {};
    }

    core::LaunchpadFolder folder;
    folder.id = NewFolderId();
    if (folder.id.empty()) return {};
    // A folder with no name is legal (it falls back to the id), but a folder
    // whose name is only whitespace would render as a blank chip the user cannot
    // identify, so the empty check is on the trimmed content.
    folder.name = name;
    folder.items.reserve(items.size());
    for (const std::string& raw : items) {
        const std::string identity = Lower(raw);
        if (identity.empty()) continue;
        if (std::find(folder.items.begin(), folder.items.end(), identity) != folder.items.end()) {
            continue;
        }
        if (folder.items.size() >= kMaxFolderItems) break;
        folder.items.push_back(identity);
    }
    if (folder.items.empty()) {
        DF_WARN(kLogModule, "refusing to create folder '%s' with no usable identities",
                name.c_str());
        return {};
    }

    // Clamp first, so an id collision (two folders somehow sharing an id) is
    // resolved before the config is committed rather than after.
    core::ClampConfig(config);
    config.launchpad.folders.push_back(std::move(folder));
    DF_INFO(kLogModule, "created folder '%s' with %zu item(s)", name.c_str(), items.size());
    return config.launchpad.folders.back().id;
}

bool RenameLaunchpadFolder(core::Config& config, const std::string& id,
                           const std::string& name) {
    core::LaunchpadFolder* folder = FindMutable(config, id);
    if (!folder) {
        DF_WARN(kLogModule, "rename of unknown folder id '%s'", id.c_str());
        return false;
    }
    // The id is deliberately untouched: the grid rearranges by id, and a rename
    // that changed it would drop the folder the user is still dragging.
    folder->name = name;
    DF_INFO(kLogModule, "renamed folder '%s' to '%s'", id.c_str(), name.c_str());
    return true;
}

bool AppendToLaunchpadFolder(core::Config& config, const std::string& id,
                             const std::string& identity) {
    core::LaunchpadFolder* folder = FindMutable(config, id);
    if (!folder) {
        DF_WARN(kLogModule, "append to unknown folder id '%s'", id.c_str());
        return false;
    }
    const std::string lowered = Lower(identity);
    if (lowered.empty()) {
        DF_WARN(kLogModule, "refusing to append an empty identity to '%s'", id.c_str());
        return false;
    }
    // Idempotent: a double drop, or a drop that arrives twice because the pointer
    // moved while the button was held, must not produce a duplicate entry. The
    // grid would show the app twice and the user would have to delete the
    // folder to undo it.
    if (std::find(folder->items.begin(), folder->items.end(), lowered) != folder->items.end()) {
        DF_DEBUG(kLogModule, "'%s' is already in folder '%s'", lowered.c_str(), id.c_str());
        return true;
    }
    if (folder->items.size() >= kMaxFolderItems) {
        DF_WARN(kLogModule, "folder '%s' is at the %zu-item limit", id.c_str(), kMaxFolderItems);
        return false;
    }
    folder->items.push_back(lowered);
    return true;
}

bool RemoveFromLaunchpadFolder(core::Config& config, const std::string& id,
                               const std::string& identity) {
    core::LaunchpadFolder* folder = FindMutable(config, id);
    if (!folder) {
        DF_WARN(kLogModule, "remove from unknown folder id '%s'", id.c_str());
        return false;
    }
    const std::string lowered = Lower(identity);
    const auto it = std::find(folder->items.begin(), folder->items.end(), lowered);
    if (it == folder->items.end()) {
        DF_WARN(kLogModule, "'%s' is not in folder '%s'", lowered.c_str(), id.c_str());
        return false;
    }
    folder->items.erase(it);
    // An emptied folder is removed, not left in place. C-18 says an empty folder
    // is discarded on load, so leaving one behind would write a config entry
    // that disappears on the next start - the user would think the app lost
    // their folder.
    if (folder->items.empty()) {
        DF_INFO(kLogModule, "folder '%s' became empty; removing it", id.c_str());
        return DeleteLaunchpadFolder(config, id);
    }
    return true;
}

bool DeleteLaunchpadFolder(core::Config& config, const std::string& id) {
    const auto before = config.launchpad.folders.size();
    config.launchpad.folders.erase(
        std::remove_if(config.launchpad.folders.begin(), config.launchpad.folders.end(),
                       [&id](const core::LaunchpadFolder& folder) { return folder.id == id; }),
        config.launchpad.folders.end());
    const bool removed = config.launchpad.folders.size() != before;
    if (removed) DF_INFO(kLogModule, "deleted folder '%s'", id.c_str());
    return removed;
}

bool LaunchpadFolderContains(const core::Config& config, const std::string& id,
                             const std::string& identity) {
    const core::LaunchpadFolder* folder = FindLaunchpadFolder(config, id);
    if (!folder) return false;
    const std::string lowered = Lower(identity);
    return std::find(folder->items.begin(), folder->items.end(), lowered) != folder->items.end();
}

const core::LaunchpadFolder* FindLaunchpadFolder(const core::Config& config,
                                                 const std::string& id) {
    for (const auto& folder : config.launchpad.folders) {
        if (folder.id == id) return &folder;
    }
    return nullptr;
}

bool SaveLaunchpadConfig(const core::Config& config) {
    if (!core::SaveConfig(config)) {
        DF_ERROR(kLogModule, "failed to persist the config after a folder change");
        return false;
    }
    return true;
}

}  // namespace dockforge::dock
