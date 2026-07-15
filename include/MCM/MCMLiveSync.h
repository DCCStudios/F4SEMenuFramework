#pragma once

#include <string>

// Pushes hotkey rebinds made in our translated MCM pages into the *running*
// native MCM (mcm.dll), so the change takes effect without a game reload.
//
// How: MCM's DLL registers a "root.mcm" code object on the pause-menu movie
// (Interface/MainMenu.swf) whose functions — SetKeybind / RemapKeybind /
// ClearKeybind — call directly into its live KeybindManager and set its dirty
// flag (so MCM itself re-persists Keybinds.json on the next game save). We
// invoke those functions through the pause menu's GFx movie.
//
// Constraint: root.mcm only exists while the PauseMenu movie is loaded, and
// our overlay dismisses the pause menu. So pushes are queued and flushed the
// next time the pause menu is open (the user pressing ESC once is enough).
// The Keybinds.json write done by MCMKeybindStore remains the fallback that
// applies at the next save load if the push never gets a chance to run.
namespace MCMLiveSync {

    enum class Status {
        None,     // no rebind pushed for this control this session
        Pending,  // queued, waiting for the pause menu to open
        Synced,   // delivered to the running MCM — active now
        Failed,   // push attempted but rejected (falls back to load-time apply)
    };

    // Queue a rebind (or unbind when a_vkKeycode == 0) for delivery into the
    // running MCM. Keycode is a Windows VK code — the convention MCM's
    // KeybindManager uses internally and in Keybinds.json.
    void QueuePush(const std::string& a_modName, const std::string& a_keybindId,
                   unsigned int a_vkKeycode, int a_modifiers);

    // Request the reverse sync: read every translated MCM keybind's current
    // binding from the *running* MCM (root.mcm.GetKeybind) and import changed
    // ones into HotkeyManager, so rebinds made in the native MCM this session
    // show up in our pages. Runs at the same pause-menu-open opportunity as
    // pushes; keybinds with a pending push are skipped (the user's newest
    // rebind in OUR menu wins). Called when the overlay opens.
    void RequestPull();

    // Called once per rendered frame (Present hook). Cheap when idle: only
    // when pushes are pending does it check for an open pause menu and
    // schedule a flush on the F4SE UI task thread.
    void OnFrame();

    // Sync state for one control, for UI feedback next to the hotkey button.
    Status GetStatus(const std::string& a_modName, const std::string& a_keybindId);

}  // namespace MCMLiveSync
