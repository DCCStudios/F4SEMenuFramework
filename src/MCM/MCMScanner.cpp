#include "MCM/MCMScanner.h"

namespace MCMScanner {

    std::filesystem::path GetScanBasePath() {
        // The game's working directory is always the Fallout 4 root folder.
        // MCM uses Data/MCM/Config/<ModName>/ for its configuration files.
        return std::filesystem::current_path() / "Data" / "MCM" / "Config";
    }

    std::filesystem::path GetUserSettingsBasePath() {
        // MCM saves user-changed values to Data/MCM/Settings/<ModName>/settings.ini
        // This is separate from the Config folder which holds mod-supplied defaults.
        return std::filesystem::current_path() / "Data" / "MCM" / "Settings";
    }

    std::vector<MCMModInfo> Scan() {
        std::vector<MCMModInfo> mods;

        auto basePath = GetScanBasePath();
        if (!std::filesystem::exists(basePath) || !std::filesystem::is_directory(basePath)) {
            logger::info("[MCMScanner] MCM config directory not found: {}", basePath.string());
            return mods;
        }

        auto userSettingsBase = GetUserSettingsBasePath();
        bool userSettingsExist = std::filesystem::exists(userSettingsBase) &&
                                 std::filesystem::is_directory(userSettingsBase);

        logger::info("[MCMScanner] Scanning config: {}", basePath.string());
        logger::info("[MCMScanner] User settings dir: {} ({})", userSettingsBase.string(),
            userSettingsExist ? "found" : "not found");

        for (const auto& entry : std::filesystem::directory_iterator(basePath)) {
            if (!entry.is_directory()) continue;

            MCMModInfo info;
            info.modName = entry.path().filename().string();
            info.basePath = entry.path();

            auto configFile = entry.path() / "config.json";
            if (!std::filesystem::exists(configFile)) {
                logger::debug("[MCMScanner] Skipping '{}' — no config.json", info.modName);
                continue;
            }
            info.configPath = configFile;

            // Default settings from the mod's Config folder
            auto settingsFile = entry.path() / "settings.ini";
            if (std::filesystem::exists(settingsFile)) {
                info.settingsPath = settingsFile;
            }

            // User-saved settings from the Settings folder (takes priority over defaults).
            // The canonical location (what real MCM writes) is the FLAT file
            // Data/MCM/Settings/<ModName>.ini; the folder-style
            // Data/MCM/Settings/<ModName>/settings.ini is a legacy fallback.
            if (userSettingsExist) {
                auto flatUserFile = userSettingsBase / (info.modName + ".ini");
                auto legacyUserFile = userSettingsBase / info.modName / "settings.ini";
                if (std::filesystem::exists(flatUserFile)) {
                    info.userSettingsPath = flatUserFile;
                } else if (std::filesystem::exists(legacyUserFile)) {
                    info.userSettingsPath = legacyUserFile;
                }
            }

            auto keybindsFile = entry.path() / "keybinds.json";
            if (std::filesystem::exists(keybindsFile)) {
                info.keybindsPath = keybindsFile;
            }

            // Check for Translation/ subfolder containing *_en.txt files
            auto translationDir = entry.path() / "Translation";
            if (std::filesystem::exists(translationDir) && std::filesystem::is_directory(translationDir)) {
                info.translationDir = translationDir;
            }

            logger::info("[MCMScanner] Found MCM mod: '{}' (config={}, defaults={}, userSettings={}, keybinds={})",
                info.modName,
                !info.configPath.empty() ? "yes" : "no",
                !info.settingsPath.empty() ? "yes" : "no",
                !info.userSettingsPath.empty() ? "yes" : "no",
                !info.keybindsPath.empty() ? "yes" : "no");

            mods.push_back(std::move(info));
        }

        logger::info("[MCMScanner] Discovered {} MCM mod(s)", mods.size());
        return mods;
    }

} // namespace MCMScanner
