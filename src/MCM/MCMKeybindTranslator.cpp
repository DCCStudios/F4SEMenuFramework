#include "MCM/MCMKeybindTranslator.h"
#include "MCM/MCMKeybindStore.h"
#include "MCM/MCMPapyrusDispatch.h"
#include "HotkeyManager.h"
#include "Application.h"

#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>

namespace MCMKeybindTranslator {

    static std::vector<MCMKeybind> s_keybinds;

    // Callback thunks — since HotkeyManager expects a plain __stdcall function pointer,
    // we create a static dispatch table keyed by keybind index.
    static constexpr size_t MAX_KEYBINDS = 256;
    static std::string s_actionTable[MAX_KEYBINDS];
    static std::optional<MCMConfigParser::MCMAction> s_actionObjTable[MAX_KEYBINDS];
    static std::string s_modNameTable[MAX_KEYBINDS];
    static std::string s_keybindIdTable[MAX_KEYBINDS];   // MCM keybind id (SendEvent control name)
    static std::chrono::steady_clock::time_point s_pressTimeTable[MAX_KEYBINDS];  // for OnControlUp held duration
    static size_t s_nextSlot = 0;

    // Template thunk generator — dispatches the action for a given slot.
    // Structured (object-form) actions take priority since they preserve the
    // target form and typed params; legacy string actions are the fallback.
    // SendEvent actions deliver OnControlDown here and OnControlUp from the
    // matching release thunk, mirroring the real MCM's input handler.
    template <size_t N>
    static void __stdcall KeybindThunk() {
        if constexpr (N < MAX_KEYBINDS) {
            if (s_actionObjTable[N].has_value()) {
                if (s_actionObjTable[N]->type == "SendEvent") {
                    s_pressTimeTable[N] = std::chrono::steady_clock::now();
                    MCMPapyrusDispatch::SendControlEvent(
                        s_actionObjTable[N]->form, s_keybindIdTable[N], /*down=*/true, 0.0f);
                } else {
                    MCMPapyrusDispatch::ExecuteStructuredAction(
                        *s_actionObjTable[N], s_modNameTable[N], "",
                        MCMPapyrusDispatch::ControlValue{});  // no control value for keybinds
                }
            } else if (!s_actionTable[N].empty()) {
                MCMPapyrusDispatch::ExecuteAction(s_actionTable[N], s_modNameTable[N]);
            }
        }
    }

    // Release thunk — only wired up for SendEvent keybinds. OnControlUp's
    // second argument is the held duration in seconds (matches real MCM).
    template <size_t N>
    static void __stdcall KeybindUpThunk() {
        if constexpr (N < MAX_KEYBINDS) {
            if (s_actionObjTable[N].has_value() && s_actionObjTable[N]->type == "SendEvent") {
                const float held = std::chrono::duration<float>(
                    std::chrono::steady_clock::now() - s_pressTimeTable[N]).count();
                MCMPapyrusDispatch::SendControlEvent(
                    s_actionObjTable[N]->form, s_keybindIdTable[N], /*down=*/false, held);
            }
        }
    }

