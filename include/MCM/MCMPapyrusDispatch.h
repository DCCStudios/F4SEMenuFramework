#pragma once

#include "MCM/MCMConfigParser.h"

#include <string>
#include <atomic>
#include <variant>

// Fire-and-forget Papyrus function dispatcher for MCM button actions and value-change callbacks.
// Handles action strings like "CallFunction:ScriptName.FunctionName" and
// "CallGlobalFunction:ScriptName.FunctionName".
// Uses the game's BSTThreadScrapFunction construction mechanism (REL::ID 69733)
// to produce valid callable wrappers for DispatchStaticCall / DispatchMethodCall.
namespace MCMPapyrusDispatch {

    // Execute an MCM action string. Fire-and-forget — logs errors but does not block.
    // Action formats supported:
    //   "CallFunction:ScriptName.FunctionName"       — call on quest/form (uses sourceForm)
    //   "CallGlobalFunction:ScriptName.FunctionName" — call as global/static function
    //   "SetValue:settingName=value"                 — route through value provider
    void ExecuteAction(const std::string& action, const std::string& modName);

    // Extended version that accepts a sourceForm string to resolve the target form
    // for CallFunction dispatch. sourceForm format: "PluginName.esp|HexLocalID"
    void ExecuteActionOnForm(const std::string& action, const std::string& modName, const std::string& sourceForm);

    // The control's current value, used to substitute "{value}" placeholders
    // in structured action params. monostate = no value (plain button press).
    using ControlValue = std::variant<std::monostate, bool, int, float, std::string>;

    // Execute a fully structured MCM action (object form with form/scriptName/
    // function/params). Params are packed with their JSON-derived runtime types;
    // "{value}" placeholders are replaced with the control's new value.
    // fallbackForm is used for CallFunction when the action has no "form" field
    // (some configs rely on the control's sourceForm).
    void ExecuteStructuredAction(const MCMConfigParser::MCMAction& action,
                                 const std::string& modName,
                                 const std::string& fallbackForm,
                                 const ControlValue& value);

    // Returns true if an action is currently in-flight (for UI status indicator).
    bool IsActionPending();

    // Returns a brief status string for the UI (e.g. "Running..." or empty).
    const char* GetStatusText();

}
