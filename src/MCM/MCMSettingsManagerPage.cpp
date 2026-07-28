#include "MCM/MCMSettingsManagerPage.h"
#include "MCM/M8rIniJson.h"
#include "MCM/MCMConfigParser.h"
#include "MCM/MCMKeybindStore.h"
#include "MCM/MCMPapyrusAPI.h"
#include "MCM/MCMPapyrusDispatch.h"
#include "MCM/MCMScanner.h"
#include "MCM/MCMTranslation.h"
#include "MCM/MCMValueProvider.h"
#include "MCM/MCMWidgetRenderer.h"
#include "Application.h"     // GetKeyName(dik, device)
#include "HotkeyManager.h"
#include "imgui.h"

#include <RE/Fallout.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

// ============================================================================
// Native recreation of "MCM Settings Manager" (Nexus 56195).
//
// Ported from the decompiled AS3 under
// PluginTemplate/MCM Settings Manager/_analysis/as3/scripts/M8r/:
//   McmSettingsReader.as   -> settings database build (which controls count)
//   McmSetting.as          -> storage paths, value get/set, compare, display
//   McmSettings.as         -> stored tree walks (counts, fake entries)
//   McmSettingsStorage.as  -> slot/preset persistence (M8rJSON + IniChunked)
//   MainGui.as             -> slot/preset list view + actions
//   DetailsView.as         -> per-slot details view + per-mod/setting actions
//   MCMSettingsManager.as  -> apply flow
//
// DATA-FORMAT COMPATIBILITY IS THE CONTRACT: slots and exports written here
// load in the original Flash manager and vice versa. The wire codec lives in
// MCM/M8rIniJson.* and is validated offline by swf/test/m8rjsontest.
// ============================================================================

namespace MCMSettingsManagerPage {

    namespace fs = std::filesystem;
    using M8rIniJson::Value;

    // The manager's own MCM identity (storage INI + translation folder name).
    static constexpr const char* kManagerMod = "_MCMSettingsManager";
    static constexpr const char* kManagerSection = "MCMSettingsManager";
    static constexpr int kMaxSlots = 10;
    // IniChunked parameters from McmSettingsStorage.as: new IniChunked(...,3,65000)
    static constexpr int kMaxChunks = 3;
    static constexpr size_t kMaxChunkBytes = 65000;

    static void NoOpHotkeyCallback() {}

    // ------------------------------------------------------------------
    // Translations (Data/MCM/Config/_MCMSettingsManager/Translation/)
    // ------------------------------------------------------------------
    static MCMTranslation::TranslationMap s_tr;
    static bool s_trLoaded = false;

    static std::string Tr(const std::string& key) {
        if (!s_trLoaded) {
            s_trLoaded = true;
            std::error_code ec;
            fs::path dir = MCMScanner::GetScanBasePath() / kManagerMod / "Translation";
            if (fs::exists(dir, ec)) {
                s_tr = MCMTranslation::LoadDirectory(dir);
            }
        }
        // ResolveAndStrip also converts the {newline} tokens the help texts use.
        return MCMTranslation::ResolveAndStrip(key, s_tr);
    }

    // "$CountSetting"/"$CountSettings" pluralization + {count} substitution,
    // exactly like helperBuildChangedNotice in MainGui.as.
    static std::string TrCount(const std::string& baseKey, int count) {
        std::string s = Tr(count == 1 ? baseKey : baseKey + "s");
        size_t pos;
        while ((pos = s.find("{count}")) != std::string::npos) {
            s.replace(pos, 7, std::to_string(count));
        }
        return s;
    }

    // ------------------------------------------------------------------
    // AS3 value coercions (loose equality / Number() / int() / Boolean())
    // ------------------------------------------------------------------

    // AS3 Number(string): trimmed, "" -> 0, invalid -> NaN.
    static double As3StringToNumber(const std::string& s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return 0.0;
        size_t e = s.find_last_not_of(" \t\r\n");
        std::string t = s.substr(b, e - b + 1);
        char* end = nullptr;
        double v = std::strtod(t.c_str(), &end);
        if (end != t.c_str() + t.size()) return std::numeric_limits<double>::quiet_NaN();
        return v;
    }

    // AS3 Number(x) for our value kinds (arithmetic context: null -> 0).
    static double As3ToNumber(const Value& v) {
        switch (v.kind) {
            case Value::Kind::Int:    return static_cast<double>(v.intVal);
            case Value::Kind::Double: return v.dblVal;
            case Value::Kind::Bool:   return v.boolVal ? 1.0 : 0.0;
            case Value::Kind::String: return As3StringToNumber(v.strVal);
            case Value::Kind::Null:   return 0.0;
            default:                  return std::numeric_limits<double>::quiet_NaN();
        }
    }

    // AS3 int(x) = ToInt32 (modular wrap).
    static int As3ToInt(const Value& v) {
        double d = As3ToNumber(v);
        if (std::isnan(d) || std::isinf(d)) return 0;
        return static_cast<int>(static_cast<uint32_t>(static_cast<int64_t>(d)));
    }

    // AS3 Boolean(x): number != 0, string non-empty (yes, "false" is true).
    static bool As3ToBool(const Value& v) {
        switch (v.kind) {
            case Value::Kind::Int:    return v.intVal != 0;
            case Value::Kind::Double: return v.dblVal != 0.0 && !std::isnan(v.dblVal);
            case Value::Kind::Bool:   return v.boolVal;
            case Value::Kind::String: return !v.strVal.empty();
            default:                  return false;
        }
    }

    // AS3 String(x).
    static std::string As3ToString(const Value& v) {
        switch (v.kind) {
            case Value::Kind::Int:    return std::to_string(v.intVal);
            case Value::Kind::Double: return M8rIniJson::NumberToString(v.dblVal);
            case Value::Kind::Bool:   return v.boolVal ? "true" : "false";
            case Value::Kind::String: return v.strVal;
            case Value::Kind::Null:   return "null";
            default:                  return "";
        }
    }

    // AS3 loose equality (==) for the kinds that occur in slots.
    static bool LooseEq(const Value& a, const Value& b) {
        if (a.kind == Value::Kind::Null || b.kind == Value::Kind::Null) {
            return a.kind == Value::Kind::Null && b.kind == Value::Kind::Null;
        }
        if (a.kind == Value::Kind::Object || b.kind == Value::Kind::Object) {
            return false;  // AS3 compares object references — never our case
        }
        if (a.kind == Value::Kind::String && b.kind == Value::Kind::String) {
            return a.strVal == b.strVal;
        }
        // Mixed / numeric: both sides convert to Number (NaN compares false).
        return As3ToNumber(a) == As3ToNumber(b);
    }

    // ------------------------------------------------------------------
    // Managed setting (port of McmSetting)
    // ------------------------------------------------------------------

    struct ManagedSetting {
        // Storage base type (path[1]): "ini", "glb", "prp", "prs", "hot".
        std::string baseType;
        // DB grouping key: the mod page this control lives on.
        std::string groupMod;
        // path[0]: the element's mod (honors per-control modName override).
        std::string elementMod;
        std::vector<std::string> path;        // full storage path
        std::string pathString;               // joined with '>'
        std::string label;                    // control display text (translated)
        std::string controlType;              // "switcher", "stepper", ...
        std::string controlId;                // for OnMCMSettingChange
        MCMConfigParser::ValueSource source;  // provider access (non-hotkey)
        std::string hotkeyMgrId;              // "MCM.<mod>.<id>" (hotkey only)
        std::string hotkeyCtrlId;             // keybind id within the mod
        bool boolReadAsInt = false;           // ModSettingInt with 'b' key quirk
        std::vector<MCMConfigParser::OptionItem> options;  // stepper/dropdown display
        std::optional<MCMConfigParser::MCMAction> actionObj;
        std::string actionStr;

        // isFloat() from McmSetting.as: GlobalValue / ModSettingFloat /
        // PropertyValueFloat use epsilon compare instead of loose equality.
        bool IsFloatKind() const {
            using ST = MCMConfigParser::SourceType;
            return source.type == ST::GlobalValue ||
                   source.type == ST::ModSettingFloat ||
                   source.type == ST::PropertyValueFloat;
        }
        bool IsBoolKind() const {
            using ST = MCMConfigParser::SourceType;
            return source.type == ST::ModSettingBool ||
                   source.type == ST::PropertyValueBool ||
                   controlType == "switcher";
        }
        bool IsHotkey() const { return baseType == "hot"; }
        bool IsProperty() const { return baseType == "prp" || baseType == "prs"; }
    };

    // ------------------------------------------------------------------
    // Session state
    // ------------------------------------------------------------------

    struct SlotCache {
        bool loaded = false;
        // nullopt = slot holds no data (free / deleted / corrupt).
        std::optional<Value> data;
    };

    // One modal dialog at a time; kind selects the layout.
    struct Modal {
        enum class Kind { None, Alert, Confirm, Input };
        Kind kind = Kind::None;
        std::string text;
        char input[160]{};
        std::function<void()> onYes;                    // Confirm
        std::function<void(const std::string&)> onInput; // Input
        bool openRequested = false;
    };

