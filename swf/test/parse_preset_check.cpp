// One-shot: parse FrameworkTestPreset.ini's sSettings with the framework codec.
#include "MCM/M8rIniJson.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

static int g_fails = 0;

static void CheckNum(const M8rIniJson::Value& root,
                     const std::vector<std::string>& path, double expect) {
    const auto* v = root.Find(path);
    if (!v || !v->IsNumber()) {
        std::printf("FAIL missing/non-number at %s\n", path.back().c_str());
        ++g_fails;
        return;
    }
    if (v->AsDouble() != expect) {
        std::printf("FAIL %s: got %g expect %g\n", path.back().c_str(),
                    v->AsDouble(), expect);
        ++g_fails;
        return;
    }
    std::printf("  OK %s = %g\n", path.back().c_str(), expect);
}

static void CheckStr(const M8rIniJson::Value& root,
                     const std::vector<std::string>& path, const char* expect) {
    const auto* v = root.Find(path);
    if (!v || v->kind != M8rIniJson::Value::Kind::String) {
        std::printf("FAIL missing/non-string at %s\n", path.back().c_str());
        ++g_fails;
        return;
    }
    if (v->strVal != expect) {
        std::printf("FAIL %s: got '%s' expect '%s'\n", path.back().c_str(),
                    v->strVal.c_str(), expect);
        ++g_fails;
        return;
    }
    std::printf("  OK %s = %s\n", path.back().c_str(), expect);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: parse_preset_check <Preset.ini>\n");
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    if (!f) {
        std::printf("FAIL open %s\n", argv[1]);
        return 1;
    }
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    const std::string key = "sSettings=";
    auto pos = content.find(key);
    if (pos == std::string::npos) {
        std::printf("FAIL no sSettings=\n");
        return 1;
    }
    std::string json = content.substr(pos + key.size());
    while (!json.empty() && (json.back() == '\n' || json.back() == '\r')) json.pop_back();

    auto parsed = M8rIniJson::Parse(json);
    if (!parsed || !parsed->IsObject()) {
        std::printf("FAIL parse\n");
        return 1;
    }
    std::printf("OK parsed %zu top-level mod(s)\n", parsed->object.size());

    CheckNum(*parsed, {"UneducatedShooter", "ini", "Inertia", "frotLimitX"}, 12.3);
    CheckNum(*parsed, {"UneducatedShooter", "ini", "Inertia", "brotDisableInADS"}, 0);
    CheckStr(*parsed, {"UneducatedShooter", "hot", "keyLeanLeft"}, "69;0");
    CheckStr(*parsed, {"UneducatedShooter", "hot", "keyLeanRight"}, "81;0");
    CheckNum(*parsed, {"CapsWidget", "prp", "CapsWidget.esp", "F99", "iStyle"}, 2);
    CheckNum(*parsed, {"CapsWidget", "prp", "CapsWidget.esp", "F99", "iHeight"}, 500);
    CheckStr(*parsed, {"FastAddItemMenu", "hot", "OpenAddItemMenuKey"}, "121;0");
    CheckStr(*parsed, {"WorkshopFramework", "hot", "StartupWorkshop"}, "80;0");
    CheckNum(*parsed, {"UnlimitedSurvivalMode", "ini", "Settings", "iEnableFastTravel"}, 1);
    CheckNum(*parsed, {"UnlimitedSurvivalMode", "ini", "Settings", "iEnableConsole"}, 1);

    std::printf("%s (%d failure(s))\n", g_fails ? "FAILED" : "ALL PASS", g_fails);
    return g_fails ? 1 : 0;
}
