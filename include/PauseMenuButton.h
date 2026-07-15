#pragma once

// Pause-menu integration for the F4SE Menu Framework.
//
// Registers an F4SE Scaleform callback that injects our own shipped SWF
// (Data/Interface/F4SEFramework.swf) into the game's pause menu. That SWF adds
// a single "F4SE Framework" list row (placed above MCM's row when MCM is
// present) which, when selected, calls back into the DLL to open the ImGui
// overlay. See src/PauseMenuButton.cpp and swf/src/F4SEFrameworkPause.as.
namespace PauseMenuButton
{
    // Registers the Scaleform callback. Call once during F4SEPlugin_Load, after
    // F4SE::Init (the Scaleform interface must be available).
    void Install();
}