    struct Session {
        bool built = false;
        std::vector<ManagedSetting> db;              // ordered settings database
        std::map<std::string, size_t> dbByPath;      // pathString -> index
        std::vector<std::string> modNames;           // sorted DB group mods
        std::map<std::string, std::vector<size_t>> byMod;
        std::map<std::string, std::string> modFullNames;

        // Current live values keyed by pathString. Absent = not yet read
        // (async property) or unreadable. `unavailable` marks failed reads.
        std::map<std::string, Value> current;
        std::set<std::string> pendingProps;          // async reads in flight
        std::set<std::string> unavailable;

        std::array<SlotCache, kMaxSlots + 1> slots;  // index 1..10
        bool presetsLoaded = false;
        std::map<std::string, Value> presets;        // baseName -> stored tree

        // View state
        bool detailsOpen = false;
        bool detailsIsSlot = false;                  // false = preset
        int detailsSlot = 0;
        std::string detailsPreset;
        bool filterOnlyStored = true;                // DetailsView defaults
        bool filterOnlyChanged = false;
        std::map<std::string, bool> modOpen;         // collapse state per mod

        Modal modal;
        Modal pendingModal;                          // chained confirm -> input
        bool firstStartChecked = false;

        // Count caches — recomputing CountChanged scans the whole DB with
        // deep tree lookups; doing that per slot per frame is measurable on
        // large modlists, so counts refresh only when something changed.
        struct RowCounts { bool hasData = false; int all = 0, changed = 0, mods = 0; };
        bool countsDirty = true;
        std::array<RowCounts, kMaxSlots + 1> slotCounts;
        std::map<std::string, RowCounts> presetCounts;
        int lastSlot = 1, nextFree = 0;
        std::map<std::string, RowCounts> detailModCounts;  // per-mod, viewed tree
        std::string detailCountsKey;                       // which tree they belong to
    };

    static Session s;

    // ------------------------------------------------------------------
    // Raw storage access (the manager's own MCM settings INI)
    // ------------------------------------------------------------------

    static std::string GetRaw(const std::string& key) {
        auto v = MCMValueProvider::GetModSettingRaw(kManagerMod, key + ":" + kManagerSection);
        return v.value_or("");
    }

    static void SetRaw(const std::string& key, const std::string& value) {
        MCMValueProvider::SetModSettingRaw(kManagerMod, key + ":" + kManagerSection, value);
    }

    static std::string ReadSlotName(int slot) {
        std::string n = GetRaw("sSettingsSlotName" + std::to_string(slot));
        return n.empty() ? ("Slot " + std::to_string(slot)) : n;
    }

    static void WriteSlotName(int slot, const std::string& name) {
        SetRaw("sSettingsSlotName" + std::to_string(slot), name);
    }

    // Port of McmSettingsStorage.readSlotSettings: chunked first, then the
    // legacy single-key fallback; any corruption / parse failure -> no data.
    static std::optional<Value> ReadSlotFromDisk(int slot) {
        const std::string keyBase = "SettingsSlot" + std::to_string(slot);
        std::string log;
        auto payload = M8rIniJson::ReadChunked(keyBase, kMaxChunks, GetRaw, &log);
        if (!log.empty()) {
            logger::debug("[MCMSettingsManager] Slot {}: {}", slot, log);
        }
        std::string text;
        if (payload.has_value()) {
            text = *payload;
        }
        if (text.empty()) {
            text = GetRaw("s" + keyBase);  // pre-chunking legacy storage
        }
        if (text.empty()) return std::nullopt;
        auto parsed = M8rIniJson::Parse(text);
        if (!parsed.has_value()) {
            logger::warn("[MCMSettingsManager] Slot {} contains unparseable data — treated as empty", slot);
            return std::nullopt;
        }
        return parsed;
    }

    static SlotCache& GetSlot(int slot) {
        SlotCache& sc = s.slots[static_cast<size_t>(slot)];
        if (!sc.loaded) {
            sc.loaded = true;
            sc.data = ReadSlotFromDisk(slot);
        }
        return sc;
    }

    // Port of writeSlotSettings: data==nullptr deletes the slot (writes an
    // empty payload, which the reader treats as no data — same as the AS3).
    static void WriteSlot(int slot, const Value* data) {
        const std::string keyBase = "SettingsSlot" + std::to_string(slot);
        const std::string payload = data ? M8rIniJson::Stringify(*data) : "";
        std::string log;
        if (!M8rIniJson::WriteChunked(keyBase, kMaxChunks, kMaxChunkBytes, payload, GetRaw, SetRaw, &log)) {
            logger::warn("[MCMSettingsManager] Slot {} write incomplete: {}", slot, log);
        } else {
            logger::info("[MCMSettingsManager] Slot {} written: {}", slot, log);
        }
        // Clear the legacy single-key storage if an old install left one.
        if (!GetRaw("s" + keyBase).empty()) {
            SetRaw("s" + keyBase, "");
        }
        SlotCache& sc = s.slots[static_cast<size_t>(slot)];
        sc.loaded = true;
        if (data) sc.data = *data; else sc.data.reset();
        s.countsDirty = true;
    }

    // Port of writeExportSettings: single un-chunked key in a separate INI
    // ("MCM Exported Settings.ini"), 65000-byte cap, verified by read-back.
    static bool WriteExport(const Value& data) {
        const std::string json = M8rIniJson::Stringify(data);
        if (json.size() > 65000) return false;
        MCMValueProvider::SetModSettingRaw("MCM Exported Settings", "sSettings:MCMSettings", json);
        auto back = MCMValueProvider::GetModSettingRaw("MCM Exported Settings", "sSettings:MCMSettings");
        return back.has_value() && *back == json;
    }

    // ------------------------------------------------------------------
    // Presets (Data/MCM/Settings/Presets/*.ini, read-only)
    // ------------------------------------------------------------------

