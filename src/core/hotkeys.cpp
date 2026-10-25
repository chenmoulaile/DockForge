// DockForge - hotkeys and screen hotspots, implementation (T-069, C-24).
#include "core/hotkeys.h"

#include "core/log.h"

#include <dwmapi.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <unordered_set>

namespace dockforge::core {
namespace {

using json = nlohmann::json;

constexpr char kLogModule[] = "hotkeys";

// The C-24 modifier bits actually stored in config. Everything else (including
// a stray MOD_NOREPEAT a user copy-pasted into the file) is masked off here;
// MOD_NOREPEAT is added only at the RegisterHotKey call site.
constexpr int kModifierMask = MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;  // 0xF

constexpr DWORD kHotkeyAlreadyRegistered = 1409;  // ERROR_HOTKEY_ALREADY_REGISTERED

// An id is [a-z0-9_.-]{1,64} (C-24). Used by both the catalog lookups and the
// import validator.
bool ValidId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// Key that decides whether two hotkey entries collide on the physical keyboard.
int ComboKey(int masked_modifiers, int vk) { return (masked_modifiers << 8) | (vk & 0xFF); }

bool InRect(const RECT& rect, POINT pt) {
    return pt.x >= rect.left && pt.x < rect.right && pt.y >= rect.top && pt.y < rect.bottom;
}

struct EnumMinimizeCtx {
    HWND skip = nullptr;
    int minimized = 0;
    std::string detail;
};

BOOL CALLBACK MinimizeEnumProc(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<EnumMinimizeCtx*>(lparam);
    if (hwnd == ctx->skip) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;

    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) != 0 || (ex & WS_EX_NOACTIVATE) != 0) return TRUE;
    // Cloaked windows (suspended UWP ghosts) would "minimize" a window that
    // is not on any desktop; DWMWA_CLOAKED says so without guessing.
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        cloaked != 0) {
        return TRUE;
    }
    // Ownerless top-level windows only: anything with an owner is a dialog or
    // a tooltip of something that will be minimized anyway.
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

    // Skip the shell's own chrome (taskbar, tray, start menu).
    wchar_t cls[64]{};
    if (GetClassNameW(hwnd, cls, 64) > 0) {
        if (_wcsicmp(cls, L"Shell_TrayWnd") == 0 || _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
            _wcsicmp(cls, L"Progman") == 0 || _wcsicmp(cls, L"WorkerW") == 0 ||
            _wcsicmp(cls, L"Windows.UI.Composition.DesktopWindowContentBridge") == 0) {
            return TRUE;
        }
    }

    ShowWindow(hwnd, SW_MINIMIZE);
    ++ctx->minimized;
    return TRUE;
}

}  // namespace

// ---- Actions ---------------------------------------------------------------

const ActionInfo* FindAction(const std::string& name) {
    for (const ActionInfo& info : kActions) {
        if (name == info.name) return &info;
    }
    return nullptr;
}

int ActionCount() { return static_cast<int>(std::size(kActions)); }

// ---- Catalog -----------------------------------------------------------------

const HotkeyCatalogEntry* FindHotkeyCatalogEntry(const std::string& id) {
    for (const HotkeyCatalogEntry& entry : kHotkeyCatalog) {
        if (id == entry.id) return &entry;
    }
    return nullptr;
}

const HotspotCatalogEntry* FindHotspotCatalogEntry(const std::string& id) {
    for (const HotspotCatalogEntry& entry : kHotspotCatalog) {
        if (id == entry.id) return &entry;
    }
    return nullptr;
}

// ---- HotkeyHost ---------------------------------------------------------------

HotkeyHost::HotkeyHost(HWND window, RegisterFn register_fn, UnregisterFn unregister_fn)
    : window_(window), register_(register_fn), unregister_(unregister_fn) {
    if (register_ == nullptr) register_ = &RegisterHotKey;
    if (unregister_ == nullptr) unregister_ = &UnregisterHotKey;
}

HotkeyHost::~HotkeyHost() { UnregisterAll(); }

