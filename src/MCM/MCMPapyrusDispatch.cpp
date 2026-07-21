#include "MCM/MCMPapyrusDispatch.h"
#include "MCM/PapyrusFunctionArgs.h"

#include <chrono>

namespace MCMPapyrusDispatch {

    static std::atomic<bool> s_pending{false};
    static std::string s_statusText;
    static std::chrono::steady_clock::time_point s_lastActionTime;

    // Resolve a form from a sourceForm string ("Plugin.esp|HexID" or raw hex)
    static RE::TESForm* ResolveFormFromSource(const std::string& sourceForm) {
        if (sourceForm.empty()) return nullptr;

        auto pipePos = sourceForm.find('|');
        if (pipePos == std::string::npos) {
            try {
                uint32_t formId = static_cast<uint32_t>(std::stoul(sourceForm, nullptr, 16));
                return RE::TESForm::GetFormByID(formId);
            } catch (...) {
                return nullptr;
            }
        }

        std::string pluginName = sourceForm.substr(0, pipePos);
        std::string localIdStr = sourceForm.substr(pipePos + 1);
        while (!localIdStr.empty() && (localIdStr.back() == ' ' || localIdStr.back() == '\t'))
            localIdStr.pop_back();

        uint32_t localFormId = 0;
        try {
            localFormId = static_cast<uint32_t>(std::stoul(localIdStr, nullptr, 16));
        } catch (...) {
            return nullptr;
        }

        auto* handler = RE::TESDataHandler::GetSingleton();
        if (!handler) return nullptr;

        for (auto* file : handler->compiledFileCollection.files) {
            if (!file) continue;
            if (_stricmp(file->GetFilename().data(), pluginName.c_str()) == 0) {
                uint32_t fullFormId = (static_cast<uint32_t>(file->GetCompileIndex()) << 24) | localFormId;
                return RE::TESForm::GetFormByID(fullFormId);
            }
        }

        for (auto* file : handler->compiledFileCollection.smallFiles) {
            if (!file) continue;
            if (_stricmp(file->GetFilename().data(), pluginName.c_str()) == 0) {
                uint32_t fullFormId = 0xFE000000u |
                    (static_cast<uint32_t>(file->GetSmallFileCompileIndex()) << 12) |
                    (localFormId & 0xFFF);
                return RE::TESForm::GetFormByID(fullFormId);
            }
        }

        return nullptr;
    }

    void ExecuteAction(const std::string& action, const std::string& modName) {
        ExecuteActionOnForm(action, modName, "");
    }

