#pragma once

#include <string>

// MCMPapyrusAPI — full native-code replacement for the MCM Papyrus script API.
//
// The real Mod Configuration Menu ships a tiny MCM.pex whose functions are all
// declared `native global`; the actual implementations live in MCM.dll. We ship
// the same MCM.pex (pure declarations, no bytecode) and provide the native
// implementations here, so mods that call `MCM.GetModSettingInt(...)` etc. work
// without the real MCM installed. Values are read/written through
// MCMValueProvider so they stay in sync with the translated F4SE Menu Framework
// pages.
//
// This module also dispatches the external events that the real MCM fires via
// F4SE's RegisterForExternalEvent mechanism:
//   - OnMCMSettingChange(string modName, string id)   [+ "|ModName" filtered variant]
//   - OnMCMMenuOpen(string modName)                    [+ filtered variant]
//   - OnMCMMenuClose(string modName)                   [+ filtered variant]
//   - OnMCMOpen() / OnMCMClose()                       [legacy whole-menu events]
namespace MCMPapyrusAPI {

    // Version code reported by GetVersionCode(). The real MCM 1.40 reports 9
    // (see f4mcm Config.h: PLUGIN_VERSION 9) — we match it so version checks
    // in mod scripts behave identically.
    inline constexpr int kVersionCode = 9;

    // F4SE Papyrus registration callback. Registered via
    // F4SE::GetPapyrusInterface()->Register() in F4SEPlugin_Load.
    // Binds all `MCM` script natives. If the real MCM.dll is loaded in the
    // process, registration is skipped entirely so the real MCM's natives are
    // authoritative (avoids double-binding).
    bool RegisterFunctions(RE::BSScript::IVirtualMachine* a_vm);

    // Returns true if our natives were actually bound (i.e. we are acting as
    // the MCM API provider in this session).
    bool AreNativesBound();

    // --- External event dispatch (mirrors the real MCM's SendExternalEvent calls) ---

    // Fired when a control's value is committed in the translated UI.
    // Sends "OnMCMSettingChange" and "OnMCMSettingChange|<modName>", both with
    // (modName, controlId) arguments — matching the real MCM AS3 behavior.
    void DispatchSettingChanged(const std::string& modName, const std::string& controlId);

    // Fired when a mod's translated page becomes the displayed page.
    // Sends "OnMCMMenuOpen" and "OnMCMMenuOpen|<modName>" with NO arguments —
    // the shipped MCM.swf sends none (its .psc doc comment is wrong), and the
    // Papyrus VM drops calls whose argument count mismatches the handler.
    void DispatchMenuOpen(const std::string& modName);

    // Fired when a mod's translated page stops being displayed.
    // Sends only "OnMCMMenuClose|<modName>" (no arguments); real MCM has no
    // unfiltered OnMCMMenuClose send site.
    void DispatchMenuClose(const std::string& modName);

    // Legacy whole-menu events fired by the real MCM when the pause-menu MCM
    // opens/closes. We map them to the F4SE Menu Framework main window.
    void DispatchMCMOpen();
    void DispatchMCMClose();

}