std::vector<HotkeyHost::Result> HotkeyHost::Apply(const std::vector<HotkeyConfig>& hotkeys) {
    UnregisterAll();

    std::vector<Result> results;
    std::unordered_set<int> seen_combos;
    int next_id = kIdBase;

    for (const HotkeyConfig& entry : hotkeys) {
        if (!entry.enabled) continue;
        if (static_cast<int>(results.size()) >= kMaxHotkeys) {
            DF_WARN(kLogModule, "more than %d enabled hotkeys, extra entries skipped",
                    kMaxHotkeys);
            break;
        }

        Result result;
        result.id = entry.id;

        const int masked = entry.modifiers & kModifierMask;
        if (!ValidId(entry.id) || masked == 0 || entry.vk < 1 || entry.vk > 254 ||
            FindAction(entry.action) == nullptr) {
            // Invalid entries never reach Win32; they report as failures with a
            // distinctive error rather than being dropped silently.
            result.ok = false;
            result.error = ERROR_INVALID_PARAMETER;
            DF_WARN(kLogModule, "hotkey '%s' invalid (mods=%d vk=%d action=%s), not registered",
                    entry.id.c_str(), entry.modifiers, entry.vk, entry.action.c_str());
            results.push_back(std::move(result));
            continue;
        }

        const int combo = ComboKey(masked, entry.vk);
        if (!seen_combos.insert(combo).second) {
            // Our own config lists the combination twice: the second copy would
            // fail with 1409 against OUR first registration, which is real but
            // misleading in the settings UI. Report it without touching Win32.
            result.ok = false;
            result.error = kHotkeyAlreadyRegistered;
            result.already_registered = true;
            DF_WARN(kLogModule,
                    "hotkey '%s' repeats an earlier combination in the same config, skipped",
                    entry.id.c_str());
            results.push_back(std::move(result));
            continue;
        }

        // The single real call site: C-24 requires MOD_NOREPEAT on every
        // registration so a held key does not machine-gun the action.
        SetLastError(0);
        const BOOL ok = register_(window_, next_id,
                                  static_cast<UINT>(masked) | MOD_NOREPEAT,
                                  static_cast<UINT>(entry.vk));
        const DWORD err = GetLastError();
        result.ok = ok != FALSE;
        result.error = err;
        result.already_registered = !result.ok && err == kHotkeyAlreadyRegistered;
        if (result.ok) {
            active_.push_back(Registration{next_id, entry});
            ++next_id;
        } else if (result.already_registered) {
            DF_WARN(kLogModule, "hotkey '%s' (%s) 已被其他程序占用 (err=%lu)",
                    entry.id.c_str(), entry.action.c_str(), err);
        } else {
            DF_WARN(kLogModule, "hotkey '%s' RegisterHotKey failed err=%lu", entry.id.c_str(),
                    err);
        }
        results.push_back(std::move(result));
    }
    DF_INFO(kLogModule, "applied %zu hotkey entries, %d registered, %zu failed",
            hotkeys.size(), ActiveCount(), results.size() - static_cast<size_t>(ActiveCount()));
    return results;
}

void HotkeyHost::UnregisterAll() {
    for (const Registration& reg : active_) {
        if (!unregister_(window_, reg.wp_id)) {
            // Logged, not fatal: the id was either already released (window
            // destroyed) or the system dropped it; there is nothing to retry.
            DF_DEBUG(kLogModule, "UnregisterHotKey id=0x%X err=%lu", reg.wp_id, GetLastError());
        }
    }
    active_.clear();
}

const HotkeyConfig* HotkeyHost::FindByWParam(WPARAM wp) const {
    for (const Registration& reg : active_) {
        if (static_cast<WPARAM>(reg.wp_id) == wp) return &reg.config;
    }
    return nullptr;
}

// ---- Hotspots ---------------------------------------------------------------

RECT HotspotRect(const HotspotConfig& hotspot, const RECT& monitor) {
    const int size = std::clamp(hotspot.size_px, HotspotSizeRange().min,
                                HotspotSizeRange().max);
    RECT rect{};

    switch (hotspot.corner) {
        case HotspotCorner::TopLeft:
            rect = {monitor.left, monitor.top, monitor.left + size, monitor.top + size};
            return rect;
        case HotspotCorner::TopRight:
            rect = {monitor.right - size, monitor.top, monitor.right, monitor.top + size};
            return rect;
        case HotspotCorner::BottomLeft:
            rect = {monitor.left, monitor.bottom - size, monitor.left + size, monitor.bottom};
            return rect;
        case HotspotCorner::BottomRight:
            rect = {monitor.right - size, monitor.bottom - size, monitor.right,
                    monitor.bottom};
            return rect;
        case HotspotCorner::None:
            break;
    }

    // Whole-edge strip. Corner authority (hotkeys.h) means we only get here
    // when corner == None.
    switch (hotspot.edge) {
        case HotspotEdge::Top:
            return {monitor.left, monitor.top, monitor.right, monitor.top + size};
        case HotspotEdge::Bottom:
            return {monitor.left, monitor.bottom - size, monitor.right, monitor.bottom};
        case HotspotEdge::Left:
            return {monitor.left, monitor.top, monitor.left + size, monitor.bottom};
        case HotspotEdge::Right:
            return {monitor.right - size, monitor.top, monitor.right, monitor.bottom};
    }
    return rect;
}

