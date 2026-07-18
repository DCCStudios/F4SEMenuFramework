// Offline validator for the native FallUI HUD editor recreation.
//
// Includes the real FallUIHudEditor.cpp with two shadowed headers (fakes/):
//   RE/Fallout.h            -> stub NiColor + REL call (fixed HUD color)
//   MCM/MCMValueProvider.h  -> in-memory INI store
// MCMTranslation and ImGui are the REAL implementations (linked in), so the
// translation file parsing and Tr() resolution paths are exercised for real.
//
// What it verifies (the data-format compatibility contract):
//   1. StringPacker: unpack -> pack -> unpack roundtrip on every
//      sLayoutGlobalSettings string found in real shipped preset INIs.
//   2. Widget lines: for every widget line in every preset INI,
//      UnpackWidgetConfig -> PackWidgetConfig must preserve visibility,
//      transform, and the EXACT set of explicit modifiers. A dropped key
//      means either a catalog gap (option id missing) or a wrong stdValue
//      (our default equals a value the Flash editor considered non-default).
//   3. Migration: synthetic pre-1.4.0 layout gets the documented transforms
//      (Brd* -> SB* bar conversion, 1.5x position rescale).
//   4. Full session load with a real game-root layout (translations, preset
//      import, easy mode) against files copied from the shipped mods.
//
// Usage: falluitest <gameRootDir> <presetIni> [presetIni...]
//   gameRootDir must contain Data/Interface/FallUI HUD/{Translation,
//   Importable HUD Layouts} (copied from the real FallUI - HUD mod).

#include "../../src/MCM/FallUIHudEditor.cpp"

#include <cstdio>
#include <filesystem>

using namespace FallUIHudEditor;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        ++g_checks;                                            \
        if (!(cond)) {                                         \
            ++g_failures;                                      \
            std::printf("FAIL: " __VA_ARGS__);                 \
            std::printf("\n");                                 \
        }                                                      \
    } while (0)

// ---------------------------------------------------------------------------
// Canonical view of a packed widget line for comparison: transform fields as
// text plus the k=v modifier set (order-independent).
// ---------------------------------------------------------------------------
struct CanonLine {
    bool ok = false;
    std::string vis, x, y, sx, sy, rot;
    std::map<std::string, std::string> mods;
};

