#include "MCM/MCMKeybindStore.h"
#include "MCM/MCMLiveSync.h"
#include "MCM/MCMScanner.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <fstream>
#include <map>
#include <mutex>
#include <vector>
#include <nlohmann/json.hpp>

namespace MCMKeybindStore {

    // One entry of Keybinds.json. "keycode" is a Windows VK code; "modifiers"
    // is the real MCM's ctrl/alt/shift bitmask which the framework doesn't use
    // but must preserve so we don't destroy information on write-back.
    struct StoredKeybind {
        std::string id;
        std::string modName;
        int keycode = 0;
        int modifiers = 0;
    };

    static std::vector<StoredKeybind> s_entries;
    static bool s_loaded = false;
    static std::mutex s_mutex;

    // hotkeyId ("MCM.<mod>.<id>") -> (modName, keybindId) routing table
    static std::map<std::string, std::pair<std::string, std::string>> s_mappings;

    static std::filesystem::path StorePath() {
        return MCMScanner::GetUserSettingsBasePath() / "Keybinds.json";
    }

    // ------------------------------------------------------------------
    // VK <-> DIK translation.
    // MapVirtualKey handles the main keyboard block; keys whose DIK codes
    // live in the "extended" range (0x80+) don't round-trip through it and
    // use an explicit table (same fixups as the hotkey capture flow).
    // ------------------------------------------------------------------

    struct KeyFixup { unsigned int vk; unsigned int dik; };
    static constexpr KeyFixup kExtendedKeys[] = {
        { VK_UP,       0xC8 }, { VK_DOWN,   0xD0 }, { VK_LEFT,   0xCB }, { VK_RIGHT,  0xCD },
        { VK_HOME,     0xC7 }, { VK_END,    0xCF }, { VK_PRIOR,  0xC9 }, { VK_NEXT,   0xD1 },
        { VK_INSERT,   0xD2 }, { VK_DELETE, 0xD3 }, { VK_RCONTROL, 0x9D }, { VK_RMENU, 0xB8 },
        { VK_DIVIDE,   0xB5 }, { VK_SNAPSHOT, 0xB7 }, { VK_PAUSE, 0xC5 },
        { VK_LWIN,     0xDB }, { VK_RWIN,   0xDC }, { VK_APPS,   0xDD },
        { VK_NUMLOCK,  0x45 },
    };

    unsigned int VKToDIK(unsigned int vk) {
        if (vk == 0) return 0;
        for (const auto& f : kExtendedKeys) {
            if (f.vk == vk) return f.dik;
        }
        return MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    }