bool SameHotkeys(const std::vector<HotkeyConfig>& a, const std::vector<HotkeyConfig>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].modifiers != b[i].modifiers || a[i].vk != b[i].vk ||
            a[i].action != b[i].action || a[i].enabled != b[i].enabled) {
            return false;
        }
    }
    return true;
}

bool SameHotspots(const std::vector<HotspotConfig>& a, const std::vector<HotspotConfig>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].edge != b[i].edge || a[i].corner != b[i].corner ||
            a[i].size_px != b[i].size_px || a[i].action != b[i].action ||
            a[i].enabled != b[i].enabled) {
            return false;
        }
    }
    return true;
}

bool HotspotTracker::ShouldPoll(double delta_ms) {
    since_poll_ms_ += delta_ms;
    if (since_poll_ms_ < kPollIntervalMs) return false;
    since_poll_ms_ = 0.0;
    return true;
}

int HotspotTracker::Poll(POINT cursor, const std::vector<HotspotConfig>& hotspots,
                         const RECT& monitor) {
    // Which hotspot (if any) the cursor is inside right now.
    int inside = -1;
    for (std::size_t i = 0; i < hotspots.size(); ++i) {
        if (!hotspots[i].enabled) continue;
        if (InRect(HotspotRect(hotspots[i], monitor), cursor)) {
            inside = static_cast<int>(i);
            break;  // first match wins; overlapping zones resolve by list order
        }
    }

    const int previous = active_index_;
    active_index_ = inside;
    // Fire only on the leave->enter transition of a (possibly different) zone:
    // staying inside, or moving between two overlapping zones, fires nothing.
    if (inside >= 0 && previous < 0) return inside;
    return -1;
}

void HotspotTracker::Reset() { active_index_ = -1; }

// ---- Import / export --------------------------------------------------------

std::string ExportShortcutsJson(const Config& config) {
    json document = json::object();
    json hotkeys = json::array();
    for (const HotkeyConfig& entry : config.hotkeys) {
        hotkeys.push_back(json{
            {"id", entry.id},
            {"modifiers", entry.modifiers & kModifierMask},
            {"vk", entry.vk},
            {"action", entry.action},
            {"enabled", entry.enabled},
        });
    }
    json hotspots = json::array();
    for (const HotspotConfig& entry : config.hotspots) {
        json item = json{
            {"edge", ToString(entry.edge)},
            {"size_px", entry.size_px},
            {"action", entry.action},
            {"enabled", entry.enabled},
        };
        item["id"] = entry.id;
        item["corner"] = entry.corner == HotspotCorner::None ? json() : json(ToString(entry.corner));
        hotspots.push_back(std::move(item));
    }
    document["hotkeys"] = std::move(hotkeys);
    document["hotspots"] = std::move(hotspots);
    return document.dump(2);
}

