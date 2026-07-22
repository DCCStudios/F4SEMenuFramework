#include "MCM/FallUIHudEditor.h"
#include "MCM/FallUIHudArt.h"
#include "MCM/MCMValueProvider.h"
#include "MCM/MCMTranslation.h"
#include "imgui.h"
#include "imgui_internal.h"  // BringWindowToDisplayFront (panel z-order)

#include <RE/Fallout.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

// ============================================================================
// Native recreation of FallUI's Flash-embedded MCM applications.
//
// Ported from the decompiled FallUI - HUD lib.swf (M8r.* packages):
//   HUDMenuStruct.as      -> widget catalog (groups, positions, sizes, titles)
//   HUDLayoutOptions.as   -> per-widget option model + pack/unpack formats
//   HudLayoutManager.as   -> editor behavior (profiles, import/export, modes)
//   HudItem.as            -> widget interaction (drag, wheel-scale, save)
//   StringPacker.as       -> "name;type;value;..." object serialization
//   FallUIIconLibrary.as  -> icon preset auto-sync logic
//
// DATA-FORMAT COMPATIBILITY IS THE CONTRACT: everything written here must be
// byte-compatible with what the Flash editor writes, because FallUI's runtime
// HUD swf parses Data/MCM/Settings/FallUIHUD.ini itself (via its IniReader)
// and applies the same packed strings.
// ============================================================================

namespace FallUIHudEditor {

    namespace fs = std::filesystem;

    // ------------------------------------------------------------------
    // FallUI HUD palette (HUDStyle.as)
    // ------------------------------------------------------------------
    static constexpr int kFo4Green = 757521;        // 0x0B8F11
    static constexpr int kFallUIHudBlue = 2544639;  // 0x26D3FF

    // Special color-slot sentinels (HUDLayoutOptions.as)
    static constexpr int COLOR_HUD = -1;   // player's HUD color
    static constexpr int COLOR_RED = -2;   // warning red
    static constexpr int COLOR_SUB = -3;   // subtitle gray
    static constexpr int COLOR_NONE = -4;  // no tint (white)
    static constexpr int COLOR_POWER = -5; // (unused by the options we expose)

    // Tools.mixColors(a, b, f) = a*(1-f) + b*f per channel
    static ImU32 MixRGB(int a, int b, float f, float alpha = 1.0f) {
        auto ch = [&](int shift) {
            float va = static_cast<float>((a >> shift) & 0xFF);
            float vb = static_cast<float>((b >> shift) & 0xFF);
            return static_cast<int>(va * (1.0f - f) + vb * f);
        };
        return IM_COL32(ch(16), ch(8), ch(0), static_cast<int>(alpha * 255.0f));
    }

