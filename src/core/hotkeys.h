// DockForge - hotkeys and screen hotspots (T-069, CONTRACTS.md C-24).
//
// Two engines live here, both driven by the same C-24 config lists:
//
//   HotkeyHost     owns RegisterHotKey/UnregisterHotKey for the enabled
//                  hotkeys. MOD_NOREPEAT is OR'ed in at the one real call
//                  site (C-24 rule); a failed registration reports
//                  GetLastError, with 1409 classified as "taken by another
//                  program" so the settings window can say so. The destructor
//                  unregisters everything - the no-residue exit guarantee.
//
//   HotspotTracker the GetCursorPos + <=10Hz poll side of C-24. Pure geometry
//                  and a fire-once-per-entry state machine, unit-testable
//                  without a mouse.
//
// Both dispatch through the same action table (the four DoD categories), and
// both are OFF until the user turns them on in the settings window.
#pragma once

#include "core/config.h"

#include <windows.h>

#include <string>
#include <vector>

namespace dockforge::core {

// ---- Actions (DoD: 四类热键) -------------------------------------------
// The action table is the closed vocabulary a hotkey/hotspot `action` field
// may name. Four categories, one Chinese label each for the DoD mapping.
enum class HotkeyCategory {
    ComponentToggle,      // 组件显隐
    SystemControl,        // 系统控制
    DisplayMode,          // 显示模式
    WindowManagement,     // 窗口管理
};

struct ActionInfo {
    const char* name;
    HotkeyCategory category;
};

// The shipped vocabulary. Keep in sync with RunShortcutAction in main.cpp and
// with the labels in settings_window.cpp.
constexpr ActionInfo kActions[] = {
    {"toggle_dock", HotkeyCategory::ComponentToggle},
    {"toggle_launchpad", HotkeyCategory::ComponentToggle},
    {"toggle_settings", HotkeyCategory::ComponentToggle},
    {"toggle_taskbar", HotkeyCategory::SystemControl},
    {"restore_taskbar", HotkeyCategory::SystemControl},
    {"cycle_dock_side", HotkeyCategory::DisplayMode},
    {"toggle_magnify", HotkeyCategory::DisplayMode},
    {"minimize_all", HotkeyCategory::WindowManagement},
    {"minimize_foreground", HotkeyCategory::WindowManagement},
};

// Returns nullptr for an unknown action name.
const ActionInfo* FindAction(const std::string& name);
int ActionCount();

// ---- Default catalog (settings checkbox <-> C-24 entry) -----------------
// A fresh config.json has no hotkeys/hotspots. The settings window still needs
// something to offer, so it lists this catalog; flipping a checkbox on writes
// the matching entry into config.hotkeys/hotspots with these defaults (still
// editable by hand afterwards - the catalog only seeds).
struct HotkeyCatalogEntry {
    const char* id;
    int modifiers;  // C-24 bits only, MOD_NOREPEAT added at registration
    int vk;
    const char* action;
};

struct HotspotCatalogEntry {
    const char* id;
    HotspotEdge edge;
    HotspotCorner corner;
    int size_px;
    const char* action;
};

// Combos avoid the two built-ins: Ctrl+Alt+Space (Launchpad, T-041) and
// Ctrl+Alt+T (emergency taskbar restore, T-061) are registered unconditionally
// by main.cpp and must not be shadowed by a catalog default.
constexpr HotkeyCatalogEntry kHotkeyCatalog[] = {
    {"toggle_dock", MOD_CONTROL | MOD_ALT, 'D', "toggle_dock"},
    {"toggle_launchpad", MOD_CONTROL | MOD_ALT, 'L', "toggle_launchpad"},
    {"toggle_taskbar", MOD_CONTROL | MOD_ALT, 'B', "toggle_taskbar"},
    {"cycle_dock_side", MOD_CONTROL | MOD_ALT, 'P', "cycle_dock_side"},
    {"toggle_magnify", MOD_CONTROL | MOD_ALT, 'G', "toggle_magnify"},
    {"minimize_all", MOD_CONTROL | MOD_ALT, 'M', "minimize_all"},
};

constexpr HotspotCatalogEntry kHotspotCatalog[] = {
    {"corner_tl", HotspotEdge::Top, HotspotCorner::TopLeft, 4, "toggle_launchpad"},
    {"corner_br", HotspotEdge::Bottom, HotspotCorner::BottomRight, 4, "minimize_all"},
    {"edge_top", HotspotEdge::Top, HotspotCorner::None, 2, "toggle_dock"},
};

const HotkeyCatalogEntry* FindHotkeyCatalogEntry(const std::string& id);
const HotspotCatalogEntry* FindHotspotCatalogEntry(const std::string& id);

// ---- HotkeyHost ----------------------------------------------------------
// Registers the enabled hotkeys of one config snapshot on one HWND.
//
// The Win32 calls go through injectable function pointers so the bookkeeping
// (which entries registered, how failures classify, the unregister-on-swap
// sweep) is unit-testable without grabbing real global hotkeys from a test
// runner - a test binary that registered Ctrl+Alt+D machine-wide would
// pollute every parallel gate run.
class HotkeyHost {
  public:
    using RegisterFn = BOOL(WINAPI*)(HWND, int, UINT, UINT);
    using UnregisterFn = BOOL(WINAPI*)(HWND, int);