bool ImportShortcutsJson(Config& config, const std::string& json_text, std::string* error) {
    json document;
    try {
        document = json::parse(json_text);
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    }
    if (!document.is_object()) {
        if (error) *error = "root is not a JSON object";
        return false;
    }

    // Parse and validate BOTH lists before touching `config`: an import that
    // half-applies would leave the config armed with shortcuts whose actions
    // were never checked.
    std::vector<HotkeyConfig> hotkeys;
    if (document.contains("hotkeys")) {
        const auto& array = document.at("hotkeys");
        if (!array.is_array()) {
            if (error) *error = "hotkeys is not an array";
            return false;
        }
        for (const auto& item : array) {
            if (!item.is_object()) {
                if (error) *error = "hotkeys entry is not an object";
                return false;
            }
            HotkeyConfig entry;
            if (!item.contains("id") || !item.at("id").is_string()) {
                if (error) *error = "hotkeys entry has no string id";
                return false;
            }
            entry.id = item.at("id").get<std::string>();
            if (!ValidId(entry.id)) {
                if (error) *error = "hotkey id '" + entry.id + "' is not [a-z0-9_.-]{1,64}";
                return false;
            }
            if (item.contains("modifiers") && item.at("modifiers").is_number()) {
                entry.modifiers =
                    static_cast<int>(item.at("modifiers").get<double>()) & kModifierMask;
            }
            if (item.contains("vk") && item.at("vk").is_number()) {
                entry.vk = std::clamp(static_cast<int>(item.at("vk").get<double>()), 1, 254);
            }
            if (item.contains("enabled") && item.at("enabled").is_boolean()) {
                entry.enabled = item.at("enabled").get<bool>();
            }
            if (!item.contains("action") || !item.at("action").is_string()) {
                if (error) *error = "hotkey '" + entry.id + "' has no string action";
                return false;
            }
            entry.action = item.at("action").get<std::string>();
            if (FindAction(entry.action) == nullptr) {
                // Stricter than ParseConfig on purpose: an import naming an
                // unknown action is a copy from a different version, and
                // silently keeping it would read as "the shortcut does
                // nothing" rather than "the import was refused".
                if (error) *error = "hotkey '" + entry.id + "' names unknown action '" +
                                           entry.action + "'";
                return false;
            }
            hotkeys.push_back(std::move(entry));
        }
    }

    std::vector<HotspotConfig> hotspots;
    if (document.contains("hotspots")) {
        const auto& array = document.at("hotspots");
        if (!array.is_array()) {
            if (error) *error = "hotspots is not an array";
            return false;
        }
        for (const auto& item : array) {
            if (!item.is_object()) {
                if (error) *error = "hotspots entry is not an object";
                return false;
            }
            HotspotConfig entry;
            if (!item.contains("id") || !item.at("id").is_string()) {
                if (error) *error = "hotspots entry has no string id";
                return false;
            }
            entry.id = item.at("id").get<std::string>();
            if (!ValidId(entry.id)) {
                if (error) *error = "hotspot id '" + entry.id + "' is not [a-z0-9_.-]{1,64}";
                return false;
            }
            if (!item.contains("edge") || !item.at("edge").is_string() ||
                !ParseHotspotEdge(item.at("edge").get<std::string>(), entry.edge)) {
                if (error) *error = "hotspot '" + entry.id + "' has no valid edge";
                return false;
            }
            if (item.contains("corner")) {
                const auto& corner = item.at("corner");
                if (corner.is_string() &&
                    !ParseHotspotCorner(corner.get<std::string>(), entry.corner)) {
                    if (error) *error = "hotspot '" + entry.id + "' has an invalid corner";
                    return false;
                }
            }
            if (item.contains("size_px") && item.at("size_px").is_number()) {
                entry.size_px = std::clamp(static_cast<int>(item.at("size_px").get<double>()),
                                           HotspotSizeRange().min, HotspotSizeRange().max);
            }
            if (item.contains("enabled") && item.at("enabled").is_boolean()) {
                entry.enabled = item.at("enabled").get<bool>();
            }
            if (!item.contains("action") || !item.at("action").is_string()) {
                if (error) *error = "hotspot '" + entry.id + "' has no string action";
                return false;
            }
            entry.action = item.at("action").get<std::string>();
            if (FindAction(entry.action) == nullptr) {
                if (error) *error = "hotspot '" + entry.id + "' names unknown action '" +
                                           entry.action + "'";
                return false;
            }
            hotspots.push_back(std::move(entry));
        }
    }

    // Merge by id: imported replaces in place, unknown appends, unmentioned
    // locals survive (the stable-id rule C-24 exists for exactly this).
    for (const HotkeyConfig& imported : hotkeys) {
        const auto it = std::find_if(config.hotkeys.begin(), config.hotkeys.end(),
                                     [&](const HotkeyConfig& existing) {
                                         return existing.id == imported.id;
                                     });
        if (it != config.hotkeys.end()) {
            *it = imported;
        } else {
            config.hotkeys.push_back(imported);
        }
    }
    for (const HotspotConfig& imported : hotspots) {
        const auto it = std::find_if(config.hotspots.begin(), config.hotspots.end(),
                                     [&](const HotspotConfig& existing) {
                                         return existing.id == imported.id;
                                     });
        if (it != config.hotspots.end()) {
            *it = imported;
        } else {
            config.hotspots.push_back(imported);
        }
    }
    DF_INFO(kLogModule, "imported %zu hotkey(s), %zu hotspot(s) by id", hotkeys.size(),
            hotspots.size());
    return true;
}

int MinimizeAllWindows(HWND skip, std::string* detail) {
    EnumMinimizeCtx ctx;
    ctx.skip = skip;
    EnumWindows(&MinimizeEnumProc, reinterpret_cast<LPARAM>(&ctx));
    if (detail) *detail = ctx.detail;
    DF_INFO(kLogModule, "minimize_all minimized %d window(s)", ctx.minimized);
    return ctx.minimized;
}

}  // namespace dockforge::core
