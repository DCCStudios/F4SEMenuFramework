#pragma once

// Pause-menu integration for the F4SE Menu Framework.
//
// Registers an F4SE Scaleform callback that injects our own shipped SWF
// (Data/Interface/F4SEFramework.swf) into the game's pause menu. That SWF
// (and/or a native inject on PauseMenu open) adds a single "F4SE FRAMEWORK"
// list row at the top of the pause list which, when selected, calls back into
// the DLL to open the ImGui overlay. Placement is not tied to MCM.
// See src/PauseMenuButton.cpp and swf/src/F4SEFrameworkPause.as.
namespace PauseMenuButton
{
    // Registers the Scaleform callback. Call once during F4SEPlugin_Load, after
    // F4SE::Init (the Scaleform interface must be available).
    void Install();

    // Registers the PauseMenu open/close sink so the list row can be inserted
    // from C++ as soon as the menu opens (does not wait for F4SEFramework.swf).
    // Call once when RE::UI is available (kGameDataReady is safe).
    void RegisterMenuEvents();
}
