#include "MCM/MCMValueProvider.h"
#include "MCM/MCMScanner.h"
#include "MCM/PapyrusFunctionArgs.h"
#include <fstream>
#include <sstream>
#include <map>
#include <mutex>
#include <memory>
#include <set>
#include <SimpleIni.h>

namespace MCMValueProvider {

    // Use unique_ptr since CSimpleIniA is non-copyable
    static std::map<std::string, std::unique_ptr<CSimpleIniA>> s_settingsData;
    static std::map<std::string, std::filesystem::path> s_settingsPaths;
    // Mods whose in-memory INI we actually modified since the last flush.
    // CRITICAL: only these may ever be written to disk. Rewriting untouched
    // mods' files corrupted them once already (see StripBOMs) — a file we
    // never changed must never be touched.
    static std::set<std::string> s_dirtyMods;
    static std::mutex s_mutex;

    // Loads the layered settings INI for one mod into a fresh CSimpleIniA.
    // Layer order (later loads override earlier keys):
    //   1. Data/MCM/Config/<Mod>/settings.ini        — mod-shipped defaults
    //   2. Data/MCM/Settings/<Mod>/settings.ini      — legacy folder-style user file
    //   3. Data/MCM/Settings/<Mod>.ini               — canonical flat user file (what real MCM writes)
    // Returns nullptr if none of the files exist.
    // NOTE: caller must hold s_mutex.
    static std::unique_ptr<CSimpleIniA> LoadLayeredSettings(const std::string& modName) {
        auto ini = std::make_unique<CSimpleIniA>();
        ini->SetUnicode();
        bool loaded = false;

        auto defaultsPath = MCMScanner::GetScanBasePath() / modName / "settings.ini";
        if (std::filesystem::exists(defaultsPath) &&
            ini->LoadFile(defaultsPath.string().c_str()) >= 0) {
            loaded = true;
            logger::debug("[MCMValueProvider] Loaded default settings.ini for '{}'", modName);
        }

        auto legacyUserPath = MCMScanner::GetUserSettingsBasePath() / modName / "settings.ini";
        if (std::filesystem::exists(legacyUserPath) &&
            ini->LoadFile(legacyUserPath.string().c_str()) >= 0) {
            loaded = true;
            logger::debug("[MCMValueProvider] Loaded legacy user settings for '{}'", modName);
        }

        auto flatUserPath = MCMScanner::GetUserSettingsBasePath() / (modName + ".ini");
        if (std::filesystem::exists(flatUserPath) &&
            ini->LoadFile(flatUserPath.string().c_str()) >= 0) {
            loaded = true;
            logger::info("[MCMValueProvider] Loaded user settings for '{}' ({})",
                modName, flatUserPath.string());
        }

        return loaded ? std::move(ini) : nullptr;
    }

    // Registers a mod's settings cache entry, creating an empty INI if no
    // files exist yet (so subsequent writes still work). Caller holds s_mutex.
    static CSimpleIniA* EnsureModLoaded(const std::string& modName) {
        auto it = s_settingsData.find(modName);
        if (it != s_settingsData.end() && it->second) {
            return it->second.get();
        }

        auto ini = LoadLayeredSettings(modName);
        if (!ini) {
            // Nothing on disk yet — start with an empty store so SetModSetting
            // calls (from Papyrus or the UI) can create the file on flush.
            ini = std::make_unique<CSimpleIniA>();
            ini->SetUnicode();
        }

        // Write path: ALWAYS the canonical flat file Data/MCM/Settings/<Mod>.ini,
        // matching real MCM's SettingStore::CommitModSetting behavior so both
        // systems (and any mod tooling) agree on where user values live.
        s_settingsPaths[modName] = MCMScanner::GetUserSettingsBasePath() / (modName + ".ini");

        auto* raw = ini.get();
        s_settingsData[modName] = std::move(ini);
        return raw;
    }

