#include "MCM/MCMPapyrusAPI.h"
#include "MCM/MCMValueProvider.h"
#include "MCM/MCMWidgetRenderer.h"
#include "MCM/PapyrusFunctionArgs.h"
#include "Config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace MCMPapyrusAPI {

    // Set once RegisterFunctions has successfully bound the natives.
    static std::atomic<bool> s_nativesBound{ false };

    // ------------------------------------------------------------------
    // Native implementations (script "MCM", all global/static functions).
    //
    // Global Papyrus functions receive std::monostate as the "self"
    // parameter under CommonLibF4's BSScriptUtil binding machinery.
    // Value access is routed through MCMValueProvider's raw mod-setting
    // API, which layers Config defaults under user-saved values and
    // writes to the canonical flat file Data/MCM/Settings/<ModName>.ini.
    // ------------------------------------------------------------------

    static bool IsInstalled(std::monostate) {
        // We ARE the MCM API provider in this session (registration is
        // skipped entirely when the real MCM.dll is present).
        return true;
    }

    static std::int32_t GetVersionCode(std::monostate) {
        return kVersionCode;
    }

    static void RefreshMenu(std::monostate) {
        // Real MCM re-pulls all displayed values from their sources.
        // Our equivalent: invalidate the widget renderer's cached control
        // states so the next rendered frame re-reads every value.
        MCMWidgetRenderer::InvalidateAllStates();
    }

    static std::int32_t GetModSettingInt(std::monostate, std::string a_modName, std::string a_setting) {
        auto raw = MCMValueProvider::GetModSettingRaw(a_modName, a_setting);
        if (!raw.has_value()) return -1;  // matches real MCM's "missing setting" result
        try { return std::stoi(*raw); } catch (...) { return -1; }
    }

    static bool GetModSettingBool(std::monostate, std::string a_modName, std::string a_setting) {
        auto raw = MCMValueProvider::GetModSettingRaw(a_modName, a_setting);
        if (!raw.has_value()) return false;
        return *raw != "0" && !raw->empty();  // real MCM treats any non-"0" as true
    }

    static float GetModSettingFloat(std::monostate, std::string a_modName, std::string a_setting) {
        auto raw = MCMValueProvider::GetModSettingRaw(a_modName, a_setting);
        if (!raw.has_value()) return -1.0f;
        try { return std::stof(*raw); } catch (...) { return -1.0f; }
    }

    static std::string GetModSettingString(std::monostate, std::string a_modName, std::string a_setting) {
        auto raw = MCMValueProvider::GetModSettingRaw(a_modName, a_setting);
        return raw.value_or(std::string{});
    }

    static void SetModSettingInt(std::monostate, std::string a_modName, std::string a_setting, std::int32_t a_value) {
        MCMValueProvider::SetModSettingRaw(a_modName, a_setting, std::to_string(a_value));
    }

    static void SetModSettingBool(std::monostate, std::string a_modName, std::string a_setting, bool a_value) {
        MCMValueProvider::SetModSettingRaw(a_modName, a_setting, a_value ? "1" : "0");
    }

    static void SetModSettingFloat(std::monostate, std::string a_modName, std::string a_setting, float a_value) {
        MCMValueProvider::SetModSettingRaw(a_modName, a_setting, std::to_string(a_value));
    }

    static void SetModSettingString(std::monostate, std::string a_modName, std::string a_setting, std::string a_value) {
        MCMValueProvider::SetModSettingRaw(a_modName, a_setting, a_value);
    }

    // ------------------------------------------------------------------
    // Registration
    // ------------------------------------------------------------------

    bool RegisterFunctions(RE::BSScript::IVirtualMachine* a_vm) {
        if (!a_vm) return false;

        // If the real MCM.dll is loaded in this process, its plugin registers
        // the exact same natives. Binding ours as well would fight over the
        // same function slots — defer to the real MCM entirely.
        // (This callback runs when the VM initializes, which is after ALL
        // F4SE plugins' Load functions have run, so the check is reliable.)
        if (GetModuleHandleA("mcm.dll") != nullptr) {
            logger::info("[MCMPapyrusAPI] Real MCM.dll detected — skipping MCM native registration (real MCM is authoritative)");
            return true;
        }

        // Bind every function declared in MCM.psc. taskletCallable=true for the
        // read-only queries (same as real MCM's kFunctionFlag_NoWait on
        // IsInstalled/GetVersionCode); setters must run on the VM thread.
        a_vm->BindNativeMethod("MCM", "IsInstalled", IsInstalled, true);
        a_vm->BindNativeMethod("MCM", "GetVersionCode", GetVersionCode, true);
        a_vm->BindNativeMethod("MCM", "RefreshMenu", RefreshMenu, true);
        a_vm->BindNativeMethod("MCM", "GetModSettingInt", GetModSettingInt, true);
        a_vm->BindNativeMethod("MCM", "GetModSettingBool", GetModSettingBool, true);
        a_vm->BindNativeMethod("MCM", "GetModSettingFloat", GetModSettingFloat, true);
        a_vm->BindNativeMethod("MCM", "GetModSettingString", GetModSettingString, true);
        a_vm->BindNativeMethod("MCM", "SetModSettingInt", SetModSettingInt);
        a_vm->BindNativeMethod("MCM", "SetModSettingBool", SetModSettingBool);
        a_vm->BindNativeMethod("MCM", "SetModSettingFloat", SetModSettingFloat);
        a_vm->BindNativeMethod("MCM", "SetModSettingString", SetModSettingString);

        s_nativesBound.store(true);
        logger::info("[MCMPapyrusAPI] MCM script natives registered — F4SE Menu Framework is acting as the MCM API provider");
        return true;
    }

    bool AreNativesBound() {
        return s_nativesBound.load();
    }

    // ------------------------------------------------------------------
    // External event dispatch
    //
    // Mirrors how the real MCM fires script events: F4SE keeps a registry of
    // RegisterForExternalEvent registrations per event name; for each
    // registrant we dispatch a Papyrus method call on the registered handle
    // with the registered callback name. Argument packing goes through the
    // same BSTThreadScrapFunction machinery as our action dispatch.
    // ------------------------------------------------------------------

    // Payload carried through the C-style registrant functor.
    struct EventArgs {
        std::vector<std::string> args;  // all MCM event args are strings
    };

    // Dispatches one registrant. Called by F4SE once per registration.
    static void F4SEAPI RegistrantFunctor(std::uint64_t a_handle, const char* a_scriptName,
                                          const char* a_callbackName, void* a_data) {
        auto* payload = static_cast<EventArgs*>(a_data);
        if (!payload || !a_scriptName || !a_callbackName) return;

        auto* gameVM = RE::GameVM::GetSingleton();
        if (!gameVM || !gameVM->GetVM()) return;
        auto* vm = gameVM->GetVM().get();

        // Pack the string args (0, 1 or 2 of them) into a scrap function.
        RE::BSTThreadScrapFunction<bool(RE::BSScrapArray<RE::BSScript::Variable>&)> scrapFunc;
        switch (payload->args.size()) {
            case 0:
                scrapFunc = (PapyrusFunctionArgs::FunctionArgs<>{ vm }).get();
                break;
            case 1:
                scrapFunc = (PapyrusFunctionArgs::FunctionArgs<RE::BSFixedString>{
                    vm, RE::BSFixedString(payload->args[0].c_str()) }).get();
                break;
            default:
                scrapFunc = (PapyrusFunctionArgs::FunctionArgs<RE::BSFixedString, RE::BSFixedString>{
                    vm, RE::BSFixedString(payload->args[0].c_str()),
                    RE::BSFixedString(payload->args[1].c_str()) }).get();
                break;
        }

        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> nullCallback;
        vm->DispatchMethodCall(a_handle,
            RE::BSFixedString(a_scriptName),
            RE::BSFixedString(a_callbackName),
            scrapFunc, nullCallback);
    }

    // Fires a single external event name to all its registrants.
    static void FireExternalEvent(const std::string& eventName, EventArgs& payload) {
        auto* papyrus = F4SE::GetPapyrusInterface();
        if (!papyrus) return;
        papyrus->GetExternalEventRegistrations(eventName, &payload, RegistrantFunctor);
    }

    void DispatchSettingChanged(const std::string& modName, const std::string& controlId) {
        // Real MCM only sends the event when the control has an id.
        if (controlId.empty()) return;

        EventArgs payload;
        payload.args = { modName, controlId };
        // Both the unfiltered and the "|ModName" filtered event receive
        // (modName, id) — verified against the MCM AS3 source (OptionsList.as).
        FireExternalEvent("OnMCMSettingChange", payload);
        FireExternalEvent("OnMCMSettingChange|" + modName, payload);
        logger::debug("[MCMPapyrusAPI] OnMCMSettingChange fired (mod='{}', id='{}')", modName, controlId);
    }

    void DispatchMenuOpen(const std::string& modName) {
        EventArgs payload;
        payload.args = { modName };
        FireExternalEvent("OnMCMMenuOpen", payload);
        FireExternalEvent("OnMCMMenuOpen|" + modName, payload);
        logger::debug("[MCMPapyrusAPI] OnMCMMenuOpen fired (mod='{}')", modName);
    }

    void DispatchMenuClose(const std::string& modName) {
        EventArgs payload;
        payload.args = { modName };
        FireExternalEvent("OnMCMMenuClose", payload);
        FireExternalEvent("OnMCMMenuClose|" + modName, payload);
        logger::debug("[MCMPapyrusAPI] OnMCMMenuClose fired (mod='{}')", modName);
    }

    void DispatchMCMOpen() {
        EventArgs payload;  // legacy event carries no arguments (MCM_Main.as)
        FireExternalEvent("OnMCMOpen", payload);
    }

    void DispatchMCMClose() {
        EventArgs payload;
        FireExternalEvent("OnMCMClose", payload);
    }

} // namespace MCMPapyrusAPI
