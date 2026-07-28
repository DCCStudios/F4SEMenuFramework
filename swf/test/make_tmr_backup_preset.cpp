// Snapshots the live TMR MCM Settings on disk into a Settings Manager
// Presets/*.ini so Apply can restore after FrameworkTestPreset.
#include "MCM/M8rIniJson.h"

#include <cstdio>
#include <fstream>
#include <string>

using namespace M8rIniJson;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: make_tmr_backup_preset <out.ini>\n");
        return 2;
    }

    Value root = Value::MakeObject();

    // ---- CapsWidget ----
    // No CapsWidget.ini in TMR overwrite/mods; these are the config.json
    // defaults (Cap Style = Default, Vertical Position = 410). Enough to
    // undo FrameworkTestPreset's Nuka/500 markers.
    root.Set({"CapsWidget", "prp", "CapsWidget.esp", "F99", "iStyle"}, Value::MakeInt(0));
    root.Set({"CapsWidget", "prp", "CapsWidget.esp", "F99", "iHeight"}, Value::MakeInt(410));

    // ---- Hotkeys from overwrite/MCM/Settings/Keybinds.json ----
    root.Set({"FastAddItemMenu", "hot", "OpenAddItemMenuKey"}, Value::MakeString("121;0")); // F10
    root.Set({"UneducatedShooter", "hot", "keyLeanLeft"}, Value::MakeString("69;0"));      // E
    root.Set({"UneducatedShooter", "hot", "keyLeanRight"}, Value::MakeString("81;0"));     // Q
    root.Set({"WorkshopFramework", "hot", "StartupWorkshop"}, Value::MakeString("80;0"));  // P

    // ---- UneducatedShooter.ini (overwrite) ----
    root.Set({"UneducatedShooter", "ini", "Inertia", "frotLimitX"}, Value::MakeDouble(0.0));
    root.Set({"UneducatedShooter", "ini", "Inertia", "frotLimitY"}, Value::MakeDouble(0.0));
    root.Set({"UneducatedShooter", "ini", "Inertia", "frotDivX"}, Value::MakeDouble(50.0));
    root.Set({"UneducatedShooter", "ini", "Inertia", "frotDivY"}, Value::MakeDouble(50.0));
    root.Set({"UneducatedShooter", "ini", "Inertia", "frotDivXADS"}, Value::MakeDouble(0.0));
    root.Set({"UneducatedShooter", "ini", "Inertia", "frotDivYADS"}, Value::MakeDouble(0.0));
    root.Set({"UneducatedShooter", "ini", "Inertia", "frotADSConditionMult"}, Value::MakeDouble(0.0));
    root.Set({"UneducatedShooter", "ini", "Inertia", "frotReturnDiv"}, Value::MakeDouble(1.0));
    root.Set({"UneducatedShooter", "ini", "Inertia", "brotDisableInADS"}, Value::MakeInt(1));

    root.Set({"UneducatedShooter", "ini", "Realism", "bRealism"}, Value::MakeInt(0));
    root.Set({"UneducatedShooter", "ini", "Realism", "bRealismDisableInPA"}, Value::MakeInt(0));
    root.Set({"UneducatedShooter", "ini", "Realism", "fRealismDefaultRotation"}, Value::MakeDouble(-2.5));
    root.Set({"UneducatedShooter", "ini", "Realism", "fRealismRatio"}, Value::MakeDouble(1.2));
    root.Set({"UneducatedShooter", "ini", "Realism", "fRealismYawRatio"}, Value::MakeDouble(0.8));
    root.Set({"UneducatedShooter", "ini", "Realism", "fRealismRatioADS"}, Value::MakeDouble(0.2));
    root.Set({"UneducatedShooter", "ini", "Realism", "fRealismRotLimit"}, Value::MakeDouble(10.0));
    root.Set({"UneducatedShooter", "ini", "Realism", "fRealismYawRotLimit"}, Value::MakeDouble(12.5));
    root.Set({"UneducatedShooter", "ini", "Realism", "fRealismReturnStep"}, Value::MakeDouble(0.05));
    root.Set({"UneducatedShooter", "ini", "Realism", "fRealismReturnStepADS"}, Value::MakeDouble(4.0));

    root.Set({"UneducatedShooter", "ini", "Leaning", "bleanDisable"}, Value::MakeInt(0));
    root.Set({"UneducatedShooter", "ini", "Leaning", "bToggleLean"}, Value::MakeInt(0));
    root.Set({"UneducatedShooter", "ini", "Leaning", "fleanTimeCost"}, Value::MakeDouble(0.20));
    root.Set({"UneducatedShooter", "ini", "Leaning", "fleanMax"}, Value::MakeDouble(15.0));
    root.Set({"UneducatedShooter", "ini", "Leaning", "fleanMax3rd"}, Value::MakeDouble(29.9));
    root.Set({"UneducatedShooter", "ini", "Leaning", "bADSOnly"}, Value::MakeInt(0));
    root.Set({"UneducatedShooter", "ini", "Leaning", "bR6Style"}, Value::MakeInt(0));
    root.Set({"UneducatedShooter", "ini", "Leaning", "bRotateInput"}, Value::MakeInt(1));

    root.Set({"UneducatedShooter", "ini", "Height", "bdynamicHeight"}, Value::MakeInt(1));
    root.Set({"UneducatedShooter", "ini", "Height", "fheightDiffThreshold"}, Value::MakeDouble(5.0));
    root.Set({"UneducatedShooter", "ini", "Height", "fheightBuffer"}, Value::MakeDouble(4.0));
    root.Set({"UneducatedShooter", "ini", "Height", "fminHeight"}, Value::MakeDouble(20.0));

    // ---- UnlimitedSurvivalMode.ini (mod defaults / shipped settings) ----
    root.Set({"UnlimitedSurvivalMode", "ini", "Settings", "iEnableFastTravel"}, Value::MakeInt(1));
    root.Set({"UnlimitedSurvivalMode", "ini", "Settings", "iEnableConsole"}, Value::MakeInt(1));
    root.Set({"UnlimitedSurvivalMode", "ini", "Settings", "iEnableGodMode"}, Value::MakeInt(1));
    root.Set({"UnlimitedSurvivalMode", "ini", "Settings", "iEnableQuickSave"}, Value::MakeInt(1));
    root.Set({"UnlimitedSurvivalMode", "ini", "Settings", "iEnableSaveLoadButton"}, Value::MakeInt(1));
    root.Set({"UnlimitedSurvivalMode", "ini", "Settings", "iCanReenableSurvivalMode"}, Value::MakeInt(1));
    root.Set({"UnlimitedSurvivalMode", "ini", "Settings", "iShowAutoSaveSettings"}, Value::MakeInt(1));
    root.Set({"UnlimitedSurvivalMode", "ini", "Settings", "iEnableAutoSave"}, Value::MakeInt(1));
    root.Set({"UnlimitedSurvivalMode", "ini", "Settings", "iShowEnemyRedDotOnCompass"}, Value::MakeInt(0));
    root.Set({"UnlimitedSurvivalMode", "ini", "Settings", "iShowLocationsOnCompass"}, Value::MakeInt(1));
    root.Set({"UnlimitedSurvivalMode", "ini", "Settings", "iDisableStimpakAmmoWeight"}, Value::MakeInt(0));
    root.Set({"UnlimitedSurvivalMode", "ini", "Settings", "iEnableAchievement"}, Value::MakeInt(1));

    // ---- Upscaling.ini (overwrite) ----
    root.Set({"Upscaling", "ini", "Settings", "iUpscaleMethodPreference"}, Value::MakeInt(2));
    root.Set({"Upscaling", "ini", "Settings", "iQualityMode"}, Value::MakeInt(0));

    const std::string wire = Stringify(root);
    auto back = Parse(wire);
    if (!back || Stringify(*back) != wire) {
        std::printf("FAIL round-trip\n");
        return 1;
    }

    std::ofstream out(argv[1], std::ios::binary | std::ios::trunc);
    if (!out) {
        std::printf("FAIL write %s\n", argv[1]);
        return 1;
    }
    out << "; F4SE Menu Framework: TMR current-settings backup preset.\n"
           "; Snapshot of live MCM Settings + Keybinds.json at generation time.\n"
           "; Apply this after FrameworkTestPreset to put TMR back.\n"
           "; New presets need a game restart under MO2 before they appear.\n"
           ";\n"
           "; Sources:\n"
           ";   overwrite/MCM/Settings/UneducatedShooter.ini\n"
           ";   overwrite/MCM/Settings/Upscaling.ini\n"
           ";   overwrite/MCM/Settings/Keybinds.json\n"
           ";   Unlimited Survival Mode mod Settings/UnlimitedSurvivalMode.ini\n"
           ";   CapsWidget: config.json defaults (no user CapsWidget.ini on disk)\n"
           "\n"
           "[MCMSettings]\n"
           "sSettings="
        << wire << "\n";
    out.close();

    std::printf("OK wrote %s (%zu bytes, %zu mods)\n", argv[1], wire.size(),
                root.object.size());
    return 0;
}