    // One-time repair pass: strip UTF-8 BOMs from the user settings INIs.
    //
    // Earlier framework builds saved through CSimpleIni::SaveFile with its
    // default a_bAddSignature=true, which prepends a UTF-8 BOM (EF BB BF).
    // The native MCM reads these files with the Windows profile APIs
    // (GetPrivateProfileSectionNames / GetPrivateProfileSection), and with a
    // BOM in front of the first "[", the FIRST SECTION of the file becomes
    // invisible to those APIs — so the native MCM silently loses every value
    // in it (settings then read as missing: -1/0, e.g. the "all volume
    // sliders reset to 0" incident). Our own SimpleIni reader is BOM-tolerant,
    // which is why the translated menus still showed the correct values while
    // the native MCM did not. Heal any file we polluted.
    static void StripBOMs() {
        const auto dir = MCMScanner::GetUserSettingsBasePath();
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec)) {
            return;
        }

        std::size_t repaired = 0;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            auto path = entry.path();
            if (path.extension() != L".ini") continue;

            std::ifstream in(path, std::ios::binary);
            if (!in.is_open()) continue;
            std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close();

            if (data.size() < 3 ||
                static_cast<unsigned char>(data[0]) != 0xEF ||
                static_cast<unsigned char>(data[1]) != 0xBB ||
                static_cast<unsigned char>(data[2]) != 0xBF) {
                continue;  // no BOM — untouched by the bug
            }

            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                logger::warn("[MCMValueProvider] Cannot repair BOM in '{}' (file locked?)", path.string());
                continue;
            }
            out.write(data.data() + 3, static_cast<std::streamsize>(data.size() - 3));
            ++repaired;
            logger::info("[MCMValueProvider] Stripped UTF-8 BOM from '{}'", path.filename().string());
        }

        if (repaired > 0) {
            logger::warn("[MCMValueProvider] Repaired {} BOM-corrupted settings file(s) — the native MCM "
                "could not read the first section of these files until now", repaired);
        }
    }

    void Init(const std::vector<MCMScanner::MCMModInfo>& mods) {
        // Heal BOM-corrupted files BEFORE loading anything, so both this
        // provider and the native MCM (which reads at its own pace) see
        // clean files from here on.
        StripBOMs();

        std::lock_guard lock(s_mutex);

        for (const auto& mod : mods) {
            EnsureModLoaded(mod.modName);
        }

        logger::info("[MCMValueProvider] Initialized with {} mod settings file(s)", s_settingsData.size());
    }

    // --- ModSettings (INI) Provider ---

    static ValueResult GetModSetting(const std::string& modName, const MCMConfigParser::ValueSource& source) {
        ValueResult result;
        std::lock_guard lock(s_mutex);

        auto* ini = EnsureModLoaded(modName);
        if (!ini) {
            result.stringVal = source.defaultValue;
            if (source.type == MCMConfigParser::SourceType::ModSettingBool) {
                result.boolVal = (source.defaultValue == "1" || source.defaultValue == "true");
            } else if (source.type == MCMConfigParser::SourceType::ModSettingInt) {
                try { result.intVal = std::stoi(source.defaultValue); } catch (...) {}
            } else if (source.type == MCMConfigParser::SourceType::ModSettingFloat) {
                try { result.floatVal = std::stof(source.defaultValue); } catch (...) {}
            }
            return result;
        }

        // Parse section from settingName (format: "key:Section" or just "key")
        std::string section = "Main";
        std::string key = source.settingName;
        auto colonPos = key.find(':');
        if (colonPos != std::string::npos) {
            section = key.substr(colonPos + 1);
            key = key.substr(0, colonPos);
        }

        const char* val = ini->GetValue(section.c_str(), key.c_str());
        std::string strVal = val ? val : source.defaultValue;
        result.stringVal = strVal;

        // Populate ALL numeric fields regardless of the declared type — MCM
        // control types don't map 1:1 to storage types (e.g. a switcher can
        // be backed by ModSettingInt), so every consumer must find its value.
        switch (source.type) {
            case MCMConfigParser::SourceType::ModSettingBool:
                result.boolVal = (strVal == "1" || strVal == "true" || strVal == "True");
                result.intVal = result.boolVal ? 1 : 0;
                result.floatVal = result.boolVal ? 1.0f : 0.0f;
                break;
            case MCMConfigParser::SourceType::ModSettingInt:
                try { result.intVal = std::stoi(strVal); } catch (...) { result.intVal = 0; }
                result.boolVal = result.intVal != 0;
                result.floatVal = static_cast<float>(result.intVal);
                break;
            case MCMConfigParser::SourceType::ModSettingFloat:
                try { result.floatVal = std::stof(strVal); } catch (...) { result.floatVal = 0.0f; }
                result.intVal = static_cast<int>(result.floatVal);
                result.boolVal = result.floatVal != 0.0f;
                break;
            case MCMConfigParser::SourceType::ModSettingString:
                break;
            default:
                break;
        }

        return result;
    }

    static ProviderStatus SetModSetting(const std::string& modName, const MCMConfigParser::ValueSource& source, const std::string& value) {
        std::lock_guard lock(s_mutex);

        auto* ini = EnsureModLoaded(modName);

        std::string section = "Main";
        std::string key = source.settingName;
        auto colonPos = key.find(':');
        if (colonPos != std::string::npos) {
            section = key.substr(colonPos + 1);
            key = key.substr(0, colonPos);
        }

        ini->SetValue(section.c_str(), key.c_str(), value.c_str());
        s_dirtyMods.insert(modName);  // only dirty mods are ever flushed to disk
        return ProviderStatus::Available;
    }

    // --- GlobalValue Provider ---

    // Parse MCM sourceForm format: "PluginName.esp|LocalFormID" → runtime form ID
    // Local form ID is in hex. Uses the plugin's load order index to compute full form ID.
    static RE::TESForm* ResolveFormFromSourceForm(const std::string& sourceForm) {
        auto pipePos = sourceForm.find('|');
        if (pipePos == std::string::npos) {
            // Try parsing as a raw hex form ID (fallback)
            try {
                uint32_t formId = static_cast<uint32_t>(std::stoul(sourceForm, nullptr, 16));
                return RE::TESForm::GetFormByID(formId);
            } catch (...) {
                return nullptr;
            }
        }

        std::string pluginName = sourceForm.substr(0, pipePos);
        std::string localIdStr = sourceForm.substr(pipePos + 1);

        // Trim whitespace from localIdStr
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

        // Search regular (full) plugins first
        for (auto* file : handler->compiledFileCollection.files) {
            if (!file) continue;
            std::string_view fname = file->GetFilename();
            if (_stricmp(fname.data(), pluginName.c_str()) == 0) {
                uint32_t fullFormId = (static_cast<uint32_t>(file->GetCompileIndex()) << 24) | localFormId;
                return RE::TESForm::GetFormByID(fullFormId);
            }
        }

        // Search light (ESL-flagged) plugins
        for (auto* file : handler->compiledFileCollection.smallFiles) {
            if (!file) continue;
            std::string_view fname = file->GetFilename();
            if (_stricmp(fname.data(), pluginName.c_str()) == 0) {
                uint32_t fullFormId = 0xFE000000u |
                    (static_cast<uint32_t>(file->GetSmallFileCompileIndex()) << 12) |
                    (localFormId & 0xFFF);
                return RE::TESForm::GetFormByID(fullFormId);
            }
        }

        return nullptr;
    }

    static ValueResult GetGlobalValue(const MCMConfigParser::ValueSource& source) {
        ValueResult result;

        auto* form = ResolveFormFromSourceForm(source.sourceForm);
        if (!form) {
            result.status = ProviderStatus::FormNotLoaded;
            result.statusMessage = "Form " + source.sourceForm + " not loaded (plugin may be missing)";
            try { result.floatVal = std::stof(source.defaultValue); } catch (...) {}
            return result;
        }

        auto* global = form->As<RE::TESGlobal>();
        if (!global) {
            result.status = ProviderStatus::FormNotLoaded;
            result.statusMessage = "Form is not a TESGlobal";
            return result;
        }

        result.floatVal = global->value;
        result.intVal = static_cast<int>(global->value);
        result.boolVal = global->value != 0.0f;
        result.stringVal = std::to_string(static_cast<int>(global->value));
        return result;
    }

    static ProviderStatus SetGlobalValue(const MCMConfigParser::ValueSource& source, float val) {
        auto* form = ResolveFormFromSourceForm(source.sourceForm);
        if (!form) return ProviderStatus::FormNotLoaded;

        auto* global = form->As<RE::TESGlobal>();
        if (!global) return ProviderStatus::FormNotLoaded;

        global->value = val;
        return ProviderStatus::Available;
    }

    // --- PropertyValue Provider ---
    // Accesses Papyrus script properties through the VM's GetPropertyValue /
    // SetPropertyValue virtuals (the same route the real MCM uses), which
    // handle property->variable resolution and fire Papyrus Get/Set handlers.
    //
    // Reads are asynchronous: the VM delivers the value to a callback functor
    // on its own thread, so the renderer polls TryTakePropertyResult() each
    // frame and starts from the config default until the real value arrives.

    // MCM configs frequently omit "scriptName" for PropertyValue sources (the
    // real MCM resolves the script attached to the form automatically). The
    // fork of CommonLibF4 we build against doesn't expose the VM's attached-
    // scripts table as a member, but its layout is stable: verified identical
    // (lock @ 0xBDF8, map @ 0xBE00) in both the pre-NG (1.10.163) and post-NG
    // (1.10.980+) CommonLibF4 definitions of BSScript::Internal::VirtualMachine.
    static constexpr std::ptrdiff_t VM_ATTACHED_SCRIPTS_LOCK_OFFSET = 0xBDF8;
    static constexpr std::ptrdiff_t VM_ATTACHED_SCRIPTS_MAP_OFFSET  = 0xBE00;

    // Engine's Internal::AttachedScript = BSTPointerAndFlags<BSTSmartPointer<Object>, 1>:
    // an Object* whose low bit is a flag. Mask the low bits to recover the pointer
    // (objects are heap-allocated, so at least 8-byte aligned).
    struct RawAttachedScript {
        std::uintptr_t bits;
        [[nodiscard]] RE::BSScript::Object* get() const {
            return reinterpret_cast<RE::BSScript::Object*>(bits & ~std::uintptr_t(7));
        }
    };
    static_assert(sizeof(RawAttachedScript) == 0x8);

    using AttachedScriptsMap =
        RE::BSTHashMap<std::uint64_t, RE::BSTSmallSharedArray<RawAttachedScript>>;

    // Returns strong references to every script object bound to the handle.
    static std::vector<RE::BSTSmartPointer<RE::BSScript::Object>> GetObjectsForHandle(
        RE::BSScript::IVirtualMachine* vmRaw, std::uint64_t handle)
    {
        std::vector<RE::BSTSmartPointer<RE::BSScript::Object>> result;
        auto* base = reinterpret_cast<std::byte*>(vmRaw);
        auto* lock = reinterpret_cast<RE::BSSpinLock*>(base + VM_ATTACHED_SCRIPTS_LOCK_OFFSET);
        auto* map  = reinterpret_cast<AttachedScriptsMap*>(base + VM_ATTACHED_SCRIPTS_MAP_OFFSET);

        lock->lock();
        // Layout sanity check — a wildly implausible element count means the
        // offsets don't match this runtime, so bail rather than walk garbage.
        if (map->size() < 0x100000) {
            auto it = map->find(handle);
            if (it != map->end()) {
                for (const auto& raw : it->second) {
                    if (auto* obj = raw.get(); obj) {
                        result.emplace_back(obj);  // BSTSmartPointer ctor IncRefs
                    }
                }
            }
        } else {
            logger::warn("[MCMValueProvider] attachedScripts layout sanity check failed — skipping script auto-resolution");
        }
        lock->unlock();
        return result;
    }

    // Resolves the candidate script objects for a PropertyValue source.
    // With an explicit scriptName only that bound object is returned; with an
    // empty scriptName every script attached to the form is a candidate and
    // callers probe each until the property resolves (matching real MCM
    // behavior, where scriptName is optional in valueOptions).
    static std::vector<RE::BSTSmartPointer<RE::BSScript::Object>> ResolveCandidateObjects(
        RE::TESForm* form, const std::string& scriptName)
    {
        std::vector<RE::BSTSmartPointer<RE::BSScript::Object>> result;

        auto* vm = RE::GameVM::GetSingleton();
        if (!vm || !vm->GetVM() || !form) return result;

        auto* virtualMachine = vm->GetVM().get();
        auto& handlePolicy = virtualMachine->GetObjectHandlePolicy();

        auto handle = handlePolicy.GetHandleForObject(
            static_cast<std::uint32_t>(form->GetFormType()), form);
        if (handle == handlePolicy.EmptyHandle()) return result;

        if (!scriptName.empty()) {
            RE::BSTSmartPointer<RE::BSScript::Object> scriptObj;
            if (virtualMachine->FindBoundObject(handle, scriptName.c_str(), false, scriptObj, false) && scriptObj) {
                result.push_back(std::move(scriptObj));
            }
            return result;
        }

        return GetObjectsForHandle(virtualMachine, handle);
    }

    // --- Async read plumbing ---

    struct AsyncReadState {
        bool done = false;      // result is ready for pickup
        bool consumed = false;  // renderer already took it
        ValueResult result;
    };

    static std::mutex s_asyncMutex;
    static std::map<std::string, AsyncReadState> s_asyncReads;

    // Converts the Variable delivered by the VM into a ValueResult with every
    // numeric field populated so any control type can consume it.
    static ValueResult VariableToResult(const RE::BSScript::Variable& var) {
        ValueResult r;
        r.status = ProviderStatus::Available;

        if (var.is<bool>()) {
            r.boolVal = RE::BSScript::get<bool>(var);
            r.intVal = r.boolVal ? 1 : 0;
            r.floatVal = r.boolVal ? 1.0f : 0.0f;
            r.stringVal = r.boolVal ? "1" : "0";
        } else if (var.is<std::int32_t>()) {
            r.intVal = RE::BSScript::get<std::int32_t>(var);
            r.boolVal = r.intVal != 0;
            r.floatVal = static_cast<float>(r.intVal);
            r.stringVal = std::to_string(r.intVal);
        } else if (var.is<float>()) {
            r.floatVal = RE::BSScript::get<float>(var);
            r.intVal = static_cast<int>(r.floatVal);
            r.boolVal = r.floatVal != 0.0f;
            r.stringVal = std::to_string(r.floatVal);
        } else if (var.is<RE::BSFixedString>()) {
            auto s = RE::BSScript::get<RE::BSFixedString>(var);
            r.stringVal = s.c_str() ? s.c_str() : "";
        } else {
            // Object/struct/array properties can't be shown in a widget.
            r.status = ProviderStatus::Error;
        }
        return r;
    }

    // Callback functor handed to IVirtualMachine::GetPropertyValue. The VM
    // invokes operator() on its own thread with the property's value; we stash
    // it for the render thread to pick up. Lifetime is refcount-managed via
    // BSIntrusiveRefCounted (both our BSTSmartPointer and the engine inc/dec
    // the same counter; whoever drops the last reference deletes through the
    // virtual destructor).
    class PropertyReadCallback final : public RE::BSScript::IStackCallbackFunctor {
    public:
        explicit PropertyReadCallback(std::string key) : _key(std::move(key)) {}

        void CallQueued() override {}
        void CallCanceled() override {
            std::lock_guard lock(s_asyncMutex);
            auto& st = s_asyncReads[_key];
            st.result.status = ProviderStatus::Error;
            st.result.statusMessage = "Property read cancelled by VM";
            st.done = true;
        }
        void StartMultiDispatch() override {}
        void EndMultiDispatch() override {}
        void operator()(RE::BSScript::Variable a_result) override {
            std::lock_guard lock(s_asyncMutex);
            auto& st = s_asyncReads[_key];
            st.result = VariableToResult(a_result);
            st.done = true;
        }
        bool CanSave() override { return false; }

    private:
        std::string _key;
    };

    void RequestPropertyRead(const std::string& requestKey, const MCMConfigParser::ValueSource& source) {
        {
            std::lock_guard lock(s_asyncMutex);
            if (s_asyncReads.contains(requestKey)) return;  // already in flight / done
            s_asyncReads.emplace(requestKey, AsyncReadState{});
        }

        auto storeFailure = [&](ProviderStatus status, std::string msg) {
            std::lock_guard lock(s_asyncMutex);
            auto& st = s_asyncReads[requestKey];
            st.result.status = status;
            st.result.statusMessage = std::move(msg);
            st.done = true;
        };

        auto* form = ResolveFormFromSourceForm(source.sourceForm);
        if (!form) {
            storeFailure(ProviderStatus::FormNotLoaded,
                "Form " + source.sourceForm + " not loaded (plugin may be missing)");
            return;
        }

        auto* vm = RE::GameVM::GetSingleton();
        if (!vm || !vm->GetVM()) {
            storeFailure(ProviderStatus::Error, "Papyrus VM not available");
            return;
        }
        auto* virtualMachine = vm->GetVM().get();

        auto candidates = ResolveCandidateObjects(form, source.scriptName);
        if (candidates.empty()) {
            storeFailure(ProviderStatus::ScriptMissing,
                source.scriptName.empty()
                    ? "No script attached to form " + source.sourceForm
                    : "Script '" + source.scriptName + "' not bound to form");
            return;
        }

        // Probe candidates in order: GetPropertyValue returns false immediately
        // when the object type has no such property, so the first accepting
        // object owns the read (with an explicit scriptName there is only one).
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback{
            new PropertyReadCallback(requestKey) };
        for (auto& obj : candidates) {
            if (virtualMachine->GetPropertyValue(obj, source.propertyName.c_str(), callback)) {
                logger::debug("[MCMValueProvider] Dispatched property read '{}' on '{}'",
                    source.propertyName, obj->type ? obj->type->name.c_str() : "<unknown>");
                return;
            }
        }

        storeFailure(ProviderStatus::PropertyMissing,
            "Property '" + source.propertyName + "' not found on any script attached to " + source.sourceForm);
    }

    bool TryTakePropertyResult(const std::string& requestKey, ValueResult& out) {
        std::lock_guard lock(s_asyncMutex);
        auto it = s_asyncReads.find(requestKey);
        if (it == s_asyncReads.end() || !it->second.done || it->second.consumed) return false;
        out = it->second.result;
        it->second.consumed = true;
        return true;
    }

    void InvalidateAsyncPropertyReads() {
        std::lock_guard lock(s_asyncMutex);
        // Only forget finished reads — an in-flight callback still needs its
        // slot to land in (erasing it would just recreate the entry, but the
        // fresh dispatch triggered afterwards could then race the stale one).
        std::erase_if(s_asyncReads, [](const auto& kv) { return kv.second.done; });
    }

    static ValueResult GetPropertyValue(const MCMConfigParser::ValueSource& source) {
        ValueResult result;

        auto* form = ResolveFormFromSourceForm(source.sourceForm);
        if (!form) {
            result.status = ProviderStatus::FormNotLoaded;
            result.statusMessage = "Form " + source.sourceForm + " not loaded (plugin may be missing)";
            try { result.floatVal = std::stof(source.defaultValue); } catch (...) {}
            return result;
        }

        auto* vm = RE::GameVM::GetSingleton();
        if (!vm || !vm->GetVM()) {
            result.status = ProviderStatus::Error;
            result.statusMessage = "Papyrus VM not available";
            return result;
        }

        // Confirm at least one script object is reachable for this source; the
        // real value arrives via the async read the renderer kicks off.
        auto candidates = ResolveCandidateObjects(form, source.scriptName);
        if (candidates.empty()) {
            result.status = ProviderStatus::ScriptMissing;
            result.statusMessage = source.scriptName.empty()
                ? "No script attached to form " + source.sourceForm
                : "Script '" + source.scriptName + "' not bound to form";
            try { result.floatVal = std::stof(source.defaultValue); } catch (...) {}
            return result;
        }

        // Script confirmed. Report defaults until the async read completes.
        result.status = ProviderStatus::Available;

        try {
            if (source.type == MCMConfigParser::SourceType::PropertyValueFloat) {
                result.floatVal = std::stof(source.defaultValue);
                result.intVal = static_cast<int>(result.floatVal);
                result.stringVal = source.defaultValue;
            } else if (source.type == MCMConfigParser::SourceType::PropertyValueInt) {
                result.intVal = std::stoi(source.defaultValue);
                result.floatVal = static_cast<float>(result.intVal);
                result.stringVal = source.defaultValue;
            } else if (source.type == MCMConfigParser::SourceType::PropertyValueBool) {
                result.boolVal = (source.defaultValue == "1" || source.defaultValue == "true");
                result.intVal = result.boolVal ? 1 : 0;
                result.stringVal = source.defaultValue;
            } else {
                result.stringVal = source.defaultValue;
            }
        } catch (...) {}

        return result;
    }

    static ProviderStatus SetPropertyValueImpl(const MCMConfigParser::ValueSource& source, RE::BSScript::Variable& newVal) {
        auto* form = ResolveFormFromSourceForm(source.sourceForm);
        if (!form) return ProviderStatus::FormNotLoaded;

        auto* vm = RE::GameVM::GetSingleton();
        if (!vm || !vm->GetVM()) return ProviderStatus::Error;

        auto* virtualMachine = vm->GetVM().get();

        auto candidates = ResolveCandidateObjects(form, source.scriptName);
        if (candidates.empty()) {
            logger::warn("[MCMValueProvider] SetPropertyValue: no script object for '{}' on form 0x{}",
                source.scriptName.empty() ? "<auto>" : source.scriptName, source.sourceForm);
            return ProviderStatus::ScriptMissing;
        }

        // Probe candidates: SetPropertyValue returns false when the object's
        // type has no such property, so try each attached script until one
        // accepts the write. Triggers the property's Papyrus Set handler.
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> nullCallback;
        for (auto& obj : candidates) {
            if (virtualMachine->SetPropertyValue(obj, source.propertyName.c_str(), newVal, nullCallback)) {
                logger::debug("[MCMValueProvider] SetPropertyValue '{}' on '{}' succeeded",
                    source.propertyName, obj->type ? obj->type->name.c_str() : "<unknown>");
                return ProviderStatus::Available;
            }
        }

        logger::warn("[MCMValueProvider] SetPropertyValue '{}' failed on all {} script(s) of form 0x{}",
            source.propertyName, candidates.size(), source.sourceForm);
        return ProviderStatus::PropertyMissing;
    }

    // --- Public API ---

    ValueResult GetValue(const std::string& modName, const MCMConfigParser::ValueSource& source) {
        switch (source.type) {
            case MCMConfigParser::SourceType::ModSettingBool:
            case MCMConfigParser::SourceType::ModSettingInt:
            case MCMConfigParser::SourceType::ModSettingFloat:
            case MCMConfigParser::SourceType::ModSettingString:
                return GetModSetting(modName, source);

            case MCMConfigParser::SourceType::GlobalValue:
                return GetGlobalValue(source);

            case MCMConfigParser::SourceType::PropertyValueBool:
            case MCMConfigParser::SourceType::PropertyValueInt:
            case MCMConfigParser::SourceType::PropertyValueFloat:
            case MCMConfigParser::SourceType::PropertyValueString:
                return GetPropertyValue(source);

            default: {
                ValueResult r;
                r.stringVal = source.defaultValue;
                return r;
            }
        }
    }

    ProviderStatus SetBool(const std::string& modName, const MCMConfigParser::ValueSource& source, bool val) {
        switch (source.type) {
            case MCMConfigParser::SourceType::ModSettingBool:
                return SetModSetting(modName, source, val ? "1" : "0");
            // Switchers may be backed by int storage (MCM's storage table
            // allows Bool OR Int for switchers) — write 1/0 with the int type.
            case MCMConfigParser::SourceType::ModSettingInt:
                return SetModSetting(modName, source, val ? "1" : "0");
            case MCMConfigParser::SourceType::GlobalValue:
                return SetGlobalValue(source, val ? 1.0f : 0.0f);
            case MCMConfigParser::SourceType::PropertyValueBool: {
                RE::BSScript::Variable v;
                v = val;
                return SetPropertyValueImpl(source, v);
            }
            case MCMConfigParser::SourceType::PropertyValueInt: {
                RE::BSScript::Variable v;
                v = static_cast<std::int32_t>(val ? 1 : 0);
                return SetPropertyValueImpl(source, v);
            }
            case MCMConfigParser::SourceType::PropertyValueFloat: {
                RE::BSScript::Variable v;
                v = val ? 1.0f : 0.0f;
                return SetPropertyValueImpl(source, v);
            }
            default:
                return ProviderStatus::Error;
        }
    }

    ProviderStatus SetInt(const std::string& modName, const MCMConfigParser::ValueSource& source, int val) {
        switch (source.type) {
            case MCMConfigParser::SourceType::ModSettingInt:
                return SetModSetting(modName, source, std::to_string(val));
            case MCMConfigParser::SourceType::GlobalValue:
                return SetGlobalValue(source, static_cast<float>(val));
            case MCMConfigParser::SourceType::PropertyValueInt: {
                RE::BSScript::Variable v;
                v = static_cast<std::int32_t>(val);
                return SetPropertyValueImpl(source, v);
            }
            default:
                return ProviderStatus::Error;
        }
    }

    ProviderStatus SetFloat(const std::string& modName, const MCMConfigParser::ValueSource& source, float val) {
        switch (source.type) {
            case MCMConfigParser::SourceType::ModSettingFloat:
                return SetModSetting(modName, source, std::to_string(val));
            case MCMConfigParser::SourceType::GlobalValue:
                return SetGlobalValue(source, val);
            case MCMConfigParser::SourceType::PropertyValueFloat: {
                RE::BSScript::Variable v;
                v = val;
                return SetPropertyValueImpl(source, v);
            }
            default:
                return ProviderStatus::Error;
        }
    }

    ProviderStatus SetString(const std::string& modName, const MCMConfigParser::ValueSource& source, const std::string& val) {
        switch (source.type) {
            case MCMConfigParser::SourceType::ModSettingString:
                return SetModSetting(modName, source, val);
            case MCMConfigParser::SourceType::PropertyValueString: {
                RE::BSScript::Variable v;
                v = RE::BSFixedString(val.c_str());
                return SetPropertyValueImpl(source, v);
            }
            default:
                return ProviderStatus::Error;
        }
    }

    std::string GetStatusTooltip(ProviderStatus status, const MCMConfigParser::ValueSource& source) {
        switch (status) {
            case ProviderStatus::Available:
                return "";
            case ProviderStatus::FormNotLoaded:
                return "Requires form 0x" + source.sourceForm + " (plugin may not be loaded)";
            case ProviderStatus::ScriptMissing:
                return source.scriptName.empty()
                    ? "No script attached to form " + source.sourceForm
                    : "Script '" + source.scriptName + "' not found on form";
            case ProviderStatus::PropertyMissing:
                return source.scriptName.empty()
                    ? "Property '" + source.propertyName + "' not found on form " + source.sourceForm
                    : "Property '" + source.propertyName + "' not found on script '" + source.scriptName + "'";
            case ProviderStatus::Error:
                return "Unexpected error accessing value";
            default:
                return "Unknown status";
        }
    }

    // --- Raw mod-setting access (backs the MCM Papyrus natives) ---

    // Splits MCM's "key:Section" naming into its parts (section defaults to "Main").
    static void SplitSettingName(const std::string& settingName, std::string& key, std::string& section) {
        section = "Main";
        key = settingName;
        auto colonPos = key.find(':');
        if (colonPos != std::string::npos) {
            section = key.substr(colonPos + 1);
            key = key.substr(0, colonPos);
        }
    }

    std::optional<std::string> GetModSettingRaw(const std::string& modName, const std::string& settingName) {
        std::lock_guard lock(s_mutex);

        auto* ini = EnsureModLoaded(modName);
        std::string key, section;
        SplitSettingName(settingName, key, section);

        const char* val = ini->GetValue(section.c_str(), key.c_str());
        if (!val) return std::nullopt;
        return std::string(val);
    }

    void SetModSettingRaw(const std::string& modName, const std::string& settingName, const std::string& value) {
        {
            std::lock_guard lock(s_mutex);

            auto* ini = EnsureModLoaded(modName);
            std::string key, section;
            SplitSettingName(settingName, key, section);
            ini->SetValue(section.c_str(), key.c_str(), value.c_str());
            s_dirtyMods.insert(modName);
        }
        // Persist immediately — Papyrus callers expect the value to survive
        // even if the game crashes right after (real MCM commits per write too).
        // FlushAll only writes dirty mods, so this touches exactly one file.
        FlushAll();
    }

    void ReloadAll() {
        std::lock_guard lock(s_mutex);
        std::size_t reloaded = 0;
        for (auto& [modName, ini] : s_settingsData) {
            // Re-run the layered load (defaults -> legacy folder -> flat user
            // file). If nothing could be read from disk (files deleted, or a
            // transient VFS/lock failure) KEEP the previous cache — replacing
            // a populated cache with an empty one and later flushing it would
            // destroy the user's saved values.
            auto fresh = LoadLayeredSettings(modName);
            if (!fresh) {
                continue;
            }
            ini = std::move(fresh);
            ++reloaded;
        }
        // A reload discards any unflushed edits, so nothing is dirty anymore
        // (in practice every write flushes immediately; this is belt-and-braces).
        s_dirtyMods.clear();
        logger::info("[MCMValueProvider] Reloaded settings from disk for {} mod(s)", reloaded);
    }

    void FlushAll() {
        std::lock_guard lock(s_mutex);
        // ONLY mods whose values we changed this session get written. Never
        // rewrite untouched mods' files: a wholesale rewrite through SimpleIni
        // once added UTF-8 BOMs to every settings file, which made the first
        // section of each file invisible to the native MCM's Windows-profile-
        // API reader and effectively reset those mods' settings (the
        // "volume sliders all 0" incident). See StripBOMs() for the repair.
        for (auto it = s_dirtyMods.begin(); it != s_dirtyMods.end();) {
            const auto& modName = *it;
            auto dataIt = s_settingsData.find(modName);
            auto pathIt = s_settingsPaths.find(modName);
            if (dataIt == s_settingsData.end() || !dataIt->second || pathIt == s_settingsPaths.end()) {
                it = s_dirtyMods.erase(it);
                continue;
            }

            // Ensure the parent directory exists (creates Data/MCM/Settings/ if needed)
            auto parentDir = pathIt->second.parent_path();
            if (!parentDir.empty() && !std::filesystem::exists(parentDir)) {
                std::error_code ec;
                std::filesystem::create_directories(parentDir, ec);
                if (ec) {
                    logger::error("[MCMValueProvider] Failed to create settings dir '{}': {}",
                        parentDir.string(), ec.message());
                    ++it;  // keep dirty — retried on the next flush
                    continue;
                }
            }

            // a_bAddSignature=false: NEVER write a UTF-8 BOM. The native MCM
            // reads these files with GetPrivateProfileSection*, which cannot
            // see the first section of a BOM-prefixed file.
            if (dataIt->second->SaveFile(pathIt->second.string().c_str(), false) < 0) {
                logger::error("[MCMValueProvider] Failed to save '{}'", pathIt->second.string());
                ++it;  // keep dirty — retried on the next flush
                continue;
            }
            it = s_dirtyMods.erase(it);
        }
    }

} // namespace MCMValueProvider
