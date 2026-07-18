#include "MCM/MCMConfigParser.h"
#include <fstream>

namespace MCMConfigParser {

    using json = nlohmann::json;

    SourceType ParseSourceType(const std::string& str) {
        if (str == "ModSettingBool") return SourceType::ModSettingBool;
        if (str == "ModSettingInt") return SourceType::ModSettingInt;
        if (str == "ModSettingFloat") return SourceType::ModSettingFloat;
        if (str == "ModSettingString") return SourceType::ModSettingString;
        if (str == "GlobalValue") return SourceType::GlobalValue;
        if (str == "PropertyValueBool") return SourceType::PropertyValueBool;
        if (str == "PropertyValueInt") return SourceType::PropertyValueInt;
        if (str == "PropertyValueFloat") return SourceType::PropertyValueFloat;
        if (str == "PropertyValueString") return SourceType::PropertyValueString;
        return SourceType::None;
    }

    static ValueSource ParseValueSource(const json& j) {
        ValueSource vs;
        if (j.contains("sourceType") && j["sourceType"].is_string()) {
            vs.type = ParseSourceType(j["sourceType"].get<std::string>());
        }
        if (j.contains("sourceForm") && j["sourceForm"].is_string()) vs.sourceForm = j["sourceForm"].get<std::string>();
        if (j.contains("scriptName") && j["scriptName"].is_string()) vs.scriptName = j["scriptName"].get<std::string>();
        if (j.contains("propertyName") && j["propertyName"].is_string()) vs.propertyName = j["propertyName"].get<std::string>();
        if (j.contains("settingName") && j["settingName"].is_string()) vs.settingName = j["settingName"].get<std::string>();
        if (j.contains("defaultValue")) {
            if (j["defaultValue"].is_string()) {
                vs.defaultValue = j["defaultValue"].get<std::string>();
            } else {
                vs.defaultValue = j["defaultValue"].dump();
            }
        }
        return vs;
    }

    static std::optional<GroupCondition> ParseGroupCondition(const json& j) {
        if (!j.contains("groupCondition")) return std::nullopt;

        const auto& gc = j["groupCondition"];
        GroupCondition cond;
        if (gc.contains("sourceSettingName") && gc["sourceSettingName"].is_string()) cond.sourceSettingName = gc["sourceSettingName"].get<std::string>();
        if (gc.contains("operator") && gc["operator"].is_string()) cond.operator_ = gc["operator"].get<std::string>();
        if (gc.contains("compareValue")) {
            if (gc["compareValue"].is_string()) {
                cond.compareValue = gc["compareValue"].get<std::string>();
            } else {
                cond.compareValue = gc["compareValue"].dump();
            }
        }
        if (gc.contains("sourceType") && gc["sourceType"].is_string()) {
            cond.sourceType = ParseSourceType(gc["sourceType"].get<std::string>());
        }
        return cond;
    }

    // Parses one entry of an action "params" array into a typed ActionParam.
    // The special string "{value}" becomes a placeholder that is substituted
    // with the control's current value at dispatch time.
    static ActionParam ParseActionParam(const json& p) {
        ActionParam param;
        if (p.is_boolean()) {
            param.type = ActionParam::Type::Bool;
            param.boolVal = p.get<bool>();
        } else if (p.is_number_integer()) {
            param.type = ActionParam::Type::Int;
            param.intVal = p.get<int>();
        } else if (p.is_number_float()) {
            param.type = ActionParam::Type::Float;
            param.floatVal = p.get<float>();
        } else if (p.is_string()) {
            std::string s = p.get<std::string>();
            if (s == "{value}") {
                param.type = ActionParam::Type::ValuePlaceholder;
            } else {
                param.type = ActionParam::Type::String;
                param.stringVal = std::move(s);
            }
        } else {
            // Unknown JSON type — pass its serialized form as a string
            param.type = ActionParam::Type::String;
            param.stringVal = p.dump();
        }
        return param;
    }

