#pragma once

// Detects whether the native MCM plugin (MCM.dll) is loaded in the process.
//
// Policy (user decision — no consent popup):
//   - Real MCM absent  -> our MCM compatibility layer loads freely.
//   - Real MCM present -> our translation layer is silently disabled by
//     default so the real MCM stays authoritative. The user can force-enable
//     both via the "MCMCompatWhenNativePresent" toggle in the framework
//     settings window (persisted in F4SEMenuFramework.ini).
namespace MCMConflictCheck {

    enum class ConflictState {
        NotChecked,      // Haven't checked yet
        NoConflict,      // Real MCM not loaded, safe to proceed
        ConflictSkip,    // Real MCM loaded — our layer auto-disabled (default)
        ConflictAllow    // Real MCM loaded but user force-enabled our layer
    };

    // Performs the conflict check. Should be called on kGameDataReady.
    void Check();

    // Returns current conflict state.
    ConflictState GetState();

    // Returns true if the native MCM.dll is loaded in this process.
    bool IsNativeMCMPresent();

    // Returns true if our MCM compat menus should be loaded
    // (no real MCM, or the user force-enabled coexistence).
    bool ShouldLoadMCMMenus();

    // Returns true only while the native MCM (or another pause sub-panel) is
    // actually displayed right now — i.e. MCM.dll is present, the pause menu is
    // open, AND the main pause list (root.Menu_mc.MainPanel_mc) is hidden,
    // which the game does only after the player drills into a sub-panel (MCM's
    // config sets MainPanel_mc.visible = false on open). A plain ESC pause
    // keeps the main list visible and therefore does NOT count — so freezing
    // the game before opening OUR overlay no longer wrongly locks the pages.
    // (We deliberately do NOT key on root.mcm_loader.content: MCM loads that
    // clip the instant the pause menu opens, so it is not an "is-open" signal.)
    // Used to lock our translated pages so the two systems can't write the same
    // setting files simultaneously.
    bool IsNativeMCMMenuOpen();

}