    void ExecuteActionOnForm(const std::string& action, const std::string& modName, const std::string& sourceForm) {
        if (action.empty()) return;

        logger::debug("[MCMPapyrusDispatch] Executing action '{}' for mod '{}'", action, modName);
        s_pending.store(true);
        s_statusText = "Running...";
        s_lastActionTime = std::chrono::steady_clock::now();

        auto colonPos = action.find(':');
        if (colonPos == std::string::npos) {
            logger::warn("[MCMPapyrusDispatch] Invalid action format (no ':'): {}", action);
            s_pending.store(false);
            s_statusText.clear();
            return;
        }

        std::string actionType = action.substr(0, colonPos);
        std::string actionTarget = action.substr(colonPos + 1);

        if (actionType == "CallFunction") {
            // CallFunction:ScriptName.FunctionName â€” calls on the form associated with the MCM config
            auto dotPos = actionTarget.find('.');
            if (dotPos == std::string::npos) {
                logger::error("[MCMPapyrusDispatch] Invalid function format (no '.'): {}", actionTarget);
                s_pending.store(false);
                s_statusText.clear();
                return;
            }

            std::string scriptName = actionTarget.substr(0, dotPos);
            std::string funcName = actionTarget.substr(dotPos + 1);

            // Resolve form from sourceForm if provided, otherwise try to find quest by script name
            RE::TESForm* targetForm = nullptr;
            if (!sourceForm.empty()) {
                targetForm = ResolveFormFromSource(sourceForm);
            }

            if (targetForm) {
                bool result = PapyrusFunctionArgs::CallFunctionOnForm(
                    targetForm, scriptName, funcName);

                if (result) {
                    logger::info("[MCMPapyrusDispatch] Successfully dispatched {}.{} on form 0x{:X} (mod: {})",
                        scriptName, funcName, targetForm->GetFormID(), modName);
                    s_statusText = scriptName + "." + funcName + " OK";
                } else {
                    logger::warn("[MCMPapyrusDispatch] DispatchMethodCall failed for {}.{} (mod: {})",
                        scriptName, funcName, modName);
                    s_statusText = scriptName + "." + funcName + " (failed)";
                }
            } else {
                // Fallback: try as global function if we can't find the form
                bool result = PapyrusFunctionArgs::CallGlobalFunction(scriptName, funcName);
                if (result) {
                    logger::info("[MCMPapyrusDispatch] Dispatched {}.{} as global (no form, mod: {})",
                        scriptName, funcName, modName);
                    s_statusText = scriptName + "." + funcName + " OK";
                } else {
                    logger::warn("[MCMPapyrusDispatch] Global fallback failed for {}.{} (mod: {})",
                        scriptName, funcName, modName);
                    s_statusText = scriptName + "." + funcName + " (no form)";
                }
            }

        } else if (actionType == "CallGlobalFunction") {
            // CallGlobalFunction:ScriptName.FunctionName â€” static/global call
            auto dotPos = actionTarget.find('.');
            if (dotPos == std::string::npos) {
                logger::error("[MCMPapyrusDispatch] Invalid function format (no '.'): {}", actionTarget);
                s_pending.store(false);
                s_statusText.clear();
                return;
            }

            std::string scriptName = actionTarget.substr(0, dotPos);
            std::string funcName = actionTarget.substr(dotPos + 1);

            bool result = PapyrusFunctionArgs::CallGlobalFunction(scriptName, funcName);
            if (result) {
                logger::info("[MCMPapyrusDispatch] Successfully dispatched global {}.{} (mod: {})",
                    scriptName, funcName, modName);
                s_statusText = scriptName + "." + funcName + " OK";
            } else {
                logger::warn("[MCMPapyrusDispatch] DispatchStaticCall failed for {}.{} (mod: {})",
                    scriptName, funcName, modName);
                s_statusText = scriptName + "." + funcName + " (failed)";
            }

        } else if (actionType == "SetValue") {
            auto eqPos = actionTarget.find('=');
            if (eqPos != std::string::npos) {
                std::string settingName = actionTarget.substr(0, eqPos);
                std::string value = actionTarget.substr(eqPos + 1);
                logger::info("[MCMPapyrusDispatch] SetValue: {} = {} (mod: {})", settingName, value, modName);
                s_statusText = "Set: " + settingName;
            }
        } else {
            logger::warn("[MCMPapyrusDispatch] Unknown action type: {}", actionType);
        }

        s_pending.store(false);
    }

    // ------------------------------------------------------------------
    // Structured action dispatch (object-form actions with typed params)
    // ------------------------------------------------------------------

