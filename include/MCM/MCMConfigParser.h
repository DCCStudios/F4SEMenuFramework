#pragma once

#include <string>
#include <vector>
#include <variant>
#include <optional>
#include <nlohmann/json.hpp>

// Parses MCM config.json files into an internal widget tree representation.
// Each MCM mod's config.json defines the menu layout, control types, and value bindings.
namespace MCMConfigParser {

    // Value source types corresponding to MCM's sourceType field
    enum class SourceType {
        None,
        ModSettingBool,
        ModSettingInt,
        ModSettingFloat,
        ModSettingString,
        GlobalValue,
        PropertyValueBool,
        PropertyValueInt,
        PropertyValueFloat,
        PropertyValueString
    };

    // Describes where a control reads/writes its value
    struct ValueSource {
        SourceType type = SourceType::None;
        std::string sourceForm;       // form ID string (hex) for GlobalValue/PropertyValue
        std::string scriptName;       // script name for PropertyValue
        std::string propertyName;     // property name for PropertyValue
        std::string settingName;      // INI key name for ModSetting (e.g. "bMyToggle")
        std::string defaultValue;     // default value as string
    };

    // An individual option in a dropdown/stepper
    struct OptionItem {
        std::string text;
        std::string value;  // what gets stored when selected
    };

    // Group condition for showing/hiding controls
    struct GroupCondition {
        std::string sourceSettingName;  // the setting to check
        std::string operator_;          // "==", "!=", ">", "<", etc.
        std::string compareValue;       // value to compare against
        SourceType sourceType = SourceType::None;
    };

    // Logical operator applied to a set of groupControl references.
    // MCM supports: N (single), [N,M,...] (OR), {"OR":[..]}, {"AND":[..]},
    // {"ONLY":[..]} (all listed on AND all others on the page off).
    enum class GroupConditionOp {
        None,  // no group condition — always visible
        Or,    // visible if ANY referenced group is on
        And,   // visible if ALL referenced groups are on
        Only   // visible if all referenced groups are on AND all other groups are off
    };

    // A typed parameter for a structured Papyrus action.
    // MCM action "params" arrays can contain JSON bools/ints/floats/strings;
    // the special string "{value}" is substituted with the control's new value
    // (with its native type) at dispatch time.
    struct ActionParam {
        // ValuePlaceholder ("{value}") passes the control's current value with
        // its native runtime type. The ValueAs* variants come from MCM's typed
        // cast prefixes combined with the placeholder (e.g. "{i}{value}") and
        // coerce the control's value to that Papyrus type before packing.
        // StringTemplate is a string with "{value}" embedded in longer text
        // (e.g. FallSouls' "bConsole|{value}"); the control's value is
        // stringified and substituted in place, mirroring MCM's AS3
        // `obj.replace(/{value}/, params.value)`.
        enum class Type { Bool, Int, Float, String, ValuePlaceholder,
                          ValueAsInt, ValueAsFloat, ValueAsBool, StringTemplate };
        Type type = Type::String;
        bool boolVal = false;
        int intVal = 0;
        float floatVal = 0.0f;
        std::string stringVal;
    };

    // Fully structured MCM action (object form from config.json / keybinds.json):
    //   {"type": "CallFunction", "form": "Mod.esp|800", "scriptName": "MyScript",
    //    "function": "SetIntensity", "params": [50.0, "{value}"]}
    // The full set of MCM action types (verified against the real MCM's
    // MCMKeybinds.cpp and its AS3 OptionsList): CallFunction,
    // CallGlobalFunction, CallExternalFunction (invokes a function an F4SE
    // plugin registered on the Scaleform "root.f4se.plugins.<plugin>" object),
    // RunConsoleCommand (uses "command"), SendEvent (uses "form" only —
    // delivers OnControlDown/OnControlUp to the form's script with the
    // keybind id as control name).
    struct MCMAction {
        std::string type;        // "CallFunction", "CallGlobalFunction", "CallExternalFunction", "RunConsoleCommand", "SendEvent"
        std::string form;        // "Plugin.esp|HexID" (CallFunction / SendEvent)
        std::string scriptName;  // script name ("script" key for global calls); optional for CallFunction
        std::string function;    // function name (Call* types)
        std::string command;     // console command (RunConsoleCommand)
        std::string plugin;      // Scaleform plugin-object name (CallExternalFunction)
        std::vector<ActionParam> params;
    };

