#include "MCM/MCMConflictCheck.h"
#include "Config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace MCMConflictCheck {

    static ConflictState s_state = ConflictState::NotChecked;
    static bool s_nativeMCMPresent = false;

    namespace {
        // CommonLibF4's RE::Scaleform::GFx::Value exposes GetNumber()/GetString()
        // but provides no boolean accessor. The class layout is ABI-stable and
        // guarded by a static_assert (sizeof(Value) == 0x20): the ValueUnion sits
        // at offset 0x10 and a boolean occupies its first byte. This lets us read
        // a Flash Boolean property (e.g. DisplayObject.visible) that we fetched
        // via GetVariable.
        bool ReadGfxBool(const RE::Scaleform::GFx::Value& a_val) {
            static_assert(sizeof(RE::Scaleform::GFx::Value) == 0x20);
            return *(reinterpret_cast<const std::uint8_t*>(std::addressof(a_val)) + 0x10) != 0;
        }
    }

    void Check() {
        // Detection is DLL-only: the real MCM is a pure F4SE plugin (MCM.dll)
        // and ships no .esp/.esm, so checking loaded game plugins would only
        // produce false positives (any plugin containing "mcm" in its name).
        s_nativeMCMPresent = (GetModuleHandleA("mcm.dll") != nullptr);

        if (!s_nativeMCMPresent) {
            s_state = ConflictState::NoConflict;
            logger::info("[MCMConflictCheck] Real MCM not detected — F4SE Menu Framework will provide MCM menus and the MCM script API");
            return;
        }

        // Real MCM is running. Default: silently step aside so the real MCM
        // remains the single source of truth (no duplicate menus, no value
        // desync, no double Papyrus callbacks). The user can force-enable
        // coexistence from the framework settings window.
        if (Config::MCMCompatWhenNativePresent) {
            s_state = ConflictState::ConflictAllow;
            logger::warn("[MCMConflictCheck] Real MCM detected but user force-enabled MCM compatibility — both systems will run (duplicate menus/callbacks possible)");
        } else {
            s_state = ConflictState::ConflictSkip;
            logger::info("[MCMConflictCheck] Real MCM detected — MCM compatibility layer auto-disabled (enable 'Load MCM menus alongside real MCM' in settings to override)");
        }
    }

    ConflictState GetState() {
        return s_state;
    }

    bool IsNativeMCMPresent() {
        return s_nativeMCMPresent;
    }

    bool ShouldLoadMCMMenus() {
        return s_state == ConflictState::NoConflict || s_state == ConflictState::ConflictAllow;
    }

    bool IsNativeMCMMenuOpen() {
        // Only meaningful when the real MCM is actually installed (our pages
        // then only exist because the user force-enabled coexistence).
        if (!s_nativeMCMPresent) return false;

        auto* ui = RE::UI::GetSingleton();
        if (!ui) return false;

        // The pause menu movie must at least be on the stack for MCM's config
        // panel to be showing.
        if (!ui->GetMenuOpen("PauseMenu")) return false;

        // IMPORTANT: a loaded "root.mcm_loader.content" is NOT a valid "MCM is
        // open" signal. MCM's DLL (RegisterScaleform) injects mcm_loader and
        // immediately loads MCM.swf into content the moment the pause menu
        // opens — long before, and regardless of whether, the player ever
        // selects "Mod Configuration". Keying on it made any plain ESC pause
        // look like an open MCM (the bug this replaced).
        //
        // The reliable, MCM-version-independent signal is the game's own pause
        // navigation state: the main pause list (root.Menu_mc.MainPanel_mc) is
        // visible while you sit at the ESC menu, and the game hides it whenever
        // you drill into a sub-panel — which is exactly what MCM does when its
        // config opens (MCM_Main sets MainMenu.MainPanel_mc.visible = false).
        // So "MainPanel_mc hidden while paused" == "a config/sub-panel is up".
        static std::uint64_t s_lastPollTick = 0;
        static bool s_cachedOpen = false;
        const std::uint64_t now = GetTickCount64();
        if (now - s_lastPollTick < 250) {
            return s_cachedOpen;
        }
        s_lastPollTick = now;

        s_cachedOpen = false;
        auto menu = ui->GetMenu("PauseMenu");
        if (menu && menu->uiMovie) {
            RE::Scaleform::GFx::Value mainPanelVisible;
            if (menu->uiMovie->GetVariable(std::addressof(mainPanelVisible),
                    "root.Menu_mc.MainPanel_mc.visible")) {
                // Hidden main list => the player has navigated into a sub-panel
                // (MCM's config, or a vanilla sub-menu). Only in that state can
                // the real MCM be writing settings, so that is when we lock our
                // translated pages.
                s_cachedOpen = !ReadGfxBool(mainPanelVisible);
            }
        }
        return s_cachedOpen;
    }

} // namespace MCMConflictCheck