    // Parses the standard MCM object-form action.
    // Both "scriptName" (CallFunction) and "script" (CallGlobalFunction) keys
    // are accepted for the script name field.
    std::optional<MCMAction> ParseAction(const json& act) {
        if (!act.is_object()) return std::nullopt;

        MCMAction action;
        if (act.contains("type") && act["type"].is_string()) action.type = act["type"].get<std::string>();
        if (act.contains("form") && act["form"].is_string()) action.form = act["form"].get<std::string>();
        if (act.contains("scriptName") && act["scriptName"].is_string()) action.scriptName = act["scriptName"].get<std::string>();
        if (action.scriptName.empty() && act.contains("script") && act["script"].is_string()) {
            action.scriptName = act["script"].get<std::string>();
        }
        if (act.contains("function") && act["function"].is_string()) action.function = act["function"].get<std::string>();
        if (act.contains("command") && act["command"].is_string()) action.command = act["command"].get<std::string>();

        if (act.contains("params") && act["params"].is_array()) {
            for (const auto& p : act["params"]) {
                action.params.push_back(ParseActionParam(p));
            }
        }

        // Per-type validation, matching what the real MCM's keybind/action
        // executor actually requires:
        //  - CallFunction:       form + function (scriptName optional — MCM
        //                        defaults to ScriptObject when omitted)
        //  - CallGlobalFunction: script + function
        //  - RunConsoleCommand:  command
        //  - SendEvent:          form (OnControlDown/Up delivered to its script)
        if (action.type.empty()) return std::nullopt;
        if (action.type == "RunConsoleCommand") {
            if (action.command.empty()) return std::nullopt;
        } else if (action.type == "SendEvent") {
            if (action.form.empty()) return std::nullopt;
        } else if (action.function.empty()) {
            return std::nullopt;
        }
        return action;
    }

    static std::vector<OptionItem> ParseOptions(const json& j) {
        std::vector<OptionItem> items;
        if (!j.is_array()) return items;

        for (const auto& opt : j) {
            OptionItem item;
            if (opt.is_string()) {
                // Plain string option: ["Option1", "Option2"]
                item.text = opt.get<std::string>();
                item.value = item.text;
            } else if (opt.is_object()) {
                // Object option: {"text": "...", "value": ...}
                if (opt.contains("text") && opt["text"].is_string()) item.text = opt["text"].get<std::string>();
                if (opt.contains("value")) {
                    if (opt["value"].is_string()) {
                        item.value = opt["value"].get<std::string>();
                    } else {
                        item.value = opt["value"].dump();
                    }
                }
            } else {
                // Fallback: dump whatever it is
                item.text = opt.dump();
                item.value = item.text;
            }
            items.push_back(std::move(item));
        }
        return items;
    }