    static ImU32 RGBIntToImU32(int rgb, float alpha = 1.0f) {
        return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF,
                        static_cast<int>(alpha * 255.0f));
    }

    // ------------------------------------------------------------------
    // Translation (Data/Interface/FallUI HUD/Translation/Translate_en.txt)
    // ------------------------------------------------------------------
    static MCMTranslation::TranslationMap s_editorTr;
    static bool s_trLoaded = false;

    static void EnsureTranslationsLoaded() {
        if (s_trLoaded) return;
        s_trLoaded = true;
        // The editor ships its own translation files next to the importable
        // layouts. LoadDirectory picks Translate_en.txt as the base and
        // overlays Translate_<game language>.txt per key — the same selection
        // the original FallUI editor makes from the game language.
        auto dir = fs::current_path() / "Data" / "Interface" / "FallUI HUD" / "Translation";
        s_editorTr = MCMTranslation::LoadDirectory(dir);
        if (s_editorTr.empty()) {
            // Directory enumeration can fail under MO2's virtual filesystem —
            // fall back to opening the known filenames directly.
            s_editorTr = MCMTranslation::LoadFile(dir / "Translate_en.txt");
            if (MCMTranslation::GetLanguage() != "en") {
                auto langMap = MCMTranslation::LoadFile(
                    dir / ("Translate_" + MCMTranslation::GetLanguage() + ".txt"));
                for (auto& [k, v] : langMap) {
                    s_editorTr[k] = std::move(v);
                }
            }
        }
    }

    // Resolve "$Key" / "${Key}" tokens through the FallUI HUD translation file.
    static std::string Tr(const std::string& token) {
        EnsureTranslationsLoaded();
        return MCMTranslation::Resolve(token, s_editorTr);
    }

    // ------------------------------------------------------------------
    // StringPacker port (M8r.Helper.StringPacker)
    // Format: name;type;value;... with types b/i/f/s (and o/O nesting, which
    // FallUI's layout data never uses at top level except sPluginProperties).
    // ------------------------------------------------------------------
    struct PackedValue {
        enum class Kind { Bool, Int, Float, String };
        Kind kind = Kind::String;
        bool b = false;
        int i = 0;
        double f = 0.0;
        std::string s;

        static PackedValue B(bool v) { PackedValue p; p.kind = Kind::Bool; p.b = v; return p; }
        static PackedValue I(int v) { PackedValue p; p.kind = Kind::Int; p.i = v; return p; }
        static PackedValue F(double v) { PackedValue p; p.kind = Kind::Float; p.f = v; return p; }
        static PackedValue S(std::string v) { PackedValue p; p.kind = Kind::String; p.s = std::move(v); return p; }

        // Loose numeric view (AS-style truthiness / int() coercion)
        double AsNumber() const {
            switch (kind) {
                case Kind::Bool: return b ? 1.0 : 0.0;
                case Kind::Int: return i;
                case Kind::Float: return f;
                default: {
                    try { return std::stod(s); } catch (...) { return 0.0; }
                }
            }
        }
        bool AsBool() const { return AsNumber() != 0.0 || (kind == Kind::String && s == "true"); }
        int AsInt() const { return static_cast<int>(AsNumber()); }
    };

    using PackedObject = std::map<std::string, PackedValue>;

    // AS Number formatting: integers print without decimals; non-integers use
    // the SHORTEST representation that round-trips (what AS3 Number.toString()
    // does — e.g. 1.309999999999999 must NOT become 1.3099999999999999,
    // shipped presets contain such values). std::to_chars gives exactly that.
    static std::string FormatNumber(double v) {
        if (std::floor(v) == v && std::abs(v) < 1e15) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
            return buf;
        }
        char buf[64];
        auto res = std::to_chars(buf, buf + sizeof(buf), v);
        return std::string(buf, res.ptr);
    }

    static std::string StringPackerPack(const PackedObject& obj) {
        std::vector<std::string> parts;
        for (const auto& [key, val] : obj) {
            parts.push_back(key);
            switch (val.kind) {
                case PackedValue::Kind::Bool:
                    parts.push_back("b");
                    parts.push_back(val.b ? "1" : "0");
                    break;
                case PackedValue::Kind::Int:
                    parts.push_back("i");
                    parts.push_back(std::to_string(val.i));
                    break;
                case PackedValue::Kind::Float:
                    parts.push_back("f");
                    parts.push_back(FormatNumber(val.f));
                    break;
                case PackedValue::Kind::String: {
                    parts.push_back("s");
                    // Escape ';' exactly like the original ("&_SEM§" is UTF-8 here)
                    std::string esc = val.s;
                    size_t pos = 0;
                    while ((pos = esc.find(';', pos)) != std::string::npos) {
                        esc.replace(pos, 1, "&_SEM\xC2\xA7");
                        pos += 7;
                    }
                    parts.push_back(esc);
                    break;
                }
            }
        }
        std::string out;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i) out += ";";
            out += parts[i];
        }
        return out;
    }

    static PackedObject StringPackerUnpack(const std::string& str) {
        PackedObject obj;
        std::vector<std::string> tok;
        {
            size_t start = 0;
            while (true) {
                size_t pos = str.find(';', start);
                if (pos == std::string::npos) {
                    tok.push_back(str.substr(start));
                    break;
                }
                tok.push_back(str.substr(start, pos - start));
                start = pos + 1;
            }
        }
        size_t i = 0;
        while (i + 1 < tok.size()) {
            const std::string& name = tok[i];
            const std::string& type = tok[i + 1];
            if (type == "b" && i + 2 < tok.size()) {
                obj[name] = PackedValue::B(tok[i + 2] == "1");
                i += 3;
            } else if (type == "i" && i + 2 < tok.size()) {
                int v = 0; try { v = std::stoi(tok[i + 2]); } catch (...) {}
                obj[name] = PackedValue::I(v);
                i += 3;
            } else if (type == "f" && i + 2 < tok.size()) {
                double v = 0; try { v = std::stod(tok[i + 2]); } catch (...) {}
                obj[name] = PackedValue::F(v);
                i += 3;
            } else if (type == "s" && i + 2 < tok.size()) {
                std::string v = tok[i + 2];
                size_t pos = 0;
                while ((pos = v.find("&_SEM\xC2\xA7")) != std::string::npos) {
                    v.replace(pos, 7, ";");
                }
                obj[name] = PackedValue::S(std::move(v));
                i += 3;
            } else if (type == "o" || type == "O") {
                // Nested objects only occur in sPluginProperties, which the
                // editor round-trips untouched — flatten-skip the wrapper.
                i += 2;
            } else {
                break;  // malformed — stop like the original
            }
        }
        return obj;
    }

    // ------------------------------------------------------------------
    // Widget catalog (HUDMenuStruct.mcStruct)
    // Child entries: [x, y, w, h, title, depth?, anchorX?, anchorY?]
    // Coordinates are 1280x720 stage px; itemX/itemY persist in 1920x1080.
    // ------------------------------------------------------------------
    struct WidgetDef {
        const char* group;   // parent group instance name
        const char* name;    // widget instance name
        float vx, vy;        // vanilla position (group-relative, stage px)
        float w, h;          // bounding size (stage px)
        const char* title;   // fallback display title
        int depth;           // insertion depth (-1 = append); kept for parity
        float ax, ay;        // anchor percentages (overrides group align)
        bool hasAnchorOverride;
    };

    struct GroupDef {
        const char* name;
        float x, y;          // group origin on the 1280x720 stage
        const char* align;   // "left" / "center" / "right" / "" (affects default anchor)
        const char* valign;  // only "" in practice (no group sets valign)
    };

    static const GroupDef kGroups[] = {
        { "HUDNotificationsGroup_mc", 64.0f, 36.0f, "left", "" },
        { "TopCenterGroup_mc", 640.0f, 36.0f, "center", "" },
        { "TopRightGroup_mc", 1216.0f, 36.0f, "", "" },
        { "LeftMeters_mc", 64.0f, 684.0f, "left", "" },
        { "CenterGroup_mc", 640.0f, 360.0f, "", "" },
        { "RightMeters_mc", 1216.0f, 684.0f, "right", "" },
        { "BottomCenterGroup_mc", 640.0f, 684.0f, "", "" },
        { "_ext_extra", 0.0f, 0.0f, "", "" },
    };

    static const WidgetDef kWidgets[] = {
        // HUDNotificationsGroup_mc
        { "HUDNotificationsGroup_mc", "XPMeter_mc", -2, 272, 266, 65, "XP", -1, 0, 0, false },
        { "HUDNotificationsGroup_mc", "QuestUpdates_mc", -3, 208, 500, 60, "Quest updates", -1, 0, 0, false },
        { "HUDNotificationsGroup_mc", "ObjectiveUpdates_mc", 0, 272, 344, 28, "Objective updates", -1, 0, 0, false },
        { "HUDNotificationsGroup_mc", "QuestVaultBoy_mc", 7, 44, 210, 152, "Quest Vault Boy", -1, 0, 0, false },
        { "HUDNotificationsGroup_mc", "Messages_mc", 8, 44, 330, 36, "Messages (REAL)", -1, 0, 0, false },
        { "HUDNotificationsGroup_mc", "TutorialText_mc", 8, 44, 330, 161, "Tutorial", -1, 0, 0, false },
        // Not in the original's mcStruct (the Flash editor reads the live
        // display object), but it IS a configurable widget: presets persist
        // sHUDNotificationsGroup_mc__PromptMessageHolder_mc. Position from the
        // HUDMenu.swf placement (7,3); size from the PromptMessageWidget
        // TextField bounds. Title mirrors $PromptMessageHolder_mc.
        { "HUDNotificationsGroup_mc", "PromptMessageHolder_mc", 7, 3, 330, 36, "Messages (deprecated)", -1, 0, 0, false },
        // TopCenterGroup_mc
        { "TopCenterGroup_mc", "StealthMeter_mc", 0, 92, 166, 32, "Stealth", -1, 0, 0, false },
        { "TopCenterGroup_mc", "EnemyHealthMeter_mc", 0, 2, 204, 40, "Enemy", -1, 0, 0, false },
        // TopRightGroup_mc
        { "TopRightGroup_mc", "VaultBoyCondition_mc", 0, 0, 100, 180, "Condition Vault Boy", 0, 100, 0, true },
        // LeftMeters_mc
        { "LeftMeters_mc", "HPMeter_mc", 32, -32, 237, 37, "HP", 1, 15, 15, true },
        { "LeftMeters_mc", "RadsMeter_mc", 262, -46, 100, 60, "Rads", 0, 100, 75, true },
        { "LeftMeters_mc", "LocationText_mc", 0, -116, 386, 40, "Location splash", 2, 0, 0, false },
        // CenterGroup_mc
        { "CenterGroup_mc", "QuickContainerWidget_mc", 253, -134, 270, 234, "Quick-loot", 0, 50, 20, true },
        { "CenterGroup_mc", "HUDCrosshair_mc", 0, 0, 73, 75, "Crosshair", 0, 50, 50, true },
        { "CenterGroup_mc", "ExplosiveIndicatorBase_mc", 0, 0, 50, 50, "Grenades indicator", 0, 50, 50, true },
        { "CenterGroup_mc", "DirectionalHitIndicatorBase_mc", 0, 0, 50, 50, "Direct hit indicator", 0, 50, 50, true },
        { "CenterGroup_mc", "HitIndicator_mc", 0, 0, 50, 50, "Hit indicator", 1, 50, 50, true },
        { "CenterGroup_mc", "RolloverWidget_mc", 253, 0, 325, 140, "Item rollover", 0, 50, 50, true },
        // RightMeters_mc
        { "RightMeters_mc", "HUDActiveEffectsWidget_mc", -30, -22, 100, 30, "Effects", -1, 100, 0, false },
        { "RightMeters_mc", "ActionPointMeter_mc", -30, -32, 238, 37, "AP", 0, 85, 15, true },
        { "RightMeters_mc", "FatigueWarning_mc", -256, -46, 237, 50, "Fatigue warning", 0, 10, 90, true },
        { "RightMeters_mc", "AmmoCount_mc", 0, -84, 52, 84, "Ammo", 0, 100, 50, true },
        { "RightMeters_mc", "FlashLightWidget_mc", 0, -233, 44, 44, "Flash", 0, 100, 100, true },
        { "RightMeters_mc", "ExplosiveAmmoCount_mc", 0, -150, 62, 46, "Grenades", 0, 100, 50, true },
        { "RightMeters_mc", "PowerArmorLowBatteryWarning_mc", 0, -410, 400, 25, "Power Armor low battery", -1, 100, 0, false },
        // BottomCenterGroup_mc
        { "BottomCenterGroup_mc", "CompassWidget_mc", 0, -24, 500, 40, "Compass", 3, 50, 90, true },
        { "BottomCenterGroup_mc", "CritMeter_mc", 0, -80, 256, 33, "Critical meter", 2, 50, 50, true },
        { "BottomCenterGroup_mc", "PerkVaultBoy_mc", 0, -214, 275, 200, "Perk Vault Boy", 1, 50, 50, true },
        { "BottomCenterGroup_mc", "SubtitleText_mc", 0, -30, 550, 162, "Subtitles", 0, 50, 90, true },
        // _ext_extra (prompt message swf; x/y already include the +64/+36 offset)
        { "_ext_extra", "_ext_promptMessageSwf", 72, 79, 33, 36, "Prompt message", -1, 0, 0, false },
    };
    static constexpr int kWidgetCount = static_cast<int>(sizeof(kWidgets) / sizeof(kWidgets[0]));

    static const GroupDef* FindGroup(const std::string& name) {
        for (const auto& g : kGroups) {
            if (name == g.name) return &g;
        }
        return nullptr;
    }

    // Default anchor for a widget: from the group's align unless overridden.
    static void DefaultAnchor(const WidgetDef& w, float& ax, float& ay) {
        ax = 0.0f;
        ay = 0.0f;
        if (const GroupDef* g = FindGroup(w.group)) {
            if (std::string(g->align) == "right") ax = 100.0f;
            else if (std::string(g->align) == "center") ax = 50.0f;
        }
        if (w.hasAnchorOverride) {
            ax = w.ax;
            ay = w.ay;
        }
    }

    // ------------------------------------------------------------------
    // Option model (HUDLayoutOptions.as)
    //
    // Every option the original edit panel offers, with the same ids, types,
    // ranges, order (pos) and stdValues. Handlers/appliers act only on the
    // Flash preview and are irrelevant here (the runtime HUD re-applies
    // options itself from the packed string), so they are not ported.
    //
    // stdValues marked "SWF-derived" in the original (text field sizes,
    // alignments, bar justification) are approximated: they only affect the
    // default shown in the panel — the pack format drops values equal to the
    // stdValue exactly like the original, so persisted data stays clean.
    // ------------------------------------------------------------------
    struct Option {
        std::string id;
        std::string name;         // may contain $ / ${} translation tokens
        enum class Type { Bool, Int, Color, GrpTitle, UiTitle, UiText } type = Type::Bool;
        double pos = 0.0;
        bool deprecated = false;
        bool easyDrag = false;
        bool isGlobalSetting = false;
        int minV = 0, maxV = 100, step = 1;
        bool hasStd = false;
        PackedValue std;          // Bool for bool options, Int for int/color
        std::string onlyIf;       // comma-separated ids, '!' negates
        std::string checkType;    // value-to-text mapping key
    };

    using OptionSet = std::vector<Option>;  // kept sorted by pos

    // Font list (FontLoader.as) for checkType "font"
    static const char* kFontNames[] = {
        "Roboto Condenced", "Roboto Condenced Bold", "Augusta Two", "Brody",
        "Bankir Retro", "Beast", "Arial", "Miniature", "Futura",
        "Futura Condenced", "Helvetica Condenced", "Share-TechMono",
        "WienLight", "PTSans Narrow Bold", "Proxima Nowa Condenced",
        "Handwritten Institute", "Creation Club Font"
    };
    static constexpr int kFontCount = 17;

    static const char* kCrosshairStateNames[] = { "$None", "$Dot", "$Aiming", "$Command", "$Activate" };

    // Builder helpers -----------------------------------------------------

    class OptionBuilder {
    public:
        std::map<std::string, OptionSet> sets;  // parentIdent -> options
        std::set<std::string> colorAdded;       // parent+"."+childPath dedupe

        OptionSet& Parent(const std::string& ident) { return sets[ident]; }

        Option& Add(const std::string& parent, Option o) {
            auto& set = sets[parent];
            set.push_back(std::move(o));
            return set.back();
        }

        Option MakeTitle(const std::string& id, double pos, const std::string& name,
                         Option::Type type = Option::Type::GrpTitle) {
            Option o;
            o.id = id; o.pos = pos; o.name = name; o.type = type;
            return o;
        }
        Option MakeBool(const std::string& id, double pos, const std::string& name, bool std_) {
            Option o;
            o.id = id; o.pos = pos; o.name = name; o.type = Option::Type::Bool;
            o.hasStd = true; o.std = PackedValue::B(std_);
            return o;
        }
        Option MakeInt(const std::string& id, double pos, const std::string& name,
                       int std_, int minV, int maxV, int step = 1) {
            Option o;
            o.id = id; o.pos = pos; o.name = name; o.type = Option::Type::Int;
            o.hasStd = true; o.std = PackedValue::I(std_);
            o.minV = minV; o.maxV = maxV; o.step = step;
            return o;
        }
        // Int option whose stdValue is a (possibly fractional) Number — the
        // original's SWF-derived stds are raw TextField metrics like 43.6.
        // Keeping them fractional matters for pack fidelity: a fractional std
        // never equals a user-set integer, so such values are always written
        // out explicitly, exactly as the Flash editor does.
        Option MakeIntD(const std::string& id, double pos, const std::string& name,
                        double std_, int minV, int maxV) {
            Option o = MakeInt(id, pos, name, 0, minV, maxV);
            o.std = (std::floor(std_) == std_) ? PackedValue::I(static_cast<int>(std_))
                                               : PackedValue::F(std_);
            return o;
        }
        Option MakeColor(const std::string& id, double pos, const std::string& name, int std_) {
            Option o;
            o.id = id; o.pos = pos; o.name = name; o.type = Option::Type::Color;
            o.hasStd = true; o.std = PackedValue::I(std_);
            return o;
        }

        // addColorOptions(parent, childPath, prefix, stdColor, pos, name, onlyIf)
        // -9999 means "look up defaultColors, else COLOR_NONE" — resolved by caller.
        void AddColorOptions(const std::string& parent, const std::string& childPath,
                             const std::string& prefix, int stdColor, double pos,
                             const std::string& name = "", const std::string& onlyIf = "") {
            colorAdded.insert(parent + "." + childPath);
            Option o = MakeColor(prefix + "Cl", pos, name.empty() ? "${Color}" : name, stdColor);
            o.onlyIf = onlyIf;
            Add(parent, std::move(o));
        }
    };

    // defaultColors table (HUDLayoutOptions.as) — element path -> default color
    struct DefaultColor { const char* path; int color; };
    static const DefaultColor kDefaultColors[] = {
        { "RightMeters_mc.ActionPointMeter_mc.Optional_mc", COLOR_RED },
        { "RightMeters_mc.ActionPointMeter_mc.MeterBar_mc", COLOR_HUD },
        { "RightMeters_mc.ActionPointMeter_mc.DisplayText_tf", COLOR_HUD },
        { "RightMeters_mc.ActionPointMeter_mc.ActionPointSegments_mc", COLOR_RED },
        { "RightMeters_mc.ActionPointMeter_mc.Bracket_mc", COLOR_HUD },
        { "RightMeters_mc.FatigueWarning_mc", COLOR_RED },
        { "RightMeters_mc.AmmoCount_mc", COLOR_HUD },
        { "RightMeters_mc.FlashLightWidget_mc", COLOR_HUD },
        { "RightMeters_mc.ExplosiveAmmoCount_mc", COLOR_HUD },
        { "RightMeters_mc.PowerArmorLowBatteryWarning_mc", COLOR_HUD },
        { "BottomCenterGroup_mc.CritMeter_mc", COLOR_HUD },
        { "BottomCenterGroup_mc.PerkVaultBoy_mc", COLOR_HUD },
        { "BottomCenterGroup_mc.SubtitleText_mc.SubtitleText_tf", COLOR_SUB },
        { "BottomCenterGroup_mc.SubtitleText_mc.SpeakerName_tf", COLOR_HUD },
        { "LeftMeters_mc.HPMeter_mc.MeterBar_mc", COLOR_HUD },
        { "LeftMeters_mc.HPMeter_mc.RadsBar_mc", COLOR_RED },
        { "LeftMeters_mc.HPMeter_mc.DisplayText_tf", COLOR_HUD },
        { "LeftMeters_mc.HPMeter_mc.Bracket_mc", COLOR_HUD },
        { "LeftMeters_mc.RadsMeter_mc", COLOR_RED },
        { "LeftMeters_mc.LocationText_mc.RegionText", COLOR_HUD },
        { "CenterGroup_mc.ExplosiveIndicatorBase_mc", COLOR_RED },
        { "CenterGroup_mc.DirectionalHitIndicatorBase_mc", COLOR_RED },
        { "CenterGroup_mc.HitIndicator_mc", COLOR_HUD },
        { "TopRightGroup_mc.VaultBoyCondition_mc", COLOR_HUD },
        { "HUDNotificationsGroup_mc.XPMeter_mc", COLOR_HUD },
        { "HUDNotificationsGroup_mc.QuestUpdates_mc", COLOR_HUD },
        { "HUDNotificationsGroup_mc.ObjectiveUpdates_mc", COLOR_HUD },
        { "HUDNotificationsGroup_mc.QuestVaultBoy_mc", COLOR_HUD },
        { "HUDNotificationsGroup_mc.TutorialText_mc", COLOR_HUD },
    };

    static int LookupDefaultColor(const std::string& elementPath) {
        for (const auto& dc : kDefaultColors) {
            if (elementPath == dc.path) return dc.color;
        }
        return COLOR_NONE;
    }

    // Text-field defaults the original reads from the loaded HUDMenu.swf.
    // Extracted from FallUI - HUD's shipped HUDMenu.swf (DefineEditText
    // bounds in twips/20 + font size), see swf/test/extract_textfields.py.
    // Fractional width/height values are the REAL TextField metrics and are
    // kept fractional (see MakeIntD). autoSize follows the original's
    // special-case name list; live-autoSized dimensions can't be reproduced
    // offline and use the static bounds.
    struct TextFieldDefaults {
        double width = 100, height = 28;
        int fontSize = 22;
        int align = 0;      // 0 left / 1 center / 2 right
        int autoSize = 0;   // 0 none / 1 left / 2 center / 3 right
        bool shrink = false;
        bool visDefault = true;  // for the "_mod*" Vis-variant fields
    };

    // addTextOptions port. `useVisVariant` mirrors the special-case for the
    // FallUI-added "_mod*" fields (option id prefix+"Vis" instead of prefix+"V").
    static void AddTextOptions(OptionBuilder& b, const std::string& parent,
                               const std::string& prefix, const std::string& childPath,
                               const std::string& title, double pos,
                               const TextFieldDefaults& tf, bool isUiTitle = false,
                               int colorStd = -9999, bool noApplier = false) {
        // Detect the Vis-variant fields by name (same list as the original)
        std::string leaf = childPath;
        if (auto dot = leaf.rfind('.'); dot != std::string::npos) leaf = leaf.substr(dot + 1);
        const bool visVariant = (leaf == "_modStealthPercent" || leaf == "_modRadsText_tf" ||
                                 leaf == "_modPercentTextField");

        if (!title.empty()) {
            b.Add(parent, b.MakeTitle(prefix + "Title", pos += 0.001, title,
                                      isUiTitle ? Option::Type::UiTitle : Option::Type::GrpTitle));
        }
        std::string visId = prefix + (visVariant ? "Vis" : "V");
        {
            Option o = b.MakeBool(visId, pos += 0.001, "${Visible}", visVariant ? tf.visDefault : true);
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt(prefix + "OffX", pos += 0.001, "${Offset} ${X}", 0, -300, 300);
            o.onlyIf = visId;
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt(prefix + "OffY", pos += 0.001, "${Offset} ${Y}", 0, -300, 300);
            o.onlyIf = visId; o.easyDrag = true;
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeIntD(prefix + "Width", pos += 0.001, "${Width}", tf.width, 1, 600);
            o.onlyIf = visId;
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeIntD(prefix + "Height", pos += 0.001, "${Height}", tf.height, 1, 300);
            o.onlyIf = visId;
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt(prefix + "FontType", pos += 0.001, "${Font}", -1, -1, kFontCount - 1);
            o.onlyIf = visId; o.checkType = "font";
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt(prefix + "Size", pos += 0.001, "${Fontsize}", tf.fontSize, 1, 100);
            o.onlyIf = visId;
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt(prefix + "Align", pos += 0.001, "${Align}", tf.align, 0, 2);
            o.onlyIf = visId; o.checkType = "tfAlign";
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt(prefix + "Rot", pos += 0.001, "${Rotation}", 0, -180, 180);
            o.onlyIf = visId;
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt(prefix + "AutoSize", pos += 0.001, "${TextFieldAutoSize}",
                                 tf.autoSize, 0, 3);
            o.onlyIf = visId; o.checkType = "tfAutoSize";
            b.Add(parent, std::move(o));
        }
        pos += 0.01;
        {
            Option o = b.MakeBool(prefix + "Shrink", pos, "${TextFieldAutoShrink}", tf.shrink);
            o.onlyIf = visId; o.checkType = "tfShrink";
            b.Add(parent, std::move(o));
        }
        pos += 0.069;  // applier slot (not ported)
        (void)noApplier;
        if (colorStd != -9998) {
            int c = colorStd;
            if (c == -9999) c = LookupDefaultColor(parent + "." + childPath);
            b.AddColorOptions(parent, childPath, prefix, c, pos += 0.001, "", visId);
        }
    }

    // addPosOptions port (icon-ish sub-elements: visibility, offset, scale, rotation, color)
    static void AddPosOptions(OptionBuilder& b, const std::string& parent,
                              const std::string& prefix, const std::string& childPath,
                              const std::string& title, double pos,
                              bool noOffset = false, bool splitScale = false) {
        b.Add(parent, b.MakeTitle(prefix + "Title", pos += 0.001, title));
        b.Add(parent, b.MakeBool(prefix + "V", pos += 0.001, "${Visible}", true));
        if (!noOffset) {
            Option ox = b.MakeInt(prefix + "OX", pos += 0.001, "${Offset} ${X}", 0, -300, 300);
            ox.onlyIf = prefix + "V";
            b.Add(parent, std::move(ox));
            Option oy = b.MakeInt(prefix + "OY", pos += 0.001, "${Offset} ${Y}", 0, -300, 300);
            oy.onlyIf = prefix + "V"; oy.easyDrag = true;
            b.Add(parent, std::move(oy));
        }
        if (splitScale) {
            Option sx = b.MakeInt(prefix + "ScaleX", pos += 0.001, "${Scale} ${X}", 100, 0, 300);
            sx.onlyIf = prefix + "V";
            b.Add(parent, std::move(sx));
            Option sy = b.MakeInt(prefix + "ScaleY", pos += 0.001, "${Scale} ${Y}", 100, 0, 300);
            sy.onlyIf = prefix + "V";
            b.Add(parent, std::move(sy));
        } else {
            Option s = b.MakeInt(prefix + "Scale", pos += 0.001, "${Scale}", 100, 0, 300);
            s.onlyIf = prefix + "V";
            b.Add(parent, std::move(s));
        }
        Option r = b.MakeInt(prefix + "Rot", pos += 0.001, "${Rotation}", 0, -180, 180);
        r.onlyIf = prefix + "V";
        b.Add(parent, std::move(r));
        int c = LookupDefaultColor(parent + "." + childPath);
        b.AddColorOptions(parent, childPath, prefix, c, pos += 0.001);
    }

    // addGradientOptions port
    static void AddGradientOptions(OptionBuilder& b, const std::string& parent,
                                   const std::string& prefix, double pos,
                                   const std::string& title = "",
                                   const std::string& onlyIfTop = "") {
        {
            Option o = b.MakeTitle(prefix + "GrTitle", pos += 0.001,
                                   title.empty() ? "${Gradient}" : title, Option::Type::UiTitle);
            o.onlyIf = onlyIfTop;
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeBool(prefix + "GrV", pos += 0.001, "${Gradient}", false);
            o.onlyIf = onlyIfTop;
            b.Add(parent, std::move(o));
        }
        std::string cond = (onlyIfTop.empty() ? "" : onlyIfTop + ",") + prefix + "GrV";
        {
            Option o = b.MakeColor(prefix + "GrC1", pos += 0.001, "${Color} 1", 16777215);
            o.onlyIf = cond;
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeColor(prefix + "GrC2", pos += 0.001, "${Color} 2", 16777215);
            o.onlyIf = cond;
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt(prefix + "GrA1", pos += 0.001, "${Alpha} 1", 100, 0, 100);
            o.onlyIf = cond;
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt(prefix + "GrA2", pos += 0.001, "${Alpha} 2", 100, 0, 100);
            o.onlyIf = cond;
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt(prefix + "GrR", pos += 0.001, "${Rotation}", 0, 0, 360);
            o.onlyIf = cond;
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeBool(prefix + "GrRad", pos += 0.001, "${Radial}", false);
            o.onlyIf = cond;
            b.Add(parent, std::move(o));
        }
    }

    // addBackgroundOptions port
    struct BackgroundOpts {
        bool visible = true;
        bool isUiTitle = false;
        std::string onlyIfTop;
        std::string title;
        int padding = 0;
        int alpha = -1;
    };

    static void AddBackgroundOptions(OptionBuilder& b, const std::string& parent,
                                     const std::string& prefix, const std::string& childPath,
                                     double pos, const BackgroundOpts& opts = {}) {
        (void)childPath;
        std::string titleName = opts.title.empty() ? "${Background}/${Border}" : opts.title;
        {
            Option o = b.MakeTitle(prefix + "BgBorderTitle", pos += 0.001, titleName,
                                   opts.isUiTitle ? Option::Type::UiTitle : Option::Type::GrpTitle);
            o.onlyIf = opts.onlyIfTop;
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeBool(prefix + "SV", pos += 0.001, "${Background}/${Border}", opts.visible);
            o.onlyIf = opts.onlyIfTop;
            b.Add(parent, std::move(o));
        }
        std::string cond = prefix + "SV";
        if (!opts.onlyIfTop.empty()) cond += "," + opts.onlyIfTop;
        {
            Option o = b.MakeBool(prefix + "SPadv", pos += 0.001, "${Padding} - ${Advanced}", false);
            o.onlyIf = cond;
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt(prefix + "SP", pos += 0.001, "${Padding}", opts.padding, -100, 100);
            o.onlyIf = cond + ",!" + prefix + "SPadv";
            b.Add(parent, std::move(o));
        }
        for (const char* side : { "T", "R", "B", "L" }) {
            static const std::map<std::string, std::string> names = {
                { "T", "${Padding} - ${Top}" }, { "R", "${Padding} - ${Right}" },
                { "B", "${Padding} - ${Bottom}" }, { "L", "${Padding} - ${Left}" },
            };
            Option o = b.MakeInt(prefix + "SP" + side, pos += 0.001, names.at(side),
                                 opts.padding, -100, 100);
            o.onlyIf = cond + "," + prefix + "SPadv";
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt(prefix + "SR", pos += 0.001, "${Round_border}", 0, 0, 200);
            o.onlyIf = cond;
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeTitle(prefix + "BgTitle", pos += 0.001, "$Background", Option::Type::UiTitle);
            o.onlyIf = cond;
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeColor(prefix + "SC", pos += 0.001, "${Color}", COLOR_HUD);
            o.onlyIf = cond;
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt(prefix + "SA", pos += 0.001, "${Alpha} (\xE2\x80\xB0)", opts.alpha, -1, 100);
            o.onlyIf = cond;
            b.Add(parent, std::move(o));
        }
        AddGradientOptions(b, parent, prefix, pos += 0.001, "${Background} - ${Gradient}", cond);
        pos += 0.02;
        {
            Option o = b.MakeTitle(prefix + "BorderTitle", pos += 0.001, "$Border", Option::Type::UiTitle);
            o.onlyIf = cond;
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt(prefix + "SBW", pos += 0.001, "${Width}", 0, 0, 100);
            o.onlyIf = cond;
            b.Add(parent, std::move(o));
        }
        std::string condW = cond + "," + prefix + "SBW";
        {
            Option o = b.MakeColor(prefix + "SBC", pos += 0.001, "${Color}", COLOR_NONE);
            o.onlyIf = condW;
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt(prefix + "SBA", pos += 0.001, "${Alpha} (%)", 100, -1, 100);
            o.onlyIf = condW;
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt(prefix + "SBS", pos += 0.001, "${Style}", 1, 1, 18);
            o.onlyIf = condW; o.checkType = "borderStyles";
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt(prefix + "SBCaps", pos += 0.001, "${CapLength}", 5, 0, 100);
            o.onlyIf = condW;
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt(prefix + "SBP", pos += 0.001, "${Placement}", 0, 0, 2);
            o.onlyIf = condW; o.checkType = "borderPlacements";
            b.Add(parent, std::move(o));
        }
    }

    // addBracketOptions port. stdVisible/stdHorizontal are SWF-derived in the
    // original (live component's bShowBrackets/BracketStyle). BSUIComponent
    // defaults BracketStyle to "horizontal", and shipped presets confirm the
    // editor treated BrkS=true (horizontal) as the default for TutorialText
    // (they contain explicit BrkS=false overrides) — so default to true here;
    // Messages_mc is forced to false by the original (addBracketOptions).
    static void AddBracketOptions(OptionBuilder& b, const std::string& parent,
                                  const std::string& prefix, double pos, int stdColor,
                                  bool stdVisible = true, bool stdHorizontal = true) {
        b.Add(parent, b.MakeTitle(prefix + "BracketsTitle", pos += 0.0001, "${Brackets}"));
        b.Add(parent, b.MakeBool(prefix + "BrkV", pos += 0.0001, "${Brackets}", stdVisible));
        {
            Option o = b.MakeBool(prefix + "BrkS", pos += 0.0001, "${Side}", stdHorizontal);
            o.onlyIf = prefix + "BrkV";
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeColor(prefix + "BrkCl", pos += 0.0001, "${Color}", stdColor);
            o.onlyIf = prefix + "BrkV";
            b.Add(parent, std::move(o));
        }
    }

    // addBarOptions port. Registers the bar prefix for the <1.4.0 migration.
    struct BarGroupRef { std::string parent; std::string prefix; };
    static std::vector<BarGroupRef> s_barGroups;

    static void AddBarOptions(OptionBuilder& b, const std::string& parent,
                              const std::string& childPath, const std::string& prefix,
                              const std::string& title, double pos,
                              bool stdFillRight = false, int barRotStd = 0) {
        s_barGroups.push_back({ parent, prefix });
        b.Add(parent, b.MakeTitle(prefix + "Title", pos += 0.001, title));
        b.Add(parent, b.MakeBool(prefix + "V", pos += 0.001, "${Visible}", true));
        std::string cond = prefix + "V";
        auto addInt = [&](const char* suffix, const char* name, int std_, int minV, int maxV,
                          bool deprecated = false, const char* checkType = "") {
            Option o = b.MakeInt(prefix + suffix, pos += 0.001, name, std_, minV, maxV);
            o.onlyIf = cond; o.deprecated = deprecated; o.checkType = checkType;
            b.Add(parent, std::move(o));
        };
        addInt("OX", "${Offset} ${X}", 0, -300, 300);
        addInt("OY", "${Offset} ${Y}", 0, -300, 300);
        addInt("Width", "${Width}", 200, 1, 500);
        addInt("Height", "${Height}", 6, 1, 100);
        addInt("Rot", "${Rotation}", barRotStd, -180, 180);
        addInt("Slices", "${Slices}", 1, 1, 50);
        addInt("SlicesWidth", "${Slices} - ${Width} (%)", 75, 1, 100);
        addInt("SlicesRound", "${Slices} - ${Round_border}", 0, 0, 100);
        {
            Option o = b.MakeBool(prefix + "Dir", pos += 0.001, "${Fill_bar_to_left}", stdFillRight);
            o.onlyIf = cond; o.deprecated = true;
            b.Add(parent, std::move(o));
        }
        addInt("Dir2", "${Align}", stdFillRight ? 2 : 0, 0, 2, false, "tfAlign");
        addInt("BrdS", "${Border} - ${Style}", 0, 0, 8, true);
        addInt("BrdP", "${Border} - ${Padding}", 3, 0, 50, true);
        addInt("BrdW", "${Border} - ${Width}", 3, 0, 50, true);
        addInt("BrdCL", "${Border} - ${CapLength}", 5, 0, 100, true);

        // Bar color (RadsBar red; HP/AP meter bars HUD; enemy/crit meter none;
        // xpbar / LevelUPBar gets no bar color option in the original)
        if (childPath == "RadsBar_mc") {
            b.AddColorOptions(parent, childPath, prefix, COLOR_RED, pos += 0.001, "", cond);
        } else if (childPath == "MeterBar_mc") {
            int c = (parent == "LeftMeters_mc.HPMeter_mc" ||
                     parent == "RightMeters_mc.ActionPointMeter_mc") ? COLOR_HUD : COLOR_NONE;
            b.AddColorOptions(parent, childPath, prefix, c, pos += 0.001, "", cond);
            if (parent == "RightMeters_mc.ActionPointMeter_mc") {
                b.AddColorOptions(parent, "Optional_mc", prefix + "O", COLOR_RED,
                                  pos += 0.001, "$Fatigue", cond);
            }
        }

        // Gradient + background/border blocks apply to every bar
        AddGradientOptions(b, parent, prefix + "bar", pos += 0.001, "${Bar} - ${Gradient}");
        pos += 0.02;
        BackgroundOpts bg;
        bg.visible = false;
        bg.isUiTitle = true;
        bg.onlyIfTop = cond;
        bg.title = "${Bar} - ${Background}/${Border}";
        AddBackgroundOptions(b, parent, prefix, childPath, pos, bg);
        pos += 0.111;  // BarApply applier slot

        // Bar percent text overlay (FallUI-added "_modPercentTextField")
        // MeterBarWidget creates this at runtime as a 0x0 TextField with
        // font size 26 and default (left) alignment — see MeterBarWidget.as.
        TextFieldDefaults tfp;
        tfp.width = 0; tfp.height = 0; tfp.fontSize = 26; tfp.align = 0;
        tfp.visDefault = false;
        AddTextOptions(b, parent, prefix + "Txt", childPath + "._modPercentTextField",
                       "${Bar}: ${Text} (%)", pos += 0.001, tfp, true);
    }

    // addAnchorOptions port
    static void AddAnchorOptions(OptionBuilder& b, const std::string& parent) {
        double pos = 19.0;
        b.Add(parent, b.MakeTitle("AnchorTitle", pos += 0.001, "${Anchor}"));
        {
            Option o = b.MakeBool("previewShowAnchor", pos += 0.001,
                                  "(${Preview}) ${Show_anchor_position}", false);
            o.isGlobalSetting = true;
            b.Add(parent, std::move(o));
        }
        b.Add(parent, b.MakeTitle("AnchorLocaleTitle", pos += 0.001, "${Anchor} (${Locale})",
                                  Option::Type::UiTitle));
        b.Add(parent, b.MakeInt("AsOX", pos += 0.001, "${Offset} X", 0, -1280, 1280));
        {
            Option o = b.MakeInt("AsOY", pos += 0.001, "${Offset} Y", 0, -720, 720);
            o.easyDrag = true;
            b.Add(parent, std::move(o));
        }
        b.Add(parent, b.MakeTitle("AnchorGlobalTitle", pos += 0.001, "${Anchor} (${Global})",
                                  Option::Type::UiTitle));
        const int stdPos = (parent == "CenterGroup_mc.HUDCrosshair_mc") ? 0 : 1;
        {
            Option o = b.MakeInt("AgPx", pos += 0.001, "${Position} X", stdPos, 0, 6);
            o.checkType = "globalPositionsX";
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt("AgPy", pos += 0.001, "${Position} Y", stdPos, 0, 6);
            o.checkType = "globalPositionsY";
            b.Add(parent, std::move(o));
        }
    }

    // add3DOptions port
    static void Add3DOptions(OptionBuilder& b, const std::string& parent, double pos) {
        b.Add(parent, b.MakeTitle("D3DTitle", pos += 0.001, "3D"));
        b.Add(parent, b.MakeBool("act3D", pos += 0.001, "${3D_Effects}", false));
        {
            Option o = b.MakeTitle("D3DNoteText", pos += 0.001, "${3D_Note}", Option::Type::UiText);
            o.onlyIf = "act3D";
            b.Add(parent, std::move(o));
        }
        if (parent == "BottomCenterGroup_mc.CompassWidget_mc") {
            Option o = b.MakeTitle("D3DNoteCompassText", pos += 0.001, "${3D_Compass_Note}",
                                   Option::Type::UiText);
            o.onlyIf = "act3D";
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeTitle("Rtitle", pos += 0.001, "${Rotation}", Option::Type::UiTitle);
            o.onlyIf = "act3D";
            b.Add(parent, std::move(o));
        }
        for (const char* axis : { "RX", "RY", "RZ" }) {
            std::string name = std::string("${Rotation} ") + axis[1];
            Option o = b.MakeInt(axis, pos += 0.001, name, 0, -180, 180);
            o.onlyIf = "act3D";
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeTitle("LocPerspTitle", pos += 0.001, "Perspective projection",
                                   Option::Type::UiTitle);
            o.onlyIf = "act3D";
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeBool("LP", pos += 0.001, "Local perspective", false);
            o.onlyIf = "act3D"; o.checkType = "localPerspectiveOn";
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt("LPX", pos += 0.001, "X", 0, -640, 640);
            o.onlyIf = "act3D";
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt("LPY", pos += 0.001, "Y", 0, -360, 360);
            o.onlyIf = "act3D"; o.easyDrag = true;
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt("LPFOV", pos += 0.001, "Field of view", 55, 0, 180);
            o.onlyIf = "act3D";
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeTitle("PivotTitle", pos += 0.001, "Pivot point", Option::Type::UiTitle);
            o.onlyIf = "act3D";
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt("PX", pos += 0.001, "X", 0, -1280, 1280);
            o.onlyIf = "act3D";
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt("PY", pos += 0.001, "Y", 0, -720, 720);
            o.onlyIf = "act3D";
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt("PZ", pos += 0.001, "Z", 0, -720, 720);
            o.onlyIf = "act3D";
            b.Add(parent, std::move(o));
        }
        b.Add(parent, b.MakeTitle("D3DSpacer", pos += 0.001, "", Option::Type::UiTitle));
    }

    // addButtonHintsOptions port
    static void AddButtonHintsOptions(OptionBuilder& b, const std::string& parent, double pos) {
        b.Add(parent, b.MakeTitle("btnHntsTitle", pos += 0.001, "$Button_hints"));
        b.Add(parent, b.MakeBool("hideButtons", pos += 0.001, "${Hide_buttons}", false));
        auto addInt = [&](const char* id, const char* name, int std_, int minV, int maxV,
                          int step = 1, const char* checkType = "") {
            Option o = b.MakeInt(id, pos += 0.001, name, std_, minV, maxV, step);
            o.onlyIf = "!hideButtons"; o.checkType = checkType;
            b.Add(parent, std::move(o));
        };
        addInt("butHntsScale", "$Scale", 65, 0, 200, 5);
        addInt("butHntsOffX", "${Offset} ${Y}", 0, -200, 200);   // (Y — original's label/id quirk kept)
        addInt("butHntsOffX2", "${Offset} ${X}", 0, -200, 200);
        addInt("butHntsAlign", "${Align}", 1, 0, 2, 1, "tfAlign");
        addInt("butHntsAlpha", "$Alpha", 65, 0, 100, 5);
        pos += 0.001;  // applier slot
        b.AddColorOptions(parent, "ButtonHintBar_mc", "btnHnts", -1, pos += 0.001,
                          "$Color", "!hideButtons");
    }

    // addTextFieldEnhancedOptions port (RolloverWidget)
    static void AddTextFieldEnhancedOptions(OptionBuilder& b, const std::string& parent,
                                            const std::string& title, double pos) {
        b.Add(parent, b.MakeTitle("rolloverTitle", pos += 0.001, title));
        {
            Option o = b.MakeInt("blockAlign", pos += 0.001, "${Block} - ${Align}", 1, 0, 2);
            o.checkType = "tfAlign";
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt("textAlign", pos += 0.001, "${Text} - ${Align}", 0, 0, 2);
            o.checkType = "tfAlign";
            b.Add(parent, std::move(o));
        }
        {
            Option o = b.MakeInt("fontType", pos += 0.001, "${Font}", -1, -1, kFontCount - 1);
            o.checkType = "font";
            b.Add(parent, std::move(o));
        }
        b.Add(parent, b.MakeInt("textSize", pos += 0.001, "${Text} - ${Size}", 20, 1, 50));
        b.Add(parent, b.MakeInt("subScale", pos += 0.001, "${Item_subtitle} - ${Scale}", 65, 0, 100, 5));
        b.Add(parent, b.MakeInt("subAlpha", pos += 0.001, "${Item_subtitle} - ${Alpha}", 65, 0, 100, 5));
        {
            Option o = b.MakeInt("iconAlign", pos += 0.001, "${Icon} - ${Align}", 0, 0, 3);
            o.checkType = "iconAlign";
            b.Add(parent, std::move(o));
        }
        b.Add(parent, b.MakeInt("iconSize", pos += 0.001, "${Icon} - ${Size}", 26, 0, 100));
    }

    // --- Full option catalog assembly (mirrors _layoutOptions + the
    //     layoutOptions getter body, in the same order) ---
    static std::map<std::string, OptionSet> s_options;
    static bool s_optionsBuilt = false;

    static void SimpleHideBool(OptionBuilder& b, const std::string& parent, const std::string& id,
                               double pos, const std::string& name, bool deprecated = false) {
        Option o = b.MakeBool(id, pos, name, false);
        o.deprecated = deprecated;
        b.Add(parent, std::move(o));
    }

    static void BuildOptionCatalog() {
        if (s_optionsBuilt) return;
        s_optionsBuilt = true;
        s_barGroups.clear();

        OptionBuilder b;
        std::string p;

        // ---- static _layoutOptions table ----
        p = "LeftMeters_mc.HPMeter_mc";
        SimpleHideBool(b, p, "alignRight", 3.0025, "${Move_text_to_right}", true);

        p = "RightMeters_mc.ActionPointMeter_mc";
        {
            Option o = b.MakeBool("alignLeft", 3.0025, "${Move_text_to_left}", false);
            o.deprecated = true; o.onlyIf = "textV";
            b.Add(p, std::move(o));
        }

        p = "RightMeters_mc.HUDActiveEffectsWidget_mc";
        b.Add(p, b.MakeTitle("iconTitle", 1.0, "${Icons}"));
        {
            Option o = b.MakeInt("dir", 1.1, "${Direction}", 0, 0, 3);
            o.checkType = "direction";
            b.Add(p, std::move(o));
        }
        b.Add(p, b.MakeInt("spc", 1.2, "${Spacing}", 3, -10, 50));
        b.Add(p, b.MakeColor("iconColor", 1.3, "${Color}", COLOR_HUD));

        p = "BottomCenterGroup_mc.CritMeter_mc";
        SimpleHideBool(b, p, "hideBar", 0.2, "${Hide_bar}", true);
        SimpleHideBool(b, p, "hideBracket", 0.3, "${Hide_bracket}");
        SimpleHideBool(b, p, "hideStars", 0.4, "Hide stars");

        p = "HUDNotificationsGroup_mc.XPMeter_mc";
        SimpleHideBool(b, p, "hideLvlUp", 0.4, "${Hide_text}: ${Level_up}");
        SimpleHideBool(b, p, "hideBar", 0.5, "${Hide_bar}", true);
        SimpleHideBool(b, p, "hideBracket", 0.7, "${Hide_bracket}");
        SimpleHideBool(b, p, "alignRight", 1.0025, "${Move_text_to_right}", true);

        p = "CenterGroup_mc.QuickContainerWidget_mc";
        b.Add(p, b.MakeInt("prevItems", -0.5, "(${Preview_item_count})", 5, 0, 5));
        b.Add(p, b.MakeBool("prevMode", -0.6, "(${Preview_mode_workbench})", false));
        SimpleHideBool(b, p, "hideBackground", 0.4, "${Hide_background}", true);
        SimpleHideBool(b, p, "hideTitle", 0.5, "${Hide_name}", true);
        {
            Option o = b.MakeInt("lstDir", 0.7, "${Direction}", 0, 0, 2);
            o.checkType = "upcenterdown";
            b.Add(p, std::move(o));
        }
        b.Add(p, b.MakeInt("onEmptyAlpha", 0.8, "${On_empty_quick_loot} - ${Alpha}", 6, 0, 100));

        p = "BottomCenterGroup_mc.CompassWidget_mc";
        SimpleHideBool(b, p, "noBackground", 1.0, "${Hide_background}");
        b.Add(p, b.MakeTitle("BracketsTitle", 2.0, "${Brackets}"));
        SimpleHideBool(b, p, "hideBrackets", 2.1, "${Hide_brackets}/HUDFramework", true);
        SimpleHideBool(b, p, "hideBrackets2", 2.11, "${Hide_brackets}");
        b.Add(p, b.MakeColor("color", 2.2, "${Brackets} - ${Color}(+HUDFramework)", COLOR_HUD));
        b.Add(p, b.MakeColor("color2", 2.3, "${Brackets} - ${Color}", COLOR_NONE));

        SimpleHideBool(b, "RightMeters_mc.FlashLightWidget_mc", "noBackground", 0.5,
                       "${Hide_background}", true);
        SimpleHideBool(b, "RightMeters_mc.ExplosiveAmmoCount_mc", "noBackground", 0.5,
                       "${Hide_background}", true);
        SimpleHideBool(b, "TopCenterGroup_mc.StealthMeter_mc", "hideBrackets", 1.0,
                       "${Hide_brackets}");

        p = "TopCenterGroup_mc.EnemyHealthMeter_mc";
        SimpleHideBool(b, p, "hideHP", 0.1, "${Hide_bar}", true);
        SimpleHideBool(b, p, "hideBracket", 0.2, "${Hide_bracket}");

        SimpleHideBool(b, "HUDNotificationsGroup_mc.QuestVaultBoy_mc", "noBackground", 0.1,
                       "${Hide_background}", true);

        p = "_ext_extra._ext_promptMessageSwf";
        {
            Option o = b.MakeInt("PMTFAlign", 1.0, "${Align}", 1, 0, 3);
            o.checkType = "tfAutoSize";
            b.Add(p, std::move(o));
        }

        b.Add("HUDNotificationsGroup_mc.Messages_mc",
              b.MakeInt("prevItems", -0.5, "(${Preview_item_count})", 1, 1, 7));

        // ---- generated options (layoutOptions getter body) ----
        // Text-field defaults are approximations of the SWF-derived values.
        TextFieldDefaults tf;

        // HP meter
        p = "LeftMeters_mc.HPMeter_mc";
        tf = {}; tf.width = 34; tf.height = 36.95; tf.fontSize = 25; tf.align = 0; tf.autoSize = 1;
        AddTextOptions(b, p, "text", "DisplayText_tf", "${Text}: ${HP}", 3, tf);
        AddBarOptions(b, p, "MeterBar_mc", "hpbar", "${Bar}: ${HP}", 5);
        // _modRadsText_tf is Tools.copyTextField(DisplayText_tf) — same metrics
        tf = {}; tf.width = 34; tf.height = 36.95; tf.fontSize = 25; tf.visDefault = false;
        AddTextOptions(b, p, "textRads", "_modRadsText_tf", "${Text}: ${RADS}", 6, tf);
        // RadsBar_mc.Justification = "right" in HUDMenu.swf (HPMeter_41 timeline
        // setProp), so its Dir std is true / Dir2 std is 2.
        AddBarOptions(b, p, "RadsBar_mc", "radbar", "${Bar}: ${RADS}", 7, /*stdFillRight=*/true);
        {
            double lp = 15.0;
            b.Add(p, b.MakeTitle("itemsTitle", lp += 0.00001, "$Brackets"));
            SimpleHideBool(b, p, "hideBracket", lp += 0.00001, "${Hide_bracket}");
            b.AddColorOptions(p, "Bracket_mc", "br", COLOR_HUD, lp += 0.00001, "$Color");
        }

        // Rads meter
        AddPosOptions(b, "LeftMeters_mc.RadsMeter_mc", "icon", "RadsIcon_mc", "${Icon}", 1);
        tf = {}; tf.width = 155.2; tf.height = 46.2; tf.fontSize = 32; tf.align = 2;
        AddTextOptions(b, "LeftMeters_mc.RadsMeter_mc", "textNr", "RadsNumber_tf",
                       "${Text}: ${Number}", 2, tf);
        tf = {}; tf.width = 74; tf.height = 93.4; tf.fontSize = 32;
        AddTextOptions(b, "LeftMeters_mc.RadsMeter_mc", "textRADS", "RADS_tf",
                       "${Text}: ${RADS}", 3, tf);
        tf = {}; tf.width = 386; tf.height = 30.35; tf.fontSize = 20; tf.autoSize = 1;
        AddTextOptions(b, "LeftMeters_mc.LocationText_mc", "text", "RegionText", "${Text}", 1, tf);

        // AP meter
        p = "RightMeters_mc.ActionPointMeter_mc";
        tf = {}; tf.width = 32; tf.height = 36.95; tf.fontSize = 25; tf.align = 0;
        AddTextOptions(b, p, "text", "DisplayText_tf", "${Text}: ${AP}", 3, tf);
        AddBarOptions(b, p, "MeterBar_mc", "apbar", "${Bar}: ${AP}", 4, /*stdFillRight=*/true);
        AddPosOptions(b, p, "apseqs", "ActionPointSegments_mc", "${AP} - ${Segments}", 6, false, true);
        {
            double lp = 15.0;
            b.Add(p, b.MakeTitle("itemsTitle", lp += 0.00001, "$Brackets"));
            SimpleHideBool(b, p, "hideBracket", lp += 0.00001, "${Hide_bracket}");
            b.AddColorOptions(p, "Bracket_mc", "br", COLOR_HUD, lp += 0.00001, "$Color");
        }

        // Right meters misc
        AddPosOptions(b, "RightMeters_mc.FatigueWarning_mc", "icon", "FatigueHead_mc", "${Icon}", 1);
        tf = {}; tf.width = 201.25; tf.height = 46.2; tf.fontSize = 32;
        AddTextOptions(b, "RightMeters_mc.FatigueWarning_mc", "text", "FatigueWarning_tf",
                       "${Text}", 2, tf);
        AddPosOptions(b, "RightMeters_mc.ExplosiveAmmoCount_mc", "icon", "TypeIcon_mc", "${Icon}", 2);
        tf = {}; tf.width = 52; tf.height = 46.2; tf.fontSize = 32; tf.align = 2;
        AddTextOptions(b, "RightMeters_mc.ExplosiveAmmoCount_mc", "text", "AvailableCount_tf",
                       "${Text}", 3, tf);
        AddBackgroundOptions(b, "RightMeters_mc.ExplosiveAmmoCount_mc", "", "", 4);
        tf = {}; tf.width = 52; tf.height = 46.2; tf.fontSize = 32; tf.align = 2;
        AddTextOptions(b, "RightMeters_mc.AmmoCount_mc", "textClip", "ClipCount_tf",
                       "${Text}: ${Clip_ammo}", 2, tf);
        AddPosOptions(b, "RightMeters_mc.AmmoCount_mc", "icon", "AmmoLineInstance", "${Icon}: ---", 3);
        tf = {}; tf.width = 52; tf.height = 46.2; tf.fontSize = 32; tf.align = 0;
        AddTextOptions(b, "RightMeters_mc.AmmoCount_mc", "textTotal", "ReserveCount_tf",
                       "${Text}: ${Total_ammo} 2", 4, tf);
        {
            BackgroundOpts bg; bg.visible = false;
            AddBackgroundOptions(b, "RightMeters_mc.AmmoCount_mc", "", "", 5, bg);
        }
        tf = {}; tf.width = 400; tf.height = 25.1; tf.fontSize = 16; tf.align = 2;
        AddTextOptions(b, "RightMeters_mc.PowerArmorLowBatteryWarning_mc", "text",
                       "WarningTextHolder_mc.PowerArmorLowBatteryWarning_tf", "${Text}", 1, tf);

        // Enemy health
        p = "TopCenterGroup_mc.EnemyHealthMeter_mc";
        AddBarOptions(b, p, "MeterBar_mc", "emybar", "${Bar}: ${Enemy}", 2.5);
        tf = {}; tf.width = 79.75; tf.height = 36.95; tf.fontSize = 25; tf.align = 1; tf.autoSize = 2;
        AddTextOptions(b, p, "text", "DisplayText_tf", "${Text}: ${Enemy}", 3, tf);
        AddPosOptions(b, p, "iconSkull", "SkullIcon_mc", "${Icon}: ${Skull}", 4, true);
        AddPosOptions(b, p, "iconLeg", "LegendaryIcon_mc", "${Icon}: ${Legendary}", 5, true);

        // Stealth
        tf = {}; tf.width = 149.75; tf.height = 30.35; tf.fontSize = 20; tf.align = 1; tf.autoSize = 2;
        AddTextOptions(b, "TopCenterGroup_mc.StealthMeter_mc", "text", "StealthTextInstance",
                       "${Text}: ${Hidden}", 2, tf);
        // _modStealthPercent is Tools.copyTextField(StealthTextInstance) — same metrics
        tf = {}; tf.width = 149.75; tf.height = 30.35; tf.fontSize = 20; tf.align = 1; tf.visDefault = false;
        AddTextOptions(b, "TopCenterGroup_mc.StealthMeter_mc", "textPerc", "_modStealthPercent",
                       "${Text}: ${Percent}", 10, tf);

        // Tutorial
        p = "HUDNotificationsGroup_mc.TutorialText_mc";
        AddPosOptions(b, p, "icon", "TutorialHeads_mc", "${Icon}", 1);
        tf = {}; tf.width = 250; tf.height = 161.8; tf.fontSize = 19;
        AddTextOptions(b, p, "text", "TutorialText_tf", "${Text}", 2, tf);
        AddBracketOptions(b, p, "", 3, COLOR_NONE);
        AddBackgroundOptions(b, p, "", "", 4);

        // XP meter
        p = "HUDNotificationsGroup_mc.XPMeter_mc";
        tf = {}; tf.width = 34; tf.height = 36.95; tf.fontSize = 25;
        AddTextOptions(b, p, "textXP", "xptext", "${Text}: ${XP}", 1, tf);
        tf = {}; tf.width = 112; tf.height = 43.6; tf.fontSize = 30;
        AddTextOptions(b, p, "textNr", "NumberText", "${Text}: ${Number}", 2, tf);
        tf = {}; tf.width = 19; tf.height = 43.6; tf.fontSize = 30;
        AddTextOptions(b, p, "textPlus", "PlusSign", "${Text}: +", 3, tf);
        AddBarOptions(b, p, "LevelUPBar", "xpbar", "${Bar}: ${XP}", 10);

        // Quest updates
        p = "HUDNotificationsGroup_mc.QuestUpdates_mc";
        tf = {}; tf.width = 188; tf.height = 30.35; tf.fontSize = 20; tf.autoSize = 1;
        AddTextOptions(b, p, "text1", "UpdateType_tf", "${Text}: ${Type}", 2, tf);
        tf = {}; tf.width = 500; tf.height = 96.3; tf.fontSize = 35; tf.autoSize = 1;
        AddTextOptions(b, p, "text2", "QuestName_tf", "${Text}: ${Quest}", 3, tf);

        // Crit meter
        p = "BottomCenterGroup_mc.CritMeter_mc";
        AddBarOptions(b, p, "MeterBar_mc", "crtbar", "${Bar}: ${Critical}", 2.5);
        tf = {}; tf.width = 48; tf.height = 33; tf.fontSize = 22; tf.align = 2;
        AddTextOptions(b, p, "text", "DisplayText_tf", "${Text}", 5, tf);

        // Subtitles
        p = "BottomCenterGroup_mc.SubtitleText_mc";
        {
            BackgroundOpts bg; bg.visible = false;
            AddBackgroundOptions(b, p, "stbg", "", 0.5, bg);
        }
        tf = {}; tf.width = 550; tf.height = 31.7; tf.fontSize = 21; tf.align = 1; tf.autoSize = 2;
        AddTextOptions(b, p, "textSpeaker", "SpeakerName_tf", "${Text}: ${Speaker}", 1, tf);
        tf = {}; tf.width = 550; tf.height = 138.5; tf.fontSize = 21; tf.align = 1;
        AddTextOptions(b, p, "text", "SubtitleText_tf", "${Text}", 2, tf);

        // Active effects background
        {
            BackgroundOpts bg;
            bg.title = "${Icons} - ${Background}/${Border}";
            AddBackgroundOptions(b, "RightMeters_mc.HUDActiveEffectsWidget_mc", "ec",
                                 "ClipHolderInternal", 2, bg);
        }

        // Quick-loot
        p = "CenterGroup_mc.QuickContainerWidget_mc";
        AddBackgroundOptions(b, p, "bphbg", "ListHeaderAndBracket_mc.BracketPairHolder_mc", 3);
        tf = {}; tf.width = 270; tf.height = 30.35; tf.fontSize = 20; tf.align = 1; tf.autoSize = 2;
        AddTextOptions(b, p, "text", "ListHeaderAndBracket_mc.ContainerName_mc.textField_tf",
                       "${Text}: ${Name}", 4, tf, false, COLOR_HUD);
        AddButtonHintsOptions(b, p, 18);
        {
            double lp = 15.0;
            b.Add(p, b.MakeTitle("itemsTitle", lp += 0.00001, "$Items"));
            {
                Option o = b.MakeInt("fontType", lp += 0.00001, "${Font}", -1, -1, kFontCount - 1);
                o.checkType = "font";
                b.Add(p, std::move(o));
            }
            b.Add(p, b.MakeColor("_fuiIHCl", lp += 0.00001, "${Items} - ${Selection}", COLOR_HUD));
            b.Add(p, b.MakeColor("_fuiIiiCl", lp += 0.00001, "${Items} - ${Icon}", COLOR_HUD));
            b.Add(p, b.MakeColor("_fuiITCl", lp += 0.00001, "${Items} - ${Title}", COLOR_HUD));
            b.Add(p, b.MakeColor("_fuiISCl", lp += 0.00001, "${Items} - ${Item_subtitle}", COLOR_HUD));
            b.Add(p, b.MakeColor("_fuiIilCl", lp += 0.00001, "${Items} - ${Icon} - ${Legendary}", COLOR_HUD));
            b.Add(p, b.MakeColor("_fuiIibCl", lp += 0.00001, "${Items} - ${Icon} - ${Better_than_equipped}", COLOR_HUD));
            b.Add(p, b.MakeColor("_fuiIisCl", lp += 0.00001, "${Items} - ${Icon} - ${Tagged_for_search}", COLOR_HUD));
            b.Add(p, b.MakeTitle("bracketsTitle", lp += 0.00001, "$Brackets"));
            SimpleHideBool(b, p, "hideBracket", lp += 0.00001, "${Hide_bracket}");
            lp = 17.0;
            b.Add(p, b.MakeTitle("ColorsTabTitle", lp += 0.00001, "${Colors}"));
            b.Add(p, b.MakeColor("WCl", lp += 0.00001, "$WARNING_color", COLOR_RED));
            b.AddColorOptions(p, "ListHeaderAndBracket_mc.BracketPairHolder_mc", "bph", COLOR_HUD,
                              lp += 0.00001, "$Color", "!hideBracket");
        }

        // Quest Vault Boy + flashlight
        AddBracketOptions(b, "HUDNotificationsGroup_mc.QuestVaultBoy_mc", "", 1, COLOR_NONE);
        {
            BackgroundOpts bg; bg.padding = -1;
            AddBackgroundOptions(b, "HUDNotificationsGroup_mc.QuestVaultBoy_mc", "", "", 2, bg);
        }
        AddBackgroundOptions(b, "RightMeters_mc.FlashLightWidget_mc", "", "", 1);

        // Messages
        p = "HUDNotificationsGroup_mc.Messages_mc";
        {
            double lp = 18.0;
            AddBracketOptions(b, p, "", 0.8, COLOR_HUD, true, false);
            AddBackgroundOptions(b, p, "", "", 1);
            b.Add(p, b.MakeTitle("_ItemsTitle", lp += 0.00001, "${Colors}"));
            b.Add(p, b.MakeColor("_fuiIiiCl", lp += 0.00001, "${Icon}", COLOR_HUD));
            b.Add(p, b.MakeColor("_fuiITCl", lp += 0.00001, "${Title}", COLOR_HUD));
            b.Add(p, b.MakeColor("_fuiISCl", lp += 0.00001, "${Item_subtitle}", COLOR_HUD));
            b.Add(p, b.MakeColor("_IcP", lp += 0.00001, "${Added}", 4521796));
            b.Add(p, b.MakeColor("_IcN", lp += 0.00001, "${Removed}", 16729156));
            b.Add(p, b.MakeColor("_WifiCl", lp += 0.00001, "${Radio_icon}", COLOR_HUD));
            b.Add(p, b.MakeTitle("msgTxtTitle", lp += 0.001, "$Text"));
            {
                Option o = b.MakeInt("msgTxtFontType", lp += 0.001, "${Font}", -1, -1, kFontCount - 1);
                o.checkType = "font";
                b.Add(p, std::move(o));
            }
            b.Add(p, b.MakeInt("msgTxtSize", lp += 0.001, "${Fontsize}", 19, 1, 30));
            {
                Option o = b.MakeInt("msgTxtAlign", lp += 0.001, "${Align}", 0, 0, 2);
                o.checkType = "tfAlign";
                b.Add(p, std::move(o));
            }
        }

        // Objective updates
        p = "HUDNotificationsGroup_mc.ObjectiveUpdates_mc";
        {
            double lp = 18.0;
            {
                BackgroundOpts bg;
                AddBackgroundOptions(b, p, "item", "", 1, bg);
            }
            b.Add(p, b.MakeTitle("msgTxtTitle", lp += 0.001, "$Text"));
            {
                Option o = b.MakeInt("msgTxtFontType", lp += 0.001, "${Font}", -1, -1, kFontCount - 1);
                o.checkType = "font";
                b.Add(p, std::move(o));
            }
            b.Add(p, b.MakeInt("msgTxtSize", lp += 0.001, "${Fontsize}", 18, 1, 30));
            {
                Option o = b.MakeInt("msgTxtAlign", lp += 0.001, "${Align}", 0, 0, 2);
                o.checkType = "tfAlign";
                b.Add(p, std::move(o));
            }
        }

        // Crosshair
        p = "CenterGroup_mc.HUDCrosshair_mc";
        {
            double lp = 3.0;
            b.Add(p, b.MakeTitle("aTitle", lp += 0.001, "$Style"));
            {
                Option o = b.MakeInt("prevStyle", lp += 0.001, "(${Preview}) - ${Style}", 2, 0, 4);
                o.checkType = "crosshairTypes";
                b.Add(p, std::move(o));
            }
            b.Add(p, b.MakeInt("prevSize", lp += 0.001, "(${Preview}) - ${Aiming} - ${Size}", 19, 19, 100));
            static const char* kStates[] = { "None", "Dot", "Standard", "Command", "Activate" };
            for (int i = 0; i < 5; ++i) {
                Option o = b.MakeInt(std::string("a") + kStates[i], lp += 0.001,
                                     kCrosshairStateNames[i], i, 0, 4);
                o.checkType = "crosshairTypes";
                b.Add(p, std::move(o));
            }
            b.Add(p, b.MakeInt("stdLineLen", lp += 0.001, "${CrosshairLineLength}", 20, 1, 100));
            b.Add(p, b.MakeInt("stdSizFac", lp += 0.001, "${Aiming_spread_factor}", 100, 1, 300));
            lp = 17.0;
            b.Add(p, b.MakeTitle("ColorsTabTitle", lp += 0.00001, "${Colors}"));
            b.Add(p, b.MakeColor("Cl", lp += 0.00001, "$Color", COLOR_HUD));
            b.Add(p, b.MakeColor("WCl", lp += 0.00001, "$WARNING_color", COLOR_RED));
            b.colorAdded.insert(p + ".");  // widget-level color now defined
        }

        // Compass
        p = "BottomCenterGroup_mc.CompassWidget_mc";
        {
            double lp = 3.0;
            b.Add(p, b.MakeTitle("markerTitle", lp += 0.001, "$Marker"));
            for (const char* m : { "Direction", "Location", "Quest", "QuestDoor",
                                   "PlayerSet", "Enemy", "EnemyTargeted", "Recon" }) {
                std::string ms = m;
                b.Add(p, b.MakeTitle("M" + ms + "Title", lp += 0.00001, "${" + ms + "}",
                                     Option::Type::UiTitle));
                b.Add(p, b.MakeBool("M" + ms + "V", lp += 0.00001, "${Visible}", true));
                {
                    Option o = b.MakeColor("M" + ms + "Cl", lp += 0.00001, "${Color}",
                                           ms.find("Enemy") != std::string::npos ? COLOR_RED : COLOR_HUD);
                    o.onlyIf = "M" + ms + "V";
                    b.Add(p, std::move(o));
                }
            }
            // Marker distance text (SWF-derived defaults approximated)
            b.Add(p, b.MakeTitle("MDistTitle", lp += 0.00001, "${Text}: ${Distance}",
                                 Option::Type::UiTitle));
            TextFieldDefaults dtf;
            dtf.width = 31.85; dtf.height = 25.1; dtf.fontSize = 16; dtf.align = 1;
            AddTextOptions(b, p, "MDistTf", "MarkerIcon_mc.Distance_tf", "", lp + 0.00002, dtf,
                           true, -9998, true);
            lp += 0.02;
            {
                BackgroundOpts bg; bg.visible = false;
                AddBackgroundOptions(b, p, "", "_modCompassBgAndBorder", 5, bg);
            }
        }

        // Rollover widget
        p = "CenterGroup_mc.RolloverWidget_mc";
        AddTextFieldEnhancedOptions(b, p, "${RolloverWidget_mc}", 1);
        {
            double lp = 2.0;
            {
                Option o = b.MakeInt("_fuiIVA", lp += 0.00001, "${Vertical_align}", 2, 0, 2);
                o.checkType = "upcenterdown";
                b.Add(p, std::move(o));
            }
            b.Add(p, b.MakeInt("_fuiIVAH", lp += 0.00001, "${Vertical_align} - ${Height}", 90, 1, 200));
            AddButtonHintsOptions(b, p, 6);
            lp = 5.0;
            b.Add(p, b.MakeColor("_fuiIiiCl", lp += 0.00001, "${Items} - ${Icon}", COLOR_HUD));
            b.Add(p, b.MakeColor("_fuiITCl", lp += 0.00001, "${Items} - ${Title}", COLOR_HUD));
            b.Add(p, b.MakeColor("_fuiISCl", lp += 0.00001, "${Items} - ${Item_subtitle}", COLOR_HUD));
            b.Add(p, b.MakeColor("_fuiIilCl", lp += 0.00001, "${Items} - ${Icon} - ${Legendary}", COLOR_HUD));
            b.Add(p, b.MakeColor("_fuiIisCl", lp += 0.00001, "${Items} - ${Icon} - ${Tagged_for_search}", COLOR_HUD));
            lp = 17.0;
            b.Add(p, b.MakeTitle("ColorsTabTitle", lp += 0.00001, "${Colors}"));
            b.Add(p, b.MakeColor("WCl", lp += 0.00001, "$WARNING_color", COLOR_RED));
        }

        // Anchor + 3D options for every parent that has options so far
        // (except the prompt message swf), mirroring the original loop.
        {
            std::vector<std::string> parents;
            for (const auto& [ident, _] : b.sets) parents.push_back(ident);
            for (const auto& ident : parents) {
                if (ident == "_ext_extra._ext_promptMessageSwf") continue;
                AddAnchorOptions(b, ident);
                Add3DOptions(b, ident, 99);
            }
        }

        // defaultColors loop: adds ${Colors} sections + per-element color
        // options for anything not already covered above. The generated id is
        // the element path stripped to its uppercase letters/digits + "Cl";
        // widget-level entries (no child path) get plain "Cl" named "$Widget".
        {
            double cpos = 17.0;
            std::string lastParent;
            for (const auto& dc : kDefaultColors) {
                std::string path = dc.path;
                // parent = first two segments, child = remainder
                size_t d1 = path.find('.');
                size_t d2 = path.find('.', d1 + 1);
                std::string parent = (d2 == std::string::npos) ? path : path.substr(0, d2);
                std::string child = (d2 == std::string::npos) ? "" : path.substr(d2 + 1);
                if (b.colorAdded.count(parent + "." + child)) continue;

                std::string prefix;
                std::string label = "$Widget";
                if (!child.empty()) {
                    for (char c : child) {
                        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) prefix += c;
                    }
                    label = child;
                }
                if (lastParent != parent) {
                    b.Add(parent, b.MakeTitle("ColorsTabTitle", cpos += 0.00001, "${Colors}"));
                    lastParent = parent;
                }
                b.AddColorOptions(parent, child, prefix, dc.color, cpos += 0.00001, label);
            }
        }

        // Prompt message color + permanent-visibility toggles
        b.AddColorOptions("_ext_extra._ext_promptMessageSwf", "", "", COLOR_HUD, 1, "${Color}");
        for (const char* ident : { "RightMeters_mc.ExplosiveAmmoCount_mc",
                                   "RightMeters_mc.AmmoCount_mc",
                                   "RightMeters_mc.ActionPointMeter_mc",
                                   "LeftMeters_mc.HPMeter_mc" }) {
            b.Add(ident, b.MakeBool("_fuiPerVis", 0.1, "${Permanent_visible}", false));
        }

        // Sort every set by pos (stable to keep same-pos insertion order)
        for (auto& [ident, set] : b.sets) {
            std::stable_sort(set.begin(), set.end(),
                             [](const Option& a, const Option& o) { return a.pos < o.pos; });
        }
        s_options = std::move(b.sets);
    }

    static const OptionSet* OptionsFor(const std::string& parentIdent) {
        BuildOptionCatalog();
        auto it = s_options.find(parentIdent);
        return it == s_options.end() ? nullptr : &it->second;
    }

    static const Option* FindOption(const OptionSet* set, const std::string& id) {
        if (!set) return nullptr;
        for (const auto& o : *set) {
            if (o.id == id) return &o;
        }
        return nullptr;
    }

    // ------------------------------------------------------------------
    // Widget config pack/unpack (HUDLayoutOptions.packWidgetConfig /
    // unpackWidgetConfig): "on:<x>x<y>*<sx>*<sy>r<rot>:<k=v,k=v>"
    // ------------------------------------------------------------------
    struct WidgetConfig {
        bool visible = true;
        double x = 0, y = 0;         // 1920x1080 space
        double scaleX = 1, scaleY = 1;
        double rotation = 0;
        PackedObject mods;           // merged stdValues + explicit overrides
    };

    // Parse "k=v" modifiers using the option model's types (bool/int/else-string).
    static void UnpackModifiers(const std::string& parentIdent, const std::string& str,
                                PackedObject& out, bool skipStdMerge = false) {
        const OptionSet* set = OptionsFor(parentIdent);
        if (!set) return;
        if (!skipStdMerge) {
            for (const auto& o : *set) {
                if (o.hasStd) out[o.id] = o.std;
            }
        }
        size_t start = 0;
        while (start <= str.size()) {
            size_t comma = str.find(',', start);
            std::string pair = str.substr(start, comma == std::string::npos ? std::string::npos
                                                                            : comma - start);
            size_t eq = pair.find('=');
            if (eq != std::string::npos) {
                std::string key = pair.substr(0, eq);
                std::string val = pair.substr(eq + 1);
                if (const Option* opt = FindOption(set, key)) {
                    if (opt->type == Option::Type::Bool) {
                        out[key] = PackedValue::B(val == "true");
                    } else if (opt->type == Option::Type::Int ||
                               opt->type == Option::Type::Color) {
                        // Colors are ints on disk (the original stores them as
                        // strings when loaded, ints when set — both pack as k=v)
                        int v = 0; try { v = std::stoi(val); } catch (...) {}
                        out[key] = PackedValue::I(v);
                    } else {
                        out[key] = PackedValue::S(val);
                    }
                }
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }

    static std::optional<WidgetConfig> UnpackWidgetConfig(const std::string& parentIdent,
                                                          const std::string& str) {
        // Regex from the original:
        // ^(on|off):(-?\d+(\.\d+)?)x(-?\d+(\.\d+)?)\*(\d+(\.\d+)?)\*(\d+(\.\d+)?)(r-?\d+)?(:(.*))?
        if (str.rfind("on:", 0) != 0 && str.rfind("off:", 0) != 0) return std::nullopt;
        WidgetConfig cfg;
        cfg.visible = str[1] == 'n';  // "on"
        size_t p = str.find(':') + 1;

        auto readNum = [&](double& out) -> bool {
            size_t s = p;
            if (p < str.size() && (str[p] == '-' || str[p] == '+')) ++p;
            while (p < str.size() && (isdigit(static_cast<unsigned char>(str[p])) || str[p] == '.')) ++p;
            if (p == s) return false;
            try { out = std::stod(str.substr(s, p - s)); } catch (...) { return false; }
            return true;
        };

        if (!readNum(cfg.x)) return std::nullopt;
        if (p >= str.size() || str[p] != 'x') return std::nullopt;
        ++p;
        if (!readNum(cfg.y)) return std::nullopt;
        if (p >= str.size() || str[p] != '*') return std::nullopt;
        ++p;
        if (!readNum(cfg.scaleX)) return std::nullopt;
        if (p >= str.size() || str[p] != '*') return std::nullopt;
        ++p;
        if (!readNum(cfg.scaleY)) return std::nullopt;
        if (p < str.size() && str[p] == 'r') {
            ++p;
            readNum(cfg.rotation);
        }
        if (p < str.size() && str[p] == ':') {
            UnpackModifiers(parentIdent, str.substr(p + 1), cfg.mods);
        } else {
            UnpackModifiers(parentIdent, "", cfg.mods);
        }
        return cfg;
    }

    // AS-style number to string for the packed line (integers bare, floats full)
    static std::string PackNum(double v) { return FormatNumber(v); }

    static std::string PackModValue(const PackedValue& v) {
        switch (v.kind) {
            case PackedValue::Kind::Bool: return v.b ? "true" : "false";
            case PackedValue::Kind::Int: return std::to_string(v.i);
            case PackedValue::Kind::Float: return FormatNumber(v.f);
            default: return v.s;
        }
    }

    static bool ValueEqualsStd(const PackedValue& v, const PackedValue& std_) {
        if (v.kind == std_.kind) {
            switch (v.kind) {
                case PackedValue::Kind::Bool: return v.b == std_.b;
                case PackedValue::Kind::Int: return v.i == std_.i;
                case PackedValue::Kind::Float: return v.f == std_.f;
                default: return v.s == std_.s;
            }
        }
        return v.AsNumber() == std_.AsNumber() && v.kind != PackedValue::Kind::String &&
               std_.kind != PackedValue::Kind::String;
    }

    static std::string PackWidgetConfig(const std::string& parentIdent, const WidgetConfig& cfg) {
        const OptionSet* set = OptionsFor(parentIdent);
        std::vector<std::string> parts;
        for (const auto& [key, val] : cfg.mods) {
            if (const Option* opt = FindOption(set, key); opt && opt->hasStd) {
                if (ValueEqualsStd(val, opt->std)) continue;
            }
            parts.push_back(key + "=" + PackModValue(val));
        }
        std::string mods;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i) mods += ",";
            mods += parts[i];
        }
        return std::string(cfg.visible ? "on" : "off") + ":" + PackNum(cfg.x) + "x" +
               PackNum(cfg.y) + "*" + PackNum(cfg.scaleX) + "*" + PackNum(cfg.scaleY) +
               "r" + std::to_string(static_cast<long long>(std::llround(cfg.rotation))) +
               ":" + mods;
    }

    // ------------------------------------------------------------------
    // Settings access (MCM value provider raw API — same backing store the
    // MCM Papyrus natives use; writes flush to Data/MCM/Settings/<mod>.ini)
    // ------------------------------------------------------------------
    static std::string IniGet(const std::string& mod, const std::string& key,
                              const std::string& section) {
        auto v = MCMValueProvider::GetModSettingRaw(mod, key + ":" + section);
        return v.value_or("");
    }

    static void IniSet(const std::string& mod, const std::string& key,
                       const std::string& section, const std::string& value) {
        MCMValueProvider::SetModSettingRaw(mod, key + ":" + section, value);
    }

    static bool IniGetBool(const std::string& mod, const std::string& key,
                           const std::string& section) {
        std::string v = IniGet(mod, key, section);
        return v == "1" || v == "true" || v == "True" || v == "TRUE";
    }

    static int IniGetInt(const std::string& mod, const std::string& key,
                         const std::string& section) {
        try { return std::stoi(IniGet(mod, key, section)); } catch (...) { return 0; }
    }

    static constexpr const char* kMod = "FallUIHUD";
    static constexpr const char* kExportMod = "FallUIHUD-Layout-Export";

    static std::string WidgetIniKey(const WidgetDef& w) {
        return std::string("s") + w.group + "__" + w.name;
    }
    static std::string ParentIdent(const WidgetDef& w) {
        return std::string(w.group) + "." + w.name;
    }

    // ------------------------------------------------------------------
    // Editor session state
    // ------------------------------------------------------------------
    struct WidgetState {
        WidgetConfig cfg;
        std::string title;                 // translated display title
        // First-loaded packed line per profile ("Revert last changes" target)
        std::map<int, std::string> startData;
    };

    struct EditorState {
        bool loaded = false;

        WidgetState widgets[kWidgetCount];
        PackedObject globalSettings;       // sEditorSettings (HUDLayoutEditor)
        PackedObject globalLayoutSettings; // sLayoutGlobalSettings (HUDConfig)

        int currentProfile = 1;
        bool easyMode = false;
        std::string easyModeLayout;
        bool defHudAdapter = false;

        int selected = -1;                 // widget index being edited
        bool selectionLocked = false;
        std::string filter = "$All";
        bool showWidgetsList = false;
        bool showGlobalSettings = false;
        bool showImportOverlay = false;
        int zoomQuadrant = -1;             // -1 = fit; 0..3 = 2x into quadrant

        // Canvas rectangle on screen this frame (set by RenderCanvas); the
        // floating panels (edit / widgets list / import / settings) anchor to it.
        ImVec2 canvasMin{}, canvasMax{};

        std::string messageTitle, messageText;  // modal message
        double noticeFlashUntil = 0.0;

        // Cached list of importable layouts (basename without .ini)
        std::vector<std::string> importableLayouts;
        bool defHudXmlExists = false;
    };

    static EditorState s_ed;

    // Default editor settings (HudLayoutManager.defaultGlobalSettings)
    static const std::map<std::string, PackedValue> kDefaultGlobalSettings = {
        { "showMeasurement", PackedValue::B(false) },
        { "showAdvancedOptions", PackedValue::B(false) },
        { "previewPanelBgAlpha", PackedValue::I(50) },
        { "previewShowAnchor", PackedValue::B(false) },
        { "showWidgetBg", PackedValue::B(false) },
    };

    static PackedValue GetGlobalSetting(const std::string& key) {
        if (auto it = s_ed.globalSettings.find(key); it != s_ed.globalSettings.end()) return it->second;
        if (auto it = kDefaultGlobalSettings.find(key); it != kDefaultGlobalSettings.end()) return it->second;
        return PackedValue::I(0);
    }

    static void SaveGlobalSettings() {
        IniSet(kMod, "sEditorSettings", "HUDLayoutEditor", StringPackerPack(s_ed.globalSettings));
    }

    static void SaveGlobalLayoutSettings() {
        IniSet(kMod, "sLayoutGlobalSettings", "HUDConfig", StringPackerPack(s_ed.globalLayoutSettings));
    }

    static PackedValue GetGlobalLayoutSetting(const std::string& key) {
        if (auto it = s_ed.globalLayoutSettings.find(key); it != s_ed.globalLayoutSettings.end()) {
            return it->second;
        }
        return PackedValue::I(0);
    }

    // Colors -----------------------------------------------------------

    static int GameHudColorRGB() {
        // Match the game's current HUD color when available (COLOR_HUD slot).
        // The multi-runtime CommonLibF4 wraps this with per-runtime IDs
        // ({34363 OG, 2248840 NG/AE} — verified present in every address
        // library bin from 1.10.163 through 1.11.221).
        RE::NiColor c = RE::HUDMenuUtils::GetGameplayHUDColor();
        auto ch = [](float v) { return std::clamp(static_cast<int>(v * 255.0f), 0, 255); };
        return (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b);
    }

    // HUDLayoutOptions.getHudColor port (resolves slots too)
    static int ResolveHudColor(int c) {
        if (c <= -100 && c >= -110) {
            c = GetGlobalLayoutSetting("ColorSlot" + std::to_string(-(c + 100))).AsInt();
        }
        if (c == COLOR_HUD) return GameHudColorRGB();
        if (c == COLOR_NONE || c == -99) return 16777215;
        if (c == COLOR_RED) return (237 << 16) | (84 << 8) | 53;
        if (c == COLOR_SUB) return (186 << 16) | (186 << 8) | 186;
        return c;
    }

    static std::string HudColorName(int c) {
        if (c == COLOR_HUD) return Tr("$HUD_color");
        if (c == COLOR_NONE || c == -99) return Tr("$None");
        if (c == COLOR_RED) return Tr("$WARNING_color");
        if (c == COLOR_SUB) return Tr("$GRAY_color");
        if (c <= -100 && c >= -110) {
            return Tr("$Color_slot") + " " + std::to_string(-(c + 100));
        }
        char buf[48];
        snprintf(buf, sizeof(buf), "%s: %d, %d, %d", Tr("$RGB").c_str(),
                 (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
        return buf;
    }

    // ------------------------------------------------------------------
    // Layout load/save
    // ------------------------------------------------------------------

    static void ApplyVanilla(int idx) {
        const WidgetDef& w = kWidgets[idx];
        WidgetConfig& cfg = s_ed.widgets[idx].cfg;
        cfg = WidgetConfig{};
        cfg.x = std::floor(w.vx * 1.5);
        cfg.y = std::floor(w.vy * 1.5);
        cfg.scaleX = cfg.scaleY = 1;
        cfg.rotation = 0;
        cfg.visible = true;
        cfg.mods.clear();
        UnpackModifiers(ParentIdent(w), "", cfg.mods);  // merge stdValues
    }

    static void SaveWidget(int idx) {
        if (s_ed.easyMode) return;  // saving disabled in easy mode (original behavior)
        const WidgetDef& w = kWidgets[idx];
        IniSet(kMod, WidgetIniKey(w), "HUDConfig",
               PackWidgetConfig(ParentIdent(w), s_ed.widgets[idx].cfg));
        s_ed.noticeFlashUntil = ImGui::GetTime() + 0.8;
    }

    static void ApplySavedString(int idx, const std::string& line, int profileForStartData) {
        const WidgetDef& w = kWidgets[idx];
        auto cfg = UnpackWidgetConfig(ParentIdent(w), line);
        if (cfg.has_value()) {
            auto& st = s_ed.widgets[idx];
            if (profileForStartData > 0 && !st.startData.count(profileForStartData)) {
                st.startData[profileForStartData] = line;
            }
            if (!st.startData.count(0)) st.startData[0] = line;
            st.cfg = std::move(*cfg);
        } else {
            ApplyVanilla(idx);
        }
    }

    // --- Layout version migration (updateIniHudConfigLayoutOptionsToCurrentVersion) ---
    // We implement the migrations up to 1.4.0 (int 1004000) and never bump the
    // stored version beyond that, so a newer Flash editor can still run its own
    // later migrations if the user switches back.
    static constexpr int kMigrationVersion = 1004000;

    static void MigrateLayout(std::map<std::string, std::string>& hudConfig,
                              PackedObject& globalLayoutSettings, bool persistVersion) {
        int ver = 0;
        if (auto it = globalLayoutSettings.find("_editorVersion"); it != globalLayoutSettings.end()) {
            ver = it->second.AsInt();
        }
        if (ver >= kMigrationVersion) return;

        auto get = [&](const std::string& key) -> std::string {
            auto it = hudConfig.find(key);
            return it == hudConfig.end() ? "" : it->second;
        };
        auto repack = [&](const std::string& ident, const std::string& key, WidgetConfig& cfg) {
            hudConfig[key] = PackWidgetConfig(ident, cfg);
        };

        // < 1.2.3: Messages widget adopted the PromptMessageHolder slot + x shift
        if (ver < 1002003) {
            std::string cur = get("sHUDNotificationsGroup_mc__Messages_mc");
            if (cur.empty() || cur == "on:7x44*1*1r0:") {
                hudConfig["sHUDNotificationsGroup_mc__Messages_mc"] =
                    get("sHUDNotificationsGroup_mc__PromptMessageHolder_mc");
            }
            cur = get("sHUDNotificationsGroup_mc__Messages_mc");
            if (!cur.empty() && cur != "on:7x3*1*1r0:" && cur != "on:7x3*1*1:") {
                auto cfg = UnpackWidgetConfig("HUDNotificationsGroup_mc.Messages_mc", cur);
                if (cfg.has_value() && !cfg->mods.count("msgTxtAlign")) {
                    cfg->x += 28;
                    repack("HUDNotificationsGroup_mc.Messages_mc",
                           "sHUDNotificationsGroup_mc__Messages_mc", *cfg);
                }
            }
        }

        // < 1.2.4: recentre stray hit/explosive indicators
        if (ver < 1002004) {
            for (auto [ident, key, badX, badY] :
                 { std::tuple{ "CenterGroup_mc.DirectionalHitIndicatorBase_mc",
                               "sCenterGroup_mc__DirectionalHitIndicatorBase_mc", 300.0, -200.0 },
                   std::tuple{ "CenterGroup_mc.ExplosiveIndicatorBase_mc",
                               "sCenterGroup_mc__ExplosiveIndicatorBase_mc", 300.0, -250.0 } }) {
                auto cfg = UnpackWidgetConfig(ident, get(key));
                if (cfg.has_value() && cfg->x == badX && cfg->y == badY) {
                    cfg->x = 0; cfg->y = 0;
                    repack(ident, key, *cfg);
                }
            }
        }

        // < 1.3.4: drop obsolete apseqsScale
        if (ver < 1003004) {
            auto cfg = UnpackWidgetConfig("RightMeters_mc.ActionPointMeter_mc",
                                          get("sRightMeters_mc__ActionPointMeter_mc"));
            if (cfg.has_value() && cfg->mods.count("apseqsScale")) {
                cfg->mods.erase("apseqsScale");
                repack("RightMeters_mc.ActionPointMeter_mc",
                       "sRightMeters_mc__ActionPointMeter_mc", *cfg);
            }
        }

        // < 1.4.0: rollover vertical align defaults, 1.5x position rescale,
        //          bar Brd* -> SB* conversion
        if (ver < 1004000) {
            {
                auto cfg = UnpackWidgetConfig("CenterGroup_mc.RolloverWidget_mc",
                                              get("sCenterGroup_mc__RolloverWidget_mc"));
                if (cfg.has_value()) {
                    const bool hasIVA = cfg->mods.count("_fuiIVA") && cfg->mods["_fuiIVA"].AsInt() != 0;
                    if (hasIVA && (!cfg->mods.count("_fuiIVAH") || cfg->mods["_fuiIVAH"].AsInt() == 0)) {
                        cfg->mods["_fuiIVAH"] = PackedValue::I(60);
                    }
                    if (!hasIVA) cfg->mods["_fuiIVA"] = PackedValue::I(0);
                    repack("CenterGroup_mc.RolloverWidget_mc",
                           "sCenterGroup_mc__RolloverWidget_mc", *cfg);
                }
            }
            for (int i = 0; i < kWidgetCount; ++i) {
                const WidgetDef& w = kWidgets[i];
                auto cfg = UnpackWidgetConfig(ParentIdent(w), get(WidgetIniKey(w)));
                if (cfg.has_value() && (cfg->x != 0 || cfg->y != 0)) {
                    cfg->x = std::round(cfg->x * 1.5);
                    cfg->y = std::round(cfg->y * 1.5);
                    repack(ParentIdent(w), WidgetIniKey(w), *cfg);
                }
            }
            for (const auto& bar : s_barGroups) {
                std::string key = "s" + bar.parent;
                for (size_t pos2 = key.find('.'); pos2 != std::string::npos; pos2 = key.find('.')) {
                    key.replace(pos2, 1, "__");
                }
                auto cfg = UnpackWidgetConfig(bar.parent, get(key));
                if (!cfg.has_value()) continue;
                const std::string& pre = bar.prefix;
                auto has = [&](const std::string& k) {
                    auto it = cfg->mods.find(k);
                    return it != cfg->mods.end() && it->second.AsNumber() != 0;
                };
                if (has(pre + "BrdS")) {
                    if (!has(pre + "SV")) {
                        cfg->mods[pre + "SV"] = PackedValue::B(true);
                        cfg->mods[pre + "SA"] = PackedValue::I(0);
                    }
                    cfg->mods[pre + "SBS"] = cfg->mods[pre + "BrdS"];
                    auto num = [&](const std::string& k, double def) {
                        auto it = cfg->mods.find(k);
                        return (it != cfg->mods.end() && it->second.AsNumber() != 0)
                                   ? it->second.AsNumber() : def;
                    };
                    cfg->mods[pre + "SP"] = PackedValue::I(static_cast<int>(std::round(num(pre + "BrdP", 3) * 1.5)));
                    cfg->mods[pre + "SBW"] = PackedValue::I(static_cast<int>(std::round(num(pre + "BrdW", 3) * 1.5)));
                    cfg->mods[pre + "SBCaps"] = PackedValue::I(static_cast<int>(std::round(num(pre + "BrdCL", 5) * 1.5)));
                    cfg->mods[pre + "SBA"] = PackedValue::I(100);
                }
                cfg->mods.erase(pre + "BrdS");
                cfg->mods.erase(pre + "BrdP");
                cfg->mods.erase(pre + "BrdW");
                cfg->mods.erase(pre + "BrdCL");
                if (has(pre + "Dir")) {
                    cfg->mods.erase(pre + "Dir");
                    cfg->mods[pre + "Dir2"] = PackedValue::I(2);
                }
                repack(bar.parent, key, *cfg);
            }
        }

        globalLayoutSettings["_editorVersion"] = PackedValue::I(kMigrationVersion);
        if (persistVersion) {
            SaveGlobalLayoutSettings();
        }
    }

    // Loads a HUDConfig key/value map into the widget states (loadLayoutIni).
    // Matches the original's persistence exactly: the global layout settings
    // and the migrated _editorVersion are ALWAYS written back to the INI
    // (setGlobalLayoutSettings and setGlobalLayoutSetting both save), while
    // the per-widget lines are saved only when saveWidgets is true (the
    // original's param2=false path: normal load and profile switches; easy-
    // mode previews pass param2=true and skip widget saves). Persisting the
    // version is what prevents the <1.4.0 migration's 1.5x position rescale
    // from re-running — and re-scaling — on every subsequent load.
    static void LoadLayoutMap(std::map<std::string, std::string> hudConfig, bool saveWidgets) {
        // Global layout settings come from the map when present
        if (auto it = hudConfig.find("sLayoutGlobalSettings"); it != hudConfig.end()) {
            s_ed.globalLayoutSettings = StringPackerUnpack(it->second);
        } else {
            s_ed.globalLayoutSettings.clear();
        }
        SaveGlobalLayoutSettings();
        MigrateLayout(hudConfig, s_ed.globalLayoutSettings, true);

        for (int i = 0; i < kWidgetCount; ++i) {
            ApplyVanilla(i);
            auto it = hudConfig.find(WidgetIniKey(kWidgets[i]));
            if (it != hudConfig.end() && !it->second.empty()) {
                ApplySavedString(i, it->second, s_ed.currentProfile);
            }
            if (saveWidgets) SaveWidget(i);
        }
    }

    // Loads the active profile's layout from FallUIHUD.ini. The original
    // saves every widget back after a normal load (loadLayoutIni param2=false),
    // which also persists freshly migrated lines — mirror that.
    static void LoadCurrentLayoutFromIni() {
        std::map<std::string, std::string> hudConfig;
        hudConfig["sLayoutGlobalSettings"] = IniGet(kMod, "sLayoutGlobalSettings", "HUDConfig");
        for (int i = 0; i < kWidgetCount; ++i) {
            hudConfig[WidgetIniKey(kWidgets[i])] = IniGet(kMod, WidgetIniKey(kWidgets[i]), "HUDConfig");
        }
        LoadLayoutMap(std::move(hudConfig), true);
    }

    // Simple INI reader for importable layout files (section -> key -> value).
    // Shipped presets come in UTF-8 AND UTF-16 LE (e.g. "3D-Demo.ini" starts
    // with an FF FE BOM) — Flash's IniReader handles both, so handle both.
    static std::map<std::string, std::string> ReadLayoutIniFile(const fs::path& path) {
        std::map<std::string, std::string> hudConfig;
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return hudConfig;
        std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

        // UTF-16 LE/BE with BOM -> UTF-8 (layout data is ASCII; non-ASCII
        // code units only ever appear in comments, so drop them).
        if (raw.size() >= 2 && ((static_cast<unsigned char>(raw[0]) == 0xFF &&
                                 static_cast<unsigned char>(raw[1]) == 0xFE) ||
                                (static_cast<unsigned char>(raw[0]) == 0xFE &&
                                 static_cast<unsigned char>(raw[1]) == 0xFF))) {
            const bool le = static_cast<unsigned char>(raw[0]) == 0xFF;
            std::string utf8;
            utf8.reserve(raw.size() / 2);
            for (size_t i = 2; i + 1 < raw.size(); i += 2) {
                unsigned lo = static_cast<unsigned char>(raw[le ? i : i + 1]);
                unsigned hi = static_cast<unsigned char>(raw[le ? i + 1 : i]);
                unsigned cu = (hi << 8) | lo;
                if (cu < 0x80) utf8 += static_cast<char>(cu);
            }
            raw = std::move(utf8);
        }

        std::istringstream fs2(raw);
        std::string line, section;
        while (std::getline(fs2, line)) {
            // strip BOM/whitespace/CR
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            size_t s = line.find_first_not_of(" \t\xEF\xBB\xBF");
            if (s == std::string::npos) continue;
            line = line.substr(s);
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            if (line[0] == '[') {
                size_t e = line.find(']');
                section = (e == std::string::npos) ? "" : line.substr(1, e - 1);
                continue;
            }
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            // trim
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
            size_t vs = val.find_first_not_of(" \t");
            val = (vs == std::string::npos) ? "" : val.substr(vs);
            if (section == "HUDConfig") hudConfig[key] = val;
        }
        return hudConfig;
    }

    static fs::path ImportableLayoutsDir() {
        return fs::current_path() / "Data" / "Interface" / "FallUI HUD" / "Importable HUD Layouts";
    }

    static void ScanImportableLayouts() {
        s_ed.importableLayouts.clear();
        std::error_code ec;
        try {
            for (fs::directory_iterator it(ImportableLayoutsDir(), ec), end; !ec && it != end;
                 it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                auto p = it->path();
                // extension()/stem() .string() convert via the ANSI code page
                // and throw std::system_error for non-representable characters
                // (user-renamed HUD layout presets can contain anything);
                // u8string() is UTF-8 and never throws.
                auto toUtf8 = [](const fs::path& part) {
                    const auto u8 = part.u8string();
                    return std::string(u8.begin(), u8.end());
                };
                std::string ext = toUtf8(p.extension());
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (ext == ".ini") {
                    s_ed.importableLayouts.push_back(toUtf8(p.stem()));
                }
            }
        } catch (const std::system_error&) {
            // Leave whatever was collected before the failing entry.
        }
        std::sort(s_ed.importableLayouts.begin(), s_ed.importableLayouts.end());
        std::error_code ec2;
        s_ed.defHudXmlExists =
            fs::exists(fs::current_path() / "Data" / "Interface" / "DEF_CONF" / "DEF_HUD.xml", ec2);
    }

    static void ShowMessage(const std::string& title, const std::string& text) {
        s_ed.messageTitle = title;
        s_ed.messageText = text;
    }

    // Full session (re)load — mirrors HudLayoutManager.init0/init4
    static void LoadSession() {
        BuildOptionCatalog();
        EnsureTranslationsLoaded();
        s_ed = EditorState{};
        s_ed.loaded = true;

        if (auto v = IniGet(kMod, "sEditorSettings", "HUDLayoutEditor"); !v.empty()) {
            s_ed.globalSettings = StringPackerUnpack(v);
        }
        if (auto v = IniGet(kMod, "sLayoutGlobalSettings", "HUDConfig"); !v.empty()) {
            s_ed.globalLayoutSettings = StringPackerUnpack(v);
        }

        s_ed.easyMode = IniGetBool(kMod, "bLayoutEasyMode", "FallUIHUD");
        s_ed.easyModeLayout = IniGet(kMod, "sEasyModeUseLayout", "FallUIHUD");
        s_ed.defHudAdapter = IniGetBool(kMod, "bDefHudAdapterLoadConfig", "FallUIHUD");
        if (s_ed.defHudAdapter) {
            s_ed.easyMode = true;
            s_ed.easyModeLayout = "DEF_HUD.xml";
        }
        s_ed.currentProfile = IniGetInt(kMod, "iCurrentHUDProfile", "HUDConfigProfiles");
        if (s_ed.easyMode) {
            s_ed.currentProfile = -1;
        } else if (s_ed.currentProfile < 1) {
            s_ed.currentProfile = 1;
        }

        // Widget titles: Translate("$<name>"), fall back to the struct title
        for (int i = 0; i < kWidgetCount; ++i) {
            std::string token = std::string("$") + kWidgets[i].name;
            std::string tr = Tr(token);
            // If the token didn't resolve, the fallback humanizes it — prefer
            // the original struct title in that case for exactness.
            if (s_editorTr.find(kWidgets[i].name) == s_editorTr.end()) {
                tr = kWidgets[i].title;
            }
            s_ed.widgets[i].title = tr;
        }

        ScanImportableLayouts();

        if (s_ed.easyMode && s_ed.easyModeLayout != "DEF_HUD.xml" && !s_ed.easyModeLayout.empty()) {
            auto map = ReadLayoutIniFile(ImportableLayoutsDir() / (s_ed.easyModeLayout + ".ini"));
            LoadLayoutMap(std::move(map), false);
        } else {
            LoadCurrentLayoutFromIni();
        }
    }

    // Profile switching (eventSwitchProfile port)
    static void SwitchProfile(int target) {
        if (s_ed.currentProfile == -1 || s_ed.currentProfile == target) return;

        // Pack the current widgets + global layout settings into sProfile<n>
        std::string packed;
        for (int i = 0; i < kWidgetCount; ++i) {
            if (!packed.empty()) packed += ";";
            packed += std::string(kWidgets[i].name) + "~" +
                      PackWidgetConfig(ParentIdent(kWidgets[i]), s_ed.widgets[i].cfg);
        }
        packed += "$\xC2\xA7$" + StringPackerPack(s_ed.globalLayoutSettings);
        IniSet(kMod, "sProfile" + std::to_string(s_ed.currentProfile), "HUDConfigProfiles", packed);

        s_ed.currentProfile = target;
        IniSet(kMod, "iCurrentHUDProfile", "HUDConfigProfiles", std::to_string(target));

        // Load the target profile
        std::string stored = IniGet(kMod, "sProfile" + std::to_string(target), "HUDConfigProfiles");
        std::string widgetsPart = stored;
        std::string globalsPart;
        if (auto sep = stored.find("$\xC2\xA7$"); sep != std::string::npos) {
            widgetsPart = stored.substr(0, sep);
            globalsPart = stored.substr(sep + 4);
        }
        std::map<std::string, std::string> hudConfig;
        hudConfig["sLayoutGlobalSettings"] = globalsPart;
        size_t start = 0;
        while (start < widgetsPart.size()) {
            size_t semi = widgetsPart.find(';', start);
            std::string entry = widgetsPart.substr(
                start, semi == std::string::npos ? std::string::npos : semi - start);
            if (auto tilde = entry.find('~'); tilde != std::string::npos) {
                std::string name = entry.substr(0, tilde);
                for (int i = 0; i < kWidgetCount; ++i) {
                    if (name == kWidgets[i].name) {
                        hudConfig[WidgetIniKey(kWidgets[i])] = entry.substr(tilde + 1);
                        break;
                    }
                }
            }
            if (semi == std::string::npos) break;
            start = semi + 1;
        }
        LoadLayoutMap(std::move(hudConfig), true);
    }

    // Export (exportLayout port) — writes FallUIHUD-Layout-Export.ini
    static void ExportLayout() {
        // Timestamp like the original: "YYYY-MM-DD HH:MM"
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        char stamp[32];
        snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
        IniSet(kExportMod, "sExportTime", "ExportInfo", stamp);

        // Readback verification (the original checks the write landed)
        if (IniGet(kExportMod, "sExportTime", "ExportInfo") != stamp) {
            ShowMessage(Tr("$Export"), Tr("$ExportFailedMsg"));
            return;
        }
        IniSet(kExportMod, "sLayoutGlobalSettings", "HUDConfig",
               StringPackerPack(s_ed.globalLayoutSettings));
        for (int i = 0; i < kWidgetCount; ++i) {
            IniSet(kExportMod, WidgetIniKey(kWidgets[i]), "HUDConfig",
                   PackWidgetConfig(ParentIdent(kWidgets[i]), s_ed.widgets[i].cfg));
        }
        std::string msg = Tr("$ExportSuccessMsg");
        // The original replaces "#newline#" markers
        size_t pos = 0;
        while ((pos = msg.find("#newline#")) != std::string::npos) msg.replace(pos, 9, "\n");
        ShowMessage(Tr("$Export"), msg + "\n\n[Fallout 4]\\data\\MCM\\Settings\\FallUIHUD-Layout-Export.ini");
    }

    // ------------------------------------------------------------------
    // Option row rendering (DynamicOptionsPanel port)
    // ------------------------------------------------------------------

    // Value-to-text tables for checkTypes
    static const char* const* CheckTypeLabels(const std::string& checkType, int& count) {
        static const char* tfAlign[] = { "left", "center", "right" };
        static const char* tfAutoSize[] = { "none", "left", "center", "right" };
        static const char* iconAlign[] = { "left", "top", "right", "bottom" };
        static const char* direction[] = { "left", "top", "right", "bottom" };
        static const char* upcenterdown[] = { "top", "center", "bottom" };
        static const char* gposX[] = { "off", "auto", "left", "center-left", "center", "center-right", "right" };
        static const char* gposY[] = { "off", "auto", "top", "center-top", "center", "center-bottom", "bottom" };
        static const char* borderStyles[] = { "off", "simple", "simple2", "fo4_vertical", "fo4_horizontal",
                                              "fo4_top", "fo4_right", "fo4_bottom", "fo4_left", "fo3_LB",
                                              "fo3_LT", "fo3_RT", "fo3_RB", "fo3_bar_LB", "fo3_bar_LT",
                                              "fo3_bar_RT", "fo3_bar_RB", "fo3_bar_B", "fo3_bar_T" };
        static const char* borderPlacements[] = { "outside", "middle", "inside" };

        if (checkType == "tfAlign") { count = 3; return tfAlign; }
        if (checkType == "tfAutoSize") { count = 4; return tfAutoSize; }
        if (checkType == "iconAlign") { count = 4; return iconAlign; }
        if (checkType == "direction") { count = 4; return direction; }
        if (checkType == "upcenterdown") { count = 3; return upcenterdown; }
        if (checkType == "globalPositionsX") { count = 7; return gposX; }
        if (checkType == "globalPositionsY") { count = 7; return gposY; }
        if (checkType == "borderStyles") { count = 19; return borderStyles; }
        if (checkType == "borderPlacements") { count = 3; return borderPlacements; }
        if (checkType == "font") { count = kFontCount; return kFontNames; }
        if (checkType == "crosshairTypes") { count = 5; return kCrosshairStateNames; }
        count = 0;
        return nullptr;
    }

    // Evaluates an option's onlyIf condition against the current modifier map.
    static bool OnlyIfSatisfied(const Option& opt, const OptionSet* set, const PackedObject& mods) {
        if (opt.onlyIf.empty()) return true;
        size_t start = 0;
        while (start <= opt.onlyIf.size()) {
            size_t comma = opt.onlyIf.find(',', start);
            std::string ref = opt.onlyIf.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start);
            bool negate = !ref.empty() && ref[0] == '!';
            if (negate) ref = ref.substr(1);
            if (!ref.empty()) {
                double v = 0;
                if (auto it = mods.find(ref); it != mods.end()) {
                    v = it->second.AsNumber();
                } else if (const Option* refOpt = FindOption(set, ref); refOpt && refOpt->hasStd) {
                    v = refOpt->std.AsNumber();
                }
                const bool truthy = v != 0;
                // Original: hidden ("skip") when (!negate && !value) || (negate && value)
                if ((!negate && !truthy) || (negate && truthy)) return false;
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return true;
    }

    // Color picker row: special slots + custom RGB, matching OLineColor.
    // Returns true when the value changed.
    static bool ColorRow(const char* label, int& color) {
        bool changed = false;
        ImGui::PushID(label);

        // Preview swatch in the resolved color
        int rgb = ResolveHudColor(color);
        ImVec4 col = ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f,
                            (rgb & 0xFF) / 255.0f, 1.0f);
        ImGui::ColorButton("##swatch", col, ImGuiColorEditFlags_NoTooltip, ImVec2(18, 18));
        ImGui::SameLine();

        std::string preview = HudColorName(color);
        ImGui::SetNextItemWidth(ImGui::CalcItemWidth() * 0.6f);
        if (ImGui::BeginCombo(label, preview.c_str())) {
            struct Slot { int value; std::string name; };
            std::vector<Slot> slots = {
                { COLOR_HUD, Tr("$HUD_color") },
                { COLOR_RED, Tr("$WARNING_color") },
                { COLOR_SUB, Tr("$GRAY_color") },
                { COLOR_NONE, Tr("$None") },
            };
            for (int i = 1; i <= 4; ++i) {
                slots.push_back({ -100 - i, Tr("$Color_slot") + " " + std::to_string(i) });
            }
            for (const auto& s : slots) {
                int srgb = ResolveHudColor(s.value);
                ImVec4 scol = ImVec4(((srgb >> 16) & 0xFF) / 255.0f, ((srgb >> 8) & 0xFF) / 255.0f,
                                     (srgb & 0xFF) / 255.0f, 1.0f);
                ImGui::ColorButton(("##s" + s.name).c_str(), scol,
                                   ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
                ImGui::SameLine();
                if (ImGui::Selectable(s.name.c_str(), color == s.value)) {
                    color = s.value;
                    changed = true;
                }
            }
            ImGui::Separator();
            // Custom RGB editor
            float rgbf[3] = { col.x, col.y, col.z };
            if (ImGui::ColorEdit3(Tr("$RGB").c_str(), rgbf,
                                  ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
                color = (static_cast<int>(rgbf[0] * 255) << 16) |
                        (static_cast<int>(rgbf[1] * 255) << 8) |
                        static_cast<int>(rgbf[2] * 255);
                changed = true;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(Tr("$RGB").c_str());
            ImGui::EndCombo();
        }
        ImGui::PopID();
        return changed;
    }

    // Renders one option set (grouped tabs + rows). `mods` is the live
    // modifier map. Returns true if anything changed this frame.
    struct OptionPanelState {
        std::string currentGroup = "_all";
    };
    static std::map<std::string, OptionPanelState> s_panelStates;

    static bool RenderOptionSet(const std::string& panelId, const OptionSet& set,
                                PackedObject& mods, bool advancedGate) {
        bool changed = false;
        const bool showAdvanced = GetGlobalSetting("showAdvancedOptions").AsBool();
        auto& panel = s_panelStates[panelId];

        if (advancedGate) {
            bool adv = showAdvanced;
            if (ImGui::Checkbox(Tr("$Show_advanced_options").c_str(), &adv)) {
                s_ed.globalSettings["showAdvancedOptions"] = PackedValue::B(adv);
                SaveGlobalSettings();
            }
            ImGui::SameLine();
            bool meas = GetGlobalSetting("showMeasurement").AsBool();
            if (ImGui::Checkbox(Tr("$Show_measurement").c_str(), &meas)) {
                s_ed.globalSettings["showMeasurement"] = PackedValue::B(meas);
                SaveGlobalSettings();
            }
            if (!GetGlobalSetting("showAdvancedOptions").AsBool()) return changed;
        }

        // Group tabs from grp_title options
        std::vector<std::pair<std::string, std::string>> groups;  // id, label
        groups.push_back({ "_all", Tr("$All") });
        bool hasGeneric = false;
        for (const auto& o : set) {
            if (o.type == Option::Type::GrpTitle) {
                groups.push_back({ o.id, Tr(o.name) });
            } else if (groups.size() == 1) {
                hasGeneric = true;
            }
        }
        if (hasGeneric && groups.size() > 1) {
            groups.insert(groups.begin() + 1, { "_generic", Tr("$Generic") });
        }
        if (groups.size() > 2) {
            float x = 0;
            const float maxW = ImGui::GetContentRegionAvail().x;
            for (size_t i = 0; i < groups.size(); ++i) {
                const auto& [gid, glabel] = groups[i];
                if (glabel.empty()) continue;
                float w = ImGui::CalcTextSize(glabel.c_str()).x + 16.0f;
                if (i > 0 && x + w < maxW) ImGui::SameLine();
                else x = 0;
                x += w + 8.0f;
                const bool active = panel.currentGroup == gid;
                if (active) ImGui::PushStyleColor(ImGuiCol_Button, MixRGB(kFo4Green, 0, 0.3f));
                if (ImGui::SmallButton((glabel + "##grp" + gid).c_str())) {
                    panel.currentGroup = gid;
                }
                if (active) ImGui::PopStyleColor();
            }
            ImGui::Separator();
        }
        if (panel.currentGroup != "_all") {
            // Validate the selected group still exists
            bool found = false;
            for (const auto& [gid, _] : groups) {
                if (gid == panel.currentGroup) { found = true; break; }
            }
            if (!found) panel.currentGroup = "_all";
        }

        // Row rendering with group filtering
        std::string activeGroup = "_generic";
        for (const auto& o : set) {
            if (o.type == Option::Type::GrpTitle) activeGroup = o.id;
            if (panel.currentGroup != "_all" &&
                activeGroup != panel.currentGroup &&
                !(panel.currentGroup == "_generic" && activeGroup == "_generic")) {
                continue;
            }
            if (!OnlyIfSatisfied(o, &set, mods)) continue;

            std::string label = Tr(o.name);
            if (o.deprecated) {
                // Deprecated options only appear while set to a non-default value
                auto it = mods.find(o.id);
                const bool isSet = it != mods.end() && o.hasStd && !ValueEqualsStd(it->second, o.std);
                if (!isSet) continue;
                label += " (" + Tr("$deprecated") + ")";
            }

            ImGui::PushID(o.id.c_str());
            switch (o.type) {
                case Option::Type::GrpTitle:
                case Option::Type::UiTitle: {
                    if (!label.empty()) {
                        ImGui::Spacing();
                        ImGui::TextColored(ImVec4(0.65f, 0.95f, 0.65f, 1.0f), "%s", label.c_str());
                    }
                    break;
                }
                case Option::Type::UiText: {
                    ImGui::TextWrapped("%s", label.c_str());
                    break;
                }
                case Option::Type::Bool: {
                    bool v = o.hasStd ? o.std.AsBool() : false;
                    if (auto it = mods.find(o.id); it != mods.end()) v = it->second.AsBool();
                    if (ImGui::Checkbox(label.c_str(), &v)) {
                        mods[o.id] = PackedValue::B(v);
                        changed = true;
                    }
                    // "(default)" reset marker when off-default
                    if (o.hasStd) {
                        auto it = mods.find(o.id);
                        if (it != mods.end() && !ValueEqualsStd(it->second, o.std)) {
                            ImGui::SameLine();
                            std::string def = std::string("(") + (o.std.AsBool() ? Tr("$ON") : Tr("$OFF")) + ")";
                            if (ImGui::SmallButton(def.c_str())) {
                                mods[o.id] = o.std;
                                changed = true;
                            }
                        }
                    }
                    break;
                }
                case Option::Type::Int: {
                    int v = o.hasStd ? o.std.AsInt() : 0;
                    if (auto it = mods.find(o.id); it != mods.end()) v = it->second.AsInt();

                    int labelCount = 0;
                    const char* const* labels = CheckTypeLabels(o.checkType, labelCount);
                    if (labels && v >= o.minV && v <= o.maxV) {
                        int li = v - o.minV;
                        // "font" checkType has min -1 mapping to std font name list
                        // where -1 shows as index 0 shifted; the original shows
                        // fontNames[v] directly with -1 = "(std)".
                        std::string valText;
                        if (o.checkType == "font") {
                            valText = (v < 0 || v >= labelCount) ? "(std)" : Tr(labels[v]);
                        } else {
                            valText = (li >= 0 && li < labelCount) ? Tr(labels[li]) : std::to_string(v);
                        }
                        ImGui::SetNextItemWidth(ImGui::CalcItemWidth() * 0.65f);
                        if (ImGui::SliderInt(label.c_str(), &v, o.minV, o.maxV, valText.c_str())) {
                            if (o.step > 1) v = o.minV + ((v - o.minV) / o.step) * o.step;
                            mods[o.id] = PackedValue::I(v);
                            changed = true;
                        }
                    } else {
                        ImGui::SetNextItemWidth(ImGui::CalcItemWidth() * 0.65f);
                        if (ImGui::SliderInt(label.c_str(), &v, o.minV, o.maxV)) {
                            if (o.step > 1) v = o.minV + ((v - o.minV) / o.step) * o.step;
                            mods[o.id] = PackedValue::I(v);
                            changed = true;
                        }
                    }
                    if (o.hasStd) {
                        auto it = mods.find(o.id);
                        if (it != mods.end() && !ValueEqualsStd(it->second, o.std)) {
                            ImGui::SameLine();
                            std::string def = "(" + std::to_string(o.std.AsInt()) + ")";
                            if (ImGui::SmallButton(def.c_str())) {
                                mods[o.id] = o.std;
                                changed = true;
                            }
                        }
                    }
                    break;
                }
                case Option::Type::Color: {
                    int v = o.hasStd ? o.std.AsInt() : COLOR_NONE;
                    if (auto it = mods.find(o.id); it != mods.end()) v = it->second.AsInt();
                    if (ColorRow(label.c_str(), v)) {
                        mods[o.id] = PackedValue::I(v);
                        changed = true;
                    }
                    if (o.hasStd) {
                        auto it = mods.find(o.id);
                        if (it != mods.end() && !ValueEqualsStd(it->second, o.std)) {
                            ImGui::SameLine();
                            if (ImGui::SmallButton(("(" + Tr("$Default") + ")").c_str())) {
                                mods[o.id] = o.std;
                                changed = true;
                            }
                        }
                    }
                    break;
                }
            }
            ImGui::PopID();
        }
        return changed;
    }

    // ------------------------------------------------------------------
    // Canvas
    // ------------------------------------------------------------------

    // Screen-space rect (canvas px) of a widget's bounding box, ignoring rotation.
    struct WidgetRect {
        ImVec2 center;    // anchor position on canvas
        ImVec2 boxMin, boxMax;
        float ax, ay;     // anchor percentages
    };

    static WidgetRect ComputeWidgetRect(int idx, ImVec2 origin, float scale,
                                        ImVec2 panOffset) {
        const WidgetDef& w = kWidgets[idx];
        const WidgetConfig& cfg = s_ed.widgets[idx].cfg;
        const GroupDef* g = FindGroup(w.group);

        float ax, ay;
        DefaultAnchor(w, ax, ay);

        // Stage position (1280x720 space): group origin + itemXY converted
        float sx = static_cast<float>(g->x + cfg.x * 1280.0 / 1920.0);
        float sy = static_cast<float>(g->y + cfg.y * 720.0 / 1080.0);

        float bw = w.w * static_cast<float>(cfg.scaleX);
        float bh = w.h * static_cast<float>(cfg.scaleY);

        WidgetRect r;
        r.ax = ax;
        r.ay = ay;
        r.center = ImVec2(origin.x + (sx + panOffset.x) * scale,
                          origin.y + (sy + panOffset.y) * scale);
        r.boxMin = ImVec2(r.center.x - bw * ax / 100.0f * scale,
                          r.center.y - bh * ay / 100.0f * scale);
        r.boxMax = ImVec2(r.boxMin.x + bw * scale, r.boxMin.y + bh * scale);
        return r;
    }

    static bool WidgetPassesFilter(int idx) {
        if (s_ed.filter == "$All") return true;
        struct Filter { const char* name; std::vector<const char*> widgets; };
        static const std::vector<Filter> kFilters = {
            { "$Notifications", { "HUDCrosshair_mc", "Messages_mc", "TutorialText_mc",
                                  "PromptMessageHolder_mc", "PowerArmorLowBatteryWarning_mc",
                                  "LocationText_mc", "XPMeter_mc", "FlashLightWidget_mc",
                                  "FatigueWarning_mc", "HUDActiveEffectsWidget_mc",
                                  "PerkVaultBoy_mc", "_ext_promptMessageSwf" } },
            { "$Quests", { "HUDCrosshair_mc", "ObjectiveUpdates_mc", "QuestUpdates_mc",
                           "QuestVaultBoy_mc", "SubtitleText_mc" } },
            { "$Exploration", { "HUDCrosshair_mc", "CompassWidget_mc", "RolloverWidget_mc",
                                "LocationText_mc", "QuickContainerWidget_mc",
                                "HUDActiveEffectsWidget_mc", "FatigueWarning_mc",
                                "FlashLightWidget_mc", "RadsMeter_mc" } },
            { "$Assault", { "HUDCrosshair_mc", "StealthMeter_mc", "ExplosiveIndicatorBase_mc",
                            "HitIndicator_mc", "DirectionalHitIndicatorBase_mc", "AmmoCount_mc",
                            "ExplosiveAmmoCount_mc", "EnemyHealthMeter_mc", "VaultBoyCondition_mc",
                            "HPMeter_mc", "ActionPointMeter_mc", "CompassWidget_mc", "CritMeter_mc" } },
        };
        for (const auto& f : kFilters) {
            if (s_ed.filter == f.name) {
                for (const char* wn : f.widgets) {
                    if (std::string(kWidgets[idx].name) == wn) return true;
                }
                return false;
            }
        }
        return true;
    }

    static const std::vector<std::string>& FilterNames() {
        static const std::vector<std::string> names = {
            "$All", "$Notifications", "$Quests", "$Exploration", "$Assault"
        };
        return names;
    }

    // Canvas interaction state
    static int s_dragging = -1;
    static ImVec2 s_dragOffset;  // mouse - widget anchor at drag start

    // ==================================================================
    // Canvas preview art
    // ==================================================================
    // The original editor renders the real HUDMenu.swf with demo data
    // (HudMenuPreview.as). We recreate each widget's appearance with ImGui
    // draw primitives driven by the CURRENT modifier values — cfg.mods holds
    // every option (defaults pre-merged by UnpackModifiers), so bars, texts,
    // brackets and colors reflect the active layout/preset live, and edit-
    // panel changes update the art immediately, like the Flash editor.

    static double ModN(const WidgetConfig& c, const std::string& id, double def = 0.0) {
        auto it = c.mods.find(id);
        return it == c.mods.end() ? def : it->second.AsNumber();
    }
    static bool ModB(const WidgetConfig& c, const std::string& id, bool def = true) {
        auto it = c.mods.find(id);
        return it == c.mods.end() ? def : it->second.AsBool();
    }
    static ImU32 ModCol(const WidgetConfig& c, const std::string& id, int defSlot, float alpha) {
        auto it = c.mods.find(id);
        const int raw = it == c.mods.end() ? defSlot : static_cast<int>(it->second.AsNumber());
        return RGBIntToImU32(ResolveHudColor(raw), alpha);
    }

    // ---- Vertex transforms --------------------------------------------
    // ImGui has no transform stack; we capture the vertex range an element
    // wrote and transform it in place (the classic ImRotate pattern). This
    // gives us widget/element rotations and an approximation of FallUI's 3D
    // effect (act3D + RX/RY/RZ + perspective), which the original applies to
    // the live Flash objects.

    static constexpr float kDeg2Rad = 3.14159265f / 180.0f;

    static void RotateVertsSince(ImDrawList* dl, int vtx0, ImVec2 pivot, float rotDeg) {
        if (rotDeg == 0.0f) return;
        const float a = rotDeg * kDeg2Rad, ca = std::cos(a), sa = std::sin(a);
        for (int i = vtx0; i < dl->VtxBuffer.Size; ++i) {
            ImDrawVert& v = dl->VtxBuffer[i];
            const float x = v.pos.x - pivot.x, y = v.pos.y - pivot.y;
            v.pos = ImVec2(pivot.x + x * ca - y * sa, pivot.y + x * sa + y * ca);
        }
    }

    // Widget-level transform: 2D rotation, plus the 3D block when act3D is
    // on (rotate around X/Y/Z then perspective-project like Flash's
    // PerspectiveProjection with the configured field of view).
    static void TransformPreviewVerts(ImDrawList* dl, int vtx0, ImVec2 pivot,
                                      const WidgetConfig& c, float scale) {
        const float rz2D = static_cast<float>(c.rotation);
        if (!ModB(c, "act3D", false)) {
            RotateVertsSince(dl, vtx0, pivot, rz2D);
            return;
        }
        const float rx = static_cast<float>(ModN(c, "RX")) * kDeg2Rad;
        const float ry = static_cast<float>(ModN(c, "RY")) * kDeg2Rad;
        const float rz = (static_cast<float>(ModN(c, "RZ")) + rz2D) * kDeg2Rad;
        float fov = static_cast<float>(ModB(c, "LP", false) ? ModN(c, "LPFOV", 55.0) : 55.0);
        fov = std::clamp(fov, 1.0f, 179.0f);
        // Focal length in canvas px (Flash derives it from stage width + FOV)
        const float focal = (640.0f * scale) / std::tan(fov * kDeg2Rad * 0.5f);
        const float cx = std::cos(rx), sxr = std::sin(rx);
        const float cy = std::cos(ry), syr = std::sin(ry);
        const float cz = std::cos(rz), szr = std::sin(rz);
        for (int i = vtx0; i < dl->VtxBuffer.Size; ++i) {
            ImDrawVert& v = dl->VtxBuffer[i];
            float x = v.pos.x - pivot.x, y = v.pos.y - pivot.y, z = 0.0f;
            { const float nx = x * cz - y * szr, ny = x * szr + y * cz; x = nx; y = ny; }
            { const float ny = y * cx - z * sxr, nz = y * sxr + z * cx; y = ny; z = nz; }
            { const float nx = x * cy + z * syr, nz = -x * syr + z * cy; x = nx; z = nz; }
            if (focal + z > 1.0f) {
                const float s = focal / (focal + z);
                x *= s;
                y *= s;
            }
            v.pos = ImVec2(pivot.x + x, pivot.y + y);
        }
    }

    // Aligned single-line text at a stage-scaled font size.
    // align: 0 left / 1 center / 2 right within [x0, x1].
    static void PrevText(ImDrawList* dl, float x0, float x1, float y, float fontPx,
                         ImU32 col, const std::string& txt, int align = 0) {
        fontPx = std::max(fontPx, 5.0f);
        ImFont* font = ImGui::GetFont();
        ImVec2 ts = font->CalcTextSizeA(fontPx, FLT_MAX, 0.0f, txt.c_str());
        float x = align == 1 ? (x0 + x1 - ts.x) * 0.5f : (align == 2 ? x1 - ts.x : x0);
        dl->AddText(font, fontPx, ImVec2(x, y), col, txt.c_str());
    }

    // Text sub-element with its mods applied (visibility, offsets, font size,
    // alignment, color). x0..x1 = alignment band, y = top edge.
    static void PrevModText(ImDrawList* dl, const WidgetConfig& c, const std::string& p,
                            float x0, float x1, float y, float scale, float alpha,
                            const std::string& txt, bool visVariant = false) {
        if (!ModB(c, p + (visVariant ? "Vis" : "V"), !visVariant)) return;
        const float sc = scale * static_cast<float>(c.scaleY);
        const float fs = static_cast<float>(ModN(c, p + "Size", 20.0)) * sc;
        const float ox = static_cast<float>(ModN(c, p + "OffX")) * sc;
        const float oy = static_cast<float>(ModN(c, p + "OffY")) * sc;
        const int vtx0 = dl->VtxBuffer.Size;
        PrevText(dl, x0 + ox, x1 + ox, y + oy, fs, ModCol(c, p + "Cl", COLOR_HUD, alpha),
                 txt, static_cast<int>(ModN(c, p + "Align", 0.0)));
        RotateVertsSince(dl, vtx0, ImVec2(x0 + ox, y + oy),
                         static_cast<float>(ModN(c, p + "Rot")));
    }

    // Meter bar honoring the bar option block (visibility, offsets, size,
    // slices, fill direction, colors, slice border). (x, yCenter) is the
    // bar's left edge / vertical center in canvas px before OX/OY.
    static void PrevModBar(ImDrawList* dl, const WidgetConfig& c, const std::string& p,
                           float x, float yCenter, float scale, float alpha, float pct) {
        if (!ModB(c, p + "V")) return;
        const int vtx0 = dl->VtxBuffer.Size;
        const float sx = scale * static_cast<float>(c.scaleX);
        const float sy = scale * static_cast<float>(c.scaleY);
        x += static_cast<float>(ModN(c, p + "OX")) * sx;
        yCenter += static_cast<float>(ModN(c, p + "OY")) * sy;
        const float w = std::max(2.0f, static_cast<float>(ModN(c, p + "Width", 200.0)) * sx);
        const float h = std::max(1.0f, static_cast<float>(ModN(c, p + "Height", 6.0)) * sy);
        const float y = yCenter - h * 0.5f;
        const int slices = std::max(1, static_cast<int>(ModN(c, p + "Slices", 1.0)));
        const float slicePct =
            std::clamp(static_cast<float>(ModN(c, p + "SlicesWidth", 75.0)) / 100.0f, 0.05f, 1.0f);
        const int dir2 = static_cast<int>(ModN(c, p + "Dir2", 0.0));
        const ImU32 lit = ModCol(c, p + "Cl", COLOR_HUD, alpha);
        const ImU32 dim = ModCol(c, p + "Cl", COLOR_HUD, alpha * 0.22f);

        if (slices <= 1) {
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), dim);
            const float fw = w * std::clamp(pct, 0.0f, 1.0f);
            const float fx = dir2 == 2 ? x + w - fw : (dir2 == 1 ? x + (w - fw) * 0.5f : x);
            dl->AddRectFilled(ImVec2(fx, y), ImVec2(fx + fw, y + h), lit);
        } else {
            const float cellW = w / static_cast<float>(slices);
            const float fillW = std::max(1.0f, cellW * slicePct);
            const int litCount =
                static_cast<int>(std::round(std::clamp(pct, 0.0f, 1.0f) * slices));
            for (int s = 0; s < slices; ++s) {
                // Fill order follows the align: 0 fills from the left,
                // 2 from the right, 1 from the center outwards.
                int rank = dir2 == 2 ? (slices - 1 - s)
                         : dir2 == 1 ? std::abs(s * 2 + 1 - slices) / 2
                                     : s;
                const bool on = rank < litCount;
                const float cx = x + s * cellW + (cellW - fillW) * 0.5f;
                dl->AddRectFilled(ImVec2(cx, y), ImVec2(cx + fillW, y + h), on ? lit : dim);
            }
        }
        // Slice border block (SB*, the <1.4.0 Brd* successor) as an outline
        if (ModB(c, p + "SV", false)) {
            const float pad = static_cast<float>(ModN(c, p + "SP", 3.0)) * sy;
            dl->AddRect(ImVec2(x - pad, y - pad), ImVec2(x + w + pad, y + h + pad),
                        ModCol(c, p + "SCl", COLOR_HUD, alpha * 0.9f), 0.0f, 0,
                        std::max(1.0f, static_cast<float>(ModN(c, p + "SBW", 3.0)) * sy * 0.5f));
        }
        // Element rotation (e.g. vertical HP bars use Rot = -90)
        RotateVertsSince(dl, vtx0, ImVec2(x, yCenter),
                         static_cast<float>(ModN(c, p + "Rot")));
    }

    // Bracket pair honoring the Brk* option block. BrkS true = "horizontal"
    // (lines across top and bottom), false = "[ ]" side brackets.
    static void PrevModBrackets(ImDrawList* dl, const WidgetConfig& c, const std::string& p,
                                ImVec2 mn, ImVec2 mx, float scale, float alpha) {
        if (!ModB(c, p + "BrkV")) return;
        const ImU32 col = ModCol(c, p + "BrkCl", COLOR_HUD, alpha);
        const float cap = 6.0f * scale;
        const float t = std::max(1.0f, 1.5f * scale);
        if (ModB(c, p + "BrkS", true)) {
            dl->AddLine(ImVec2(mn.x, mn.y), ImVec2(mx.x, mn.y), col, t);
            dl->AddLine(ImVec2(mn.x, mx.y), ImVec2(mx.x, mx.y), col, t);
        } else {
            dl->AddLine(ImVec2(mn.x, mn.y), ImVec2(mn.x, mx.y), col, t);
            dl->AddLine(ImVec2(mn.x, mn.y), ImVec2(mn.x + cap, mn.y), col, t);
            dl->AddLine(ImVec2(mn.x, mx.y), ImVec2(mn.x + cap, mx.y), col, t);
            dl->AddLine(ImVec2(mx.x, mn.y), ImVec2(mx.x, mx.y), col, t);
            dl->AddLine(ImVec2(mx.x - cap, mn.y), ImVec2(mx.x, mn.y), col, t);
            dl->AddLine(ImVec2(mx.x - cap, mx.y), ImVec2(mx.x, mx.y), col, t);
        }
    }

    // Vanilla 4-corner brackets (quick-loot header, rollover box, ...)
    static void PrevCornerBrackets(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col,
                                   float scale) {
        const float cap = std::min(8.0f * scale, (mx.x - mn.x) * 0.25f);
        const float t = std::max(1.0f, 1.5f * scale);
        dl->AddLine(ImVec2(mn.x, mn.y), ImVec2(mn.x + cap, mn.y), col, t);
        dl->AddLine(ImVec2(mn.x, mn.y), ImVec2(mn.x, mn.y + cap), col, t);
        dl->AddLine(ImVec2(mx.x - cap, mn.y), ImVec2(mx.x, mn.y), col, t);
        dl->AddLine(ImVec2(mx.x, mn.y), ImVec2(mx.x, mn.y + cap), col, t);
        dl->AddLine(ImVec2(mn.x, mx.y - cap), ImVec2(mn.x, mx.y), col, t);
        dl->AddLine(ImVec2(mn.x, mx.y), ImVec2(mn.x + cap, mx.y), col, t);
        dl->AddLine(ImVec2(mx.x, mx.y - cap), ImVec2(mx.x, mx.y), col, t);
        dl->AddLine(ImVec2(mx.x - cap, mx.y), ImVec2(mx.x, mx.y), col, t);
    }

    // Real HUDMenu.swf art for a widget, when available. Two placements:
    //   * fitBox=false — anchor-exact: the art keeps its Flash position
    //     relative to the widget's registration point (r.center).
    //   * fitBox=true  — aspect-fit into the given box (used when the art is
    //     a stand-in symbol whose registration doesn't match the widget).
    // Returns false when no art exists (caller draws primitives instead).
    static bool DrawWidgetArt(ImDrawList* dl, const std::string& name, ImVec2 anchor,
                              ImVec2 boxMin, ImVec2 boxMax, float scale,
                              const WidgetConfig& c, ImU32 tint, bool fitBox = false) {
        const FallUIHudArt::Art* art = FallUIHudArt::Get(name);
        if (!art || !art->tex) return false;
        ImVec2 mn, mx;
        if (fitBox) {
            const float bw = boxMax.x - boxMin.x, bh = boxMax.y - boxMin.y;
            const float s = std::min(bw / art->w, bh / art->h);
            const float w = art->w * s, h = art->h * s;
            mn = ImVec2(boxMin.x + (bw - w) * 0.5f, boxMin.y + (bh - h) * 0.5f);
            mx = ImVec2(mn.x + w, mn.y + h);
        } else {
            const float sx = scale * static_cast<float>(c.scaleX);
            const float sy = scale * static_cast<float>(c.scaleY);
            mn = ImVec2(anchor.x + art->originX * sx, anchor.y + art->originY * sy);
            mx = ImVec2(mn.x + art->w * sx, mn.y + art->h * sy);
        }
        dl->AddImage(art->tex, mn, mx, ImVec2(0, 0), ImVec2(1, 1), tint);
        return true;
    }

    // Vault Boy stand-in: head + shoulders outline, tinted like the original art.
    static void PrevVaultBoy(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col) {
        const float w = mx.x - mn.x, h = mx.y - mn.y;
        const ImVec2 head(mn.x + w * 0.5f, mn.y + h * 0.34f);
        const float r = std::min(w, h) * 0.20f;
        dl->AddCircle(head, r, col, 24, 2.0f);
        dl->PathArcTo(ImVec2(mn.x + w * 0.5f, mn.y + h * 0.95f), r * 1.9f,
                      3.14159265f, 2.0f * 3.14159265f, 16);
        dl->PathStroke(col, 0, 2.0f);
    }

    static void RenderWidgetPreview(int idx, const WidgetRect& r, ImDrawList* dl,
                                    float scale, float alpha) {
        const std::string name = kWidgets[idx].name;
        const WidgetConfig& c = s_ed.widgets[idx].cfg;
        const float x0 = r.boxMin.x, y0 = r.boxMin.y, x1 = r.boxMax.x, y1 = r.boxMax.y;
        const float w = x1 - x0, h = y1 - y0;
        const float sc = scale * static_cast<float>(c.scaleX);
        const ImU32 hud = RGBIntToImU32(GameHudColorRGB(), alpha);
        const ImU32 hudDim = RGBIntToImU32(GameHudColorRGB(), alpha * 0.45f);
        const ImU32 red = RGBIntToImU32(ResolveHudColor(COLOR_RED), alpha);
        const ImU32 white = IM_COL32(255, 255, 255, static_cast<int>(255 * alpha));

        if (name == "HPMeter_mc") {
            // Bracket underline (custom hideBracket bool + brCl color)
            if (!ModB(c, "hideBracket", false)) {
                dl->AddLine(ImVec2(x0, y1), ImVec2(x1, y1), ModCol(c, "brCl", COLOR_HUD, alpha),
                            std::max(1.0f, 1.5f * scale));
            }
            PrevModText(dl, c, "text", x0 + 2 * scale, x1, y0 + h * 0.10f, scale, alpha, "HP");
            PrevModBar(dl, c, "hpbar", x0 + 40 * sc, y0 + h * 0.55f, scale, alpha, 0.71f);
            PrevModBar(dl, c, "radbar", x0 + 40 * sc, y0 + h * 0.85f, scale, alpha, 0.35f);
            PrevModText(dl, c, "textRads", x0 + 2 * scale, x1, y0 + h * 0.60f, scale, alpha,
                        Tr("$RADS"), true);
        } else if (name == "ActionPointMeter_mc") {
            if (!ModB(c, "hideBracket", false)) {
                dl->AddLine(ImVec2(x0, y1), ImVec2(x1, y1), ModCol(c, "brCl", COLOR_HUD, alpha),
                            std::max(1.0f, 1.5f * scale));
            }
            PrevModText(dl, c, "text", x0, x1 - 2 * scale, y0 + h * 0.10f, scale, alpha, "AP");
            PrevModBar(dl, c, "apbar", x0 + 4 * sc, y0 + h * 0.55f, scale, alpha, 1.0f);
            // AP segment ticks under the bar
            if (ModB(c, "apseqsV")) {
                const ImU32 segCol = ModCol(c, "apseqsCl", COLOR_RED, alpha);
                for (int s = 0; s < 5; ++s) {
                    float tx = x0 + 4 * sc + (w - 44 * sc) * (s + 0.5f) / 5.0f;
                    dl->AddLine(ImVec2(tx, y1 - 4 * scale), ImVec2(tx, y1 - 1 * scale), segCol,
                                std::max(1.0f, scale));
                }
            }
        } else if (name == "RadsMeter_mc") {
            PrevModText(dl, c, "textNr", x0, x1 - 24 * sc, y0 + h * 0.15f, scale, alpha, "+1");
            PrevModText(dl, c, "textRADS", x0 + 30 * sc, x1 - 24 * sc, y0 + h * 0.15f, scale,
                        alpha, Tr("$RADS"));
            // Trefoil stand-in: circle + 3 wedge dots
            if (ModB(c, "iconV")) {
                const ImU32 icol = ModCol(c, "iconCl", COLOR_RED, alpha);
                const ImVec2 ic(x1 - 11 * sc, y0 + h * 0.5f);
                const float ir = std::min(10.0f * sc, h * 0.4f);
                dl->AddCircle(ic, ir, icol, 16, 1.5f);
                for (int k = 0; k < 3; ++k) {
                    const float a = -1.5708f + k * 2.0944f;
                    dl->AddCircleFilled(ImVec2(ic.x + std::cos(a) * ir * 0.5f,
                                               ic.y + std::sin(a) * ir * 0.5f),
                                        ir * 0.22f, icol);
                }
            }
        } else if (name == "LocationText_mc") {
            PrevModText(dl, c, "text", x0, x1, y0 + h * 0.2f, scale, alpha, Tr("$demo_region"));
        } else if (name == "XPMeter_mc") {
            PrevModText(dl, c, "textNr", x0, x1, y0, scale, alpha, "+1234");
            PrevModText(dl, c, "textXP", x0, x1, y0 + h * 0.42f, scale, alpha, "XP");
            PrevModBar(dl, c, "xpbar", x0 + 40 * sc, y1 - 5 * scale, scale, alpha, 0.35f);
        } else if (name == "EnemyHealthMeter_mc") {
            // Skull + legendary star flank the centered name; bar below
            PrevModText(dl, c, "text", x0, x1, y0 + h * 0.05f, scale, alpha,
                        Tr("$demo_raider"));
            if (ModB(c, "iconSkullV")) {
                const ImU32 icol = ModCol(c, "iconSkullCl", COLOR_NONE, alpha);
                const ImVec2 ic(x1 - 14 * sc, y0 + h * 0.3f);
                dl->AddCircle(ic, 5.0f * sc, icol, 12, 1.5f);
                dl->AddRectFilled(ImVec2(ic.x - 3 * sc, ic.y + 4 * sc),
                                  ImVec2(ic.x + 3 * sc, ic.y + 6 * sc), icol);
            }
            if (ModB(c, "iconLegV")) {
                const ImU32 icol = ModCol(c, "iconLegCl", COLOR_NONE, alpha);
                PrevText(dl, x0, x0 + 14 * sc, y0 + h * 0.2f, 12.0f * sc, icol, "*", 1);
            }
            PrevModBar(dl, c, "emybar", x0 + w * 0.1f, y1 - 4 * scale, scale, alpha, 0.66f);
        } else if (name == "StealthMeter_mc") {
            // Real backdrop art when available (dark rounded panel)
            DrawWidgetArt(dl, name, r.center, r.boxMin, r.boxMax, scale, c, white);
            const ImU32 bcol = hud;
            if (!ModB(c, "hideBrackets", false)) {
                const float bh = h * 0.8f, byc = y0 + h * 0.5f;
                dl->AddLine(ImVec2(x0, byc - bh / 2), ImVec2(x0, byc + bh / 2), bcol, 2.0f * scale);
                dl->AddLine(ImVec2(x1, byc - bh / 2), ImVec2(x1, byc + bh / 2), bcol, 2.0f * scale);
            }
            PrevModText(dl, c, "text", x0, x1, y0 + h * 0.18f, scale, alpha,
                        Tr("$Hidden").empty() ? "HIDDEN" : Tr("$Hidden"));
            PrevModText(dl, c, "textPerc", x0, x1, y0 + h * 0.68f, scale, alpha, "23%", true);
        } else if (name == "QuestUpdates_mc") {
            PrevModText(dl, c, "text1", x0 + 24 * sc, x1, y0 + h * 0.05f, scale, alpha,
                        Tr("$COMPLETED").rfind('$', 0) == 0 ? "COMPLETED" : Tr("$COMPLETED"));
            PrevModText(dl, c, "text2", x0 + 24 * sc, x1, y0 + h * 0.45f, scale, alpha,
                        Tr("$demo_quest_title"));
            dl->AddCircle(ImVec2(x0 + 10 * sc, y0 + h * 0.5f), 8.0f * sc, hud, 16, 1.5f);
        } else if (name == "ObjectiveUpdates_mc") {
            PrevModText(dl, c, "text", x0 + 14 * sc, x1, y0 + h * 0.15f, scale, alpha,
                        Tr("$ObjectiveUpdates_mc"));
            dl->AddRect(ImVec2(x0 + 2 * sc, y0 + h * 0.3f), ImVec2(x0 + 10 * sc, y0 + h * 0.7f),
                        hud, 0, 0, 1.5f);
        } else if (name == "Messages_mc") {
            PrevModBrackets(dl, c, "", r.boxMin, r.boxMax, scale, alpha);
            const int lines = std::max(1, static_cast<int>(ModN(c, "prevItems", 1.0)));
            const float lh = std::min(16.0f * scale, h / static_cast<float>(lines));
            for (int l = 0; l < lines; ++l) {
                const float ly = y0 + 2 * scale + l * lh;
                dl->AddRect(ImVec2(x0 + 2 * scale, ly), ImVec2(x0 + 2 * scale + lh * 0.7f, ly + lh * 0.7f),
                            hudDim, 0, 0, 1.0f);
                PrevText(dl, x0 + lh, x1, ly, lh * 0.7f, l == 0 ? hud : hudDim,
                         Tr("$demo_message_radio"));
            }
        } else if (name == "TutorialText_mc") {
            PrevModBrackets(dl, c, "", r.boxMin, r.boxMax, scale, alpha);
            if (ModB(c, "iconV")) {
                // Real Vault Boy head art from the SWF when available
                if (!DrawWidgetArt(dl, name, r.center, r.boxMin, r.boxMax, scale, c,
                                   ModCol(c, "iconCl", COLOR_NONE, alpha))) {
                    PrevVaultBoy(dl, ImVec2(x0 + 2 * scale, y0 + 2 * scale),
                                 ImVec2(x0 + std::min(34.0f * sc, w * 0.3f),
                                        y0 + std::min(40.0f * sc, h * 0.6f)),
                                 ModCol(c, "iconCl", COLOR_HUD, alpha));
                }
            }
            // Wrapped demo paragraph
            std::string txt = Tr("$demo_tutorial");
            for (size_t nl = txt.find("#newline#"); nl != std::string::npos;
                 nl = txt.find("#newline#")) {
                txt.replace(nl, 9, "\n");
            }
            const float fs = std::max(5.0f, static_cast<float>(ModN(c, "textSize", 19.0)) * scale *
                                                static_cast<float>(c.scaleY) * 0.8f);
            if (ModB(c, "textV")) {
                dl->AddText(ImGui::GetFont(), fs, ImVec2(x0 + w * 0.18f, y0 + 4 * scale),
                            ModCol(c, "textCl", COLOR_HUD, alpha), txt.c_str(), nullptr,
                            w * 0.8f);
            }
        } else if (name == "PromptMessageHolder_mc" || name == "_ext_promptMessageSwf") {
            PrevModText(dl, c, "text", x0 + 2 * scale, x1, y0 + h * 0.15f, scale, alpha,
                        "(!) " + Tr("$demo_message_radio"));
            if (name == "_ext_promptMessageSwf") {
                dl->AddRect(r.boxMin, r.boxMax, hudDim, 2.0f * scale, 0, 1.0f);
            }
        } else if (name == "CompassWidget_mc") {
            // Real strip backdrop when available; ticks/letters (code-placed
            // DirectionMarkerWidgets in Flash) are always drawn by us
            const bool hasArt =
                DrawWidgetArt(dl, name, r.center, r.boxMin, r.boxMax, scale, c, white);
            const float cy = y0 + h * 0.55f;
            if (!hasArt) {
                dl->AddLine(ImVec2(x0, cy + 6 * scale), ImVec2(x1, cy + 6 * scale), hud,
                            std::max(1.0f, 1.5f * scale));
            }
            const char* cards[] = { "W", "N", "E" };
            for (int t = 0; t <= 16; ++t) {
                const float tx = x0 + w * t / 16.0f;
                const bool major = (t % 4 == 0);
                dl->AddLine(ImVec2(tx, cy + (major ? -2 : 2) * scale), ImVec2(tx, cy + 5 * scale),
                            major ? hud : hudDim, 1.0f);
                if (t == 4 || t == 8 || t == 12) {
                    PrevText(dl, tx - 8 * scale, tx + 8 * scale, cy - 14 * scale, 12.0f * scale,
                             hud, cards[t / 4 - 1], 1);
                }
            }
            dl->AddTriangleFilled(ImVec2(x0 + w / 2 - 4 * scale, y1),
                                  ImVec2(x0 + w / 2 + 4 * scale, y1),
                                  ImVec2(x0 + w / 2, y1 - 5 * scale), hud);
        } else if (name == "HUDCrosshair_mc") {
            if (DrawWidgetArt(dl, name, r.center, r.boxMin, r.boxMax, scale, c, white)) {
                return;  // full art (the vanilla crosshair is white, untinted)
            }
            const ImVec2 ctr(x0 + w / 2, y0 + h / 2);
            const float g = 5.0f * sc, l = 9.0f * sc;
            dl->AddLine(ImVec2(ctr.x - g - l, ctr.y), ImVec2(ctr.x - g, ctr.y), white, 1.5f);
            dl->AddLine(ImVec2(ctr.x + g, ctr.y), ImVec2(ctr.x + g + l, ctr.y), white, 1.5f);
            dl->AddLine(ImVec2(ctr.x, ctr.y - g - l), ImVec2(ctr.x, ctr.y - g), white, 1.5f);
            dl->AddLine(ImVec2(ctr.x, ctr.y + g), ImVec2(ctr.x, ctr.y + g + l), white, 1.5f);
            dl->AddCircleFilled(ctr, 1.5f * sc, white);
        } else if (name == "HitIndicator_mc" || name == "DirectionalHitIndicatorBase_mc" ||
                   name == "ExplosiveIndicatorBase_mc") {
            const ImVec2 ctr(x0 + w / 2, y0 + h / 2);
            const float rad = std::min(w, h) * 0.42f;
            const ImU32 icol = name == "HitIndicator_mc" ? ModCol(c, "Cl", COLOR_HUD, alpha)
                                                         : ModCol(c, "Cl", COLOR_RED, alpha);
            if (DrawWidgetArt(dl, name, r.center, r.boxMin, r.boxMax, scale, c, icol)) {
                return;  // full art, tinted with the widget's color
            }
            for (int q = 0; q < 4; ++q) {
                const float a0 = q * 1.5708f + 0.35f, a1 = (q + 1) * 1.5708f - 0.35f;
                dl->PathArcTo(ctr, rad, a0, a1, 8);
                dl->PathStroke(icol, 0, 2.0f);
            }
            if (name == "ExplosiveIndicatorBase_mc") {
                PrevText(dl, x0, x1, ctr.y - 6 * scale, 12.0f * scale, icol, "!", 1);
            }
        } else if (name == "QuickContainerWidget_mc") {
            PrevCornerBrackets(dl, ImVec2(x0, y0), ImVec2(x1, y0 + h * 0.16f),
                               ModCol(c, "bphCl", COLOR_HUD, alpha), scale);
            PrevModText(dl, c, "text", x0, x1, y0 + h * 0.03f, scale, alpha,
                        Tr("$demo_quickloot_title"));
            for (int l = 0; l < 3; ++l) {
                const float ly = y0 + h * (0.24f + l * 0.14f);
                dl->AddRect(ImVec2(x0 + 4 * scale, ly), ImVec2(x0 + 12 * scale, ly + 8 * scale),
                            l == 0 ? hud : hudDim, 0, 0, 1.0f);
                PrevText(dl, x0 + 16 * scale, x1, ly - 1 * scale, 10.0f * scale,
                         l == 0 ? hud : hudDim,
                         Tr("$Item") + " " + std::to_string(l + 1));
            }
            dl->AddLine(ImVec2(x0 + 4 * scale, y1 - h * 0.12f), ImVec2(x1 - 4 * scale, y1 - h * 0.12f),
                        hudDim, 1.0f);
        } else if (name == "RolloverWidget_mc") {
            // Real chrome (bracket, star, magnifier) when available
            if (!DrawWidgetArt(dl, name, r.center, r.boxMin, r.boxMax, scale, c, hud)) {
                PrevCornerBrackets(dl, r.boxMin, r.boxMax, hud, scale);
            }
            PrevText(dl, x0, x1, y0 + h * 0.12f, 11.0f * scale, hud,
                     Tr("$demo_rollover_title"), 1);
            PrevText(dl, x0, x1, y0 + h * 0.45f, 10.0f * scale, hudDim,
                     Tr("$Item_subtitle"), 1);
        } else if (name == "SubtitleText_mc") {
            PrevModText(dl, c, "textSpeaker", x0, x1, y0, scale, alpha, "Danny Sullivan");
            if (ModB(c, "textV")) {
                const float fs = std::max(5.0f, static_cast<float>(ModN(c, "textSize", 21.0)) *
                                                    scale * static_cast<float>(c.scaleY) * 0.75f);
                std::string sub = Tr("$demo_subitle_text");
                if (size_t nb = sub.find("&nbsp;"); nb != std::string::npos) sub.replace(nb, 6, " ");
                dl->AddText(ImGui::GetFont(), fs, ImVec2(x0 + w * 0.05f, y0 + h * 0.3f),
                            ModCol(c, "textCl", COLOR_SUB, alpha), sub.c_str(), nullptr, w * 0.9f);
            }
        } else if (name == "AmmoCount_mc") {
            PrevModText(dl, c, "textClip", x0, x1 - 2 * scale, y0, scale, alpha, "015");
            if (ModB(c, "iconV")) {
                dl->AddLine(ImVec2(x0 + 4 * scale, y0 + h * 0.5f), ImVec2(x1 - 2 * scale, y0 + h * 0.5f),
                            ModCol(c, "iconCl", COLOR_HUD, alpha), std::max(1.0f, 1.5f * scale));
            }
            PrevModText(dl, c, "textTotal", x0, x1 - 2 * scale, y0 + h * 0.55f, scale, alpha, "216");
        } else if (name == "ExplosiveAmmoCount_mc") {
            if (ModB(c, "iconV")) {
                const ImU32 icol = ModCol(c, "iconCl", COLOR_HUD, alpha);
                // Real grenade art when available
                if (!DrawWidgetArt(dl, name, r.center, r.boxMin, r.boxMax, scale, c, icol)) {
                    const ImVec2 gc(x0 + 8 * sc, y0 + h * 0.55f);
                    dl->AddCircle(gc, 6.0f * sc, icol, 12, 1.5f);
                    dl->AddRectFilled(ImVec2(gc.x - 2 * sc, gc.y - 9 * sc),
                                      ImVec2(gc.x + 2 * sc, gc.y - 6 * sc), icol);
                }
            }
            PrevModText(dl, c, "text", x0, x1 - 2 * scale, y0 + h * 0.1f, scale, alpha, "08");
        } else if (name == "FlashLightWidget_mc") {
            const ImU32 icol = hud;
            dl->AddRect(ImVec2(x0 + w * 0.25f, y0 + h * 0.2f), ImVec2(x1 - w * 0.25f, y1 - h * 0.2f),
                        icol, 2.0f * scale, 0, 1.5f);
            dl->AddTriangleFilled(ImVec2(x0 + w * 0.4f, y0 + h * 0.35f),
                                  ImVec2(x0 + w * 0.4f, y1 - h * 0.35f),
                                  ImVec2(x1 - w * 0.32f, y0 + h * 0.5f),
                                  RGBIntToImU32(GameHudColorRGB(), alpha * 0.7f));
        } else if (name == "FatigueWarning_mc") {
            PrevModText(dl, c, "text", x0, x1, y0 + h * 0.1f, scale, alpha, "+Fatigue");
        } else if (name == "PowerArmorLowBatteryWarning_mc") {
            PrevModText(dl, c, "text", x0, x1 - 2 * scale, y0 + h * 0.1f, scale, alpha,
                        Tr("$demo_pa_battery_low"));
        } else if (name == "CritMeter_mc") {
            // Diamond-tipped meter with centered label
            const float cy = y0 + h * 0.5f;
            dl->AddRect(ImVec2(x0 + w * 0.15f, cy - 3 * scale), ImVec2(x1 - w * 0.15f, cy + 3 * scale),
                        hudDim, 0, 0, 1.0f);
            dl->AddRectFilled(ImVec2(x0 + w * 0.15f, cy - 3 * scale),
                              ImVec2(x0 + w * 0.5f, cy + 3 * scale), hud);
            const ImVec2 dc(x0 + w * 0.5f, cy);
            dl->AddQuadFilled(ImVec2(dc.x, dc.y - 6 * scale), ImVec2(dc.x + 4 * scale, dc.y),
                              ImVec2(dc.x, dc.y + 6 * scale), ImVec2(dc.x - 4 * scale, dc.y), hud);
            PrevModText(dl, c, "text", x0, x1 - w * 0.16f, y0, scale, alpha, "CRIT");
        } else if (name == "HUDActiveEffectsWidget_mc") {
            for (int e = 0; e < 2; ++e) {
                const float ex = x1 - (e + 1) * 16.0f * sc;
                dl->AddRect(ImVec2(ex, y0 + h * 0.2f), ImVec2(ex + 12 * sc, y0 + h * 0.2f + 12 * sc),
                            e == 0 ? hud : hudDim, 0, 0, 1.5f);
            }
        } else if (name == "PerkVaultBoy_mc" || name == "QuestVaultBoy_mc" ||
                   name == "VaultBoyCondition_mc") {
            // Real Vault Boy head art (stand-in symbol, aspect-fit into the box)
            if (!DrawWidgetArt(dl, name, r.center, r.boxMin, r.boxMax, scale, c, white,
                               /*fitBox=*/true)) {
                PrevVaultBoy(dl, ImVec2(x0 + w * 0.2f, y0 + h * 0.05f),
                             ImVec2(x1 - w * 0.2f, y1 - h * 0.15f), hud);
            }
        }
    }

    static void RenderCanvas() {
        // Fit a 16:9 canvas into the available width
        float availW = ImGui::GetContentRegionAvail().x;
        availW = std::max(availW, 320.0f);
        const float canvasW = availW;
        const float canvasH = canvasW * 720.0f / 1280.0f;
        float scale = canvasW / 1280.0f;

        // Quadrant zoom (eventMapClick port: 2x into a quadrant)
        ImVec2 pan(0, 0);
        if (s_ed.zoomQuadrant >= 0) {
            scale *= 2.0f;
            if (s_ed.zoomQuadrant == 1 || s_ed.zoomQuadrant == 3) pan.x = -640.0f;
            if (s_ed.zoomQuadrant == 2 || s_ed.zoomQuadrant == 3) pan.y = -360.0f;
        }

        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Remember the canvas rect for the floating panels
        s_ed.canvasMin = origin;
        s_ed.canvasMax = ImVec2(origin.x + canvasW, origin.y + canvasH);

        // Panel background (previewPanelBgAlpha) + green border
        const float bgAlpha = 0.8f * (GetGlobalSetting("previewPanelBgAlpha").AsInt() / 100.0f + 0.25f);
        dl->AddRectFilled(origin, ImVec2(origin.x + canvasW, origin.y + canvasH),
                          IM_COL32(0, 0, 0, static_cast<int>(std::clamp(bgAlpha, 0.15f, 1.0f) * 255)));
        dl->AddRect(origin, ImVec2(origin.x + canvasW, origin.y + canvasH),
                    MixRGB(kFo4Green, 0xFFFFFF, 0.0f, 0.5f), 0.0f, 0, 2.0f);

        // Invisible button claims the interaction area
        ImGui::InvisibleButton("##hudcanvas", ImVec2(canvasW, canvasH));
        const bool canvasHovered = ImGui::IsItemHovered();
        ImVec2 mouse = ImGui::GetIO().MousePos;

        dl->PushClipRect(origin, ImVec2(origin.x + canvasW, origin.y + canvasH), true);

        const bool interactive = !s_ed.easyMode;
        const bool showBg = GetGlobalSetting("showWidgetBg").AsBool();
        const bool showMeasure = GetGlobalSetting("showMeasurement").AsBool();
        const bool showAnchor = GetGlobalSetting("previewShowAnchor").AsBool();

        int hovered = -1;
        // Iterate topmost-last so later widgets get hover priority
        for (int i = kWidgetCount - 1; i >= 0; --i) {
            if (!WidgetPassesFilter(i)) continue;
            WidgetRect r = ComputeWidgetRect(i, origin, scale, pan);
            if (canvasHovered && hovered == -1 && s_dragging == -1 &&
                mouse.x >= r.boxMin.x && mouse.x <= r.boxMax.x &&
                mouse.y >= r.boxMin.y && mouse.y <= r.boxMax.y) {
                hovered = i;
            }
        }
        if (s_dragging >= 0) hovered = s_dragging;

        for (int i = 0; i < kWidgetCount; ++i) {
            if (!WidgetPassesFilter(i)) continue;
            const WidgetState& st = s_ed.widgets[i];
            WidgetRect r = ComputeWidgetRect(i, origin, scale, pan);

            const bool isHovered = (hovered == i) && interactive;
            const bool isSelected = (s_ed.selected == i);
            const float itemAlpha = st.cfg.visible ? 1.0f : 0.33f;

            // Widget box fill (HudItem bg): green mix when showWidgetBg,
            // else faint white outline fill
            // Box fill + art are captured together and then rotated/3D-
            // transformed around the anchor, like the live Flash objects.
            const int vtx0 = dl->VtxBuffer.Size;
            if (!s_ed.easyMode) {
                if (showBg) {
                    dl->AddRectFilled(r.boxMin, r.boxMax,
                                      MixRGB(kFo4Green, 0xFFFFFF, 0.25f,
                                             (isHovered ? 0.66f : 0.33f) * itemAlpha));
                } else {
                    dl->AddRectFilled(r.boxMin, r.boxMax,
                                      IM_COL32(255, 255, 255,
                                               static_cast<int>((isHovered ? 30 : 12) * itemAlpha)));
                    dl->AddRect(r.boxMin, r.boxMax,
                                IM_COL32(255, 255, 255,
                                         static_cast<int>((isHovered ? 168 : 84) * itemAlpha)));
                }
            }

            // Widget art recreation, driven by the current modifier values
            RenderWidgetPreview(i, r, dl, scale, itemAlpha);
            TransformPreviewVerts(dl, vtx0, r.center, st.cfg, scale);

            // Hover/selection border stays axis-aligned — it marks the hit
            // box (0x6666FF-ish blue, like the original 6711039)
            if (!s_ed.easyMode && (isHovered || isSelected)) {
                dl->AddRect(ImVec2(r.boxMin.x - 3, r.boxMin.y - 3),
                            ImVec2(r.boxMax.x + 3, r.boxMax.y + 3),
                            RGBIntToImU32(6711039, isSelected ? 1.0f : 0.8f), 0.0f, 0, 3.0f);
            }

            // Title label while hovered/selected, clamped inside the canvas
            // so it never gets cut off at the edges
            if (!s_ed.easyMode && (isHovered || isSelected)) {
                const std::string& title = st.title;
                const ImVec2 ts = ImGui::CalcTextSize(title.c_str());
                float lx = std::clamp(r.boxMin.x, origin.x + 2.0f,
                                      std::max(origin.x + 2.0f, origin.x + canvasW - ts.x - 2.0f));
                float ly = r.boxMin.y - ts.y - 4.0f;
                if (ly < origin.y + 2.0f) {
                    ly = std::min(r.boxMax.y + 4.0f, origin.y + canvasH - ts.y - 2.0f);
                }
                dl->AddText(ImVec2(lx, ly), RGBIntToImU32(kFallUIHudBlue, itemAlpha),
                            title.c_str());
            }

            // Anchor marker
            if (showAnchor) {
                dl->AddCircleFilled(r.center, 3.0f, RGBIntToImU32(kFallUIHudBlue));
            }

            // Measurement (distance to stage edges), clamped like the title
            if (showMeasure && isHovered) {
                char mbuf[64];
                snprintf(mbuf, sizeof(mbuf), "x %d  y %d",
                         static_cast<int>(st.cfg.x), static_cast<int>(st.cfg.y));
                const ImVec2 ts = ImGui::CalcTextSize(mbuf);
                float lx = std::clamp(r.boxMin.x, origin.x + 2.0f,
                                      std::max(origin.x + 2.0f, origin.x + canvasW - ts.x - 2.0f));
                float ly = std::min(r.boxMax.y + 2.0f, origin.y + canvasH - ts.y - 2.0f);
                dl->AddText(ImVec2(lx, ly), RGBIntToImU32(kFallUIHudBlue), mbuf);
            }
        }
        dl->PopClipRect();

        if (!interactive) {
            if (s_ed.easyModeLayout == "DEF_HUD.xml") {
                const char* note = "DEF_HUD.xml";
                ImVec2 ts = ImGui::CalcTextSize(note);
                dl->AddText(ImVec2(origin.x + (canvasW - ts.x) / 2, origin.y + (canvasH - ts.y) / 2),
                            MixRGB(kFo4Green, 0xFFFFFF, 0.2f), note);
            }
            return;
        }

        // --- Interactions ---
        ImGuiIO& io = ImGui::GetIO();

        // Drag start
        if (hovered >= 0 && s_dragging == -1 && canvasHovered &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (!(s_ed.selectionLocked && s_ed.selected != hovered)) {
                s_dragging = hovered;
                WidgetRect r = ComputeWidgetRect(hovered, origin, scale, pan);
                s_dragOffset = ImVec2(mouse.x - r.center.x, mouse.y - r.center.y);
            }
        }

        // Drag update
        if (s_dragging >= 0) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const WidgetDef& w = kWidgets[s_dragging];
                const GroupDef* g = FindGroup(w.group);
                // New anchor position in stage space
                float sxPix = (mouse.x - s_dragOffset.x - origin.x) / scale - pan.x;
                float syPix = (mouse.y - s_dragOffset.y - origin.y) / scale - pan.y;
                WidgetConfig& cfg = s_ed.widgets[s_dragging].cfg;
                cfg.x = std::round((sxPix - g->x) * 1920.0 / 1280.0);
                cfg.y = std::round((syPix - g->y) * 1080.0 / 720.0);
            } else {
                // Drop: persist + select (dragStop port)
                SaveWidget(s_dragging);
                s_ed.selected = s_dragging;
                s_dragging = -1;
            }
        }

        // Click (no drag movement) selects too — dragStop already selects.

        // Mouse wheel: scale (or rotate with CTRL) the hovered widget
        if (hovered >= 0 && canvasHovered && io.MouseWheel != 0.0f) {
            WidgetConfig& cfg = s_ed.widgets[hovered].cfg;
            if (io.KeyCtrl) {
                double stepR = (io.MouseWheel > 0 ? 1 : -1) * (io.KeyShift ? 10.0 : 1.0);
                cfg.rotation += stepR;
                if (cfg.rotation > 180) cfg.rotation -= 360;
                if (cfg.rotation < -180) cfg.rotation += 360;
            } else if (io.MouseWheel > 0) {
                cfg.scaleX += 0.1;
                cfg.scaleY += 0.1;
            } else {
                cfg.scaleX = std::max(0.1, cfg.scaleX - 0.1);
                cfg.scaleY = std::max(0.1, cfg.scaleY - 0.1);
            }
            SaveWidget(hovered);
            s_ed.selected = hovered;
        }
    }

    // ------------------------------------------------------------------
    // Panels
    // ------------------------------------------------------------------

    // Pale-green "paper" styling shared by the floating panels — matches the
    // original editor's light panel with dark-green text and green accents.
    static int PushPaperStyle() {
        const ImVec4 paper(0.84f, 0.94f, 0.80f, 0.97f);
        const ImVec4 paperDark(0.73f, 0.88f, 0.69f, 1.0f);
        const ImVec4 ink(0.06f, 0.27f, 0.09f, 1.0f);
        const ImVec4 grab(0.14f, 0.44f, 0.17f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, paper);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, paper);
        ImGui::PushStyleColor(ImGuiCol_Text, ink);
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.36f, 0.52f, 0.38f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, grab);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, paperDark);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.67f, 0.85f, 0.63f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.61f, 0.82f, 0.57f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, grab);
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ink);
        ImGui::PushStyleColor(ImGuiCol_Button, paperDark);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.84f, 0.61f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.57f, 0.79f, 0.54f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ink);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, paperDark);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, grab);
        ImGui::PushStyleColor(ImGuiCol_Separator, grab);
        return 18;
    }

    static constexpr ImGuiWindowFlags kPanelFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_AlwaysAutoResize;

    // Keep a floating panel above the host framework window every frame.
    // NoFocusOnAppearing means a re-opened panel would otherwise stay BELOW
    // the focused main window and become unclickable (seen with the import
    // overlay after the first import). Skips when a modal is open so the
    // message box stays on top.
    static void KeepPanelOnTop() {
        if (ImGui::GetTopMostPopupModal() == nullptr) {
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        }
    }

    // The floating widget edit panel, top-right of the canvas like the
    // original HudItemEditPanel: title + anchor readout, scale/rotation
    // sliders, nudge arrows, Show/Hide + Reset, then Advanced options.
    static void RenderEditPanel() {
        if (s_ed.selected < 0 || s_ed.easyMode) return;
        const int idx = s_ed.selected;
        const WidgetDef& w = kWidgets[idx];
        WidgetState& st = s_ed.widgets[idx];
        WidgetConfig& cfg = st.cfg;
        bool changed = false;

        const int pushed = PushPaperStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);

        const float panelW = 380.0f;
        const float maxH = std::max(240.0f, s_ed.canvasMax.y - s_ed.canvasMin.y - 12.0f);
        ImGui::SetNextWindowPos(ImVec2(s_ed.canvasMax.x - 6.0f, s_ed.canvasMin.y + 6.0f),
                                ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(panelW, 0), ImVec2(panelW, maxH));
        ImGui::Begin("##fuieditpanel", nullptr, kPanelFlags);
        KeepPanelOnTop();

        // Header: bold-ish title + lock + close
        ImGui::TextUnformatted(st.title.c_str());
        ImGui::SameLine(panelW - 96.0f);
        bool lock = s_ed.selectionLocked;
        if (ImGui::Checkbox("##lock", &lock)) s_ed.selectionLocked = lock;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lock selection");
        ImGui::SameLine();
        if (ImGui::SmallButton("X##close")) {
            s_ed.selected = -1;
            s_ed.selectionLocked = false;
            ImGui::End();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(pushed);
            return;
        }
        {
            float ax, ay;
            DefaultAnchor(w, ax, ay);
            ImGui::Text("X: %d  Y: %d   (%s: %d%% x %d%%)", static_cast<int>(cfg.x),
                        static_cast<int>(cfg.y), Tr("$Anchored_at").c_str(),
                        static_cast<int>(ax), static_cast<int>(ay));
        }
        ImGui::Spacing();

        // Two columns: sliders/buttons left, nudge arrows right
        if (ImGui::BeginTable("##edittop", 2, ImGuiTableFlags_None)) {
            ImGui::TableSetupColumn("main", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("arrows", ImGuiTableColumnFlags_WidthFixed, 84.0f);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);

            {
                float sc = static_cast<float>(cfg.scaleX) * 100.0f;
                ImGui::TextUnformatted((Tr("$Scale") + " (%, " + Tr("$Mousewheel") + ")").c_str());
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderFloat("##scale", &sc, 10.0f, 300.0f, "%.0f")) {
                    cfg.scaleX = cfg.scaleY = sc / 100.0;
                    changed = true;
                }
            }
            {
                float rot = static_cast<float>(cfg.rotation);
                ImGui::TextUnformatted((Tr("$Rotation") + " (\xC2\xB0, " + Tr("$CTRL") + "+" +
                                        Tr("$Mousewheel") + ")").c_str());
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderFloat("##rot", &rot, -180.0f, 180.0f, "%.0f")) {
                    cfg.rotation = rot;
                    changed = true;
                }
            }
            ImGui::Spacing();
            {
                const bool hidden = !cfg.visible;
                if (hidden) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.60f, 0.70f, 0.56f, 1.0f));
                if (ImGui::Button((Tr("$Show_Hide") + "##vis").c_str(), ImVec2(120, 0))) {
                    cfg.visible = !cfg.visible;
                    changed = true;
                }
                if (hidden) ImGui::PopStyleColor();
                ImGui::SameLine();
                if (ImGui::Button((Tr("$Reset") + "##reset").c_str(), ImVec2(100, 0))) {
                    ApplyVanilla(idx);
                    changed = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("$Reset_to_vanilla").c_str());
            }

            // Nudge arrows (1 px, 10 px with SHIFT — like the original's)
            ImGui::TableSetColumnIndex(1);
            {
                const float step = ImGui::GetIO().KeyShift ? 10.0f : 1.0f;
                const float bs = ImGui::GetFrameHeight();
                ImGui::Dummy(ImVec2(bs, 0));
                ImGui::SameLine();
                if (ImGui::ArrowButton("##nup", ImGuiDir_Up)) { cfg.y -= step; changed = true; }
                if (ImGui::ArrowButton("##nleft", ImGuiDir_Left)) { cfg.x -= step; changed = true; }
                ImGui::SameLine();
                ImGui::Dummy(ImVec2(bs * 0.15f, 0));
                ImGui::SameLine();
                if (ImGui::ArrowButton("##nright", ImGuiDir_Right)) { cfg.x += step; changed = true; }
                ImGui::Dummy(ImVec2(bs, 0));
                ImGui::SameLine();
                if (ImGui::ArrowButton("##ndown", ImGuiDir_Down)) { cfg.y += step; changed = true; }
            }
            ImGui::EndTable();
        }

        // Advanced options block with its own scroll area (original's second panel)
        ImGui::SeparatorText(Tr("$Advanced_options").c_str());
        const float advH = std::clamp(maxH - ImGui::GetCursorPosY() - 12.0f, 120.0f, 460.0f);
        ImGui::BeginChild("##fuiadv", ImVec2(panelW - 18.0f, advH), true);
        if (const OptionSet* set = OptionsFor(ParentIdent(w))) {
            if (RenderOptionSet("edit|" + ParentIdent(w), *set, cfg.mods, true)) {
                changed = true;
            }
        }
        ImGui::EndChild();

        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(pushed);

        if (changed) SaveWidget(idx);
    }

    // Global "Layout settings" panel (drawGlobalSettingsPanel port)
    static void RenderGlobalSettingsPanel() {
        if (!s_ed.showGlobalSettings) return;

        // Build the option set (mirrors the original's inline table)
        static OptionSet set;
        static bool built = false;
        if (!built) {
            built = true;
            OptionBuilder b;
            double pos = 1;
            set.push_back(b.MakeTitle("previewTitle", ++pos, "${Preview}", Option::Type::UiTitle));
            set.push_back(b.MakeInt("previewPanelBgAlpha", ++pos, "${Background} - ${Alpha}", 50, 0, 100));
            set.push_back(b.MakeBool("showWidgetBg", ++pos, "${Show_widget_box_background}", false));
            set.push_back(b.MakeBool("showMeasurement", ++pos, "$Show_measurement", false));
            set.push_back(b.MakeBool("previewShowAnchor", ++pos, "${Show_anchor_position}", false));
            set.push_back(b.MakeBool("showAdvancedOptions", ++pos, "${Show_advanced_options}", false));
            set.push_back(b.MakeTitle("layoutDefaultsTitle", ++pos, "${Layout} - ${Default}",
                                      Option::Type::UiTitle));
            set.push_back(b.MakeInt("shadowBgAlpha", ++pos, "${Shadow} - ${Alpha}", 50, 0, 100));
            {
                Option o = b.MakeInt("fontType", ++pos, "${Font}", 0, 0, kFontCount - 1);
                o.checkType = "font";
                set.push_back(std::move(o));
            }
            set.push_back(b.MakeBool("fixInterfaceVisibility", ++pos, "${Fix}: ${Interface_visibility}", false));
            set.push_back(b.MakeTitle("colorsTitle", ++pos, "${Layout} - ${Colors}", Option::Type::UiTitle));
            for (int i = 1; i <= 4; ++i) {
                set.push_back(b.MakeColor("ColorSlot" + std::to_string(i), ++pos,
                                          "${Color_slot} " + std::to_string(i), COLOR_NONE));
            }
        }

        const int pushed = PushPaperStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
        ImGui::SetNextWindowPos(ImVec2((s_ed.canvasMin.x + s_ed.canvasMax.x) * 0.5f,
                                       s_ed.canvasMin.y + 24.0f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(420.0f, 0),
            ImVec2(420.0f, std::max(240.0f, s_ed.canvasMax.y - s_ed.canvasMin.y - 48.0f)));
        ImGui::Begin("##fuiglobalsettings", nullptr, kPanelFlags);
        KeepPanelOnTop();

        ImGui::PushID("globalsettings");
        ImGui::SeparatorText(Tr("$Layout_settings").c_str());

        // Merge editor settings + layout settings into one working map
        PackedObject merged = s_ed.globalLayoutSettings;
        for (const auto& [k, v] : s_ed.globalSettings) {
            if (!merged.count(k)) merged[k] = v;
        }
        for (const auto& [k, v] : kDefaultGlobalSettings) {
            if (!merged.count(k)) merged[k] = v;
        }

        if (RenderOptionSet("global", set, merged, false)) {
            // Route each key to the right store (eventSaveGlobalLayoutOption)
            for (const auto& [k, v] : merged) {
                const bool isEditorSetting = kDefaultGlobalSettings.count(k) > 0;
                if (isEditorSetting) {
                    s_ed.globalSettings[k] = v;
                } else {
                    s_ed.globalLayoutSettings[k] = v;
                }
            }
            SaveGlobalSettings();
            SaveGlobalLayoutSettings();
        }
        if (ImGui::Button((Tr("$CLOSE") + "##globalclose").c_str())) {
            s_ed.showGlobalSettings = false;
        }
        ImGui::PopID();

        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(pushed);
    }

    // Import overlay (ImportOverlay port; DEF_HUD.xml conversion not supported)
    static void RenderImportOverlay() {
        if (!s_ed.showImportOverlay) return;
        const int pushed = PushPaperStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
        ImGui::SetNextWindowPos(ImVec2((s_ed.canvasMin.x + s_ed.canvasMax.x) * 0.5f,
                                       (s_ed.canvasMin.y + s_ed.canvasMax.y) * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(320.0f, 0),
            ImVec2(FLT_MAX, std::max(240.0f, s_ed.canvasMax.y - s_ed.canvasMin.y - 48.0f)));
        ImGui::Begin("##fuiimport", nullptr, kPanelFlags);
        KeepPanelOnTop();
        ImGui::PushID("importoverlay");
        ImGui::SeparatorText(Tr("$Import_layout").c_str());
        for (const auto& name : s_ed.importableLayouts) {
            if (ImGui::Button((name + "##imp").c_str(), ImVec2(300, 0))) {
                auto map = ReadLayoutIniFile(ImportableLayoutsDir() / (name + ".ini"));
                if (map.empty()) {
                    ShowMessage(Tr("$Error"), Tr("$ImportIniFailedMsg"));
                } else {
                    LoadLayoutMap(std::move(map), true);
                    ShowMessage(Tr("$Import"), Tr("$ImportSuccessMsg"));
                }
                s_ed.showImportOverlay = false;
            }
        }
        if (s_ed.defHudXmlExists) {
            ImGui::BeginDisabled();
            ImGui::Button("DEF_HUD.xml##imp", ImVec2(300, 0));
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("DEF_HUD.xml conversion requires the original FallUI HUD editor.\n"
                                  "Use Easy Mode's DEF_HUD.xml option to load it at runtime instead.");
            }
        }
        if (ImGui::Button((Tr("$Cancel") + "##impcancel").c_str(), ImVec2(300, 0))) {
            s_ed.showImportOverlay = false;
        }
        ImGui::PopID();
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(pushed);
    }

    // Widgets list (buildWidgetsList port)
    static void RenderWidgetsList() {
        if (!s_ed.showWidgetsList || s_ed.easyMode) return;
        const int pushed = PushPaperStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
        ImGui::SetNextWindowPos(ImVec2(s_ed.canvasMin.x + 6.0f, s_ed.canvasMin.y + 6.0f),
                                ImGuiCond_Always, ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(430.0f, 0),
            ImVec2(430.0f, std::max(240.0f, s_ed.canvasMax.y - s_ed.canvasMin.y - 12.0f)));
        ImGui::Begin("##fuiwidgetslist", nullptr, kPanelFlags);
        KeepPanelOnTop();
        ImGui::PushID("widgetslist");
        ImGui::SeparatorText(Tr("$Widgets_list").c_str());

        struct Filter { const char* name; std::vector<const char*> widgets; };
        static const std::vector<Filter> kFilters = {
            { "$Notifications", { "HUDCrosshair_mc", "Messages_mc", "TutorialText_mc",
                                  "PowerArmorLowBatteryWarning_mc", "LocationText_mc", "XPMeter_mc",
                                  "FlashLightWidget_mc", "FatigueWarning_mc",
                                  "HUDActiveEffectsWidget_mc", "PerkVaultBoy_mc",
                                  "_ext_promptMessageSwf" } },
            { "$Quests", { "ObjectiveUpdates_mc", "QuestUpdates_mc", "QuestVaultBoy_mc",
                           "SubtitleText_mc" } },
            { "$Exploration", { "CompassWidget_mc", "RolloverWidget_mc",
                                "QuickContainerWidget_mc", "RadsMeter_mc" } },
            { "$Assault", { "StealthMeter_mc", "ExplosiveIndicatorBase_mc", "HitIndicator_mc",
                            "DirectionalHitIndicatorBase_mc", "AmmoCount_mc",
                            "ExplosiveAmmoCount_mc", "EnemyHealthMeter_mc", "VaultBoyCondition_mc",
                            "HPMeter_mc", "ActionPointMeter_mc", "CritMeter_mc" } },
        };

        int columns = 2;
        ImGui::Columns(columns, nullptr, false);
        for (const auto& f : kFilters) {
            ImGui::TextColored(ImVec4(0.10f, 0.38f, 0.14f, 1.0f), "%s", Tr(f.name).c_str());
            // Sort by translated title like the original
            std::vector<int> idxs;
            for (const char* wn : f.widgets) {
                for (int i = 0; i < kWidgetCount; ++i) {
                    if (std::string(kWidgets[i].name) == wn) { idxs.push_back(i); break; }
                }
            }
            std::sort(idxs.begin(), idxs.end(), [](int a, int bIdx) {
                return s_ed.widgets[a].title < s_ed.widgets[bIdx].title;
            });
            for (int i : idxs) {
                const bool active = s_ed.selected == i && s_ed.selectionLocked;
                if (active) ImGui::PushStyleColor(ImGuiCol_Button, MixRGB(kFo4Green, 0, 0.3f));
                if (ImGui::Button((s_ed.widgets[i].title + "##wl" + kWidgets[i].name).c_str(),
                                  ImVec2(-1, 0))) {
                    if (active) {
                        s_ed.selectionLocked = false;
                    } else {
                        s_ed.selected = i;
                        s_ed.selectionLocked = true;
                    }
                }
                if (active) ImGui::PopStyleColor();
            }
            ImGui::NextColumn();
        }
        ImGui::Columns(1);
        ImGui::PopID();
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(pushed);
    }

    // ------------------------------------------------------------------
    // HUD layout editor top-level render
    // ------------------------------------------------------------------
    // Green section header bar in the left sidebar ("Profiles", "Tools",
    // "Zoom") — mirrors the original's dark-green header strips.
    static void SidebarHeader(const std::string& label) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float h = ImGui::GetTextLineHeight() + 6.0f;
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), MixRGB(kFo4Green, 0, 0.55f));
        dl->AddText(ImVec2(p.x + 6.0f, p.y + 3.0f), IM_COL32(220, 255, 220, 255), label.c_str());
        ImGui::Dummy(ImVec2(w, h + 2.0f));
    }

    // 2x2 quadrant mini-map for the Zoom section (eventMapClick port).
    // Clicking a quadrant zooms 2x into it; clicking the active quadrant
    // (or the outer border) returns to the fitted full view.
    static void SidebarZoomMap() {
        const float w = ImGui::GetContentRegionAvail().x;
        const float h = w * 720.0f / 1280.0f;
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGui::InvisibleButton("##zoommap", ImVec2(w, h));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        ImVec2 mouse = ImGui::GetIO().MousePos;

        for (int z = 0; z < 4; ++z) {
            const float x0 = p.x + (z % 2) * w / 2.0f;
            const float y0 = p.y + (z / 2) * h / 2.0f;
            ImVec2 a(x0 + 1, y0 + 1), b(x0 + w / 2.0f - 1, y0 + h / 2.0f - 1);
            const bool inside = hovered && mouse.x >= a.x && mouse.x < b.x &&
                                mouse.y >= a.y && mouse.y < b.y;
            const bool active = s_ed.zoomQuadrant == z;
            ImU32 fill = active ? MixRGB(kFo4Green, 0, 0.45f)
                        : inside ? MixRGB(kFo4Green, 0, 0.75f)
                                 : IM_COL32(0, 0, 0, 120);
            dl->AddRectFilled(a, b, fill);
            dl->AddRect(a, b, MixRGB(kFo4Green, 0xFFFFFF, 0.1f, 0.8f));
            if (inside && clicked) {
                s_ed.zoomQuadrant = active ? -1 : z;  // toggle back to fit
            }
        }
    }

    static void RenderHudEditor() {
        if (!s_ed.loaded) LoadSession();

        ImGui::PushID("falluihudeditor");

        // Layout mirrors the original editor: a fixed left sidebar with the
        // Profiles / Tools / Zoom sections (or the Layouts list in easy mode)
        // and the canvas + widgets/filter bar filling the rest.
        const float sidebarW = 180.0f;
        ImGui::BeginChild("##fuisidebar", ImVec2(sidebarW, 0), false);
        if (!s_ed.easyMode) {
            SidebarHeader(Tr("$Profiles"));
            for (int i = 1; i <= 3; ++i) {
                const bool active = s_ed.currentProfile == i;
                if (active) ImGui::PushStyleColor(ImGuiCol_Button, MixRGB(kFo4Green, 0, 0.3f));
                std::string label = Tr("$Profile") + " " + std::to_string(i) + "##prof";
                if (ImGui::Button(label.c_str(), ImVec2(-1, 0))) {
                    SwitchProfile(i);
                }
                if (active) ImGui::PopStyleColor();
            }

            ImGui::Spacing();
            SidebarHeader(Tr("$Tools"));
            if (ImGui::Button((Tr("$Layout_settings") + "##tools").c_str(), ImVec2(-1, 0))) {
                s_ed.showGlobalSettings = !s_ed.showGlobalSettings;
            }
            if (ImGui::Button((Tr("$Revert_last_changes") + "##tools").c_str(), ImVec2(-1, 0))) {
                // resetToStartData for every widget (current profile)
                for (int i = 0; i < kWidgetCount; ++i) {
                    auto& st = s_ed.widgets[i];
                    auto it = st.startData.find(s_ed.currentProfile);
                    if (it == st.startData.end()) it = st.startData.find(0);
                    if (it != st.startData.end()) {
                        ApplySavedString(i, it->second, 0);
                    } else {
                        ApplyVanilla(i);
                    }
                    SaveWidget(i);
                }
            }
            if (ImGui::Button((Tr("$Import_layout") + "##tools").c_str(), ImVec2(-1, 0))) {
                ScanImportableLayouts();
                s_ed.showImportOverlay = !s_ed.showImportOverlay;
            }
            if (ImGui::Button((Tr("$Export") + "##tools").c_str(), ImVec2(-1, 0))) {
                ExportLayout();
            }

            ImGui::Spacing();
            SidebarHeader(Tr("$Zoom"));
            SidebarZoomMap();

            ImGui::Spacing();
            if (ImGui::Button((Tr("$To_Easy_Mode") + "##mode").c_str(), ImVec2(-1, 0))) {
                s_ed.easyMode = true;
                IniSet(kMod, "bLayoutEasyMode", "FallUIHUD", "1");
                s_ed.defHudAdapter = (s_ed.easyModeLayout == "DEF_HUD.xml");
                IniSet(kMod, "bDefHudAdapterLoadConfig", "FallUIHUD", s_ed.defHudAdapter ? "1" : "0");
                s_ed.currentProfile = -1;
                s_ed.selected = -1;
                if (!s_ed.easyModeLayout.empty() && s_ed.easyModeLayout != "DEF_HUD.xml") {
                    auto map = ReadLayoutIniFile(ImportableLayoutsDir() / (s_ed.easyModeLayout + ".ini"));
                    LoadLayoutMap(std::move(map), false);
                }
            }
        } else {
            // ---- Easy mode: layout buttons in the sidebar ----
            SidebarHeader(Tr("$Layouts"));
            for (const auto& name : s_ed.importableLayouts) {
                const bool active = s_ed.easyModeLayout == name;
                if (active) ImGui::PushStyleColor(ImGuiCol_Button, MixRGB(kFo4Green, 0, 0.3f));
                if (ImGui::Button((name + "##easy").c_str(), ImVec2(-1, 0))) {
                    s_ed.easyModeLayout = name;
                    s_ed.defHudAdapter = false;
                    IniSet(kMod, "sEasyModeUseLayout", "FallUIHUD", name);
                    IniSet(kMod, "bDefHudAdapterLoadConfig", "FallUIHUD", "0");
                    auto map = ReadLayoutIniFile(ImportableLayoutsDir() / (name + ".ini"));
                    if (map.empty()) {
                        ShowMessage(Tr("$Error"), Tr("$ImportIniFailedMsg"));
                    } else {
                        LoadLayoutMap(std::move(map), false);
                    }
                    s_ed.noticeFlashUntil = ImGui::GetTime() + 1.0;
                }
                if (active) ImGui::PopStyleColor();
            }
            if (s_ed.defHudXmlExists || s_ed.easyModeLayout == "DEF_HUD.xml") {
                const bool active = s_ed.easyModeLayout == "DEF_HUD.xml";
                if (active) ImGui::PushStyleColor(ImGuiCol_Button, MixRGB(kFo4Green, 0, 0.3f));
                if (ImGui::Button("DEF_HUD.xml##easy", ImVec2(-1, 0))) {
                    s_ed.easyModeLayout = "DEF_HUD.xml";
                    s_ed.defHudAdapter = true;
                    IniSet(kMod, "sEasyModeUseLayout", "FallUIHUD", "DEF_HUD.xml");
                    IniSet(kMod, "bDefHudAdapterLoadConfig", "FallUIHUD", "1");
                    s_ed.noticeFlashUntil = ImGui::GetTime() + 1.0;
                }
                if (active) ImGui::PopStyleColor();
            }
            ImGui::Spacing();
            if (ImGui::Button((Tr("$To_Edit_Mode") + "##mode").c_str(), ImVec2(-1, 0))) {
                s_ed.easyMode = false;
                s_ed.defHudAdapter = false;
                IniSet(kMod, "bLayoutEasyMode", "FallUIHUD", "0");
                IniSet(kMod, "bDefHudAdapterLoadConfig", "FallUIHUD", "0");
                s_ed.currentProfile = IniGetInt(kMod, "iCurrentHUDProfile", "HUDConfigProfiles");
                if (s_ed.currentProfile < 1) s_ed.currentProfile = 1;
                LoadCurrentLayoutFromIni();
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginGroup();

        // ---- Top bar over the canvas: widgets list toggle + filters ----
        if (!s_ed.easyMode) {
            if (ImGui::Button((Tr("$Widgets_list") + "##toggle").c_str())) {
                s_ed.showWidgetsList = !s_ed.showWidgetsList;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(Tr("$Filter_widgets").c_str());
            for (const auto& f : FilterNames()) {
                ImGui::SameLine();
                const bool active = s_ed.filter == f;
                if (active) ImGui::PushStyleColor(ImGuiCol_Button, MixRGB(kFo4Green, 0, 0.3f));
                if (ImGui::SmallButton((Tr(f) + "##filter" + f).c_str())) {
                    s_ed.filter = f;
                }
                if (active) ImGui::PopStyleColor();
            }
        }

        // ---- Canvas ----
        RenderCanvas();

        // ---- Reload notice (the original's noticePanel below the canvas) ----
        {
            const bool flashing = ImGui::GetTime() < s_ed.noticeFlashUntil;
            ImVec4 c = flashing ? ImVec4(1.0f, 0.85f, 0.3f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
            ImGui::TextColored(c, "%s", Tr("$ReloadPlz").c_str());
        }
        ImGui::EndGroup();

        // ---- Panels ----
        RenderWidgetsList();
        RenderImportOverlay();
        RenderGlobalSettingsPanel();
        RenderEditPanel();

        // ---- Modal message (showMessage port) ----
        if (!s_ed.messageTitle.empty()) {
            ImGui::OpenPopup("##falluimsg");
        }
        if (ImGui::BeginPopupModal("##falluimsg", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
            ImGui::TextColored(ImVec4(0.65f, 0.95f, 0.65f, 1.0f), "%s", s_ed.messageTitle.c_str());
            ImGui::Separator();
            ImGui::TextWrapped("%s", s_ed.messageText.c_str());
            ImGui::Spacing();
            if (ImGui::Button((Tr("$OK") + "##msgok").c_str(), ImVec2(120, 0))) {
                s_ed.messageTitle.clear();
                s_ed.messageText.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::PopID();
    }

    // ------------------------------------------------------------------
    // FallUI Icon Library preset auto-sync (FallUIIconLibrary.as port)
    //
    // The original polls the MCM setting store every 50ms while its page is
    // open and keeps the "Preset" dropdown coherent with the individual
    // color settings:
    //  - selecting preset N > 0 applies that preset's setting values
    //  - editing individual settings snaps the preset to the matching one
    //    (or 0 = Custom when nothing matches)
    // We run the same logic once per rendered frame while the page shows.
    // ------------------------------------------------------------------
    namespace IconLib {
        struct Watched {
            const char* mod;
            const char* section;
            const char* key;
        };
        // The watched MCM settings (iconLibraryConfigGetters)
        static const Watched kWatched[] = {
            { "FallUIIconLibrary", "MainSettings", "bIconsColored" },
            { "FallUIIconLibrary", "MainSettings", "bIconsEffect" },
            { "FallUIIconLibrary", "MainSettings", "sIconsColorSet" },
            { "FallUIIconLibrary", "MainSettings", "iIconsColorPostEffect" },
            { "FallUIIconLibrary", "MainSettings", "iIconsColorPostEffectStrength" },
            { "FallUIIconLibrary", "MainSettings", "iIconsBrightness" },
            { "FallUIIconLibrary", "MainSettings", "iIconsSaturation" },
        };

        static std::map<std::string, std::string> s_lastChecked;
        static bool s_primed = false;

        static std::string Key(const Watched& w) {
            return std::string(w.mod) + ":" + w.section + ":" + w.key;
        }

        static std::string Get(const Watched& w) {
            return IniGet(w.mod, w.key, w.section);
        }

        static void Set(const char* key, const std::string& value) {
            for (const auto& w : kWatched) {
                if (std::string(w.key) == key) {
                    if (Get(w) != value) {
                        IniSet(w.mod, w.key, w.section, value);
                        s_lastChecked[Key(w)] = value;
                    }
                    return;
                }
            }
        }

        // The preset table (checkForChanges): values each preset applies,
        // merged over the "Normal" base.
        struct Preset { std::map<std::string, std::string> values; };
        static std::vector<Preset> BuildPresets() {
            std::map<std::string, std::string> base = {
                { "sIconsColorSet", "Default.xml" },
                { "iIconsBrightness", "100" },
                { "iIconsSaturation", "100" },
                { "bIconsColored", "1" },
                { "bIconsEffect", "1" },
            };
            std::vector<std::map<std::string, std::string>> overrides = {
                {},                                          // 0 Custom (unused)
                {},                                          // 1 Normal
                { { "iIconsBrightness", "150" } },           // 2 Pastel
                { { "iIconsSaturation", "125" } },           // 3 Intense
                { { "iIconsBrightness", "160" }, { "iIconsSaturation", "0" } },  // 4 Grayscale
                { { "bIconsColored", "0" }, { "bIconsEffect", "0" } },           // 5 Classic
            };
            std::vector<Preset> presets;
            for (auto& ov : overrides) {
                Preset p;
                p.values = base;
                for (auto& [k, v] : ov) p.values[k] = v;
                presets.push_back(std::move(p));
            }
            return presets;
        }

        // Normalize bools ("true"/"1") for comparison
        static std::string Norm(const std::string& key, std::string v) {
            if (!key.empty() && key[0] == 'b') {
                return (v == "1" || v == "true" || v == "True") ? "1" : "0";
            }
            return v;
        }

        static void Tick() {
            bool anyChanged = false;
            bool presetChanged = false;
            for (const auto& w : kWatched) {
                std::string v = Get(w);
                auto& last = s_lastChecked[Key(w)];
                if (!s_primed) {
                    last = v;
                    continue;
                }
                if (last != v) {
                    last = v;
                    anyChanged = true;
                    if (std::string(w.key) == "iIconsColorPostEffect") presetChanged = true;
                }
            }
            if (!s_primed) {
                s_primed = true;
                return;
            }
            if (!anyChanged) return;

            auto presets = BuildPresets();
            int curPreset = 0;
            try {
                curPreset = std::stoi(s_lastChecked["FallUIIconLibrary:MainSettings:iIconsColorPostEffect"]);
            } catch (...) {}

            // Preset selected -> apply its values
            if (presetChanged && curPreset > 0 && curPreset < static_cast<int>(presets.size())) {
                for (const auto& [k, v] : presets[curPreset].values) {
                    Set(k.c_str(), v);
                }
            }

            // Individual settings edited -> find the matching preset (or Custom)
            bool matched = false;
            for (int i = 1; i < static_cast<int>(presets.size()); ++i) {
                bool all = true;
                for (const auto& [k, v] : presets[i].values) {
                    std::string cur = s_lastChecked["FallUIIconLibrary:MainSettings:" + k];
                    if (Norm(k, cur) != Norm(k, v)) { all = false; break; }
                }
                if (all) {
                    Set("iIconsColorPostEffect", std::to_string(i));
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                Set("iIconsColorPostEffect", "0");
            }
        }
    }

    static void RenderIconLibrary() {
        // Preset synchronization runs every frame while the page is visible
        IconLib::Tick();
        // The original renders a live icon preview strip here; rendering the
        // actual game icons would require the IconLibrary swf pipeline, so we
        // show a subtle placeholder band in the HUD accent color instead.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float w = std::min(ImGui::GetContentRegionAvail().x, 570.0f);
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + 24.0f), IM_COL32(255, 255, 255, 10));
        dl->AddText(ImVec2(pos.x + 8, pos.y + 4), RGBIntToImU32(GameHudColorRGB(), 0.8f),
                    "FallUI Icon Library");
        ImGui::Dummy(ImVec2(w, 28.0f));
    }

    // ------------------------------------------------------------------
    // Public API
    // ------------------------------------------------------------------

    bool HandlesImageControl(const std::string& libName, const std::string& className) {
        (void)libName;
        return className == "M8r.Controller.FallUIHUD" ||
               className == "M8r.View.FallUIIconLibrary";
    }

    void RenderImageControl(const std::string& libName, const std::string& className) {
        (void)libName;
        if (className == "M8r.Controller.FallUIHUD") {
            RenderHudEditor();
        } else if (className == "M8r.View.FallUIIconLibrary") {
            RenderIconLibrary();
        }
    }

    void ResetSession() {
        s_ed = EditorState{};
        s_panelStates.clear();
        IconLib::s_lastChecked.clear();
        IconLib::s_primed = false;
        s_dragging = -1;
    }

}
