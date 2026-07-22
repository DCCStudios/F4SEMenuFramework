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
#include "FontManager.h"

namespace MCMRegistry {

    static int s_loadedModCount = 0;
    static bool s_active = false;

    // Apply translation resolution and HTML stripping to all user-visible strings
    // in a parsed MCM config
    // Resolves all display strings and returns the set of non-Latin scripts
    // (MCMTranslation::ScriptMask bits) seen in the results, so the caller
    // can request matching font coverage. Detection runs on the RESOLVED
    // text — that's exactly what ImGui will be asked to draw, regardless of
    // which file or language the value came from.
    static unsigned int ApplyTranslations(MCMConfigParser::MCMModConfig& config,
                                          const MCMTranslation::TranslationMap& map) {
        unsigned int scripts = 0;
        auto resolve = [&](std::string& text) {
            text = MCMTranslation::ResolveAndStrip(text, map);
            scripts |= MCMTranslation::DetectScripts(text);
        };

        // Resolve display name
        resolve(config.displayName);

        // Resolve page names and control strings
        for (auto& page : config.pages) {
            resolve(page.pageDisplayName);

            for (auto& ctrl : page.controls) {
                resolve(ctrl.text);
                resolve(ctrl.help);

                // Resolve option labels
                for (auto& opt : ctrl.options) {
                    resolve(opt.text);
                }
            }
        }

        // Resolve shared option labels
        for (auto& [key, options] : config.sharedOptions) {
            for (auto& opt : options) {
                resolve(opt.text);
            }
        }

        return scripts;
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

        // Coexistence: the real MCM's own input handler (active since
        // kMessage_GameLoaded) dispatches every keybind action already. Our
        // translated keybinds must not dispatch too, or one press runs the
        // action twice — toggle hotkeys then open-and-instantly-close their
        // menus (Wheel Menu, Screen Archer Menu). Bindings stay registered so
        // they remain visible/rebindable in our UI and sync via Keybinds.json.
        if (MCMConflictCheck::IsNativeMCMPresent()) {
            MCMKeybindTranslator::SetSuppressDispatch(true);
        }

        // Step 2: Scan for MCM mod folders
        auto mods = MCMScanner::Scan();
        if (mods.empty()) {
            logger::info("[MCMRegistry] No MCM mods found — nothing to load");
            return;
        }

        // Step 3: Load global translations from Data/Interface/Translations/.
        // Language comes from Fallout4Custom.ini / Fallout4.ini / GetINISetting
        // (Custom.ini first — GetINISetting alone often stays on Fallout4.ini's
        // "en" even when the player set sLanguage in Custom.ini).
        {
            const auto* langSetting = RE::GetINISetting("sLanguage:General");
            const std::string fromSetting = langSetting ? std::string(langSetting->GetString()) : "";
            const std::string lang = MCMTranslation::ResolveGameLanguage(fromSetting);
            MCMTranslation::SetLanguage(lang);
            logger::info("[MCMRegistry] Game language '{}' (GetINISetting reported '{}') — "
                         "loading translations with English fallback for missing keys",
                MCMTranslation::GetLanguage(),
                fromSetting.empty() ? "<unset>" : fromSetting);
        }
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
        unsigned int scriptMask = 0;
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
                // Confirm the active-language file is visible on the same path
                // LoadForMod uses (so "still English" can be distinguished from
                // "file not found" — many mods ship *_es.txt as English copies).
                if (MCMTranslation::GetLanguage() != "en") {
                    const auto langFile =
                        std::filesystem::current_path() / "Data" / "Interface" / "Translations" /
                        (mod.modName + "_" + MCMTranslation::GetLanguage() + ".txt");
                    std::ifstream probe(langFile);
                    if (probe.is_open()) {
                        const auto langNameU8 = langFile.filename().u8string();
                        logger::info("[MCMRegistry] Using translation file '{}' for '{}' ({} keys in merged map)",
                            std::string(langNameU8.begin(), langNameU8.end()),
                            mod.modName, interfaceTranslations.size());
                    }
                }

                auto config = MCMConfigParser::Parse(mod.configPath, mod.modName);
                if (!config.has_value()) {
                    logger::warn("[MCMRegistry] Failed to parse config for '{}' — skipping", mod.modName);
                    continue;
                }

                // Apply translation resolution + HTML stripping, collecting
                // which non-Latin scripts the resolved text actually uses.
                scriptMask |= ApplyTranslations(*config, modMap);

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

        // Make sure the font atlas can draw every script the loaded text
        // uses — the language setting alone misses translation mods that
        // ship non-Latin text in *_en.txt files (e.g. Korean packs on an
        // sLanguage=en game). No-op when coverage already suffices.
        if (scriptMask != 0) {
            FontManager::RequestScriptCoverage(scriptMask);
        }

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
