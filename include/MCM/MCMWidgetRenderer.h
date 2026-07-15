#pragma once

#include "MCM/MCMConfigParser.h"
#include <string>
#include <functional>

// Renders MCM controls as ImGui widgets within framework section pages.
// Each MCM mod gets one or more section items registered with the framework.
namespace MCMWidgetRenderer {

    // Creates render functions for a parsed MCM mod config and registers them
    // as framework section items under "MCM Mod Settings/<DisplayName>".
    void RegisterMod(const MCMConfigParser::MCMModConfig& config, const std::string& modName);

    // Unregisters all section items for a mod (not typically needed at runtime).
    void UnregisterMod(const std::string& modName);

    // Marks all cached control states dirty so they re-read their values from
    // the value provider on the next rendered frame. This is the backend of
    // the MCM.RefreshMenu() Papyrus native.
    void InvalidateAllStates();

    // Must be called once per rendered frame AFTER all windows have rendered
    // (from the Present hook). Detects page open/close transitions and fires
    // the OnMCMMenuOpen / OnMCMMenuClose external events accordingly.
    void OnFrameEnd();

    // Help text of the currently hovered/nav-focused control, for display in
    // the gamepad hint bar / help footer. Empty when nothing relevant focused.
    const std::string& GetFocusedHelpText();

    // True while a hotkey control is capturing the next key/button press.
    // Used by the gamepad back-button handler to cancel capture instead of
    // closing the menu.
    bool IsHotkeyCaptureActive();

    // Cancels an in-progress hotkey capture (gamepad B button).
    void CancelHotkeyCapture();

}
