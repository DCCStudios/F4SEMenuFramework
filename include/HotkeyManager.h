#pragma once

#include <map>
#include <string>
#include <vector>
#include <cstdint>

typedef void(__stdcall* HotkeyCallback)();

// Lightweight hotkey registry for plugin authors.
// Framework dispatches registered hotkeys via its WndProc hook and persists
// bindings to the [Hotkeys] section of F4SEMenuFramework.ini.
// Mods are responsible for their own rebind UI if desired.
class HotkeyManager {
public:
    struct HotkeyEntry {
        std::string id;
        unsigned int scanCode;        // current binding (DIK scan code)
        unsigned int defaultScanCode; // original default for reset
        HotkeyCallback callback;
        int64_t handle;
    };

    // Register a hotkey. If the INI already has a persisted binding for this id,
    // it takes precedence over defaultScanCode. Returns a handle for unregister.
    static int64_t Register(const char* id, unsigned int defaultScanCode, HotkeyCallback callback);
    static void Unregister(int64_t handle);

    // Query / change binding at runtime (for mods building their own rebind UI).
    static unsigned int GetBinding(const char* id);
    static void SetBinding(const char* id, unsigned int scanCode);

    // Returns all hotkey ids currently bound to the given scan code, excluding
    // the specified id (pass nullptr to get all). Useful for mods to check for
    // conflicts before calling SetBinding.
    static std::vector<std::string> GetConflicts(unsigned int scanCode, const char* excludeId);

    // Called from WndProc on WM_KEYDOWN first-press when no blocking window is open.
    static void Dispatch(unsigned int scanCode);

    // Load persisted bindings from INI (called once during init).
    static void Load();
    // Persist all current bindings to INI.
    static void Save();

    // Shows a centered confirmation dialog about hotkey conflicts.
    static void ShowConflictWarning(const std::string& hotkeyId, const std::vector<std::string>& conflicts, unsigned int scanCode);

    // Internal state — accessible by conflict dialog for deferred binding application.
    static inline int64_t autoIncrement = 0;
    static inline std::map<int64_t, HotkeyEntry> entriesByHandle;
    static inline std::map<std::string, int64_t> idToHandle;
};