    // WM_HOTKEY wParam ids handed out by Apply: 0x4800 + index ('H' page;
    // the built-ins use 0x4C50 'LP' and 0x5442 'TB', so there is no overlap).
    static constexpr int kIdBase = 0x4800;
    static constexpr int kMaxHotkeys = 64;

    struct Result {
        std::string id;
        bool ok = false;
        DWORD error = 0;              // GetLastError() when registration failed
        bool already_registered = false;  // error == 1409 (C-24 wording)
    };

    explicit HotkeyHost(HWND window, RegisterFn register_fn = &RegisterHotKey,
                        UnregisterFn unregister_fn = &UnregisterHotKey);
    ~HotkeyHost();  // UnregisterAll() - no system-level residue (DoD)

    HotkeyHost(const HotkeyHost&) = delete;
    HotkeyHost& operator=(const HotkeyHost&) = delete;

    // Full swap: unregisters everything currently held, then registers every
    // ENABLED entry of `hotkeys` (disabled entries are skipped silently).
    // MOD_NOREPEAT is OR'ed into the modifiers of every call. In-config
    // duplicates (same masked modifiers + vk) are detected up front: only the
    // first is sent to Win32, later ones come back as already_registered
    // errors instead of colliding with our own registration.
    std::vector<Result> Apply(const std::vector<HotkeyConfig>& hotkeys);

    // Unregisters every held id. Safe to call repeatedly.
    void UnregisterAll();

    // Maps a WM_HOTKEY wParam back to the config entry that produced it, or
    // nullptr when the id is not ours (the built-ins keep their own arms).
    const HotkeyConfig* FindByWParam(WPARAM wp) const;

    int ActiveCount() const { return static_cast<int>(active_.size()); }
    bool Empty() const { return active_.empty(); }

  private:
    struct Registration {
        int wp_id = 0;
        HotkeyConfig config;
    };

    HWND window_ = nullptr;
    RegisterFn register_ = nullptr;
    UnregisterFn unregister_ = nullptr;
    std::vector<Registration> active_;
};

// ---- Hotspots --------------------------------------------------------------
// Trigger rectangle for one hotspot on one monitor rect (virtual-screen
// coordinates). Corner set -> corner square; corner None -> whole edge strip.
RECT HotspotRect(const HotspotConfig& hotspot, const RECT& monitor);

// True when the C-24 config lists differ (used to skip no-op re-applies).
bool SameHotkeys(const std::vector<HotkeyConfig>& a, const std::vector<HotkeyConfig>& b);
bool SameHotspots(const std::vector<HotspotConfig>& a, const std::vector<HotspotConfig>& b);

// Fire-once-per-entry state machine over the enabled hotspots.
//
// Poll() must be called at most every kPollIntervalMs (the caller throttles
// with ShouldPoll). It returns the index (into `hotspots`) of the hotspot
// ENTERED on this poll, or -1. Staying inside fires nothing; every hotspot
// re-arms only after the cursor has left ALL of them - otherwise a cursor
// resting in a 2px strip would machine-gun the action at 10Hz.
class HotspotTracker {
  public:
    static constexpr double kPollIntervalMs = 100.0;  // 10Hz, the C-24 ceiling

    // Accumulates frame deltas; true at most once per kPollIntervalMs.
    bool ShouldPoll(double delta_ms);

    // See class comment. Only ENABLED hotspots participate.
    int Poll(POINT cursor, const std::vector<HotspotConfig>& hotspots,
             const RECT& monitor);

    // Index of the hotspot the cursor is currently inside (-1 = none).
    int ActiveIndex() const { return active_index_; }
    void Reset();  // forget the armed state (config swap)

  private:
    double since_poll_ms_ = 0.0;
    int active_index_ = -1;
};

// ---- Import / export (C-24) ----------------------------------------------
// The config file itself is the full export; these move just the two C-24
// arrays so a user can copy shortcuts between machines. The merge key is the
// stable `id`: an imported id replaces the local entry in place, an unknown id
// appends, local ids the import does not mention are kept.

// {"hotkeys":[...],"hotspots":[...]} - exactly the C-24 shapes SerializeConfig
// writes for the same config, so an export round-trips through ParseConfig.
std::string ExportShortcutsJson(const Config& config);

// Parses `json_text` and merges it into `config`. Validation is stricter than
// ParseConfig: an entry naming an action outside kActions rejects the WHOLE
// import (better a refused copy than a shortcut that silently does nothing),
// as does malformed JSON. Returns false and fills `error` on rejection;
// `config` is untouched then.
bool ImportShortcutsJson(Config& config, const std::string& json_text,
                         std::string* error);

// Minimize every minimize-able top-level window (action `minimize_all`).
// Skips: invisible windows, WS_EX_TOOLWINDOW, cloaked (UWP ghost) windows,
// the taskbar/tray, and `skip` (the dock itself). Returns how many were
// minimized; `detail` names what went wrong, not what succeeded.
int MinimizeAllWindows(HWND skip, std::string* detail);

}  // namespace dockforge::core