    // Thunk table — unfortunately we need a fixed set of thunks since the API
    // requires plain function pointers. We pre-instantiate a reasonable number.
    using ThunkFn = void(__stdcall*)();
    static ThunkFn s_thunkTable[] = {
        &KeybindThunk<0>,  &KeybindThunk<1>,  &KeybindThunk<2>,  &KeybindThunk<3>,
        &KeybindThunk<4>,  &KeybindThunk<5>,  &KeybindThunk<6>,  &KeybindThunk<7>,
        &KeybindThunk<8>,  &KeybindThunk<9>,  &KeybindThunk<10>, &KeybindThunk<11>,
        &KeybindThunk<12>, &KeybindThunk<13>, &KeybindThunk<14>, &KeybindThunk<15>,
        &KeybindThunk<16>, &KeybindThunk<17>, &KeybindThunk<18>, &KeybindThunk<19>,
        &KeybindThunk<20>, &KeybindThunk<21>, &KeybindThunk<22>, &KeybindThunk<23>,
        &KeybindThunk<24>, &KeybindThunk<25>, &KeybindThunk<26>, &KeybindThunk<27>,
        &KeybindThunk<28>, &KeybindThunk<29>, &KeybindThunk<30>, &KeybindThunk<31>,
        &KeybindThunk<32>, &KeybindThunk<33>, &KeybindThunk<34>, &KeybindThunk<35>,
        &KeybindThunk<36>, &KeybindThunk<37>, &KeybindThunk<38>, &KeybindThunk<39>,
        &KeybindThunk<40>, &KeybindThunk<41>, &KeybindThunk<42>, &KeybindThunk<43>,
        &KeybindThunk<44>, &KeybindThunk<45>, &KeybindThunk<46>, &KeybindThunk<47>,
        &KeybindThunk<48>, &KeybindThunk<49>, &KeybindThunk<50>, &KeybindThunk<51>,
        &KeybindThunk<52>, &KeybindThunk<53>, &KeybindThunk<54>, &KeybindThunk<55>,
        &KeybindThunk<56>, &KeybindThunk<57>, &KeybindThunk<58>, &KeybindThunk<59>,
        &KeybindThunk<60>, &KeybindThunk<61>, &KeybindThunk<62>, &KeybindThunk<63>,
    };
    static ThunkFn s_upThunkTable[] = {
        &KeybindUpThunk<0>,  &KeybindUpThunk<1>,  &KeybindUpThunk<2>,  &KeybindUpThunk<3>,
        &KeybindUpThunk<4>,  &KeybindUpThunk<5>,  &KeybindUpThunk<6>,  &KeybindUpThunk<7>,
        &KeybindUpThunk<8>,  &KeybindUpThunk<9>,  &KeybindUpThunk<10>, &KeybindUpThunk<11>,
        &KeybindUpThunk<12>, &KeybindUpThunk<13>, &KeybindUpThunk<14>, &KeybindUpThunk<15>,
        &KeybindUpThunk<16>, &KeybindUpThunk<17>, &KeybindUpThunk<18>, &KeybindUpThunk<19>,
        &KeybindUpThunk<20>, &KeybindUpThunk<21>, &KeybindUpThunk<22>, &KeybindUpThunk<23>,
        &KeybindUpThunk<24>, &KeybindUpThunk<25>, &KeybindUpThunk<26>, &KeybindUpThunk<27>,
        &KeybindUpThunk<28>, &KeybindUpThunk<29>, &KeybindUpThunk<30>, &KeybindUpThunk<31>,
        &KeybindUpThunk<32>, &KeybindUpThunk<33>, &KeybindUpThunk<34>, &KeybindUpThunk<35>,
        &KeybindUpThunk<36>, &KeybindUpThunk<37>, &KeybindUpThunk<38>, &KeybindUpThunk<39>,
        &KeybindUpThunk<40>, &KeybindUpThunk<41>, &KeybindUpThunk<42>, &KeybindUpThunk<43>,
        &KeybindUpThunk<44>, &KeybindUpThunk<45>, &KeybindUpThunk<46>, &KeybindUpThunk<47>,
        &KeybindUpThunk<48>, &KeybindUpThunk<49>, &KeybindUpThunk<50>, &KeybindUpThunk<51>,
        &KeybindUpThunk<52>, &KeybindUpThunk<53>, &KeybindUpThunk<54>, &KeybindUpThunk<55>,
        &KeybindUpThunk<56>, &KeybindUpThunk<57>, &KeybindUpThunk<58>, &KeybindUpThunk<59>,
        &KeybindUpThunk<60>, &KeybindUpThunk<61>, &KeybindUpThunk<62>, &KeybindUpThunk<63>,
    };

