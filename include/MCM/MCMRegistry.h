#pragma once

// MCMRegistry is the top-level orchestrator for the MCM backwards compatibility system.
// It coordinates scanning, parsing, conflict detection, value initialization,
// keybind registration, and widget rendering registration.
namespace MCMRegistry {

    // Initialize the entire MCM compat system.
    // Should be called on kGameDataReady after Config::Init().
    // Performs: scan -> conflict check -> parse configs -> init values -> register keybinds -> register widgets
    void Init();

    // Returns the number of successfully loaded MCM mods.
    int GetLoadedModCount();

    // Returns true if the MCM compat system is enabled and active.
    bool IsActive();

}
