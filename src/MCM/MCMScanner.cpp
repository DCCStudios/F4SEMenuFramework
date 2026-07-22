#include "MCM/MCMScanner.h"

#include <system_error>

namespace MCMScanner {

    // path.string() uses the ANSI code page on Windows and throws
    // std::system_error when a folder name isn't representable there.
    static std::string PathUtf8(const std::filesystem::path& p) {
        const auto u8 = p.u8string();
        return std::string(u8.begin(), u8.end());
    }

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
            logger::info("[MCMScanner] MCM config directory not found: {}", PathUtf8(basePath));
            return mods;
        }

        auto userSettingsBase = GetUserSettingsBasePath();
        bool userSettingsExist = std::filesystem::exists(userSettingsBase) &&
                                 std::filesystem::is_directory(userSettingsBase);

        logger::info("[MCMScanner] Scanning config: {}", PathUtf8(basePath));
        logger::info("[MCMScanner] User settings dir: {} ({})", PathUtf8(userSettingsBase),
            userSettingsExist ? "found" : "not found");

        try {
            for (const auto& entry : std::filesystem::directory_iterator(basePath)) {
                if (!entry.is_directory()) continue;

                MCMModInfo info;
                info.modName = PathUtf8(entry.path().filename());
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
        } catch (const std::system_error& e) {
            logger::warn("[MCMScanner] Directory scan aborted: {}", e.what());
        }

        logger::info("[MCMScanner] Discovered {} MCM mod(s)", mods.size());
        return mods;
    }

} // namespace MCMScanner