static CanonLine Canonicalize(const std::string& line) {
    CanonLine c;
    size_t p1 = line.find(':');
    if (p1 == std::string::npos) return c;
    c.vis = line.substr(0, p1);
    if (c.vis != "on" && c.vis != "off") return c;

    size_t p2 = line.find(':', p1 + 1);
    std::string tf = line.substr(p1 + 1, p2 == std::string::npos ? std::string::npos
                                                                 : p2 - p1 - 1);
    // <x>x<y>*<sx>*<sy>[r<rot>]
    size_t xPos = tf.find('x', tf.find_first_not_of("-+0123456789.") == std::string::npos
                               ? 0 : tf.find_first_of('x'));
    // simpler: split manually
    size_t i = 0;
    auto num = [&]() {
        size_t s = i;
        if (i < tf.size() && (tf[i] == '-' || tf[i] == '+')) ++i;
        while (i < tf.size() && (isdigit((unsigned char)tf[i]) || tf[i] == '.')) ++i;
        return tf.substr(s, i - s);
    };
    c.x = num();
    if (i >= tf.size() || tf[i] != 'x') return c;
    ++i;
    c.y = num();
    if (i >= tf.size() || tf[i] != '*') return c;
    ++i;
    c.sx = num();
    if (i >= tf.size() || tf[i] != '*') return c;
    ++i;
    c.sy = num();
    c.rot = "0";
    if (i < tf.size() && tf[i] == 'r') {
        ++i;
        c.rot = num();
    }

    if (p2 != std::string::npos) {
        std::string mods = line.substr(p2 + 1);
        size_t start = 0;
        while (start <= mods.size() && !mods.empty()) {
            size_t comma = mods.find(',', start);
            std::string pair = mods.substr(start, comma == std::string::npos
                                                      ? std::string::npos : comma - start);
            size_t eq = pair.find('=');
            if (eq != std::string::npos) {
                c.mods[pair.substr(0, eq)] = pair.substr(eq + 1);
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    c.ok = true;
    return c;
}

// Numeric-tolerant value compare: "1.30999999999999" style floats must match
// after parse; everything else must match as text (bools, colors, ints).
static bool ValuesMatch(const std::string& a, const std::string& b) {
    if (a == b) return true;
    try {
        size_t ia = 0, ib = 0;
        double da = std::stod(a, &ia), db = std::stod(b, &ib);
        return ia == a.size() && ib == b.size() && da == db;
    } catch (...) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Test 1+2: preset INI widget-line and global-settings roundtrips
// ---------------------------------------------------------------------------
static void TestPresetFile(const fs::path& iniPath) {
    std::printf("--- preset: %s\n", iniPath.filename().string().c_str());
    auto hudConfig = ReadLayoutIniFile(iniPath);
    CHECK(!hudConfig.empty(), "%s: no [HUDConfig] entries parsed",
          iniPath.filename().string().c_str());

    int widgetLines = 0;
    for (const auto& [key, line] : hudConfig) {
        if (key == "sLayoutGlobalSettings") {
            // StringPacker roundtrip (semantic: unpack(pack(unpack(x))) == unpack(x))
            PackedObject o1 = StringPackerUnpack(line);
            CHECK(!o1.empty(), "global settings unpacked empty: %s", line.c_str());
            PackedObject o2 = StringPackerUnpack(StringPackerPack(o1));
            CHECK(o1.size() == o2.size(), "StringPacker roundtrip size %zu != %zu",
                  o1.size(), o2.size());
            for (const auto& [k, v] : o1) {
                auto it = o2.find(k);
                CHECK(it != o2.end(), "StringPacker roundtrip lost key '%s'", k.c_str());
                if (it != o2.end()) {
                    CHECK(it->second.kind == v.kind &&
                          ValuesMatch(PackModValue(it->second), PackModValue(v)),
                          "StringPacker roundtrip changed '%s': '%s' -> '%s'", k.c_str(),
                          PackModValue(v).c_str(), PackModValue(it->second).c_str());
                }
            }
            continue;
        }

        // Map the INI key to a widget
        int widx = -1;
        for (int i = 0; i < kWidgetCount; ++i) {
            if (WidgetIniKey(kWidgets[i]) == key) { widx = i; break; }
        }
        CHECK(widx >= 0, "unknown widget key '%s'", key.c_str());
        if (widx < 0) continue;
        ++widgetLines;

        const std::string ident = ParentIdent(kWidgets[widx]);
        auto cfg = UnpackWidgetConfig(ident, line);
        CHECK(cfg.has_value(), "'%s' failed to unpack: %s", key.c_str(), line.c_str());
        if (!cfg.has_value()) continue;

        std::string repacked = PackWidgetConfig(ident, *cfg);
        CanonLine orig = Canonicalize(line);
        CanonLine rt = Canonicalize(repacked);
        CHECK(orig.ok && rt.ok, "'%s' canonicalize failed", key.c_str());
        if (!orig.ok || !rt.ok) continue;

        CHECK(orig.vis == rt.vis, "'%s' visibility changed %s -> %s", key.c_str(),
              orig.vis.c_str(), rt.vis.c_str());
        CHECK(ValuesMatch(orig.x, rt.x) && ValuesMatch(orig.y, rt.y),
              "'%s' position changed %sx%s -> %sx%s", key.c_str(), orig.x.c_str(),
              orig.y.c_str(), rt.x.c_str(), rt.y.c_str());
        CHECK(ValuesMatch(orig.sx, rt.sx) && ValuesMatch(orig.sy, rt.sy),
              "'%s' scale changed %s*%s -> %s*%s", key.c_str(), orig.sx.c_str(),
              orig.sy.c_str(), rt.sx.c_str(), rt.sy.c_str());
        CHECK(ValuesMatch(orig.rot, rt.rot), "'%s' rotation changed %s -> %s",
              key.c_str(), orig.rot.c_str(), rt.rot.c_str());

        for (const auto& [k, v] : orig.mods) {
            // Legacy keys the original's own unpack drops (they are gone from
            // the current option catalog; migration deletes them explicitly):
            //   apseqsScale -> replaced by apseqsScaleX/Y, removed at <1.3.4
            if (k == "apseqsScale") continue;
            auto it = rt.mods.find(k);
            CHECK(it != rt.mods.end(),
                  "'%s' DROPPED modifier %s=%s (catalog gap or stdValue collision)",
                  key.c_str(), k.c_str(), v.c_str());
            if (it != rt.mods.end()) {
                CHECK(ValuesMatch(v, it->second), "'%s' modifier %s changed '%s' -> '%s'",
                      key.c_str(), k.c_str(), v.c_str(), it->second.c_str());
            }
        }
        for (const auto& [k, v] : rt.mods) {
            CHECK(orig.mods.count(k), "'%s' ADDED spurious modifier %s=%s", key.c_str(),
                  k.c_str(), v.c_str());
        }
    }
    std::printf("    %d widget lines checked\n", widgetLines);
}

// ---------------------------------------------------------------------------
// Test 3: migration transforms (pre-1.4.0 synthetic layout)
// ---------------------------------------------------------------------------
static void TestMigration() {
    std::printf("--- migration\n");
    std::map<std::string, std::string> hudConfig;
    // Old-style (pre-1.4.0) HP meter: 1280x720-space position + Brd* border opts
    hudConfig["sLeftMeters_mc__HPMeter_mc"] = "on:100x-60*1*1r0:hpbarBrdS=10,hpbarBrdW=2";
    PackedObject gls;  // no _editorVersion -> version 0, all migrations run

    MigrateLayout(hudConfig, gls, false);

    CHECK(gls.count("_editorVersion") && gls["_editorVersion"].AsInt() == 1004000,
          "migration did not stamp _editorVersion (got %d)",
          gls.count("_editorVersion") ? gls["_editorVersion"].AsInt() : -1);

    const std::string& migratedLine = hudConfig["sLeftMeters_mc__HPMeter_mc"];
    auto cfg = UnpackWidgetConfig("LeftMeters_mc.HPMeter_mc", migratedLine);
    CHECK(cfg.has_value(), "migrated HP line failed to unpack: %s", migratedLine.c_str());
    if (cfg.has_value()) {
        CHECK(cfg->x == 150 && cfg->y == -90, "1.5x rescale wrong: %gx%g (want 150x-90)",
              cfg->x, cfg->y);
        // Unpack pre-populates std values for every catalog option (matching
        // the original), so mods.count() can't detect removal — the explicit
        // Brd* keys must be gone from the PACKED LINE instead.
        CHECK(migratedLine.find("Brd") == std::string::npos,
              "Brd* options not removed from packed line: %s", migratedLine.c_str());
        CHECK(cfg->mods.count("hpbarSBS") && cfg->mods["hpbarSBS"].AsInt() == 10,
              "BrdS -> SBS not converted");
        CHECK(cfg->mods.count("hpbarSBW") && cfg->mods["hpbarSBW"].AsInt() == 3,
              "BrdW*1.5 -> SBW wrong (got %d, want 3)",
              cfg->mods.count("hpbarSBW") ? cfg->mods["hpbarSBW"].AsInt() : -1);
        CHECK(cfg->mods.count("hpbarSV") && cfg->mods["hpbarSV"].AsBool(),
              "border conversion did not enable SV");
    }

    // Already-current version: nothing may change
    std::map<std::string, std::string> untouched;
    untouched["sLeftMeters_mc__HPMeter_mc"] = "on:150x-90*1*1r0:";
    PackedObject gls2;
    gls2["_editorVersion"] = PackedValue::I(1007000);
    MigrateLayout(untouched, gls2, false);
    CHECK(untouched["sLeftMeters_mc__HPMeter_mc"] == "on:150x-90*1*1r0:",
          "migration touched a current-version layout");
    CHECK(gls2["_editorVersion"].AsInt() == 1007000,
          "migration downgraded a newer _editorVersion");
}

// ---------------------------------------------------------------------------
// Test 4: full session load against a real game-root Data tree
// ---------------------------------------------------------------------------
static void TestSession(const fs::path& gameRoot) {
    std::printf("--- session (root: %s)\n", gameRoot.string().c_str());
    fs::current_path(gameRoot);

    // Translation resolution through the REAL translation file + parser
    std::string hud = Tr("$HUD_color");
    CHECK(!hud.empty() && hud[0] != '$', "Tr($HUD_color) unresolved: '%s'", hud.c_str());

    // Fresh vanilla session (empty store)
    MCMValueProvider::g_testStore.clear();
    ResetSession();
    LoadSession();
    CHECK(s_ed.loaded, "session did not load");
    CHECK(s_ed.currentProfile == 1, "default profile != 1 (got %d)", s_ed.currentProfile);
    CHECK(!s_ed.importableLayouts.empty(), "no importable layouts found");

    // Easy mode with a shipped preset: widgets must take the preset transform
    MCMValueProvider::g_testStore.clear();
    MCMValueProvider::SetModSettingRaw("FallUIHUD", "bLayoutEasyMode:FallUIHUD", "1");
    MCMValueProvider::SetModSettingRaw("FallUIHUD", "sEasyModeUseLayout:FallUIHUD", "Fallout 3");
    ResetSession();
    LoadSession();
    CHECK(s_ed.easyMode, "easy mode not applied");
    int hpIdx = -1;
    for (int i = 0; i < kWidgetCount; ++i) {
        if (std::string(kWidgets[i].name) == "HPMeter_mc") { hpIdx = i; break; }
    }
    CHECK(hpIdx >= 0, "HPMeter_mc missing from catalog");
    if (hpIdx >= 0) {
        const auto& cfg = s_ed.widgets[hpIdx].cfg;
        CHECK(cfg.x == -36 && cfg.y == -91,
              "Fallout 3 preset HP position wrong: %gx%g (want -36x-91)", cfg.x, cfg.y);
        CHECK(std::abs(cfg.scaleX - 1.309999999999999) < 1e-12,
              "Fallout 3 preset HP scale wrong: %.17g", cfg.scaleX);
        CHECK(cfg.mods.count("hpbarSV") && cfg.mods.at("hpbarSV").AsBool(),
              "Fallout 3 preset hpbarSV lost");
    }

    // Explicit-profile flow: store a layout via SaveWidget and re-read it
    MCMValueProvider::g_testStore.clear();
    ResetSession();
    LoadSession();
    if (hpIdx >= 0) {
        s_ed.widgets[hpIdx].cfg.x = 123;
        s_ed.widgets[hpIdx].cfg.y = -45;
        SaveWidget(hpIdx);
        auto stored = MCMValueProvider::GetModSettingRaw(
            "FallUIHUD", "sLeftMeters_mc__HPMeter_mc:HUDConfig");
        CHECK(stored.has_value() && stored->rfind("on:123x-45*", 0) == 0,
              "SaveWidget wrote '%s'", stored.value_or("<none>").c_str());
        ResetSession();
        LoadSession();
        CHECK(s_ed.widgets[hpIdx].cfg.x == 123 && s_ed.widgets[hpIdx].cfg.y == -45,
              "saved layout did not survive reload: %gx%g",
              s_ed.widgets[hpIdx].cfg.x, s_ed.widgets[hpIdx].cfg.y);
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: falluitest <gameRootDir> <presetIni> [presetIni...]\n");
        return 2;
    }

    // ImGui context for the few non-render helpers that query it (GetTime)
    ImGui::CreateContext();

    for (int i = 2; i < argc; ++i) {
        TestPresetFile(argv[i]);
    }
    TestMigration();
    TestSession(argv[1]);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
