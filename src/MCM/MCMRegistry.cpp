#include "MCM/MCMRegistry.h"
#include "MCM/MCMScanner.h"
#include "MCM/MCMConflictCheck.h"
#include "MCM/MCMConfigParser.h"
#include "MCM/MCMValueProvider.h"
#include "MCM/MCMKeybindTranslator.h"
#include "MCM/MCMKeybindStore.h"
#include "MCM/MCMLiveSync.h"
#include "MCM/MCMWidgetRenderer.h"
#include "MCM/MCMTranslation.h"
#include "Config.h"
#include "Event.h"

namespace MCMRegistry {

    static int s_loadedModCount = 0;
    static bool s_active = false;

    // Apply translation resolution and HTML stripping to all user-visible strings
    // in a parsed MCM config
    static void ApplyTranslations(MCMConfigParser::MCMModConfig& config,
                                  const MCMTranslation::TranslationMap& map) {
        // Resolve display name
        config.displayName = MCMTranslation::ResolveAndStrip(config.displayName, map);

        // Resolve page names and control strings
        for (auto& page : config.pages) {
            page.pageDisplayName = MCMTranslation::ResolveAndStrip(page.pageDisplayName, map);

            for (auto& ctrl : page.controls) {
                ctrl.text = MCMTranslation::ResolveAndStrip(ctrl.text, map);
                ctrl.help = MCMTranslation::ResolveAndStrip(ctrl.help, map);

                // Resolve option labels
                for (auto& opt : ctrl.options) {
                    opt.text = MCMTranslation::ResolveAndStrip(opt.text, map);
                }
            }
        }

        // Resolve shared option labels
        for (auto& [key, options] : config.sharedOptions) {
            for (auto& opt : options) {
                opt.text = MCMTranslation::ResolveAndStrip(opt.text, map);
            }
        }
    }

    // Fired on framework menu open/close. On open, refresh everything the
    // NATIVE MCM may have changed since we last looked (registered only when
    // mcm.dll is present — see Init).
    static void __stdcall OnMenuEvent(Event::EventType type) {
        if (type != Event::EventType::kOpenMenu) {
            return;
        }
        MCMValueProvider::ReloadAll();
        MCMWidgetRenderer::InvalidateAllStates();
        MCMLiveSync::RequestPull();
    }

    void Init() {
        // Check if MCM compat is enabled in config
        if (!Config::MCMCompatEnabled) {
            logger::info("[MCMRegistry] MCM compatibility is disabled in config");
            return;
        }

        // Step 1: Check for the real MCM. When present, the translation layer
        // silently steps aside (no consent popup) unless the user force-enabled
        // coexistence in the framework settings.
        MCMConflictCheck::Check();
        if (!MCMConflictCheck::ShouldLoadMCMMenus()) {
            // Reason already logged by MCMConflictCheck
            return;
        }

        // Step 2: Scan for MCM mod folders
        auto mods = MCMScanner::Scan();
        if (mods.empty()) {
            logger::info("[MCMRegistry] No MCM mods found — nothing to load");
            return;
        }

        // Step 3: Load global translations from Data/Interface/Translations/
        MCMTranslation::TranslationMap globalTranslations;
        auto interfaceTransDir = std::filesystem::current_path() / "Data" / "Interface" / "Translations";
        if (std::filesystem::exists(interfaceTransDir)) {
            globalTranslations = MCMTranslation::LoadDirectory(interfaceTransDir);
            if (!globalTranslations.empty()) {
                logger::info("[MCMRegistry] Loaded {} global translation entries from Interface/Translations",
                    globalTranslations.size());
            }
        }

        // Step 4: Initialize the value provider with all mod settings files
        MCMValueProvider::Init(mods);

        // Step 4b: Load the user's saved MCM hotkey assignments
        // (Data/MCM/Settings/Keybinds.json) so translated menus show and use
        // the same bindings the real MCM would.
        MCMKeybindStore::Load();

        // Step 5: Parse configs and register widgets + keybinds
        for (const auto& mod : mods) {
            try {
                // Build per-mod translation map: mod-specific entries override globals
                MCMTranslation::TranslationMap modMap = globalTranslations;

                // Load mod-specific translations from MCM/Config/<Mod>/Translation/
                if (!mod.translationDir.empty()) {
                    auto modTranslations = MCMTranslation::LoadDirectory(mod.translationDir);
                    for (auto& [k, v] : modTranslations) {
                        modMap[k] = std::move(v);
                    }
                }

                // Try loading from Data/Interface/Translations/<modName>_en.txt
                // (direct file open — works even if directory enumeration fails under USVFS)
                auto interfaceTranslations = MCMTranslation::LoadForMod(mod.modName);
                for (auto& [k, v] : interfaceTranslations) {
                    modMap[k] = std::move(v);
                }

                auto config = MCMConfigParser::Parse(mod.configPath, mod.modName);
                if (!config.has_value()) {
                    logger::warn("[MCMRegistry] Failed to parse config for '{}' — skipping", mod.modName);
                    continue;
                }

                // Apply translation resolution + HTML stripping
                ApplyTranslations(*config, modMap);

                // Register keybinds (if keybinds.json exists)
                if (!mod.keybindsPath.empty()) {
                    MCMKeybindTranslator::RegisterFromFile(mod.keybindsPath, mod.modName);
                }

                // Register the mod's pages as framework section items
                MCMWidgetRenderer::RegisterMod(*config, mod.modName);

                s_loadedModCount++;
            } catch (const std::exception& e) {
                logger::error("[MCMRegistry] Exception while loading '{}': {} — skipping", mod.modName, e.what());
            } catch (...) {
                logger::error("[MCMRegistry] Unknown exception while loading '{}' — skipping", mod.modName);
            }
        }

        s_active = (s_loadedModCount > 0);
        logger::info("[MCMRegistry] MCM compat initialized: {} mod(s) loaded successfully", s_loadedModCount);

        // Coexistence reverse-sync: when the overlay opens, pick up anything
        // the user changed through the NATIVE MCM since we last looked —
        //   * settings: MCM commits every edit straight to
        //     Data/MCM/Settings/<Mod>.ini, so re-reading the files and
        //     invalidating cached widget states is a complete sync;
        //   * keybinds: MCM only persists on game save, so the file can be
        //     stale — pull the live bindings via root.mcm instead (delivered
        //     by MCMLiveSync at the next pause-menu-open opportunity, which
        //     is immediate when entering through the pause-menu button).
        if (s_active && MCMConflictCheck::IsNativeMCMPresent()) {
            Event::AddEventListener(&OnMenuEvent, 0.0f);
        }
    }

    int GetLoadedModCount() {
        return s_loadedModCount;
    }

    bool IsActive() {
        return s_active;
    }

} // namespace MCMRegistry