    // Represents a single MCM UI control
    struct MCMControl {
        std::string type;             // "switcher", "slider", "dropdown", etc.
        std::string text;             // display label
        std::string help;             // tooltip / description
        std::string id;               // unique control ID within the mod

        ValueSource valueSource;

        // Dynamic label: "textFromStringProperty" pulls the control's display
        // text from a Papyrus string property at runtime (used by info rows
        // like IAA Backpack's equipped-backpack display). Stored as a
        // PropertyValueString source; resolved asynchronously by the renderer.
        std::optional<ValueSource> textSource;

        // Slider-specific
        float sliderMin = 0.0f;
        float sliderMax = 100.0f;
        float sliderStep = 1.0f;

        // Dropdown/stepper options
        std::vector<OptionItem> options;
        std::string optionsFrom;      // reference to sharedOptions key

        // Button action — legacy simple string form ("CallFunction:Script.Function")
        std::string action;

        // Structured action (object form with form/params) — preferred when present
        std::optional<MCMAction> actionObj;

        // Image — two forms are supported:
        //  1) MCM's real form: a Flash library symbol. The image is the exported
        //     class `imageClassName` inside Data/MCM/Config/<imageLibName>/lib.swf.
        //     This is what virtually every real MCM mod uses (banners/logos).
        //  2) A plain image file path (dds/png/svg) via "image"/"path"/"source"
        //     — non-standard but supported for mods that ship loose images.
        std::string imagePath;        // loose file path (form 2)
        std::string imageLibName;     // lib.swf folder under Data/MCM/Config (form 1)
        std::string imageClassName;   // exported symbol/class inside lib.swf (form 1)
        float imageWidth = 0.0f;      // display width in px ("width")
        float imageHeight = 0.0f;     // display height in px ("height")

        // Group visibility condition (non-standard object form kept for compat)
        std::optional<GroupCondition> groupCondition;

        // Standard MCM group condition: operator + referenced groupControl IDs
        GroupConditionOp groupConditionOp = GroupConditionOp::None;
        std::vector<int> groupConditionRefs;

        // Group control ID — this control defines a group that other controls reference
        int groupControlId = -1;

        // Text input constraints
        int maxLength = 256;

        // Spacer sizing (numLines and height are mutually exclusive per the wiki)
        int spacerLines = 1;
        float spacerHeight = 0.0f;

        // Positioner range
        float posMinX = 0.0f, posMaxX = 1920.0f;
        float posMinY = 0.0f, posMaxY = 1080.0f;

        // Per-control target mod override ("modName" on the control). The real
        // MCM lets a control read/write ANOTHER mod's settings INI — FallUI's
        // Icon Library uses this to write sItemSorterTagConfig into FallUI.ini.
        // Empty = use the owning mod's name.
        std::string modNameOverride;

        // dropdownFiles: directory listing dropdown. "path" is relative to the
        // game root (e.g. "data\\Interface\\ItemSorter"), "mask" a wildcard
        // like "*.xml". The stored value is the file NAME with extension.
        std::string filesPath;
        std::string filesMask;
    };

    // Represents a page within an MCM mod's config
    struct MCMPage {
        std::string pageDisplayName;
        std::vector<MCMControl> controls;
    };

    // The fully parsed config for one MCM mod
    struct MCMModConfig {
        std::string modName;          // folder/identifier name
        std::string displayName;      // human-readable name from config
        std::vector<MCMPage> pages;   // empty vector = single-page mod

        // Shared options that controls can reference via optionsFrom
        std::map<std::string, std::vector<OptionItem>> sharedOptions;
    };

    // Parse a config.json file into a MCMModConfig structure.
    // Returns nullopt if the file cannot be parsed or is invalid.
    std::optional<MCMModConfig> Parse(const std::filesystem::path& configPath, const std::string& modName);

    // Parse a SourceType from the MCM sourceType string
    SourceType ParseSourceType(const std::string& str);

    // Parse a structured object-form action (also used by the keybind translator).
    std::optional<MCMAction> ParseAction(const nlohmann::json& act);

}
