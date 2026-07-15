#pragma once

#include <string>
#include <vector>
#include <filesystem>

// Discovers MCM mod configuration folders on disk.
// MCM uses two separate folder hierarchies:
//   Data/MCM/Config/<ModName>/   — mod-supplied defaults (config.json, settings.ini defaults)
//   Data/MCM/Settings/<ModName>/ — user-saved values (settings.ini with player's customizations)
// When loading values, user settings take priority over config defaults.
namespace MCMScanner {

    struct MCMModInfo {
        std::string modName;                      // folder name = mod identifier
        std::filesystem::path basePath;           // full path to the mod's Config folder
        std::filesystem::path configPath;         // path to config.json (empty if not present)
        std::filesystem::path settingsPath;       // path to settings.ini DEFAULTS (in Config folder)
        std::filesystem::path userSettingsPath;   // path to settings.ini USER VALUES (in Settings folder)
        std::filesystem::path keybindsPath;       // path to keybinds.json (empty if not present)
        std::filesystem::path translationDir;     // path to Translation/ subfolder (empty if not present)
    };

    // Scans the MCM Config folder and returns info for all discovered mods.
    // Also checks the MCM Settings folder for user-saved values.
    // Only mods with a valid config.json are included.
    std::vector<MCMModInfo> Scan();

    // Returns the base config path being scanned (Data/MCM/Config).
    std::filesystem::path GetScanBasePath();

    // Returns the user settings base path (Data/MCM/Settings).
    std::filesystem::path GetUserSettingsBasePath();

}
