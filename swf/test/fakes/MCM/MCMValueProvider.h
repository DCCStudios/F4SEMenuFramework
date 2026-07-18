#pragma once

// Test-only shadow of MCM/MCMValueProvider.h. FallUIHudEditor.cpp uses only
// the raw ModSetting accessors; back them with an in-memory map so tests can
// preload INI state and inspect what the editor writes.

#include <map>
#include <optional>
#include <string>

namespace MCMValueProvider {

    // modName -> ("key:Section" -> value)
    inline std::map<std::string, std::map<std::string, std::string>> g_testStore;

    inline std::optional<std::string> GetModSettingRaw(const std::string& modName,
                                                       const std::string& settingName) {
        auto mit = g_testStore.find(modName);
        if (mit == g_testStore.end()) return std::nullopt;
        auto it = mit->second.find(settingName);
        if (it == mit->second.end()) return std::nullopt;
        return it->second;
    }

    inline void SetModSettingRaw(const std::string& modName, const std::string& settingName,
                                 const std::string& value) {
        g_testStore[modName][settingName] = value;
    }
}
