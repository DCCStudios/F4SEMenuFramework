#pragma once

#include "MCM/MCMConfigParser.h"
#include <string>
#include <vector>
#include <filesystem>

// Forward declaration to avoid circular header dependency
namespace MCMScanner { struct MCMModInfo; }

// Interface and implementations for reading/writing MCM setting values.
// Supports ModSettings (INI), GlobalValue (game globals), and PropertyValue (Papyrus properties).
namespace MCMValueProvider {

    enum class ProviderStatus {
        Available,       // Value can be read/written normally
        FormNotLoaded,   // Required form doesn't exist in the loaded game
        ScriptMissing,   // Script isn't attached to the target form
        PropertyMissing, // Property doesn't exist on the script
        Error            // Unexpected error
    };

    // Result of a value read operation
    struct ValueResult {
        ProviderStatus status = ProviderStatus::Available;
        std::string statusMessage;

        // The actual values (only one is meaningful depending on source type)
        bool boolVal = false;
        int intVal = 0;
        float floatVal = 0.0f;
        std::string stringVal;
    };

    // Initialize the value provider system (loads settings.ini files for discovered mods)
    void Init(const std::vector<MCMScanner::MCMModInfo>& mods);

    // Read a value from the appropriate backing store
    ValueResult GetValue(const std::string& modName, const MCMConfigParser::ValueSource& source);

    // Write a value to the appropriate backing store
    ProviderStatus SetBool(const std::string& modName, const MCMConfigParser::ValueSource& source, bool val);
    ProviderStatus SetInt(const std::string& modName, const MCMConfigParser::ValueSource& source, int val);
    ProviderStatus SetFloat(const std::string& modName, const MCMConfigParser::ValueSource& source, float val);
    ProviderStatus SetString(const std::string& modName, const MCMConfigParser::ValueSource& source, const std::string& val);

    // Get a human-readable status tooltip for degraded controls
    std::string GetStatusTooltip(ProviderStatus status, const MCMConfigParser::ValueSource& source);

    // --- Asynchronous Papyrus property reads ---
    // Property values can't be read synchronously (the VM resolves them on its
    // own thread), so reads are two-phase:
    //   1. RequestPropertyRead() dispatches vm->GetPropertyValue with a
    //      callback functor; safe to call repeatedly — only the first call
    //      per key dispatches until the key is invalidated.
    //   2. TryTakePropertyResult() polls for the completed result (one-shot:
    //      the result is consumed). The renderer calls this every frame.
    // requestKey must uniquely identify the reading control.
    void RequestPropertyRead(const std::string& requestKey, const MCMConfigParser::ValueSource& source);
    bool TryTakePropertyResult(const std::string& requestKey, ValueResult& out);

    // Forget all in-flight/consumed read keys so the next RequestPropertyRead
    // dispatches again (used by RefreshMenu and menu re-open).
    void InvalidateAsyncPropertyReads();

    // Re-read every mod's layered settings INIs from disk, replacing the
    // in-memory cache. Used to pick up changes the *native* MCM wrote while
    // our menu was closed (it commits each edit to Data/MCM/Settings/<Mod>.ini
    // immediately). Safe: our own writes are flushed to disk per-write, so
    // dropping the cache loses nothing.
    void ReloadAll();

    // Flush all pending INI changes to disk
    void FlushAll();

    // --- Raw mod-setting access (used by the MCM Papyrus natives) ---
    // settingName uses MCM's "key:Section" format (section defaults to "Main").
    // These work for ANY mod name, not just scanned ones: if the mod isn't in
    // the cache yet, its Config defaults + user settings INIs are loaded on
    // demand. Returns nullopt if the setting doesn't exist anywhere.
    std::optional<std::string> GetModSettingRaw(const std::string& modName, const std::string& settingName);

    // Writes a raw mod setting value and flushes the user settings file
    // (Data/MCM/Settings/<ModName>.ini — same location the real MCM writes).
    void SetModSettingRaw(const std::string& modName, const std::string& settingName, const std::string& value);

}