    static void LoadPresets() {
        if (s.presetsLoaded) return;
        s.presetsLoaded = true;
        s.presets.clear();

        std::error_code ec;
        fs::path dir = MCMScanner::GetUserSettingsBasePath() / "Presets";
        if (!fs::exists(dir, ec)) return;

        for (auto it = fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const fs::path& p = it->path();
            if (!p.has_extension()) continue;
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext != ".ini") continue;

            // Read the whole file (wide path — preset names may be non-ANSI)
            // and pull [MCMSettings] sSettings out with a light scan; preset
            // files are written by the export function so the format is known.
            std::ifstream f(p, std::ios::binary);
            if (!f) continue;
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

            bool inSection = false;
            std::string json;
            size_t pos = 0;
            while (pos < content.size()) {
                size_t eol = content.find('\n', pos);
                std::string line = content.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
                pos = eol == std::string::npos ? content.size() : eol + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
                size_t b = line.find_first_not_of(" \t");
                if (b == std::string::npos) continue;
                if (line[b] == '[') {
                    inSection = line.find("[MCMSettings]", b) == b;
                    continue;
                }
                if (!inSection) continue;
                size_t eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string key = line.substr(b, eq - b);
                while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
                if (key != "sSettings") continue;
                size_t vb = line.find_first_not_of(" \t", eq + 1);
                json = vb == std::string::npos ? "" : line.substr(vb);
                break;
            }
            if (json.empty()) {
                logger::warn("[MCMSettingsManager] Invalid content structure for preset: {}",
                             p.filename().string());
                continue;
            }
            auto parsed = M8rIniJson::Parse(json);
            if (!parsed.has_value()) {
                logger::warn("[MCMSettingsManager] Failed to parse preset: {}", p.filename().string());
                continue;
            }
            // Preset name = file stem, kept as UTF-8 (names can be non-ANSI).
            const std::u8string stemU8 = p.stem().u8string();
            std::string base(reinterpret_cast<const char*>(stemU8.data()), stemU8.size());
            s.presets[base] = std::move(*parsed);
            logger::info("[MCMSettingsManager] Loaded preset: {}", base);
        }
    }

    // ------------------------------------------------------------------
    // Settings database build (port of McmSettingsReader.readMcmModSettings)
    // ------------------------------------------------------------------

    // Splits "key:Section" like the AS3 (param2.id.split(":")). A missing
    // section yields AS3's String(undefined) = "undefined" as the object key,
    // which is what the Flash manager writes for such controls — matching it
    // keeps slot files interchangeable.
    static void SplitSettingName(const std::string& settingName, std::string& key, std::string& section) {
        size_t colon = settingName.find(':');
        if (colon == std::string::npos) {
            key = settingName;
            section = "undefined";
        } else {
            key = settingName.substr(0, colon);
            section = settingName.substr(colon + 1);
        }
    }

    static void EnsureHotkeyRegistered(const ManagedSetting& ms) {
        // Same lazy registration the widget renderer performs, so GetBinding
        // works even when the mod's own page was never opened this session.
        if (!HotkeyManager::IsRegistered(ms.hotkeyMgrId.c_str())) {
            HotkeyManager::Register(ms.hotkeyMgrId.c_str(), 0, &NoOpHotkeyCallback);
            MCMKeybindStore::RegisterMapping(ms.hotkeyMgrId, ms.groupMod, ms.hotkeyCtrlId);
            if (auto saved = MCMKeybindStore::GetSavedDIK(ms.groupMod, ms.hotkeyCtrlId); saved.has_value()) {
                HotkeyManager::ImportBinding(ms.hotkeyMgrId.c_str(), *saved);
            }
        }
    }

    // Reads a setting's live value in AS3 form (Int / Double / String).
    // Returns nullopt when the value can't be read synchronously (async
    // property still pending) or is unavailable.
    static std::optional<Value> ReadCurrentSync(const ManagedSetting& ms) {
        using ST = MCMConfigParser::SourceType;

        if (ms.IsHotkey()) {
            EnsureHotkeyRegistered(ms);
            unsigned int dik = HotkeyManager::GetBinding(ms.hotkeyMgrId.c_str());
            unsigned int vk = dik != 0 ? MCMKeybindStore::DIKToVK(dik) : 0;
            int mods = 0;
            if (auto raw = MCMKeybindStore::GetSavedRaw(ms.groupMod, ms.hotkeyCtrlId);
                raw.has_value() && static_cast<unsigned int>(raw->first) == vk) {
                mods = raw->second;
            }
            // "keycode;modifiers" — MCM's own wire form (GetKeybind port).
            return Value::MakeString(std::to_string(vk) + ";" + std::to_string(mods));
        }

        switch (ms.source.type) {
            case ST::ModSettingBool: {
                auto r = MCMValueProvider::GetValue(ms.elementMod, ms.source);
                if (r.status != MCMValueProvider::ProviderStatus::Available) return std::nullopt;
                return Value::MakeInt(r.boolVal ? 1 : 0);  // AS3 stores bools as 1/0
            }
            case ST::ModSettingInt: {
                // 'b'-prefixed int keys are read as bool then 1/0 — the AS3
                // quirk exists because such keys can hold "true"/"false" text.
                if (ms.boolReadAsInt) {
                    MCMConfigParser::ValueSource asBool = ms.source;
                    asBool.type = ST::ModSettingBool;
                    auto r = MCMValueProvider::GetValue(ms.elementMod, asBool);
                    if (r.status != MCMValueProvider::ProviderStatus::Available) return std::nullopt;
                    return Value::MakeInt(r.boolVal ? 1 : 0);
                }
                auto r = MCMValueProvider::GetValue(ms.elementMod, ms.source);
                if (r.status != MCMValueProvider::ProviderStatus::Available) return std::nullopt;
                return Value::MakeInt(r.intVal);
            }
            case ST::ModSettingFloat: {
                auto r = MCMValueProvider::GetValue(ms.elementMod, ms.source);
                if (r.status != MCMValueProvider::ProviderStatus::Available) return std::nullopt;
                return Value::MakeDouble(static_cast<double>(r.floatVal));
            }
            case ST::ModSettingString: {
                auto r = MCMValueProvider::GetValue(ms.elementMod, ms.source);
                if (r.status != MCMValueProvider::ProviderStatus::Available) return std::nullopt;
                return Value::MakeString(r.stringVal);
            }
            case ST::GlobalValue: {
                auto r = MCMValueProvider::GetValue(ms.elementMod, ms.source);
                if (r.status != MCMValueProvider::ProviderStatus::Available) return std::nullopt;
                return Value::MakeDouble(static_cast<double>(r.floatVal));
            }
            default:
                return std::nullopt;  // property types are read asynchronously
        }
    }

    static std::string PropRequestKey(const ManagedSetting& ms) {
        return "MCMSM|" + ms.pathString;
    }

    // Converts a completed async property result to the AS3 value form.
    static Value PropResultToValue(const ManagedSetting& ms, const MCMValueProvider::ValueResult& r) {
        using ST = MCMConfigParser::SourceType;
        switch (ms.source.type) {
            case ST::PropertyValueBool:   return Value::MakeInt(r.boolVal ? 1 : 0);
            case ST::PropertyValueInt:    return Value::MakeInt(r.intVal);
            case ST::PropertyValueFloat:  return Value::MakeDouble(static_cast<double>(r.floatVal));
            case ST::PropertyValueString: return Value::MakeString(r.stringVal);
            default:                      return Value{};
        }
    }

    static void BuildDatabase() {
        s.db.clear();
        s.dbByPath.clear();
        s.modNames.clear();
        s.byMod.clear();
        s.modFullNames.clear();
        s.current.clear();
        s.pendingProps.clear();
        s.unavailable.clear();

        // Control types with no persistable value (readMcmModSettings skip set).
        static const std::set<std::string> kSkipTypes = {
            "section", "text", "spacer", "image", "button", "hiddenSwitcher"
        };

        std::set<std::string> seenPaths;  // _onlyOncePlease

        MCMWidgetRenderer::VisitMods([&](const std::string& modName,
                                         const MCMConfigParser::MCMModConfig& config) {
            if (modName == kManagerMod) return;  // our own page has no settings
            s.modFullNames[modName] = config.displayName.empty() ? modName : config.displayName;

            for (const auto& page : config.pages) {
                for (const auto& ctrl : page.controls) {
                    if (kSkipTypes.count(ctrl.type)) continue;
                    const bool isHotkey = ctrl.type == "hotkey" || ctrl.type == "keyinput";
                    if (!isHotkey && ctrl.valueSource.type == MCMConfigParser::SourceType::None) continue;
                    if (isHotkey && ctrl.id.empty()) continue;

                    ManagedSetting ms;
                    ms.groupMod = modName;
                    ms.elementMod = ctrl.modNameOverride.empty() ? modName : ctrl.modNameOverride;
                    ms.controlType = ctrl.type;
                    ms.controlId = ctrl.id;
                    ms.label = ctrl.text;
                    ms.source = ctrl.valueSource;
                    ms.actionObj = ctrl.actionObj;
                    ms.actionStr = ctrl.action;
                    // Options for named display (resolve sharedOptions refs).
                    ms.options = ctrl.options;
                    if (ms.options.empty() && !ctrl.optionsFrom.empty()) {
                        auto it = config.sharedOptions.find(ctrl.optionsFrom);
                        if (it != config.sharedOptions.end()) ms.options = it->second;
                    }

                    using ST = MCMConfigParser::SourceType;
                    if (isHotkey) {
                        // Hotkeys are identified by the OWNING mod (keybinds.json
                        // identity) — matching both the AS3 and the renderer.
                        ms.baseType = "hot";
                        ms.elementMod = modName;
                        ms.hotkeyCtrlId = ctrl.id;
                        ms.hotkeyMgrId = "MCM." + modName + "." + ctrl.id;
                        ms.path = { modName, "hot", ctrl.id };
                    } else if (ms.source.type == ST::GlobalValue) {
                        size_t bar = ms.source.sourceForm.find('|');
                        if (bar == std::string::npos) continue;  // invalid, like the AS3
                        ms.baseType = "glb";
                        ms.path = { ms.elementMod, "glb",
                                    ms.source.sourceForm.substr(0, bar),
                                    ms.source.sourceForm.substr(bar + 1) };
                    } else if (ms.source.type == ST::PropertyValueBool || ms.source.type == ST::PropertyValueInt ||
                               ms.source.type == ST::PropertyValueFloat || ms.source.type == ST::PropertyValueString) {
                        size_t bar = ms.source.sourceForm.find('|');
                        if (bar == std::string::npos) continue;
                        const std::string plugin = ms.source.sourceForm.substr(0, bar);
                        const std::string formId = ms.source.sourceForm.substr(bar + 1);
                        if (!ms.source.scriptName.empty()) {
                            ms.baseType = "prs";
                            ms.path = { ms.elementMod, "prs", plugin, formId,
                                        ms.source.propertyName, ms.source.scriptName };
                        } else {
                            ms.baseType = "prp";
                            ms.path = { ms.elementMod, "prp", plugin, formId, ms.source.propertyName };
                        }
                    } else if (ms.source.type == ST::ModSettingBool || ms.source.type == ST::ModSettingInt ||
                               ms.source.type == ST::ModSettingFloat || ms.source.type == ST::ModSettingString) {
                        std::string key, section;
                        SplitSettingName(ms.source.settingName, key, section);
                        if (key.empty()) continue;
                        ms.baseType = "ini";
                        ms.boolReadAsInt = ms.source.type == ST::ModSettingInt && key[0] == 'b';
                        // AS3 path: [modName, "ini", Section, Key]
                        ms.path = { ms.elementMod, "ini", section, key };
                    } else {
                        continue;
                    }

                    ms.pathString.clear();
                    for (size_t i = 0; i < ms.path.size(); ++i) {
                        if (i) ms.pathString += '>';
                        ms.pathString += ms.path[i];
                    }
                    if (!seenPaths.insert(ms.pathString).second) continue;  // dedupe

                    // Current value: sync sources read now (settings whose read
                    // fails are dropped, like addSettingToDatabase's null check);
                    // property sources are requested asynchronously.
                    if (ms.IsProperty()) {
                        MCMValueProvider::RequestPropertyRead(PropRequestKey(ms), ms.source);
                        s.pendingProps.insert(ms.pathString);
                    } else {
                        auto v = ReadCurrentSync(ms);
                        if (!v.has_value()) continue;
                        s.current[ms.pathString] = std::move(*v);
                    }

                    size_t idx = s.db.size();
                    s.db.push_back(std::move(ms));
                    s.dbByPath[s.db[idx].pathString] = idx;
                    auto& group = s.byMod[s.db[idx].groupMod];
                    if (group.empty()) s.modNames.push_back(s.db[idx].groupMod);
                    group.push_back(idx);
                }
            }
        });

        std::sort(s.modNames.begin(), s.modNames.end());
        logger::info("[MCMSettingsManager] Database built: {} settings across {} mod(s)",
                     s.db.size(), s.modNames.size());
    }

    // Called every frame: collect completed async property reads.
    static void PumpPropertyReads() {
        if (s.pendingProps.empty()) return;
        std::vector<std::string> done;
        for (const auto& pathString : s.pendingProps) {
            auto it = s.dbByPath.find(pathString);
            if (it == s.dbByPath.end()) { done.push_back(pathString); continue; }
            const ManagedSetting& ms = s.db[it->second];
            MCMValueProvider::ValueResult r;
            if (MCMValueProvider::TryTakePropertyResult(PropRequestKey(ms), r)) {
                if (r.status == MCMValueProvider::ProviderStatus::Available) {
                    s.current[pathString] = PropResultToValue(ms, r);
                } else {
                    // The AS3 drops unreadable settings from the DB entirely;
                    // we keep the row but exclude it from counts/compares.
                    s.unavailable.insert(pathString);
                }
                done.push_back(pathString);
            }
        }
        for (const auto& k : done) s.pendingProps.erase(k);
        if (!done.empty()) s.countsDirty = true;
    }

    // Re-reads all synchronously readable current values and re-requests the
    // async ones (resetRealValuesCache + readMcmModSettings equivalent).
    static void RefreshCurrentValues() {
        MCMValueProvider::ReloadAll();
        MCMValueProvider::InvalidateAsyncPropertyReads();
        for (const auto& ms : s.db) {
            if (ms.IsProperty()) {
                MCMValueProvider::RequestPropertyRead(PropRequestKey(ms), ms.source);
                s.pendingProps.insert(ms.pathString);
                s.unavailable.erase(ms.pathString);
            } else {
                auto v = ReadCurrentSync(ms);
                if (v.has_value()) s.current[ms.pathString] = std::move(*v);
            }
        }
        s.countsDirty = true;
    }

    // ------------------------------------------------------------------
    // Compares and counts
    // ------------------------------------------------------------------

    static const Value* StoredLeaf(const Value& stored, const ManagedSetting& ms) {
        return stored.Find(ms.path);
    }

    static bool HasStored(const Value& stored, const ManagedSetting& ms) {
        return StoredLeaf(stored, ms) != nullptr;
    }

    // Port of differentToSavedValue. A setting with no readable current value
    // is never "different" (the AS3 wouldn't have it in the DB at all).
    static bool DifferentToStored(const Value& stored, const ManagedSetting& ms) {
        const Value* leaf = StoredLeaf(stored, ms);
        if (!leaf) return false;  // callers check HasStored separately
        auto curIt = s.current.find(ms.pathString);
        if (curIt == s.current.end()) return false;
        const Value& cur = curIt->second;

        if (ms.IsFloatKind()) {
            double c = As3ToNumber(cur);
            double v = As3ToNumber(*leaf);
            if (std::isnan(c) != std::isnan(v)) return true;
            double diff = std::abs(v - c);
            return !std::isnan(diff) && diff > 1e-10;
        }
        return !LooseEq(cur, *leaf);
    }

    // Structural count of stored leaves (port of readSettingsCount's tree
    // walk — leaf depth depends on the base type key).
    static int CountStoredLeaves(const Value& stored, const std::string* onlyMod) {
        if (!stored.IsObject()) return 0;
        int count = 0;
        for (const auto& [modName, types] : stored.object) {
            if (onlyMod && modName != *onlyMod) continue;
            if (!types.IsObject()) continue;
            for (const auto& [type, l1] : types.object) {
                if (!l1.IsObject()) continue;
                for (const auto& [k1, l2] : l1.object) {
                    if (type == "hot") { ++count; continue; }
                    if (!l2.IsObject()) continue;
                    for (const auto& [k2, l3] : l2.object) {
                        if (type == "glb" || type == "ini") { ++count; continue; }
                        if (type != "prp" && type != "prs") continue;
                        if (!l3.IsObject()) continue;
                        for (const auto& [k3, l4] : l3.object) {
                            if (type == "prp") { ++count; continue; }
                            if (!l4.IsObject()) continue;
                            count += static_cast<int>(l4.object.size());  // prs
                        }
                    }
                }
            }
        }
        return count;
    }

    // Count of DB settings that are stored AND differ from current
    // (readSettingsCount with the changed flag). Mod filter matches path[0].
    static int CountChanged(const Value& stored, const std::string* onlyMod) {
        int count = 0;
        for (const auto& ms : s.db) {
            if (onlyMod && ms.path[0] != *onlyMod) continue;
            if (HasStored(stored, ms) && DifferentToStored(stored, ms)) ++count;
        }
        return count;
    }

    static int CountStoredMods(const Value& stored) {
        return stored.IsObject() ? static_cast<int>(stored.object.size()) : 0;
    }

    // ------------------------------------------------------------------
    // Value display (port of getNamedValue)
    // ------------------------------------------------------------------

    // MCM Keybinds.json modifiers bitmask; display-only. NOT re-verified
    // against the MCM sources — the wire value is passed through untouched
    // either way, so a wrong name here can't corrupt data.
    static std::string ModifierPrefix(int mods) {
        std::string out;
        if (mods & 1) out += "Shift+";
        if (mods & 2) out += "Ctrl+";
        if (mods & 4) out += "Alt+";
        return out;
    }

    static std::string NamedValue(const ManagedSetting& ms, const Value& v) {
        if (ms.IsBoolKind()) {
            return As3ToBool(v) ? "ON" : "OFF";
        }
        if ((ms.controlType == "stepper" || ms.controlType == "dropdown") && !ms.options.empty()) {
            double d = As3ToNumber(v);
            if (!std::isnan(d) && d == std::floor(d) && d >= 0 && d < static_cast<double>(ms.options.size())) {
                return ms.options[static_cast<size_t>(d)].text;
            }
        }
        if (ms.IsHotkey()) {
            const std::string sv = As3ToString(v);
            size_t semi = sv.find(';');
            if (semi != std::string::npos) {
                int vk = std::atoi(sv.substr(0, semi).c_str());
                int mods = std::atoi(sv.substr(semi + 1).c_str());
                if (vk == 0) return "---";
                unsigned int dik = MCMKeybindStore::VKToDIK(static_cast<unsigned int>(vk));
                std::string name = dik != 0
                    ? GetKeyName(static_cast<int>(dik), RE::INPUT_DEVICE::kKeyboard)
                    : ("VK " + std::to_string(vk));
                return ModifierPrefix(mods) + name;
            }
        }
        return As3ToString(v);
    }

    // ------------------------------------------------------------------
    // Apply / save primitives (port of McmSetting.setValue + apply flow)
    // ------------------------------------------------------------------

    // Fires the control's MCM action and the OnMCMSettingChange events after
    // a value was applied — mirroring emulateSetValueByMCM.
    static void FireValueChanged(const ManagedSetting& ms, const Value& newVal) {
        using ST = MCMConfigParser::SourceType;
        MCMPapyrusDispatch::ControlValue cv;
        switch (ms.source.type) {
            case ST::GlobalValue:
            case ST::ModSettingFloat:
            case ST::PropertyValueFloat:
                cv = static_cast<float>(As3ToNumber(newVal));
                break;
            case ST::ModSettingInt:
            case ST::PropertyValueInt:
                cv = As3ToInt(newVal);
                break;
            case ST::ModSettingBool:
            case ST::PropertyValueBool:
                cv = As3ToBool(newVal);
                break;
            default:
                cv = As3ToString(newVal);
                break;
        }
        if (ms.actionObj.has_value()) {
            MCMPapyrusDispatch::ExecuteStructuredAction(*ms.actionObj, ms.groupMod,
                                                        ms.source.sourceForm, cv);
        } else if (!ms.actionStr.empty()) {
            MCMPapyrusDispatch::ExecuteActionOnForm(ms.actionStr, ms.groupMod, ms.source.sourceForm);
        }
        if (!ms.controlId.empty()) {
            MCMPapyrusAPI::DispatchSettingChanged(ms.elementMod, ms.controlId);
        }
    }

    // Writes one stored value into the live game. Returns false when the
    // write could not be performed (untranslatable hotkey, unknown type).
    static bool ApplyValue(const ManagedSetting& ms, const Value& v) {
        using ST = MCMConfigParser::SourceType;

        if (ms.IsHotkey()) {
            const std::string sv = As3ToString(v);
            size_t semi = sv.find(';');
            if (semi == std::string::npos) return false;
            int vk = As3ToInt(Value::MakeString(sv.substr(0, semi)));
            int mods = As3ToInt(Value::MakeString(sv.substr(semi + 1)));
            EnsureHotkeyRegistered(ms);
            if (vk == 0) {
                HotkeyManager::SetBinding(ms.hotkeyMgrId.c_str(), 0);
            } else {
                unsigned int dik = MCMKeybindStore::VKToDIK(static_cast<unsigned int>(vk));
                if (dik == 0) {
                    logger::warn("[MCMSettingsManager] Hotkey '{}' has untranslatable keycode {} — skipped",
                                 ms.pathString, vk);
                    return false;
                }
                // Known limitation: the framework's hotkey system has no
                // modifier support, so a stored modifier mask is preserved in
                // the slot but not enforced on the live binding.
                if (mods != 0) {
                    logger::info("[MCMSettingsManager] Hotkey '{}' stored with modifiers {} — "
                                 "modifiers are not applied by the framework", ms.pathString, mods);
                }
                HotkeyManager::SetBinding(ms.hotkeyMgrId.c_str(), dik);
            }
            FireValueChanged(ms, v);
            s.current[ms.pathString] = Value::MakeString(sv);
            s.countsDirty = true;
            return true;
        }

        MCMValueProvider::ProviderStatus st = MCMValueProvider::ProviderStatus::Error;
        switch (ms.source.type) {
            case ST::ModSettingFloat:
            case ST::PropertyValueFloat:
                st = MCMValueProvider::SetFloat(ms.elementMod, ms.source, static_cast<float>(As3ToNumber(v)));
                break;
            case ST::GlobalValue:
                st = MCMValueProvider::SetFloat(ms.elementMod, ms.source, static_cast<float>(As3ToNumber(v)));
                break;
            case ST::ModSettingInt:
            case ST::PropertyValueInt:
                st = MCMValueProvider::SetInt(ms.elementMod, ms.source, As3ToInt(v));
                break;
            case ST::ModSettingBool:
            case ST::PropertyValueBool:
                st = MCMValueProvider::SetBool(ms.elementMod, ms.source, As3ToBool(v));
                break;
            case ST::ModSettingString:
            case ST::PropertyValueString:
                st = MCMValueProvider::SetString(ms.elementMod, ms.source, As3ToString(v));
                break;
            default:
                return false;
        }
        if (st != MCMValueProvider::ProviderStatus::Available) {
            logger::warn("[MCMSettingsManager] Failed to apply '{}' (provider status {})",
                         ms.pathString, static_cast<int>(st));
            return false;
        }
        MCMValueProvider::FlushAll();
        FireValueChanged(ms, v);
        // Optimistic cache update (the AS3 sets mcmElmObj.value the same way);
        // property writes land asynchronously but are assumed to succeed.
        s.current[ms.pathString] = v;
        s.countsDirty = true;
        return true;
    }

    static void ShowAlert(const std::string& text);

    // Port of MCMSettingsManager.applyModSettings: apply every stored setting
    // that differs, for one mod ("" = all), then report the count.
    static void ApplyStored(const Value& stored, const std::string& onlyGroupMod) {
        int changed = 0;
        for (const auto& ms : s.db) {
            if (!onlyGroupMod.empty() && ms.groupMod != onlyGroupMod) continue;
            if (!HasStored(stored, ms) || !DifferentToStored(stored, ms)) continue;
            const Value* leaf = StoredLeaf(stored, ms);
            if (leaf && ApplyValue(ms, *leaf)) ++changed;
        }
        // Other translated pages re-read their values on next open (the
        // renderer resets control states per page transition), so no explicit
        // "Reload MCM" lock screen is needed here.
        std::string msg = Tr("$M8rMCMSM_settingsApplied");
        size_t p;
        while ((p = msg.find("{count}")) != std::string::npos) {
            msg.replace(p, 7, std::to_string(changed));
        }
        ShowAlert(msg);
        logger::info("[MCMSettingsManager] Applied {} setting(s) ({})", changed,
                     onlyGroupMod.empty() ? "all mods" : onlyGroupMod.c_str());
        // The original re-reads all live values after an apply pass
        // (updateMultipleCheckSettingsChanged) — actions fired by applied
        // controls may have changed other settings as a side effect.
        if (changed > 0) RefreshCurrentValues();
    }

    // Snapshot of every current value as a stored tree (getCurrentSettings().getCopy()).
    static Value BuildCurrentTree() {
        Value root = Value::MakeObject();
        for (const auto& ms : s.db) {
            auto it = s.current.find(ms.pathString);
            if (it == s.current.end()) continue;
            root.Set(ms.path, it->second);
        }
        return root;
    }

    // ------------------------------------------------------------------
    // Modal dialogs (alert / confirm / input)
    // ------------------------------------------------------------------

    static void ShowAlert(const std::string& text) {
        s.pendingModal.kind = Modal::Kind::Alert;
        s.pendingModal.text = text;
        s.pendingModal.onYes = nullptr;
        s.pendingModal.onInput = nullptr;
        s.pendingModal.openRequested = true;
    }

    static void ShowConfirm(const std::string& text, std::function<void()> onYes) {
        s.pendingModal.kind = Modal::Kind::Confirm;
        s.pendingModal.text = text;
        s.pendingModal.onYes = std::move(onYes);
        s.pendingModal.onInput = nullptr;
        s.pendingModal.openRequested = true;
    }

    static void ShowInput(const std::string& text, const std::string& defaultValue,
                          std::function<void(const std::string&)> onInput) {
        s.pendingModal.kind = Modal::Kind::Input;
        s.pendingModal.text = text;
        s.pendingModal.onYes = nullptr;
        s.pendingModal.onInput = std::move(onInput);
        std::snprintf(s.pendingModal.input, sizeof(s.pendingModal.input), "%s", defaultValue.c_str());
        s.pendingModal.openRequested = true;
    }

    static void RenderModal() {
        // Promote a queued dialog once nothing is open (allows confirm->input
        // chains scheduled from inside a callback).
        if (s.modal.kind == Modal::Kind::None && s.pendingModal.openRequested) {
            s.modal = std::move(s.pendingModal);
            s.pendingModal = Modal{};
            ImGui::OpenPopup("##MCMSMDialog");
        }
        if (s.modal.kind == Modal::Kind::None) return;

        ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Appearing);
        // Modal popup: gets nav/gamepad focus automatically and blocks the
        // page behind it (same pattern as the hotkey conflict dialog).
        if (ImGui::BeginPopupModal("##MCMSMDialog", nullptr,
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar)) {
            ImGui::TextWrapped("%s", s.modal.text.c_str());
            ImGui::Spacing();

            auto close = [&](bool runYes, bool runInput) {
                auto onYes = std::move(s.modal.onYes);
                auto onInput = std::move(s.modal.onInput);
                std::string input = s.modal.input;
                s.modal = Modal{};
                ImGui::CloseCurrentPopup();
                if (runYes && onYes) onYes();
                if (runInput && onInput) onInput(input);
            };

            if (s.modal.kind == Modal::Kind::Alert) {
                if (ImGui::Button("OK", ImVec2(120, 0))) close(false, false);
                ImGui::SetItemDefaultFocus();
            } else if (s.modal.kind == Modal::Kind::Confirm) {
                if (ImGui::Button("Yes", ImVec2(120, 0))) close(true, false);
                ImGui::SameLine();
                if (ImGui::Button("No", ImVec2(120, 0))) close(false, false);
                ImGui::SetItemDefaultFocus();  // default to the safe choice
            } else if (s.modal.kind == Modal::Kind::Input) {
                ImGui::SetNextItemWidth(-1.0f);
                bool enter = ImGui::InputText("##mcmsm_input", s.modal.input, sizeof(s.modal.input),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::Spacing();
                if (ImGui::Button("OK", ImVec2(120, 0)) || enter) close(false, true);
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 0))) close(false, false);
            }
            // Gamepad B / Escape also dismisses without running callbacks.
            if (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
                close(false, false);
            }
            ImGui::EndPopup();
        } else if (s.modal.kind != Modal::Kind::None) {
            // Popup was closed externally (window closed etc.) — drop state.
            s.modal = Modal{};
        }
    }

    // ------------------------------------------------------------------
    // Slot helpers used by both views
    // ------------------------------------------------------------------

    // Highest slot considered "existing" (slot 1 always is) and the first
    // free slot, mirroring updateSlotPanel's scan.
    static void ScanSlots(int& lastSlot, int& nextFree) {
        lastSlot = 1;
        nextFree = 0;
        for (int i = 1; i <= kMaxSlots; ++i) {
            const SlotCache& sc = GetSlot(i);
            if (i == 1 || sc.data.has_value()) {
                lastSlot = i;
            } else if (!nextFree) {
                nextFree = i;
            }
        }
    }

    static void SaveCurrentToSlot(int slot) {
        Value snapshot = BuildCurrentTree();
        int count = CountStoredLeaves(snapshot, nullptr);
        WriteSlot(slot, &snapshot);
        std::string msg = Tr("$M8rMCMSM_settingsStored");
        size_t p;
        while ((p = msg.find("{count}")) != std::string::npos) msg.replace(p, 7, std::to_string(count));
        ShowAlert(msg);
        logger::info("[MCMSettingsManager] Settings stored in Slot #{}", slot);
    }

    static const Value* DetailsTree();  // defined with the details view below

    // Refreshes the cached slot/preset/detail counts when values or slots
    // changed since the last frame (or the viewed details target changed).
    static void EnsureCounts() {
        const std::string detailKey = s.detailsOpen
            ? (s.detailsIsSlot ? "slot:" + std::to_string(s.detailsSlot)
                               : "preset:" + s.detailsPreset)
            : std::string{};
        const bool detailStale = detailKey != s.detailCountsKey;
        if (!s.countsDirty && !detailStale) return;

        if (s.countsDirty) {
            ScanSlots(s.lastSlot, s.nextFree);
            for (int i = 1; i <= kMaxSlots; ++i) {
                const SlotCache& sc = GetSlot(i);
                auto& rc = s.slotCounts[static_cast<size_t>(i)];
                rc.hasData = sc.data.has_value();
                rc.all = rc.hasData ? CountStoredLeaves(*sc.data, nullptr) : 0;
                rc.changed = rc.hasData ? CountChanged(*sc.data, nullptr) : 0;
                rc.mods = rc.hasData ? CountStoredMods(*sc.data) : 0;
            }
            LoadPresets();
            s.presetCounts.clear();
            for (const auto& [name, tree] : s.presets) {
                auto& rc = s.presetCounts[name];
                rc.hasData = true;
                rc.all = CountStoredLeaves(tree, nullptr);
                rc.changed = CountChanged(tree, nullptr);
                rc.mods = CountStoredMods(tree);
            }
        }

        // Per-mod counts for the viewed details tree. `mods` doubles as the
        // "settings available in the live mod" count for the Current column.
        s.detailModCounts.clear();
        s.detailCountsKey = detailKey;
        if (!detailKey.empty()) {
            if (const Value* tree = DetailsTree()) {
                std::vector<std::string> allMods = s.modNames;
                if (tree->IsObject()) {
                    for (const auto& [mod, _] : tree->object) {
                        if (std::find(allMods.begin(), allMods.end(), mod) == allMods.end()) {
                            allMods.push_back(mod);
                        }
                    }
                }
                for (const std::string& mod : allMods) {
                    auto& rc = s.detailModCounts[mod];
                    rc.hasData = true;
                    rc.all = CountStoredLeaves(*tree, &mod);
                    rc.changed = CountChanged(*tree, &mod);
                    rc.mods = 0;
                    if (auto it = s.byMod.find(mod); it != s.byMod.end()) {
                        for (size_t idx : it->second) {
                            if (s.current.count(s.db[idx].pathString)) ++rc.mods;
                        }
                    }
                }
            }
        }
        s.countsDirty = false;
    }

    // "(?)" helper with the section help text on hover/focus.
    static void HelpMarker(const std::string& help) {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || ImGui::IsItemFocused()) {
            ImGui::SetTooltip("%s", help.c_str());
        }
    }

    // ------------------------------------------------------------------
    // Main view (port of MainGui.updateSlotPanel)
    // ------------------------------------------------------------------

    static void RenderMainView() {
        const int lastSlot = s.lastSlot;
        const int nextFree = s.nextFree;

        // First-start welcome (MainGui.firstStart): slot 1 untouched -> offer
        // to snapshot the player's current settings into it.
        if (!s.firstStartChecked) {
            s.firstStartChecked = true;
            const bool slot1Empty = s.slotCounts[1].all == 0;
            if (lastSlot == 1 && slot1Empty && ReadSlotName(1) == "Slot 1") {
                ShowConfirm(Tr("$WelcomePopupText1") + "\n\n" + Tr("$WelcomePopupText2") +
                                "\n\n" + Tr("$WelcomePopupText3"),
                            [] {
                                Value snapshot = BuildCurrentTree();
                                int count = CountStoredLeaves(snapshot, nullptr);
                                WriteSlot(1, &snapshot);
                                WriteSlotName(1, Tr("$FirstPackageName"));
                                std::string msg = Tr("$M8rMCMSM_settingsStored");
                                size_t p;
                                while ((p = msg.find("{count}")) != std::string::npos)
                                    msg.replace(p, 7, std::to_string(count));
                                ShowAlert(msg);
                            });
            }
        }

        ImGui::SeparatorText(Tr("$Settings_storage_slots").c_str());
        HelpMarker(Tr("$Settings_storage_slots_help"));
        ImGui::Spacing();

        if (ImGui::BeginTable("##mcmsm_slots", 5,
                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoBordersInBody)) {
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 280.0f);
            ImGui::TableSetupColumn("counts", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("view", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("apply", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("save", ImGuiTableColumnFlags_WidthFixed);

            for (int slot = 1; slot <= std::max(1, lastSlot); ++slot) {
                const auto& rc = s.slotCounts[static_cast<size_t>(slot)];
                const bool hasData = rc.hasData;
                const int cntAll = rc.all;
                const int cntChanged = rc.changed;
                const int cntMods = rc.mods;

                ImGui::TableNextRow();
                ImGui::PushID(slot);

                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(ReadSlotName(slot).c_str());

                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if (cntAll > 0) {
                    std::string notice = TrCount("$CountMod", cntMods) + " / " +
                                         TrCount("$CountSetting", cntAll);
                    if (cntChanged > 0) notice += " / " + TrCount("$CountChanged", cntChanged);
                    ImGui::TextDisabled("%s", notice.c_str());
                } else {
                    ImGui::TextUnformatted("");
                }

                ImGui::TableNextColumn();
                ImGui::BeginDisabled(!hasData);
                if (ImGui::Button((Tr("$ViewEdit") + "##view").c_str())) {
                    s.detailsOpen = true;
                    s.detailsIsSlot = true;
                    s.detailsSlot = slot;
                    s.modOpen.clear();
                }
                ImGui::EndDisabled();

                ImGui::TableNextColumn();
                ImGui::BeginDisabled(cntChanged == 0);
                if (ImGui::Button((Tr("$Apply") + "##apply").c_str())) {
                    int slotCopy = slot;
                    ShowConfirm(Tr("$Confirm_ApplySlot"), [slotCopy] {
                        const SlotCache& sc2 = GetSlot(slotCopy);
                        if (sc2.data.has_value()) ApplyStored(*sc2.data, "");
                    });
                }
                ImGui::EndDisabled();

                ImGui::TableNextColumn();
                if (ImGui::Button((Tr("$Save") + "##save").c_str())) {
                    int slotCopy = slot;
                    ShowConfirm(Tr("$Confirm_SaveSlot"), [slotCopy] { SaveCurrentToSlot(slotCopy); });
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        if (nextFree) {
            ImGui::Spacing();
            if (ImGui::Button(Tr("$Create_new_slot").c_str())) {
                int freeSlot = nextFree;
                int last = lastSlot;
                ShowConfirm(Tr("$Confirm_CreateSlot"), [freeSlot, last] {
                    ShowInput("", "Slot " + std::to_string(last + 1),
                              [freeSlot](const std::string& name) {
                                  Value snapshot = BuildCurrentTree();
                                  WriteSlot(freeSlot, &snapshot);
                                  WriteSlotName(freeSlot, name);
                              });
                });
            }
        }

        // Presets (read-only, from Data/MCM/Settings/Presets/*.ini)
        LoadPresets();
        if (!s.presets.empty()) {
            ImGui::Spacing();
            ImGui::SeparatorText(Tr("$Presets").c_str());
            HelpMarker(Tr("$Presets_help"));
            ImGui::Spacing();

            if (ImGui::BeginTable("##mcmsm_presets", 4,
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoBordersInBody)) {
                ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 280.0f);
                ImGui::TableSetupColumn("counts", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("view", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("apply", ImGuiTableColumnFlags_WidthFixed);

                for (const auto& [name, tree] : s.presets) {
                    const auto& rc = s.presetCounts[name];
                    const int cntAll = rc.all;
                    const int cntChanged = rc.changed;
                    const int cntMods = rc.mods;

                    ImGui::TableNextRow();
                    ImGui::PushID(name.c_str());

                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(name.c_str());

                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    if (cntAll > 0) {
                        std::string notice = TrCount("$CountMod", cntMods) + " / " +
                                             TrCount("$CountSetting", cntAll);
                        if (cntChanged > 0) notice += " / " + TrCount("$CountChanged", cntChanged);
                        ImGui::TextDisabled("%s", notice.c_str());
                    } else {
                        ImGui::TextUnformatted("");
                    }

                    ImGui::TableNextColumn();
                    ImGui::BeginDisabled(cntAll == 0);
                    if (ImGui::Button((Tr("$View") + "##pview").c_str())) {
                        s.detailsOpen = true;
                        s.detailsIsSlot = false;
                        s.detailsPreset = name;
                        s.modOpen.clear();
                    }
                    ImGui::EndDisabled();

                    ImGui::TableNextColumn();
                    ImGui::BeginDisabled(cntChanged == 0);
                    if (ImGui::Button((Tr("$Apply") + "##papply").c_str())) {
                        std::string presetName = name;
                        ShowConfirm(Tr("$Confirm_ApplyPreset"), [presetName] {
                            auto it = s.presets.find(presetName);
                            if (it != s.presets.end()) ApplyStored(it->second, "");
                        });
                    }
                    ImGui::EndDisabled();

                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
    }

    // ------------------------------------------------------------------
    // Details view (port of DetailsView)
    // ------------------------------------------------------------------

    // Returns the stored tree the details view operates on, or nullptr when
    // the target vanished (slot deleted, preset removed).
    static const Value* DetailsTree() {
        if (s.detailsIsSlot) {
            const SlotCache& sc = GetSlot(s.detailsSlot);
            return sc.data.has_value() ? &*sc.data : nullptr;
        }
        auto it = s.presets.find(s.detailsPreset);
        return it != s.presets.end() ? &it->second : nullptr;
    }

    // Rewrites the currently viewed slot after an edit (details view is
    // read-only for presets, so this is slot-only by construction).
    static void RewriteViewedSlot(const Value& tree) {
        WriteSlot(s.detailsSlot, &tree);
    }

    // Extra rows for stored settings the live DB doesn't know (port of
    // getSettingsAll's McmSettingFake entries).
    struct FakeRow {
        std::vector<std::string> path;
        std::string pathString;
    };

    static void CollectFakeRows(const Value& stored, const std::string& mod, std::vector<FakeRow>& out) {
        auto modIt = stored.object.find(mod);
        if (modIt == stored.object.end() || !modIt->second.IsObject()) return;

        auto addIfUnknown = [&](std::vector<std::string> path) {
            std::string ps;
            for (size_t i = 0; i < path.size(); ++i) {
                if (i) ps += '>';
                ps += path[i];
            }
            if (s.dbByPath.count(ps)) return;
            out.push_back(FakeRow{ std::move(path), std::move(ps) });
        };

        for (const auto& [type, l1] : modIt->second.object) {
            if (!l1.IsObject()) continue;
            for (const auto& [k1, l2] : l1.object) {
                if (type == "hot") { addIfUnknown({ mod, type, k1 }); continue; }
                if (!l2.IsObject()) continue;
                for (const auto& [k2, l3] : l2.object) {
                    if (type == "glb" || type == "ini") { addIfUnknown({ mod, type, k1, k2 }); continue; }
                    if (type != "prp" && type != "prs") continue;
                    if (!l3.IsObject()) continue;
                    for (const auto& [k3, l4] : l3.object) {
                        if (type == "prp") { addIfUnknown({ mod, type, k1, k2, k3 }); continue; }
                        if (!l4.IsObject()) continue;
                        for (const auto& [k4, l5] : l4.object) {
                            addIfUnknown({ mod, type, k1, k2, k3, k4 });
                        }
                    }
                }
            }
        }
    }

    static void RenderDetailsView() {
        const Value* tree = DetailsTree();
        if (!tree) {
            s.detailsOpen = false;
            return;
        }

        const std::string title = s.detailsIsSlot
            ? ReadSlotName(s.detailsSlot)
            : (Tr("$Preset") + " " + s.detailsPreset);
        const bool editable = s.detailsIsSlot;

        // --- Header buttons (updateDetailsButtons) ---
        if (ImGui::Button(Tr("$Back").c_str())) {
            s.detailsOpen = false;
            s.modOpen.clear();
            return;
        }
        if (editable) {
            const int slot = s.detailsSlot;
            ImGui::SameLine();
            if (ImGui::Button(Tr("$Rename").c_str())) {
                ShowInput(Tr("$InputNewName"), ReadSlotName(slot), [slot](const std::string& name) {
                    if (!name.empty()) WriteSlotName(slot, name);
                });
            }
            ImGui::SameLine();
            if (ImGui::Button(Tr("$Export").c_str())) {
                if (WriteExport(*tree)) {
                    ShowAlert(Tr("$ExportedSuccessText1") + "\n\n" +
                              "Fallout 4\\data\\MCM\\Settings\\MCM Exported Settings.ini\n\n" +
                              Tr("$ExportedSuccessText2") + "\n" +
                              "Fallout 4\\data\\MCM\\Settings\\Presets\\\n\n" +
                              Tr("$ExportedSuccessText3"));
                } else {
                    ShowAlert("Error: Failed to write export. Try reducing your setting count.");
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(Tr("$Remove_unchanged_settings").c_str())) {
                ShowConfirm(Tr("$Confirm_RemoveIdentical"), [slot] {
                    SlotCache& sc = GetSlot(slot);
                    if (!sc.data.has_value()) return;
                    Value edited = *sc.data;
                    for (const auto& ms : s.db) {
                        if (HasStored(edited, ms) && !DifferentToStored(edited, ms)) {
                            edited.Remove(ms.path);
                        }
                    }
                    WriteSlot(slot, &edited);
                });
            }
            ImGui::SameLine();
            const int cntAllSlot = s.slotCounts[static_cast<size_t>(slot)].all;
            if (cntAllSlot == 0 && slot == s.lastSlot) {
                if (ImGui::Button(Tr("$Remove").c_str())) {
                    ShowConfirm(Tr("$Confirm_DeleteSlot"), [slot] {
                        WriteSlot(slot, nullptr);
                        WriteSlotName(slot, "Slot " + std::to_string(slot));
                        s.detailsOpen = false;
                        s.modOpen.clear();
                    });
                }
            } else {
                ImGui::BeginDisabled(cntAllSlot == 0);
                if (ImGui::Button(Tr("$Wipe").c_str())) {
                    ShowConfirm(Tr("$Confirm_WipeSlot"), [slot] {
                        Value empty = Value::MakeObject();
                        WriteSlot(slot, &empty);
                        logger::info("[MCMSettingsManager] Settings wiped from Slot #{}", slot);
                    });
                }
                ImGui::EndDisabled();
            }
        }

        ImGui::Spacing();
        ImGui::SeparatorText(title.c_str());
        ImGui::TextDisabled("%s", Tr("$Stored_settings").c_str());
        HelpMarker(Tr("$Stored_settings_help"));

        ImGui::Checkbox(Tr("$FilterStored").c_str(), &s.filterOnlyStored);
        ImGui::SameLine();
        if (ImGui::Checkbox(Tr("$FilterChanged").c_str(), &s.filterOnlyChanged)) {
            s.modOpen.clear();  // eventToggleOnlyChanged resets collapse state
        }
        ImGui::Spacing();

        // Column header row
        if (ImGui::BeginTable("##mcmsm_dethdr", 4, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("n", ImGuiTableColumnFlags_WidthFixed, 320.0f);
            ImGui::TableSetupColumn("s", ImGuiTableColumnFlags_WidthFixed, 170.0f);
            ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthFixed, 190.0f);
            ImGui::TableSetupColumn("c", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", (Tr("$Mod") + " / " + Tr("$Setting_name")).c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", Tr("$Stored").c_str());
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", Tr("$Current").c_str());
            ImGui::EndTable();
        }

        // Mods to show: DB mods plus stored-only mods (mod not installed).
        std::vector<std::string> allMods = s.modNames;
        if (tree->IsObject()) {
            for (const auto& [mod, _] : tree->object) {
                if (std::find(allMods.begin(), allMods.end(), mod) == allMods.end()) {
                    allMods.push_back(mod);
                }
            }
        }

        int shownStoredTotal = 0;
        for (const std::string& mod : allMods) {
            const bool modInstalled = s.byMod.count(mod) != 0;
            const auto rcIt = s.detailModCounts.find(mod);
            const int cntAll = rcIt != s.detailModCounts.end() ? rcIt->second.all : 0;
            const int cntChanged = rcIt != s.detailModCounts.end() ? rcIt->second.changed : 0;
            // Live setting count for the mod (the "Current" column notice).
            const int cntAvailable = rcIt != s.detailModCounts.end() ? rcIt->second.mods : 0;

            if (s.filterOnlyChanged && cntChanged == 0) continue;
            if (s.filterOnlyStored && cntAll == 0) continue;
            shownStoredTotal += cntAll;

            bool& open = s.modOpen.try_emplace(mod, s.filterOnlyChanged).first->second;

            ImGui::PushID(mod.c_str());
            if (ImGui::BeginTable("##modrow", 5, ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("n", ImGuiTableColumnFlags_WidthFixed, 320.0f);
                ImGui::TableSetupColumn("s", ImGuiTableColumnFlags_WidthFixed, 170.0f);
                ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthFixed, 190.0f);
                ImGui::TableSetupColumn("c", ImGuiTableColumnFlags_WidthFixed, 170.0f);
                ImGui::TableSetupColumn("r", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                std::string fullName = mod;
                if (auto it = s.modFullNames.find(mod); it != s.modFullNames.end()) fullName = it->second;
                if (ImGui::Button((std::string(open ? "-" : "+") + "##toggle").c_str(), ImVec2(26, 0))) {
                    open = !open;
                }
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(fullName.c_str());

                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", cntAll > 0 ? TrCount("$CountSetting", cntAll).c_str() : "");

                ImGui::TableNextColumn();
                if (!modInstalled) {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("< %s >", Tr("$Mod_not_installed").c_str());
                } else {
                    ImGui::BeginDisabled(cntChanged == 0);
                    if (ImGui::Button(("> " + Tr("$Apply") + " >##modapply").c_str())) {
                        std::string modCopy = mod;
                        const Value treeCopy = *tree;
                        ApplyStored(treeCopy, modCopy);
                    }
                    ImGui::EndDisabled();
                    if (editable) {
                        ImGui::SameLine();
                        ImGui::BeginDisabled(cntChanged == 0);
                        if (ImGui::Button(("< " + Tr("$Save") + " <##modsave").c_str())) {
                            // slot-mod-save: only overwrites stored values that
                            // differ (does NOT add unstored settings).
                            Value edited = *tree;
                            for (size_t idx : s.byMod[mod]) {
                                const ManagedSetting& ms = s.db[idx];
                                if (HasStored(edited, ms) && DifferentToStored(edited, ms)) {
                                    auto cur = s.current.find(ms.pathString);
                                    if (cur != s.current.end()) edited.Set(ms.path, cur->second);
                                }
                            }
                            RewriteViewedSlot(edited);
                        }
                        ImGui::EndDisabled();
                    }
                }

                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                {
                    std::string cnotice;
                    if (cntAvailable > 0) cnotice = TrCount("$CountSetting", cntAvailable);
                    if (cntChanged > 0) {
                        if (!cnotice.empty()) cnotice += " / ";
                        cnotice += TrCount("$CountChanged", cntChanged);
                    }
                    ImGui::TextDisabled("%s", cnotice.c_str());
                }

                ImGui::TableNextColumn();
                if (editable && cntAll > 0) {
                    if (ImGui::Button((Tr("$Remove") + "##modremove").c_str())) {
                        Value edited = *tree;
                        edited.object.erase(mod);
                        RewriteViewedSlot(edited);
                    }
                }
                ImGui::EndTable();
            }

            // --- Per-setting rows (populateDetailSettingsTable) ---
            if (open) {
                ImGui::Indent(30.0f);
                if (ImGui::BeginTable("##setrows", 5,
                                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("n", ImGuiTableColumnFlags_WidthFixed, 290.0f);
                    ImGui::TableSetupColumn("s", ImGuiTableColumnFlags_WidthFixed, 170.0f);
                    ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthFixed, 190.0f);
                    ImGui::TableSetupColumn("c", ImGuiTableColumnFlags_WidthFixed, 170.0f);
                    ImGui::TableSetupColumn("r", ImGuiTableColumnFlags_WidthStretch);

                    // Real DB settings for this mod, then stored-only fakes.
                    std::vector<size_t> rows;
                    if (auto it = s.byMod.find(mod); it != s.byMod.end()) rows = it->second;
                    std::vector<FakeRow> fakes;
                    CollectFakeRows(*tree, mod, fakes);

                    for (size_t idx : rows) {
                        const ManagedSetting& ms = s.db[idx];
                        const bool hasStored = HasStored(*tree, ms);
                        const bool differs = DifferentToStored(*tree, ms);
                        if (s.filterOnlyChanged && !differs) continue;
                        if (s.filterOnlyStored && !hasStored) continue;

                        ImGui::TableNextRow();
                        ImGui::PushID(static_cast<int>(idx));

                        ImGui::TableNextColumn();
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(ms.label.empty() ? ms.pathString.c_str() : ms.label.c_str());
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ms.pathString.c_str());

                        ImGui::TableNextColumn();
                        ImGui::AlignTextToFramePadding();
                        if (hasStored) {
                            const Value* leaf = StoredLeaf(*tree, ms);
                            ImGui::TextUnformatted(leaf ? NamedValue(ms, *leaf).c_str() : "");
                        } else {
                            ImGui::TextUnformatted("");
                        }

                        ImGui::TableNextColumn();
                        ImGui::BeginDisabled(!hasStored || !differs);
                        if (ImGui::Button(("> " + Tr("$Apply") + " >##sapply").c_str())) {
                            const Value* leaf = StoredLeaf(*tree, ms);
                            if (leaf) {
                                Value leafCopy = *leaf;
                                ApplyValue(ms, leafCopy);
                            }
                        }
                        ImGui::EndDisabled();
                        if (editable) {
                            ImGui::SameLine();
                            ImGui::BeginDisabled(hasStored && !differs);
                            if (ImGui::Button(("< " + Tr("$Save") + " <##ssave").c_str())) {
                                auto cur = s.current.find(ms.pathString);
                                if (cur != s.current.end()) {
                                    Value edited = *tree;
                                    edited.Set(ms.path, cur->second);
                                    RewriteViewedSlot(edited);
                                }
                            }
                            ImGui::EndDisabled();
                        }

                        ImGui::TableNextColumn();
                        ImGui::AlignTextToFramePadding();
                        {
                            auto cur = s.current.find(ms.pathString);
                            if (cur != s.current.end()) {
                                ImGui::TextUnformatted(NamedValue(ms, cur->second).c_str());
                            } else if (s.unavailable.count(ms.pathString)) {
                                ImGui::TextDisabled("(unavailable)");
                            } else {
                                ImGui::TextDisabled("...");
                            }
                        }

                        ImGui::TableNextColumn();
                        if (editable && hasStored) {
                            if (ImGui::Button((Tr("$Remove") + "##sremove").c_str())) {
                                Value edited = *tree;
                                edited.Remove(ms.path);
                                RewriteViewedSlot(edited);
                            }
                        }

                        ImGui::PopID();
                        // The viewed tree may have been replaced by an edit
                        // above — re-resolve before the next row.
                        tree = DetailsTree();
                        if (!tree) break;
                    }

                    // Stored-only entries ("Setting doesn't exist").
                    if (tree) {
                        int fakeId = 0;
                        for (const FakeRow& fr : fakes) {
                            ImGui::TableNextRow();
                            ImGui::PushID(10000 + fakeId++);

                            ImGui::TableNextColumn();
                            ImGui::AlignTextToFramePadding();
                            ImGui::TextDisabled("\"%s\"", fr.pathString.c_str());

                            ImGui::TableNextColumn();
                            ImGui::AlignTextToFramePadding();
                            const Value* leaf = tree->Find(fr.path);
                            ImGui::TextUnformatted(leaf ? As3ToString(*leaf).c_str() : "");

                            ImGui::TableNextColumn();
                            ImGui::AlignTextToFramePadding();
                            ImGui::TextDisabled("< %s >", Tr("$Setting_doesnt_exist").c_str());

                            ImGui::TableNextColumn();

                            ImGui::TableNextColumn();
                            if (editable) {
                                if (ImGui::Button((Tr("$Remove") + "##fremove").c_str())) {
                                    Value edited = *tree;
                                    edited.Remove(fr.path);
                                    RewriteViewedSlot(edited);
                                }
                            }
                            ImGui::PopID();
                            tree = DetailsTree();
                            if (!tree) break;
                        }
                    }
                    ImGui::EndTable();
                }
                ImGui::Unindent(30.0f);
            }
            ImGui::PopID();
            if (!tree) break;
        }

        if (shownStoredTotal == 0) {
            ImGui::Spacing();
            ImGui::TextDisabled("%s", Tr("$No_entries").c_str());
        }
    }

    // ------------------------------------------------------------------
    // Public API
    // ------------------------------------------------------------------

    bool HandlesImageControl(const std::string& libName, const std::string& className) {
        return libName == "_MCMSettingsManager" &&
               className == "M8r.McmSettingsManager.Controller.MCMSettingsManager";
    }

    void RenderImageControl(const std::string&, const std::string&) {
        if (!s.built) {
            s.built = true;
            BuildDatabase();
        }
        PumpPropertyReads();
        EnsureCounts();

        if (s.detailsOpen) {
            RenderDetailsView();
        } else {
            RenderMainView();
        }
        RenderModal();
    }

    void ResetSession() {
        s = Session{};
        // Translations stay loaded (static content, language can't change
        // mid-session).
    }

}