    static MCMControl ParseControl(const json& j) {
        MCMControl ctrl;

        if (j.contains("type") && j["type"].is_string()) ctrl.type = j["type"].get<std::string>();
        if (j.contains("text") && j["text"].is_string()) ctrl.text = j["text"].get<std::string>();
        if (j.contains("help") && j["help"].is_string()) ctrl.help = j["help"].get<std::string>();
        if (j.contains("id") && j["id"].is_string()) ctrl.id = j["id"].get<std::string>();

        // Value source — check both top-level and nested valueOptions
        ctrl.valueSource = ParseValueSource(j);

        // Nested valueOptions object (KARMA, NAC X, etc.)
        if (j.contains("valueOptions") && j["valueOptions"].is_object()) {
            auto& vo = j["valueOptions"];
            // Override value source from nested object
            auto nestedSource = ParseValueSource(vo);
            if (nestedSource.type != SourceType::None) {
                ctrl.valueSource = nestedSource;
            }
            // Override slider params from nested object
            if (vo.contains("min") && vo["min"].is_number()) ctrl.sliderMin = vo["min"].get<float>();
            if (vo.contains("max") && vo["max"].is_number()) ctrl.sliderMax = vo["max"].get<float>();
            if (vo.contains("step") && vo["step"].is_number()) ctrl.sliderStep = vo["step"].get<float>();
            // Override options from nested object
            if (vo.contains("options")) ctrl.options = ParseOptions(vo["options"]);
            // Handle sharedOptions reference inside valueOptions
            if (vo.contains("sharedOptions") && vo["sharedOptions"].is_string()) {
                ctrl.optionsFrom = vo["sharedOptions"].get<std::string>();
            }
            // dropdownFiles directory + wildcard mask (FallUI Icon Library etc.)
            if (vo.contains("path") && vo["path"].is_string()) ctrl.filesPath = vo["path"].get<std::string>();
            if (vo.contains("mask") && vo["mask"].is_string()) ctrl.filesMask = vo["mask"].get<std::string>();
        }

        // Per-control target-mod override: the control reads/writes settings in
        // ANOTHER mod's INI (real MCM behavior; FallUI Icon Library relies on it).
        if (j.contains("modName") && j["modName"].is_string()) {
            ctrl.modNameOverride = j["modName"].get<std::string>();
        }

        // Slider properties
        if (j.contains("sliderMin") && j["sliderMin"].is_number()) ctrl.sliderMin = j["sliderMin"].get<float>();
        if (j.contains("min") && j["min"].is_number()) ctrl.sliderMin = j["min"].get<float>();
        if (j.contains("sliderMax") && j["sliderMax"].is_number()) ctrl.sliderMax = j["sliderMax"].get<float>();
        if (j.contains("max") && j["max"].is_number()) ctrl.sliderMax = j["max"].get<float>();
        if (j.contains("sliderStep") && j["sliderStep"].is_number()) ctrl.sliderStep = j["sliderStep"].get<float>();
        if (j.contains("step") && j["step"].is_number()) ctrl.sliderStep = j["step"].get<float>();

        // Options (dropdown, stepper)
        if (j.contains("options")) ctrl.options = ParseOptions(j["options"]);
        if (j.contains("optionsFrom") && j["optionsFrom"].is_string()) ctrl.optionsFrom = j["optionsFrom"].get<std::string>();

        // Button action — can be a string ("CallFunction:Script.Function") or the
        // standard MCM object form with form/scriptName/function/params.
        if (j.contains("action")) {
            if (j["action"].is_string()) {
                ctrl.action = j["action"].get<std::string>();
            } else if (j["action"].is_object()) {
                auto parsed = ParseAction(j["action"]);
                if (parsed.has_value()) {
                    ctrl.actionObj = std::move(parsed);
                }
            }
        }

        // Dynamic label from a Papyrus string property:
        //   "textFromStringProperty": {"form": "Mod.esp|4E0",
        //                              "scriptName": "MyScript",
        //                              "propertyName": "SomeStringProp"}
        if (j.contains("textFromStringProperty") && j["textFromStringProperty"].is_object()) {
            const auto& tp = j["textFromStringProperty"];
            ValueSource ts;
            ts.type = SourceType::PropertyValueString;
            if (tp.contains("form") && tp["form"].is_string()) ts.sourceForm = tp["form"].get<std::string>();
            if (tp.contains("sourceForm") && tp["sourceForm"].is_string()) ts.sourceForm = tp["sourceForm"].get<std::string>();
            if (tp.contains("scriptName") && tp["scriptName"].is_string()) ts.scriptName = tp["scriptName"].get<std::string>();
            if (tp.contains("propertyName") && tp["propertyName"].is_string()) ts.propertyName = tp["propertyName"].get<std::string>();
            if (!ts.sourceForm.empty() && !ts.propertyName.empty()) {
                ctrl.textSource = std::move(ts);
            }
        }

        // Image — MCM's real form references a Flash library symbol:
        //   { "type":"image", "libName":"FallUIHUD", "className":"M8r.View.FallUIHUDIntro",
        //     "width":770, "height":450 }
        // The image is the exported class `className` inside the SWF at
        // Data/MCM/Config/<libName>/lib.swf. We also keep the older loose-file
        // forms ("image": "path" or { "path": ... }/"source"/"src") for mods
        // that ship plain image files instead.
        if (j.contains("libName") && j["libName"].is_string()) ctrl.imageLibName = j["libName"].get<std::string>();
        if (j.contains("className") && j["className"].is_string()) ctrl.imageClassName = j["className"].get<std::string>();

        // Loose file-path forms (non-standard, best-effort).
        if (j.contains("image")) {
            if (j["image"].is_string()) {
                ctrl.imagePath = j["image"].get<std::string>();
            } else if (j["image"].is_object() && j["image"].contains("path") && j["image"]["path"].is_string()) {
                ctrl.imagePath = j["image"]["path"].get<std::string>();
            }
        }
        if (ctrl.imagePath.empty() && j.contains("source") && j["source"].is_string()) ctrl.imagePath = j["source"].get<std::string>();
        if (ctrl.imagePath.empty() && j.contains("src") && j["src"].is_string()) ctrl.imagePath = j["src"].get<std::string>();

        // Display size: MCM uses "width"/"height"; keep imageWidth/imageHeight too.
        if (j.contains("width") && j["width"].is_number()) ctrl.imageWidth = j["width"].get<float>();
        if (j.contains("height") && j["height"].is_number()) ctrl.imageHeight = j["height"].get<float>();
        if (j.contains("imageWidth") && j["imageWidth"].is_number()) ctrl.imageWidth = j["imageWidth"].get<float>();
        if (j.contains("imageHeight") && j["imageHeight"].is_number()) ctrl.imageHeight = j["imageHeight"].get<float>();

        // For ModSetting source types, use the control id as settingName if not explicitly set
        // MCM convention: control "id" field is "key:Section" format for INI lookups
        if (ctrl.valueSource.settingName.empty() && !ctrl.id.empty()) {
            if (ctrl.valueSource.type == MCMConfigParser::SourceType::ModSettingBool ||
                ctrl.valueSource.type == MCMConfigParser::SourceType::ModSettingInt ||
                ctrl.valueSource.type == MCMConfigParser::SourceType::ModSettingFloat ||
                ctrl.valueSource.type == MCMConfigParser::SourceType::ModSettingString) {
                ctrl.valueSource.settingName = ctrl.id;
            }
        }

        // Group condition — standard MCM forms:
        //   N              -> single reference (OR of one)
        //   [N, M]         -> OR
        //   {"OR": [...]}  -> OR
        //   {"AND": [...]} -> AND
        //   {"ONLY": [...]}-> listed on AND all other page groups off
        // Plus a non-standard object form (sourceSettingName/operator/compareValue)
        // kept for configs that use it.
        if (j.contains("groupCondition")) {
            const auto& gc = j["groupCondition"];

            // Helper: pull an array (or single int) of group IDs into refs
            auto readRefs = [](const json& node, std::vector<int>& out) {
                if (node.is_number_integer()) {
                    out.push_back(node.get<int>());
                } else if (node.is_array()) {
                    for (const auto& e : node) {
                        if (e.is_number_integer()) out.push_back(e.get<int>());
                    }
                }
            };

            if (gc.is_number_integer() || gc.is_array()) {
                ctrl.groupConditionOp = GroupConditionOp::Or;
                readRefs(gc, ctrl.groupConditionRefs);
            } else if (gc.is_object()) {
                if (gc.contains("OR")) {
                    ctrl.groupConditionOp = GroupConditionOp::Or;
                    readRefs(gc["OR"], ctrl.groupConditionRefs);
                } else if (gc.contains("AND")) {
                    ctrl.groupConditionOp = GroupConditionOp::And;
                    readRefs(gc["AND"], ctrl.groupConditionRefs);
                } else if (gc.contains("ONLY")) {
                    ctrl.groupConditionOp = GroupConditionOp::Only;
                    readRefs(gc["ONLY"], ctrl.groupConditionRefs);
                } else {
                    // Non-standard comparison object form
                    ctrl.groupCondition = ParseGroupCondition(j);
                }
            }

            // Degenerate condition (no refs parsed) — treat as no condition
            if (ctrl.groupConditionRefs.empty() &&
                ctrl.groupConditionOp != GroupConditionOp::None && !ctrl.groupCondition.has_value()) {
                ctrl.groupConditionOp = GroupConditionOp::None;
            }
        }

        // Group control ID — marks this control as defining a visibility group
        if (j.contains("groupControl") && j["groupControl"].is_number_integer()) {
            ctrl.groupControlId = j["groupControl"].get<int>();
        }

        // Text input
        if (j.contains("maxLength")) ctrl.maxLength = j["maxLength"].get<int>();

        // Spacer sizing (numLines or height, not both)
        if (j.contains("numLines") && j["numLines"].is_number()) ctrl.spacerLines = j["numLines"].get<int>();
        if (j.contains("height") && j["height"].is_number()) ctrl.spacerHeight = j["height"].get<float>();

        // Positioner
        if (j.contains("posMinX")) ctrl.posMinX = j["posMinX"].get<float>();
        if (j.contains("posMaxX")) ctrl.posMaxX = j["posMaxX"].get<float>();
        if (j.contains("posMinY")) ctrl.posMinY = j["posMinY"].get<float>();
        if (j.contains("posMaxY")) ctrl.posMaxY = j["posMaxY"].get<float>();

        return ctrl;
    }