    void RegisterFromFile(const std::filesystem::path& keybindsPath, const std::string& modName) {
        std::ifstream file(keybindsPath);
        if (!file.is_open()) {
            logger::warn("[MCMKeybindTranslator] Cannot open keybinds.json for '{}'", modName);
            return;
        }

        nlohmann::json root;
        try {
            // ignore_comments: real MCM reads these with JsonCpp, which accepts
            // // comments — mod-shipped keybinds.json files may contain them.
            root = nlohmann::json::parse(file, nullptr, true, /*ignore_comments=*/true);
        } catch (const nlohmann::json::parse_error& e) {
            logger::error("[MCMKeybindTranslator] Parse error in '{}': {}", keybindsPath.string(), e.what());
            return;
        }

        if (!root.is_array()) {
            // Handle wrapper format: { "modName": "...", "keybinds": [...] }
            if (root.is_object() && root.contains("keybinds") && root["keybinds"].is_array()) {
                root = root["keybinds"];
            } else {
                logger::warn("[MCMKeybindTranslator] keybinds.json for '{}' has unrecognized format", modName);
                return;
            }
        }

        for (const auto& entry : root) {
            if (s_nextSlot >= 64) {
                logger::warn("[MCMKeybindTranslator] Maximum keybind slots (64) exhausted");
                break;
            }

            MCMKeybind kb;
            kb.modName = modName;

            if (entry.contains("id") && entry["id"].is_string()) kb.id = entry["id"].get<std::string>();
            if (entry.contains("desc") && entry["desc"].is_string()) kb.desc = entry["desc"].get<std::string>();

            // Action can be a string or the standard MCM object form.
            // Object-form actions are parsed fully (form + typed params).
            if (entry.contains("action")) {
                if (entry["action"].is_string()) {
                    kb.action = entry["action"].get<std::string>();
                } else if (entry["action"].is_object()) {
                    kb.actionObj = MCMConfigParser::ParseAction(entry["action"]);
                }
            }

            if (entry.contains("defaultKey") && entry["defaultKey"].is_number())
                kb.defaultKey = entry["defaultKey"].get<unsigned int>();
            else
                kb.defaultKey = 0;

            if (kb.id.empty()) continue;
            if (kb.action.empty() && !kb.actionObj.has_value()) {
                // All four real-MCM action types (CallFunction, CallGlobalFunction,
                // RunConsoleCommand, SendEvent) are dispatchable; anything that
                // still fails ParseAction is malformed or unknown. Register it
                // anyway so the binding is visible and rebindable — the real MCM
                // would show it too. The thunk just does nothing.
                logger::warn("[MCMKeybindTranslator] Keybind '{}' of '{}' has an unrecognized/invalid action — registering display-only",
                    kb.id, modName);
            }

            // Register with the framework's hotkey system
            std::string hotkeyId = "MCM." + modName + "." + kb.id;

            // Store action in the dispatch table
            size_t slot = s_nextSlot++;
            s_actionTable[slot] = kb.action;
            s_actionObjTable[slot] = kb.actionObj;
            s_modNameTable[slot] = modName;
            s_keybindIdTable[slot] = kb.id;

            kb.hotkeyHandle = HotkeyManager::Register(hotkeyId.c_str(), kb.defaultKey, s_thunkTable[slot]);

            // SendEvent keybinds also need OnControlUp with the held duration —
            // wire the matching key-up thunk (dispatched from WM_KEYUP).
            if (kb.actionObj.has_value() && kb.actionObj->type == "SendEvent") {
                HotkeyManager::SetReleaseCallback(hotkeyId.c_str(), s_upThunkTable[slot]);
            }

            // Route framework-side binding changes back to MCM's Keybinds.json,
            // then import the user's saved MCM binding (that file is the source
            // of truth shared with the real MCM, so it wins over both the
            // config default and any stale framework INI value).
            MCMKeybindStore::RegisterMapping(hotkeyId, modName, kb.id);
            if (auto savedDik = MCMKeybindStore::GetSavedDIK(modName, kb.id); savedDik.has_value()) {
                HotkeyManager::ImportBinding(hotkeyId.c_str(), *savedDik);
            }

            s_keybinds.push_back(std::move(kb));
        }

        logger::info("[MCMKeybindTranslator] Registered {} keybind(s) for '{}'",
            s_keybinds.size(), modName);
    }

    void UnregisterMod(const std::string& modName) {
        auto it = std::remove_if(s_keybinds.begin(), s_keybinds.end(),
            [&](const MCMKeybind& kb) {
                if (kb.modName == modName) {
                    HotkeyManager::Unregister(kb.hotkeyHandle);
                    return true;
                }
                return false;
            });
        s_keybinds.erase(it, s_keybinds.end());
    }

    const std::vector<MCMKeybind>& GetAllKeybinds() {
        return s_keybinds;
    }

} // namespace MCMKeybindTranslator
