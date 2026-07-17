#pragma once

#include <map>
#include <string>
#include <vector>
#include <cstdint>

typedef void(__stdcall* HotkeyCallback)();

// Input device type for hotkey bindings
enum class HotkeyDevice : uint8_t {
    Keyboard = 0,
    Gamepad = 1
};

// Lightweight hotkey registry for plugin authors.
// Framework dispatches registered hotkeys via its WndProc hook (keyboard)
// and GamepadInput::Poll() (controller), persisting bindings to the [Hotkeys]
// section of F4SEMenuFramework.ini.
// Mods are responsible for their own rebind UI if desired.
class HotkeyManager {
public:
    struct HotkeyEntry {
        std::string id;
        unsigned int scanCode = 0;        // current binding (DIK scan code or gamepad config code)
        unsigned int defaultScanCode = 0; // original default for reset
        HotkeyCallback callback = nullptr;
        HotkeyCallback releaseCallback = nullptr; // optional key-up callback (keyboard only)
        bool isDown = false;              // down was dispatched, awaiting matching up
        int64_t handle = -1;
        HotkeyDevice device = HotkeyDevice::Keyboard; // which input device this binding uses
    };

    // Register a keyboard hotkey. If the INI already has a persisted binding for this id,
    // it takes precedence over defaultScanCode. Returns a handle for unregister.
    static int64_t Register(const char* id, unsigned int defaultScanCode, HotkeyCallback callback);

    // Register a gamepad hotkey. configCode uses the same button codes as Config
    // (e.g. 4096=A, 8192=B, 256=LB, 9=LT, 10=RT).
    static int64_t RegisterGamepad(const char* id, unsigned int defaultConfigCode, HotkeyCallback callback);

    static void Unregister(int64_t handle);

    // Query / change binding at runtime (for mods building their own rebind UI).
    static unsigned int GetBinding(const char* id);
    static void SetBinding(const char* id, unsigned int scanCode);

    // True when a hotkey with this id has been registered.
    static bool IsRegistered(const char* id);

    // Silently overwrite a binding: no conflict dialog, no external-store
    // notification. Used when importing bindings from the MCM Keybinds.json
    // (that file is already the source of truth, so echoing back is pointless).
    static void ImportBinding(const char* id, unsigned int scanCode);

    // Returns all hotkey ids currently bound to the given scan code, excluding
    // the specified id (pass nullptr to get all). Useful for mods to check for
    // conflicts before calling SetBinding.
    static std::vector<std::string> GetConflicts(unsigned int scanCode, const char* excludeId);

    // Attach a key-up callback to an already-registered keyboard hotkey.
    // Needed by MCM SendEvent keybinds, which deliver both OnControlDown and
    // OnControlUp Papyrus events. Fired from WndProc on WM_KEYUP.
    static void SetReleaseCallback(const char* id, HotkeyCallback callback);

    // Called from WndProc on WM_KEYDOWN first-press when no blocking window is open.
    static void Dispatch(unsigned int scanCode);

    // Called from WndProc on WM_KEYUP. Fires releaseCallback for entries whose
    // down-press we previously dispatched (so a press consumed by a blocking
    // window never produces an orphaned key-up event).
    static void DispatchUp(unsigned int scanCode);

    // Called from GamepadInput::Poll() when digital buttons have rising edges.
    // buttonMask is the XInput bitmask of newly-pressed buttons.
    static void DispatchGamepad(unsigned short buttonMask);

    // Called for analog trigger rising-edge. configCode is 9 (LT) or 10 (RT).
    static void DispatchGamepadTrigger(unsigned int configCode);

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
