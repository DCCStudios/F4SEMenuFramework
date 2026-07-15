#pragma once

#include <optional>
#include <string>
#include <vector>

// Two-way sync with the real MCM's user keybind file:
//   Data/MCM/Settings/Keybinds.json
//
// The real MCM persists user hotkey assignments there as Windows VIRTUAL-KEY
// codes ({"id": "...", "keycode": 66, "modName": "...", "modifiers": 0}),
// while the framework's HotkeyManager works in DirectInput (DIK) scan codes.
// This module:
//   * loads the file once at MCM-registry init,
//   * answers "what did the user bind this MCM hotkey to?" (translated to DIK)
//     so translated menus show the same binding the real MCM would,
//   * writes changes back (translated to VK) whenever the user rebinds an
//     MCM hotkey through the F4SE menu — keeping the file the single source
//     of truth shared by both systems.
namespace MCMKeybindStore {

    // Parse Data/MCM/Settings/Keybinds.json into memory. Safe to call when the
    // file doesn't exist (store starts empty and is created on first write).
    void Load();

    // Returns the user's saved binding for an MCM keybind as a DIK scan code,
    // or nullopt if the file has no entry for it. A stored keycode that can't
    // be translated also yields nullopt.
    std::optional<unsigned int> GetSavedDIK(const std::string& modName, const std::string& keybindId);

    // Associates a framework hotkey id ("MCM.<mod>.<id>") with its MCM identity
    // so binding-change notifications can be routed back to Keybinds.json.
    void RegisterMapping(const std::string& hotkeyId, const std::string& modName, const std::string& keybindId);

    // Called by HotkeyManager whenever any binding changes (both the immediate
    // path and the conflict-dialog confirm path). No-op for hotkey ids that
    // were never registered via RegisterMapping. dikScanCode == 0 removes the
    // entry from Keybinds.json (unbind).
    void OnFrameworkBindingChanged(const std::string& hotkeyId, unsigned int dikScanCode);

    // --- Reverse sync (native MCM -> framework), used by MCMLiveSync ---

    // One registered hotkey routing entry.
    struct Mapping {
        std::string hotkeyId;   // framework id: "MCM.<mod>.<id>"
        std::string modName;
        std::string keybindId;
    };

    // Snapshot of every routing registered via RegisterMapping — the set of
    // MCM keybinds our translated pages know about.
    std::vector<Mapping> GetAllMappings();

    // Updates the in-memory store with a binding read live from the running
    // MCM (VK code; 0 = unbound) WITHOUT writing Keybinds.json — the native
    // MCM owns persisting its own change on the next game save. Keeps
    // GetSavedDIK consistent for controls that register later in the session.
    void ImportLiveBinding(const std::string& modName, const std::string& keybindId, unsigned int vkKeycode);

    // --- Key code translation helpers (shared with the widget renderer) ---

    // Windows virtual-key code -> DirectInput scan code (0 if untranslatable).
    unsigned int VKToDIK(unsigned int vk);

    // DirectInput scan code -> Windows virtual-key code (0 if untranslatable).
    unsigned int DIKToVK(unsigned int dik);

}