    // Stringifies a control value the way MCM's ActionScript does when it
    // substitutes {value} into a string template (AS3 string coercion):
    // Boolean -> "true"/"false", numbers -> decimal without trailing zeros.
    static std::string ValueToString(const ControlValue& value) {
        return std::visit([](auto&& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                return v ? "true" : "false";
            } else if constexpr (std::is_same_v<T, int>) {
                return std::to_string(v);
            } else if constexpr (std::is_same_v<T, float>) {
                // AS3 Number-to-String: integral values print without a
                // decimal point, others with the shortest round-trip form.
                if (v == static_cast<float>(static_cast<long long>(v))) {
                    return std::to_string(static_cast<long long>(v));
                }
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%g", v);
                return buf;
            } else if constexpr (std::is_same_v<T, std::string>) {
                return v;
            } else {
                return {};  // monostate â€” plain button press, no value
            }
        }, value);
    }

    // Replaces every "{value}" occurrence in a template with the stringified
    // control value ("bConsole|{value}" -> "bConsole|true").
    static std::string SubstituteTemplate(const std::string& tmpl, const ControlValue& value) {
        const std::string sub = ValueToString(value);
        std::string out = tmpl;
        std::size_t pos = 0;
        while ((pos = out.find("{value}", pos)) != std::string::npos) {
            out.replace(pos, 7, sub);
            pos += sub.size();
        }
        return out;
    }

    // Packs one ActionParam into a Papyrus Variable, substituting the control's
    // value for "{value}" placeholders with its native type.
    static void PackParam(RE::BSScript::Variable& var,
                          const MCMConfigParser::ActionParam& param,
                          const ControlValue& value) {
        using PT = MCMConfigParser::ActionParam::Type;
        switch (param.type) {
            case PT::Bool:
                RE::BSScript::PackVariable(var, param.boolVal);
                break;
            case PT::Int:
                RE::BSScript::PackVariable(var, static_cast<std::int32_t>(param.intVal));
                break;
            case PT::Float:
                RE::BSScript::PackVariable(var, param.floatVal);
                break;
            case PT::String:
                RE::BSScript::PackVariable(var, RE::BSFixedString(param.stringVal.c_str()));
                break;
            case PT::ValuePlaceholder:
                // Substitute the control's new value with its runtime type
                std::visit([&var](auto&& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, bool>) {
                        RE::BSScript::PackVariable(var, v);
                    } else if constexpr (std::is_same_v<T, int>) {
                        RE::BSScript::PackVariable(var, static_cast<std::int32_t>(v));
                    } else if constexpr (std::is_same_v<T, float>) {
                        RE::BSScript::PackVariable(var, v);
                    } else if constexpr (std::is_same_v<T, std::string>) {
                        RE::BSScript::PackVariable(var, RE::BSFixedString(v.c_str()));
                    } else {
                        // monostate â€” no value available; pack integer 0 so the
                        // call still has the expected argument count
                        RE::BSScript::PackVariable(var, static_cast<std::int32_t>(0));
                    }
                }, value);
                break;
            // "{i}{value}" / "{f}{value}" / "{b}{value}": the control's value
            // coerced to an explicit Papyrus type (MCM's typed cast prefixes).
            case PT::ValueAsInt:
                RE::BSScript::PackVariable(var, std::visit([](auto&& v) -> std::int32_t {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, bool>) return v ? 1 : 0;
                    else if constexpr (std::is_same_v<T, int>) return v;
                    else if constexpr (std::is_same_v<T, float>) return static_cast<std::int32_t>(v);
                    else if constexpr (std::is_same_v<T, std::string>) return std::atoi(v.c_str());
                    else return 0;
                }, value));
                break;
            case PT::ValueAsFloat:
                RE::BSScript::PackVariable(var, std::visit([](auto&& v) -> float {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, bool>) return v ? 1.0f : 0.0f;
                    else if constexpr (std::is_same_v<T, int>) return static_cast<float>(v);
                    else if constexpr (std::is_same_v<T, float>) return v;
                    else if constexpr (std::is_same_v<T, std::string>) return static_cast<float>(std::atof(v.c_str()));
                    else return 0.0f;
                }, value));
                break;
            case PT::ValueAsBool:
                RE::BSScript::PackVariable(var, std::visit([](auto&& v) -> bool {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, bool>) return v;
                    else if constexpr (std::is_same_v<T, int>) return v != 0;
                    else if constexpr (std::is_same_v<T, float>) return v != 0.0f;
                    else if constexpr (std::is_same_v<T, std::string>)
                        return _stricmp(v.c_str(), "true") == 0 || std::atoi(v.c_str()) != 0;
                    else return false;
                }, value));
                break;
            case PT::StringTemplate:
                // "{value}" embedded in longer text â€” substitute as a string
                RE::BSScript::PackVariable(var,
                    RE::BSFixedString(SubstituteTemplate(param.stringVal, value).c_str()));
                break;
        }
    }

    void ExecuteStructuredAction(const MCMConfigParser::MCMAction& action,
                                 const std::string& modName,
                                 const std::string& fallbackForm,
                                 const ControlValue& value) {
        // Called from the render thread (buttons) or the window-message thread
        // (keybinds); neither has an exception handler above us, so an escaped
        // C++ exception is an instant, log-less CTD. Contain and log instead.
        try {
        // Console commands need no VM or parameter packing. The real MCM runs
        // them synchronously from its input handler; we're called from the
        // window-message thread (keybinds) or render thread (buttons), so run
        // the command on the game's main thread via the F4SE task queue.
        if (action.type == "RunConsoleCommand") {
            if (action.command.empty()) {
                logger::warn("[MCMPapyrusDispatch] RunConsoleCommand with empty command (mod: {})", modName);
                return;
            }
            if (const auto* tasks = F4SE::GetTaskInterface()) {
                const std::string cmd = action.command;
                tasks->AddTask([cmd]() { RE::Console::ExecuteCommand(cmd.c_str()); });
                logger::info("[MCMPapyrusDispatch] Queued console command '{}' (mod: {})", cmd, modName);
                s_statusText = "Console: " + cmd;
                s_lastActionTime = std::chrono::steady_clock::now();
            }
            return;
        }

        // CallExternalFunction bypasses Papyrus entirely: MCM's AS3 invokes
        // `stage.f4se.plugins[<plugin>][<function>](...args)` â€” a Scaleform
        // function object the target F4SE plugin registered through F4SE's
        // scaleform interface. F4SE injects the `f4se` object into every menu
        // movie, so we invoke it on whichever always-loaded movie is available
        // (HUD during gameplay, pause menu when paused). Scaleform may only be
        // touched from the game's UI thread -> F4SE UI task.
        if (action.type == "CallExternalFunction") {
            // Pre-resolve params to plain data on this thread ({value}
            // substitution included); the UI task builds GFx::Values from it.
            struct GfxParam {
                enum class Kind { Bool, Number, String } kind = Kind::String;
                bool b = false;
                double n = 0.0;
                std::string s;
            };
            auto packed = std::make_shared<std::vector<GfxParam>>();
            using PT = MCMConfigParser::ActionParam::Type;
            for (const auto& p : action.params) {
                GfxParam gp;
                switch (p.type) {
                    case PT::Bool:   gp.kind = GfxParam::Kind::Bool;   gp.b = p.boolVal; break;
                    case PT::Int:    gp.kind = GfxParam::Kind::Number; gp.n = p.intVal; break;
                    case PT::Float:  gp.kind = GfxParam::Kind::Number; gp.n = p.floatVal; break;
                    case PT::String: gp.kind = GfxParam::Kind::String; gp.s = p.stringVal; break;
                    case PT::StringTemplate:
                        gp.kind = GfxParam::Kind::String;
                        gp.s = SubstituteTemplate(p.stringVal, value);
                        break;
                    case PT::ValuePlaceholder:
                        // Control value with its native runtime type
                        std::visit([&gp](auto&& v) {
                            using T = std::decay_t<decltype(v)>;
                            if constexpr (std::is_same_v<T, bool>) { gp.kind = GfxParam::Kind::Bool; gp.b = v; }
                            else if constexpr (std::is_same_v<T, int>) { gp.kind = GfxParam::Kind::Number; gp.n = v; }
                            else if constexpr (std::is_same_v<T, float>) { gp.kind = GfxParam::Kind::Number; gp.n = v; }
                            else if constexpr (std::is_same_v<T, std::string>) { gp.kind = GfxParam::Kind::String; gp.s = v; }
                            else { gp.kind = GfxParam::Kind::Number; gp.n = 0.0; }
                        }, value);
                        break;
                    case PT::ValueAsInt:
                    case PT::ValueAsFloat:
                        gp.kind = GfxParam::Kind::Number;
                        gp.n = std::atof(ValueToString(value).c_str());
                        break;
                    case PT::ValueAsBool: {
                        gp.kind = GfxParam::Kind::Bool;
                        const std::string sv = ValueToString(value);
                        gp.b = (_stricmp(sv.c_str(), "true") == 0 || std::atof(sv.c_str()) != 0.0);
                        break;
                    }
                }
                packed->push_back(std::move(gp));
            }

            if (const auto* tasks = F4SE::GetTaskInterface()) {
                const std::string plugin = action.plugin;
                const std::string func = action.function;
                const std::string mod = modName;
                tasks->AddUITask([plugin, func, mod, packed]() {
                    // Runs later on the game's UI thread â€” same containment
                    // rule as the dispatcher itself.
                    try {
                    auto* ui = RE::UI::GetSingleton();
                    if (!ui) {
                        return;
                    }
                    for (const char* menuName : { "HUDMenu", "PauseMenu" }) {
                        if (!ui->GetMenuOpen(menuName)) {
                            continue;
                        }
                        auto menu = ui->GetMenu(menuName);
                        if (!menu || !menu->uiMovie || !menu->uiMovie->asMovieRoot) {
                            continue;
                        }
                        auto* root = menu->uiMovie->asMovieRoot.get();

                        RE::Scaleform::GFx::Value pluginObj;
                        const std::string path = "root.f4se.plugins." + plugin;
                        if (!root->GetVariable(std::addressof(pluginObj), path.c_str()) ||
                            !pluginObj.IsObject()) {
                            continue;  // f4se object missing on this movie â€” try the next
                        }

                        std::vector<RE::Scaleform::GFx::Value> args;
                        args.reserve(packed->size());
                        for (const auto& gp : *packed) {
                            switch (gp.kind) {
                                case GfxParam::Kind::Bool:   args.emplace_back(gp.b); break;
                                case GfxParam::Kind::Number: args.emplace_back(gp.n); break;
                                case GfxParam::Kind::String: args.emplace_back(gp.s.c_str()); break;
                            }
                        }

                        RE::Scaleform::GFx::Value ret;
                        if (pluginObj.Invoke(func.c_str(), std::addressof(ret),
                                             args.empty() ? nullptr : args.data(), args.size())) {
                            logger::info("[MCMPapyrusDispatch] CallExternalFunction {}.{} invoked via {} ({} arg(s), mod: {})",
                                plugin, func, menuName, args.size(), mod);
                            return;
                        }
                        logger::warn("[MCMPapyrusDispatch] CallExternalFunction {}.{} Invoke failed on {} (mod: {})",
                            plugin, func, menuName, mod);
                        return;
                    }
                    logger::warn("[MCMPapyrusDispatch] CallExternalFunction {}.{}: no loaded movie exposes root.f4se.plugins.{} (mod: {})",
                        plugin, func, plugin, mod);
                    } catch (const std::exception& e) {
                        logger::error("[MCMPapyrusDispatch] EXCEPTION in CallExternalFunction {}.{} UI task (mod: {}): {}",
                            plugin, func, mod, e.what());
                    } catch (...) {
                        logger::error("[MCMPapyrusDispatch] EXCEPTION (non-std) in CallExternalFunction {}.{} UI task (mod: {})",
                            plugin, func, mod);
                    }
                });
                s_statusText = action.function + " OK";
                s_lastActionTime = std::chrono::steady_clock::now();
            }
            return;
        }

        auto* gameVM = RE::GameVM::GetSingleton();
        if (!gameVM || !gameVM->GetVM()) {
            logger::warn("[MCMPapyrusDispatch] Papyrus VM unavailable for structured action (mod: {})", modName);
            return;
        }
        auto* vm = gameVM->GetVM().get();

        s_statusText = "Running...";
        s_lastActionTime = std::chrono::steady_clock::now();

        // Pack all params (with {value} substitution) into a scrap array,
        // then wrap it via the game's thread-scrap-function machinery.
        const auto paramCount =
            static_cast<RE::BSScrapArray<RE::BSScript::Variable>::size_type>(action.params.size());
        RE::BSScrapArray<RE::BSScript::Variable> scrap{ paramCount };
        for (RE::BSScrapArray<RE::BSScript::Variable>::size_type i = 0; i < paramCount; ++i) {
            PackParam(scrap.at(i), action.params[i], value);
        }
        // Owner must outlive the dispatch; pass fargs.get() by reference only
        // (layout is runtime-specific on OG â€” see PapyrusFunctionArgs.h).
        PapyrusFunctionArgs::RuntimeFunctionArgs fargs{ vm, scrap };

        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> nullCallback;
        bool success = false;

        if (action.type == "CallGlobalFunction") {
            // Static/global call on the named script
            success = vm->DispatchStaticCall(
                RE::BSFixedString(action.scriptName.c_str()),
                RE::BSFixedString(action.function.c_str()),
                fargs.get(), nullCallback);
        } else if (action.type == "CallFunction") {
            // Method call on the script attached to the target form.
            // Prefer the action's own "form"; fall back to the control's sourceForm.
            const std::string& formStr = !action.form.empty() ? action.form : fallbackForm;
            auto* form = ResolveFormFromSource(formStr);
            if (!form) {
                logger::warn("[MCMPapyrusDispatch] Structured CallFunction: form '{}' not resolvable (mod: {})",
                    formStr, modName);
                s_statusText = action.function + " (form missing)";
                return;
            }

            auto& handlePolicy = vm->GetObjectHandlePolicy();
            auto handle = handlePolicy.GetHandleForObject(
                static_cast<std::uint32_t>(form->GetFormType()), form);
            if (handle == handlePolicy.EmptyHandle()) {
                logger::warn("[MCMPapyrusDispatch] Structured CallFunction: no script handle for form 0x{:X} (mod: {})",
                    form->GetFormID(), modName);
                s_statusText = action.function + " (no scripts)";
                return;
            }

            // If the config names the script, dispatch directly to it. When it
            // doesn't (the common case â€” MCM keybinds.json has no scriptName
            // field at all, and most config.json actions omit it too), mirror
            // the real MCM: its VMScript helper defaults the class name to
            // "ScriptObject", which every Papyrus script derives from, so the
            // VM's non-exact lookup returns the form's attached script and the
            // function is resolved on that object's actual type.
            if (!action.scriptName.empty()) {
                success = vm->DispatchMethodCall(handle,
                    RE::BSFixedString(action.scriptName.c_str()),
                    RE::BSFixedString(action.function.c_str()),
                    fargs.get(), nullCallback);
            } else {
                // Resolve the bound object explicitly (exactMatch=false casts
                // any attached script to ScriptObject), then dispatch on the
                // object so the function lookup uses its real script type.
                RE::BSTSmartPointer<RE::BSScript::Object> scriptObj;
                if (vm->FindBoundObject(handle, "ScriptObject", false, scriptObj, false) && scriptObj) {
                    success = vm->DispatchMethodCall(scriptObj,
                        RE::BSFixedString(action.function.c_str()),
                        fargs.get(), nullCallback);
                } else {
                    logger::warn("[MCMPapyrusDispatch] Structured CallFunction: no script bound to form 0x{:X} (mod: {})",
                        form->GetFormID(), modName);
                    handlePolicy.ReleaseHandle(handle);
                    s_statusText = action.function + " (no scripts)";
                    return;
                }
            }
            handlePolicy.ReleaseHandle(handle);
        } else {
            logger::warn("[MCMPapyrusDispatch] Unknown structured action type '{}' (mod: {})", action.type, modName);
            s_statusText.clear();
            return;
        }

        if (success) {
            logger::info("[MCMPapyrusDispatch] Structured action {}.{} dispatched with {} param(s) (mod: {})",
                action.scriptName, action.function, action.params.size(), modName);
            s_statusText = action.function + " OK";
        } else {
            logger::warn("[MCMPapyrusDispatch] Structured action {}.{} dispatch FAILED (mod: {})",
                action.scriptName, action.function, modName);
            s_statusText = action.function + " (failed)";
        }

        } catch (const std::exception& e) {
            logger::error("[MCMPapyrusDispatch] EXCEPTION executing {} {}.{} (mod: {}): {}",
                action.type, action.scriptName, action.function, modName, e.what());
            s_statusText = action.function + " (exception)";
        } catch (...) {
            logger::error("[MCMPapyrusDispatch] EXCEPTION (non-std) executing {} {}.{} (mod: {})",
                action.type, action.scriptName, action.function, modName);
            s_statusText = action.function + " (exception)";
        }
    }

    // ------------------------------------------------------------------
    // SendEvent keybind actions (OnControlDown / OnControlUp)
    // ------------------------------------------------------------------

    void SendControlEvent(const std::string& formSpec, const std::string& controlId,
                          bool down, float heldSeconds) {
        auto* gameVM = RE::GameVM::GetSingleton();
        if (!gameVM || !gameVM->GetVM()) return;
        auto* vm = gameVM->GetVM().get();

        auto* form = ResolveFormFromSource(formSpec);
        if (!form) {
            logger::warn("[MCMPapyrusDispatch] SendEvent: form '{}' not resolvable", formSpec);
            return;
        }

        auto& handlePolicy = vm->GetObjectHandlePolicy();
        auto handle = handlePolicy.GetHandleForObject(
            static_cast<std::uint32_t>(form->GetFormType()), form);
        if (handle == handlePolicy.EmptyHandle()) return;

        // The real MCM delivers these through SendPapyrusEvent(handle,
        // "ScriptObject", "OnControlDown"/"OnControlUp", ...), i.e. a method
        // call on the form's attached script resolved via the ScriptObject
        // base class. OnControlDown(controlName); OnControlUp(controlName,
        // heldSeconds). The receiving script must sit on the form itself
        // (quests, not aliases) â€” same requirement as the real MCM.
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> nullCallback;
        bool ok = false;
        RE::BSTSmartPointer<RE::BSScript::Object> scriptObj;
        if (vm->FindBoundObject(handle, "ScriptObject", false, scriptObj, false) && scriptObj) {
            if (down) {
                PapyrusFunctionArgs::FunctionArgs<RE::BSFixedString> fargs{
                    vm, RE::BSFixedString(controlId.c_str()) };
                ok = vm->DispatchMethodCall(scriptObj, RE::BSFixedString("OnControlDown"),
                                            fargs.get(), nullCallback);
            } else {
                PapyrusFunctionArgs::FunctionArgs<RE::BSFixedString, float> fargs{
                    vm, RE::BSFixedString(controlId.c_str()), heldSeconds };
                ok = vm->DispatchMethodCall(scriptObj, RE::BSFixedString("OnControlUp"),
                                            fargs.get(), nullCallback);
            }
        }
        handlePolicy.ReleaseHandle(handle);

        if (!ok) {
            logger::debug("[MCMPapyrusDispatch] SendEvent {} for '{}' on {} not handled (script may not implement it)",
                down ? "OnControlDown" : "OnControlUp", controlId, formSpec);
        }
    }

    bool IsActionPending() {
        return s_pending.load();
    }

    const char* GetStatusText() {
        if (!s_statusText.empty()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - s_lastActionTime).count();
            if (elapsed > 2000) {
                s_statusText.clear();
            }
        }
        return s_statusText.c_str();
    }

} // namespace MCMPapyrusDispatch
