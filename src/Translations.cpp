#include "Translations.h"
#include "MCM/MCMTranslation.h"

#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
using json = nlohmann::json;

namespace Translations {
    constexpr const char* kBaseFile = "Data/F4SE/Plugins/F4SEMenuFramework/F4SEMenuFrameworkStrings.json";
    const char* defaultTranslation = "missing translation";
    static inline std::map<std::string, const char*> translations;

    // Loads one flat key/value JSON file, overriding existing keys. Community
    // files are often saved in the machine ANSI codepage; convert so ImGui
    // renders the text instead of replacement diamonds. Absent/malformed files
    // are ignored (the English base already loaded stays intact).
    static void LoadFile(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return;
        std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        raw = MCMTranslation::EnsureUtf8(raw);
        try {
            auto j = nlohmann::json::parse(raw);  // tolerates a UTF-8 BOM
            if (!j.is_object()) return;
            for (auto& [key, value] : j.items()) {
                if (value.is_string()) {
                    translations[key] = _strdup(value.get<std::string>().c_str());
                }
            }
        } catch (const std::exception&) {
            logger::warn("[Translations] {} is not valid JSON — skipped", path);
        }
    }
}


void Translations::Install() {
    // English base first; every key exists here so a missing translation always
    // falls back to readable English rather than "missing translation".
    LoadFile(kBaseFile);

    // Then overlay the active language: the game's sLanguage, or the user's
    // framework language override when set (SetLanguageOverride runs before this
    // in plugin.cpp). Ships as F4SEMenuFrameworkStrings_<lang>.json next to the
    // base file; only the keys it translates are replaced, the rest stay English.
    const std::string lang = MCMTranslation::ResolveGameLanguage();
    if (!lang.empty() && lang != "en") {
        LoadFile("Data/F4SE/Plugins/F4SEMenuFramework/F4SEMenuFrameworkStrings_" + lang + ".json");
        logger::info("[Translations] framework UI language '{}'", lang);
    }
}

const const char* Translations::Get(std::string key) {
    IF_FIND(translations, key, it) {
        return it->second;
    }
    return defaultTranslation;
}
