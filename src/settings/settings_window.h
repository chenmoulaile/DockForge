// DockForge - settings window (T-050, CONTRACTS.md C-20, ADR-0021 stack).
//
// Owns one top-level HWND with its own window class and WndProc. It cannot reuse
// platform::Window for the same measured reason as the preview popup and the
// Launchpad overlay: that class shares one window class whose WndProc posts
// WM_QUIT on WM_DESTROY, which would end the dock's message loop the first time
// the user closed the settings window.
//
// Layout follows the T-050 skeleton: a self-drawn left navigation (D2D on the
// nav child's own redirection surface) with the sections Dock / Launchpad /
// Appearance / Behavior / About, and a right pane of native Win32 controls that
// bind to C-01 fields through settings::SettingsModel. The native controls are
// deliberate: a settings surface is opaque and text-heavy, and real EDIT /
// COMBOBOX / BUTTON controls give keyboard focus, IME and accessibility for
// free. DComp stays reserved for the transparent surfaces (dock, Launchpad) per
// ADR-0021; the nav pane is D2D, which is the stack the ADR actually pins.
//
// Every edit - typed or probe-injected - goes through the same 300ms debounce
// (C-20), then one flush applies the model, saves the config, and publishes
// ConfigChanged with the full snapshot. Closing the window never quits the
// process: WM_CLOSE destroys only this window.
#pragma once

#include "core/config_events.h"
#include "core/event_bus.h"
#include "settings/settings_model.h"

#include <string>

#include "platform/win_common.h"

namespace dockforge::settings {

class SettingsWindow {
  public:
    SettingsWindow() = default;
    ~SettingsWindow();

    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    static bool RegisterWindowClass(HINSTANCE instance);

    // Creates the window hidden. `initial` seeds the controls; the window keeps
    // its own working copy, which is what every flush mutates and saves. When
    // the config changes underneath the window (hot reload, T-051), the owner
    // must call ReloadConfig - the window deliberately does not subscribe
    // itself, so the dependency direction stays owner -> window.
    bool Create(HINSTANCE instance, core::EventBus& bus, const core::Config& initial);

    void Destroy();
    bool IsValid() const { return hwnd_ != nullptr; }
    void Show();
    void Hide();

    // Records an edit exactly as if the user had typed it. Shared by the native
    // controls' EN_CHANGE path and the probe channel, so a probe PASS proves the
    // chain the keyboard takes.
    void RecordEdit(const std::string& field, const std::string& text);

    // Switches the visible section ("dock", "launchpad", "appearance",
    // "behavior", "about"). Unknown names are ignored.
    void ShowSection(const std::string& name);

    // Re-seeds the controls from `config` without publishing anything.
    void ReloadConfig(const core::Config& config);

  private:
    static LRESULT CALLBACK NavWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    void CreateControls();
    void DestroyControls();
    void ApplySectionVisibility();
    void EchoValues(const std::vector<std::string>& fields);
    void FlushPending();
    void PaintNav();
    void RestartDebounceTimer();
    void OpenLogsDirectory();     // T-062 About-page action button
    static void SubclassPane(HWND pane, HWND owner);  // forwards child WM_COMMAND to owner

    HWND hwnd_ = nullptr;
    HWND nav_ = nullptr;
    HINSTANCE instance_ = nullptr;
    core::EventBus* bus_ = nullptr;
    core::Config config_{};   // the working copy every flush mutates and saves
    SettingsModel model_{};
    int active_section_ = 0;
    bool updating_controls_ = false;  // suppresses EN_CHANGE while echoing
    HFONT ui_font_ = nullptr;
    int open_logs_id_ = 0;  // control id of the About-page action button (T-062)

    // One HWND per bound field, keyed by the dotted C-01 name. Static labels are
    // not in here - they carry no state.
    std::vector<std::pair<std::string, HWND>> field_controls_;
    std::vector<HWND> section_panes_;   // one container per section, 5 entries
    std::vector<HWND> all_controls_;    // everything DestroyControls must drop
};

// Theme query used by the nav painter and the probe. `DOCKFORGE_SETTINGS_THEME`
// overrides (light|dark) so evidence screenshots can pin both palettes on one
// machine without touching the user's system theme; otherwise the system
// AppsUseLightTheme registry value decides.
bool PreferLightTheme();

}  // namespace dockforge::settings