    static std::vector<MCMControl> ParseControls(const json& j) {
        std::vector<MCMControl> controls;
        if (!j.is_array()) return controls;

        for (const auto& item : j) {
            controls.push_back(ParseControl(item));
        }
        return controls;
    }

    std::optional<MCMModConfig> Parse(const std::filesystem::path& configPath, const std::string& modName) {
        std::ifstream file(configPath);
        if (!file.is_open()) {
            logger::error("[MCMConfigParser] Failed to open: {}", configPath.string());
            return std::nullopt;
        }

        json root;
        try {
            // ignore_comments=true: real MCM parses configs with JsonCpp, whose
            // default reader accepts // and /* */ comments — and shipped mods
            // rely on that (Jump Grunt, Elzee Recoil Shake annotate every key).
            // Strict parsing made those configs fail entirely.
            root = json::parse(file, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
        } catch (const json::parse_error& e) {
            logger::error("[MCMConfigParser] JSON parse error in '{}': {}", configPath.string(), e.what());
            return std::nullopt;
        }

        MCMModConfig config;
        config.modName = modName;

        // Display name (fall back to folder name)
        if (root.contains("displayName") && root["displayName"].is_string()) {
            config.displayName = root["displayName"].get<std::string>();
        } else {
            config.displayName = modName;
        }

        // Parse shared options
        if (root.contains("sharedOptions") && root["sharedOptions"].is_object()) {
            for (auto& [key, val] : root["sharedOptions"].items()) {
                config.sharedOptions[key] = ParseOptions(val);
            }
        }

        // Parse sharedLists (alternative key used by some mods like NAC X)
        if (root.contains("sharedLists") && root["sharedLists"].is_object()) {
            for (auto& [key, val] : root["sharedLists"].items()) {
                config.sharedOptions[key] = ParseOptions(val);
            }
        }

        // Parse pages
        // MCM convention: if both "content" and "pages" exist at root level,
        // "content" is the main/default page and "pages" are additional sub-pages.
        if (root.contains("content") && root["content"].is_array()) {
            MCMPage mainPage;
            mainPage.pageDisplayName = config.displayName;
            mainPage.controls = ParseControls(root["content"]);
            config.pages.push_back(std::move(mainPage));
        }

        if (root.contains("pages") && root["pages"].is_array()) {
            for (const auto& page : root["pages"]) {
                MCMPage p;
                if (page.contains("pageDisplayName") && page["pageDisplayName"].is_string()) {
                    p.pageDisplayName = page["pageDisplayName"].get<std::string>();
                } else if (page.contains("displayName") && page["displayName"].is_string()) {
                    p.pageDisplayName = page["displayName"].get<std::string>();
                } else {
                    p.pageDisplayName = "Page";
                }

                if (page.contains("content")) {
                    p.controls = ParseControls(page["content"]);
                }
                config.pages.push_back(std::move(p));
            }
        }

        logger::info("[MCMConfigParser] Parsed '{}': {} page(s), {} shared option set(s)",
            config.displayName, config.pages.size(), config.sharedOptions.size());

        return config;
    }

} // namespace MCMConfigParser
