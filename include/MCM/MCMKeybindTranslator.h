#pragma once

#include "MCM/MCMScanner.h"
#include "MCM/MCMConfigParser.h"
#include <string>
#include <vector>
#include <optional>

// Parses keybinds.json files and registers the actions as framework hotkeys.
// Keybind actions are dispatched via Papyrus or value provider when triggered.
namespace MCMKeybindTranslator {

    struct MCMKeybind {
        std::string id;          // unique keybind identifier
        std::string desc;        // human-readable description
        std::string action;      // legacy action string (e.g. "CallFunction:Script.Func")
        std::optional<MCMConfigParser::MCMAction> actionObj;  // structured action (preferred)
        unsigned int defaultKey; // default DIK scan code
        std::string modName;     // owning mod name
        int64_t hotkeyHandle;    // handle returned by HotkeyManager
    };

    // Parse and register keybinds from a keybinds.json file for a given mod.
    void RegisterFromFile(const std::filesystem::path& keybindsPath, const std::string& modName);

    // Coexistence guard: when the REAL MCM is loaded alongside us, its own
    // input handler (MCMInput, active from kMessage_GameLoaded) already
    // dispatches every keybind action. If we dispatch too, one key press runs
    // the action twice — fatal for toggle hotkeys (Wheel Menu's ToggleMenu,
    // Screen Archer Menu's SAM.ToggleMenu open and then instantly close).
    // With suppression on, keybinds stay registered (visible + rebindable in
    // our UI, synced through Keybinds.json / live sync) but our thunks no-op.
    void SetSuppressDispatch(bool suppress);

    // Unregister all keybinds for a given mod.
    void UnregisterMod(const std::string& modName);

    // Get all registered keybinds (for debug/UI purposes).
    const std::vector<MCMKeybind>& GetAllKeybinds();

}