    unsigned int DIKToVK(unsigned int dik) {
        if (dik == 0) return 0;
        for (const auto& f : kExtendedKeys) {
            if (f.dik == dik) return f.vk;
        }
        // Non-extended scan codes translate directly.
        if (dik < 0x80) {
            return MapVirtualKeyA(dik, MAPVK_VSC_TO_VK);
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // File I/O
    // ------------------------------------------------------------------

    // Caller must hold s_mutex.
    static void SaveLocked() {
        nlohmann::json root;
        root["version"] = 1;
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : s_entries) {
            arr.push_back({
                { "id", e.id },
                { "keycode", e.keycode },
                { "modName", e.modName },
                { "modifiers", e.modifiers },
            });
        }
        root["keybinds"] = std::move(arr);

        auto path = StorePath();
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        std::ofstream file(path, std::ios::trunc);
        if (!file.is_open()) {
            logger::error("[MCMKeybindStore] Cannot write '{}'", path.string());
            return;
        }
        file << root.dump();
        logger::info("[MCMKeybindStore] Saved {} keybind(s) to '{}'", s_entries.size(), path.string());
    }

    void Load() {
        std::lock_guard lock(s_mutex);
        s_entries.clear();
        s_loaded = true;

        auto path = StorePath();
        std::ifstream file(path);
        if (!file.is_open()) {
            logger::info("[MCMKeybindStore] No user Keybinds.json yet ('{}')", path.string());
            return;
        }

        nlohmann::json root;
        try {
            file >> root;
        } catch (const nlohmann::json::parse_error& e) {
            logger::error("[MCMKeybindStore] Parse error in '{}': {}", path.string(), e.what());
            return;
        }

        if (!root.is_object() || !root.contains("keybinds") || !root["keybinds"].is_array()) {
            logger::warn("[MCMKeybindStore] Unrecognized Keybinds.json format");
            return;
        }

        for (const auto& j : root["keybinds"]) {
            if (!j.is_object()) continue;
            StoredKeybind e;
            if (j.contains("id") && j["id"].is_string()) e.id = j["id"].get<std::string>();
            if (j.contains("modName") && j["modName"].is_string()) e.modName = j["modName"].get<std::string>();
            if (j.contains("keycode") && j["keycode"].is_number()) e.keycode = j["keycode"].get<int>();
            if (j.contains("modifiers") && j["modifiers"].is_number()) e.modifiers = j["modifiers"].get<int>();
            if (e.id.empty() || e.modName.empty()) continue;
            s_entries.push_back(std::move(e));
        }

        logger::info("[MCMKeybindStore] Loaded {} user keybind(s) from '{}'", s_entries.size(), path.string());
    }

    std::optional<unsigned int> GetSavedDIK(const std::string& modName, const std::string& keybindId) {
        std::lock_guard lock(s_mutex);
        if (!s_loaded) return std::nullopt;

        for (const auto& e : s_entries) {
            if (e.modName == modName && e.id == keybindId) {
                unsigned int dik = VKToDIK(static_cast<unsigned int>(e.keycode));
                if (dik == 0) {
                    logger::warn("[MCMKeybindStore] Keybind '{}'/'{}' has untranslatable keycode {}",
                        modName, keybindId, e.keycode);
                    return std::nullopt;
                }
                return dik;
            }
        }
        return std::nullopt;
    }

    void RegisterMapping(const std::string& hotkeyId, const std::string& modName, const std::string& keybindId) {
        std::lock_guard lock(s_mutex);
        s_mappings[hotkeyId] = { modName, keybindId };
    }

    std::vector<Mapping> GetAllMappings() {
        std::lock_guard lock(s_mutex);
        std::vector<Mapping> out;
        out.reserve(s_mappings.size());
        for (const auto& [hotkeyId, ids] : s_mappings) {
            out.push_back(Mapping{ hotkeyId, ids.first, ids.second });
        }
        return out;
    }

    void ImportLiveBinding(const std::string& modName, const std::string& keybindId, unsigned int vkKeycode) {
        std::lock_guard lock(s_mutex);

        if (vkKeycode == 0) {
            // Unbound inside the native MCM — mirror by dropping the entry.
            std::erase_if(s_entries, [&](const StoredKeybind& e) {
                return e.modName == modName && e.id == keybindId;
            });
            return;
        }

        for (auto& e : s_entries) {
            if (e.modName == modName && e.id == keybindId) {
                e.keycode = static_cast<int>(vkKeycode);
                return;
            }
        }
        s_entries.push_back(StoredKeybind{ keybindId, modName, static_cast<int>(vkKeycode), 0 });
    }

    void OnFrameworkBindingChanged(const std::string& hotkeyId, unsigned int dikScanCode) {
        std::lock_guard lock(s_mutex);

        auto mapIt = s_mappings.find(hotkeyId);
        if (mapIt == s_mappings.end()) return;  // not an MCM-managed hotkey

        const auto& [modName, keybindId] = mapIt->second;

        // Unbind: drop the entry, matching what the real MCM does on unbind.
        if (dikScanCode == 0) {
            std::erase_if(s_entries, [&](const StoredKeybind& e) {
                return e.modName == modName && e.id == keybindId;
            });
            SaveLocked();
            // Also clear it inside the running MCM (delivered when the pause
            // menu is next open — root.mcm only exists on that movie).
            MCMLiveSync::QueuePush(modName, keybindId, 0, 0);
            return;
        }

        unsigned int vk = DIKToVK(dikScanCode);
        if (vk == 0) {
            logger::warn("[MCMKeybindStore] DIK 0x{:X} for '{}' has no VK equivalent — not writing to Keybinds.json",
                dikScanCode, hotkeyId);
            return;
        }

        // Update existing entry (preserving its modifiers) or append a new one.
        // Either way, queue a live push so the running MCM picks the change up
        // without waiting for the next save load.
        for (auto& e : s_entries) {
            if (e.modName == modName && e.id == keybindId) {
                e.keycode = static_cast<int>(vk);
                SaveLocked();
                MCMLiveSync::QueuePush(modName, keybindId, vk, e.modifiers);
                return;
            }
        }
        s_entries.push_back(StoredKeybind{ keybindId, modName, static_cast<int>(vk), 0 });
        SaveLocked();
        MCMLiveSync::QueuePush(modName, keybindId, vk, 0);
    }

} // namespace MCMKeybindStore
