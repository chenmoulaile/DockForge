#include "dock/tile_order.h"

#include "core/log.h"
#include "core/paths.h"

#include <algorithm>
#include <cwctype>
#include <unordered_map>
#include <unordered_set>

namespace dockforge::dock {
namespace {

std::string LowerAscii(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return text;
}

}  // namespace

std::string IdentityOf(const shell::AppEntry& app) {
    // AUMID first: for a packaged app it is the only stable identity - every
    // packaged window is hosted by ApplicationFrameHost.exe, so the path is shared
    // and would make two apps look identical (C-13b).
    if (!app.aumid.empty()) return LowerAscii(core::WideToUtf8(app.aumid));
    if (!app.namespace_parsing.empty()) {
        return LowerAscii(core::WideToUtf8(app.namespace_parsing));
    }
    if (!app.target_path.empty()) return LowerAscii(core::WideToUtf8(app.target_path));
    // Last resort: the display name. Two different apps could share one, which is
    // why it is last - but a tile with no identity at all could never be reordered
    // or hidden, and would come back after every restart.
    return LowerAscii(core::WideToUtf8(app.display_name));
}

std::vector<shell::AppEntry> ApplyOrder(const std::vector<shell::AppEntry>& apps,
                                        const std::vector<std::string>& order,
                                        const std::vector<std::string>& hidden,
                                        int max_tiles) {
    std::vector<shell::AppEntry> result;
    if (max_tiles <= 0) return result;
    result.reserve(static_cast<std::size_t>(std::min(max_tiles, 64)));

    std::unordered_set<std::string> hidden_set;
    hidden_set.reserve(hidden.size() * 2);
    for (const auto& identity : hidden) hidden_set.insert(LowerAscii(identity));

    // Index the scan by identity once. A map would be wrong here: two entries can
    // legitimately share an identity (the same .exe registered twice with different
    // arguments), and dropping the duplicate would silently lose a tile.
    std::unordered_multimap<std::string, std::size_t> by_identity;
    by_identity.reserve(apps.size() * 2);
    for (std::size_t i = 0; i < apps.size(); ++i) {
        const std::string identity = IdentityOf(apps[i]);
        if (hidden_set.count(identity) != 0) continue;
        by_identity.emplace(identity, i);
    }

    std::unordered_set<std::size_t> consumed;
    consumed.reserve(order.size() * 2);
    for (const auto& raw : order) {
        if (static_cast<int>(result.size()) >= max_tiles) break;
        const std::string identity = LowerAscii(raw);
        const auto range = by_identity.equal_range(identity);
        for (auto it = range.first; it != range.second; ++it) {
            if (consumed.count(it->second) != 0) continue;
            consumed.insert(it->second);
            result.push_back(apps[it->second]);
            break;
        }
        // A name with no matching entry is skipped, not an error: uninstalled apps
        // leave stale names behind, and refusing to start over one would be worse
        // than forgetting it.
    }

    // Curated docks do not auto-fill. Appending the unmentioned tail here would make
    // removal look broken - the user drags a tile off, and the next app in the scan
    // takes its place in the same frame. Only an unmanaged dock (no order yet)
    // populates itself, which is what makes the first run usable.
    if (!order.empty()) {
        DF_DEBUG("dock", "tile order: %zu named, %zu resolved of %zu scanned", order.size(),
                 result.size(), apps.size());
        return result;
    }

    for (std::size_t i = 0; i < apps.size(); ++i) {
        if (static_cast<int>(result.size()) >= max_tiles) break;
        if (consumed.count(i) != 0) continue;
        if (hidden_set.count(IdentityOf(apps[i])) != 0) continue;
        result.push_back(apps[i]);
    }

    return result;
}

void MoveTile(std::vector<DockTile>& tiles, int from, int to) {
    const int count = static_cast<int>(tiles.size());
    if (from < 0 || from >= count || to < 0 || to >= count || from == to) return;
    // Extracting and reinserting rather than swapping: a drag from slot 1 to slot 4
    // shifts three tiles left, it does not exchange two of them. A swap would leave
    // the dock in an order the user never drew.
    DockTile moved = std::move(tiles[static_cast<std::size_t>(from)]);
    tiles.erase(tiles.begin() + from);
    tiles.insert(tiles.begin() + to, std::move(moved));
}

bool RemoveTile(std::vector<DockTile>& tiles, int index) {
    if (index < 0 || index >= static_cast<int>(tiles.size())) return false;
    tiles.erase(tiles.begin() + index);
    return true;
}

void CommitOrder(core::Config& config, const std::vector<DockTile>& tiles,
                 const std::string& removed_identity) {
    config.dock.tiles.clear();
    config.dock.tiles.reserve(tiles.size());
    for (const auto& tile : tiles) {
        config.dock.tiles.push_back(IdentityOf(tile.app));
    }

    if (!removed_identity.empty()) {
        const std::string identity = LowerAscii(removed_identity);
        const bool known =
            std::find(config.dock.hidden.begin(), config.dock.hidden.end(), identity) !=
            config.dock.hidden.end();
        if (!known) config.dock.hidden.push_back(identity);
    }

    // The hidden list can outlive the apps it names (uninstall, reinstall). Pruning
    // is deliberately NOT done here: an app that is temporarily missing from the
    // scan - a removable drive, a failed Start-menu enumeration - would lose its
    // removal and reappear. Stale entries cost a few bytes and are filtered on use.
    if (!core::SaveConfig(config)) {
        DF_ERROR("dock", "failed to persist tile order (%zu tiles, %zu hidden)",
                 config.dock.tiles.size(), config.dock.hidden.size());
        return;
    }
    DF_INFO("dock", "tile order saved: %zu tiles, %zu hidden", config.dock.tiles.size(),
            config.dock.hidden.size());
}

}  // namespace dockforge::dock
