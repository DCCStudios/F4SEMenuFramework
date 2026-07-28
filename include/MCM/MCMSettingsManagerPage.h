#pragma once

#include <string>

// Native ImGui recreation of m8r98a4f2's "MCM Settings Manager" (Nexus 56195).
//
// The original mod is a full ActionScript 3 application embedded in a single
// MCM "image" control (className M8r.McmSettingsManager.Controller.
// MCMSettingsManager). It needs a live Scaleform stage and direct access to
// MCM's internal display list, neither of which our translation layer
// provides — so, like the FallUI HUD editor, we replace it with a native
// recreation that keeps the mod's on-disk data formats byte-compatible.
//
// What it does (ported from the decompiled AS3 under
// PluginTemplate/MCM Settings Manager/_analysis/as3/):
//  - 10 storage slots holding a snapshot of every MCM mod's settings
//    (INI mod settings, global values, Papyrus properties, hotkeys),
//    persisted through the manager's own MCM settings INI using the
//    M8rJSON + IniChunked formats (see MCM/M8rIniJson.h).
//  - Read-only presets loaded from Data/MCM/Settings/Presets/*.ini.
//  - Apply / save at three granularities: whole slot, one mod, one setting.
//  - Slot rename / export / wipe / delete / remove-unchanged tools.
//
// COMPATIBILITY IS THE CONTRACT: slots we write load in the original Flash
// manager and vice versa (the codec is validated by swf/test/m8rjsontest).
namespace MCMSettingsManagerPage {

    // True when this MCM image control is the MCM Settings Manager app.
    bool HandlesImageControl(const std::string& libName, const std::string& className);

    // Renders the recreation inside the current ImGui window. Call only when
    // HandlesImageControl() is true.
    void RenderImageControl(const std::string& libName, const std::string& className);

    // Drops all cached session state (settings database, current-value cache,
    // slot/preset caches, view state) so the next render rebuilds from the
    // live providers. Called on menu close / page leave / MCM.RefreshMenu.
    void ResetSession();

}
