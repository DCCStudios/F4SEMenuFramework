// Offline validator for the M8rIniJson codec (MCM Settings Manager formats).
// Checks the C++ port against strings the original AS3 implementation
// produces/accepts (derived from the decompiled sources in
// PluginTemplate/MCM Settings Manager/_analysis/as3/).
//
// Build: build_m8rjsontest.bat   Run: m8rjsontest.exe

#include "MCM/M8rIniJson.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>

using namespace M8rIniJson;

static int g_failures = 0;

static void Check(bool cond, const char* what) {
    if (cond) {
        std::printf("  OK   %s\n", what);
    } else {
        std::printf("  FAIL %s\n", what);
        ++g_failures;
    }
}

static void CheckEq(const std::string& got, const std::string& expect, const char* what) {
    if (got == expect) {
        std::printf("  OK   %s\n", what);
    } else {
        std::printf("  FAIL %s\n         got:    %s\n         expect: %s\n", what, got.c_str(), expect.c_str());
        ++g_failures;
    }
}

// Builds the tree for a typical stored slot:
//   { "SomeMod": { "hot": { "hkToggle": "66;0" },
//                  "ini": { "Main": { "bEnabled": 1, "fSpeed": 1.5, "iCount": 3, "sName": "he\"llo" } } },
//     "OtherMod": { "glb": { "Other.esp": { "80A": 2 } } } }
static Value BuildSampleTree() {
    Value root = Value::MakeObject();
    root.Set({ "SomeMod", "hot", "hkToggle" }, Value::MakeString("66;0"));
    root.Set({ "SomeMod", "ini", "Main", "bEnabled" }, Value::MakeInt(1));
    root.Set({ "SomeMod", "ini", "Main", "fSpeed" }, Value::MakeDouble(1.5));
    root.Set({ "SomeMod", "ini", "Main", "iCount" }, Value::MakeInt(3));
    root.Set({ "SomeMod", "ini", "Main", "sName" }, Value::MakeString("he\"llo"));
    root.Set({ "OtherMod", "glb", "Other.esp", "80A" }, Value::MakeInt(2));
    return root;
}

static void TestStringify() {
    std::printf("[stringify]\n");
    const Value root = BuildSampleTree();
    // Expected wire text, derived by hand from M8rJSON.as: sorted unquoted
    // keys, '"' -> _§QUOT§_, integral Number without decimal point.
    const std::string expect =
        "{OtherMod:{glb:{Other.esp:{80A:2}}},"
        "SomeMod:{hot:{hkToggle:\"66;0\"},"
        "ini:{Main:{bEnabled:1,fSpeed:1.5,iCount:3,"
        "sName:\"he_\xC2\xA7QUOT\xC2\xA7_llo\"}}}}";
    CheckEq(Stringify(root), expect, "sample tree wire format");

    // Number formatting quirks (AS3 Number.toString)
    CheckEq(NumberToString(3.0), "3", "integral double prints without decimal");
    CheckEq(NumberToString(-2.0), "-2", "negative integral double");
    CheckEq(NumberToString(0.25), "0.25", "fraction");
    CheckEq(NumberToString(0.1), "0.1", "shortest round-trip");
    Check(NumberToString(std::nan("")) == std::string("NaN"), "NaN literal");

    // Key colon escaping
    Value k = Value::MakeObject();
    k.Set({ "a:b" }, Value::MakeInt(7));
    CheckEq(Stringify(k), "{a_\xC2\xA7" "CL\xC2\xA7_b:7}", "colon in key escaped");

    // Newline in string value stays escaped as literal backslash-n
    Value nl = Value::MakeObject();
    nl.Set({ "s" }, Value::MakeString("a\nb"));
    CheckEq(Stringify(nl), "{s:\"a\\nb\"}", "newline escaping");
}

static void TestParse() {
    std::printf("[parse]\n");
    // Round trip of the sample tree
    const Value root = BuildSampleTree();
    const std::string wire = Stringify(root);
    auto parsed = Parse(wire);
    Check(parsed.has_value(), "round trip parses");
    if (parsed) {
        CheckEq(Stringify(*parsed), wire, "round trip is lossless");
        const Value* v = parsed->Find({ "SomeMod", "ini", "Main", "fSpeed" });
        Check(v && v->kind == Value::Kind::Double && v->dblVal == 1.5, "float leaf type/value");
        v = parsed->Find({ "SomeMod", "ini", "Main", "iCount" });
        Check(v && v->kind == Value::Kind::Int && v->intVal == 3, "int leaf type/value");
        v = parsed->Find({ "SomeMod", "hot", "hkToggle" });
        Check(v && v->kind == Value::Kind::String && v->strVal == "66;0", "hotkey string leaf");
        v = parsed->Find({ "SomeMod", "ini", "Main", "sName" });
        Check(v && v->strVal == "he\"llo", "quote unescaped on parse");
    }

    // Empty object
    auto empty = Parse("{}");
    Check(empty.has_value() && empty->IsObject() && empty->object.empty(), "empty object");

    // Literals and NaN
    auto lits = Parse("{a:true,b:false,c:null,d:NaN}");
    Check(lits.has_value(), "literals parse");
    if (lits) {
        const Value* a = lits->Find({ "a" });
        const Value* d = lits->Find({ "d" });
        Check(a && a->kind == Value::Kind::Bool && a->boolVal, "true literal");
        Check(d && d->kind == Value::Kind::Double && d->dblVal != d->dblVal, "NaN literal");
    }

    // The original parser rejects exponent notation — so must ours.
    Check(!Parse("{a:1e5}").has_value(), "exponent notation rejected");
    // Number must be followed by , } ] or end.
    Check(!Parse("{a:12x}").has_value(), "garbage after number rejected");
    // Non-object top level is rejected (original returns null).
    Check(!Parse("42").has_value(), "non-object top level rejected");
    // Truncated input
    Check(!Parse("{a:1").has_value(), "truncated object rejected");
}

static void TestRemove() {
    std::printf("[remove/prune]\n");
    Value root = BuildSampleTree();
    root.Remove({ "OtherMod", "glb", "Other.esp", "80A" });
    Check(root.Find({ "OtherMod" }) == nullptr, "empty parents pruned after remove");
    root.Remove({ "SomeMod", "ini", "Main", "bEnabled" });
    Check(root.Find({ "SomeMod", "ini", "Main", "fSpeed" }) != nullptr, "siblings survive remove");
}

static void TestChunked() {
    std::printf("[IniChunked]\n");
    std::map<std::string, std::string> store;
    auto get = [&](const std::string& k) {
        auto it = store.find(k);
        return it != store.end() ? it->second : std::string{};
    };
    auto set = [&](const std::string& k, const std::string& v) { store[k] = v; };

    // Small payload -> single chunk
    {
        const std::string payload = "{Mod:{ini:{Main:{bOn:1}}}}";
        std::string log;
        Check(WriteChunked("SettingsSlot1", 3, 65000, payload, get, set, &log), "small write succeeds");
        const std::string c0 = get("sSettingsSlot1_0");
        // Header + '<' wrapper, exactly as the original would write it.
        const std::string expect = "<[chars=26;bytes=26]" + payload + ">";
        CheckEq(c0, expect, "chunk 0 wire format");
        Check(get("iSettingsSlot1Size") == "0", "size key zeroed");
        auto back = ReadChunked("SettingsSlot1", 3, get, &log);
        Check(back.has_value() && *back == payload, "small round trip");
    }

    // Large payload -> multiple chunks; stale chunk cleanup
    {
        std::string payload = "{";
        for (int i = 0; payload.size() < 150000; ++i) {
            payload += "k" + std::to_string(i) + ":\"0123456789abcdef\",";
        }
        payload.back() = '}';
        store["sSettingsSlot2_2"] = "<stale>";  // simulate leftover from longer old value
        std::string log;
        Check(WriteChunked("SettingsSlot2", 3, 65000, payload, get, set, &log), "large write succeeds");
        Check(!get("sSettingsSlot2_0").empty() && !get("sSettingsSlot2_1").empty(), "spans multiple chunks");
        auto back = ReadChunked("SettingsSlot2", 3, get, &log);
        Check(back.has_value() && *back == payload, "large round trip");
        // Every chunk except possibly the last must stay under maxBytes+2.
        Check(get("sSettingsSlot2_0").size() <= 65002, "chunk 0 byte limit");
    }

    // Overflow: payload too big for maxChunks
    {
        std::string payload(3 * 65000, 'x');
        std::string log;
        Check(!WriteChunked("SettingsSlot3", 3, 65000, payload, get, set, &log), "overflow reported");
    }

    // Non-ASCII payload: header counts must use UTF-16 units + UTF-8 bytes
    {
        // "ü" (2 UTF-8 bytes, 1 UTF-16 unit) and "𐍈" (4 bytes, 2 units)
        const std::string payload = "{s:\"\xC3\xBC\xF0\x90\x8D\x88\"}";
        std::string log;
        Check(WriteChunked("SettingsSlot4", 3, 65000, payload, get, set, &log), "non-ascii write");
        const std::string c0 = get("sSettingsSlot4_0");
        // payload: {s:"ü𐍈"} = 6 ascii chars + 1 + 2 units = 9 UTF-16 units; 6+2+4=12 bytes
        Check(c0.find("[chars=9;bytes=12]") != std::string::npos, "non-ascii header counts");
        auto back = ReadChunked("SettingsSlot4", 3, get, &log);
        Check(back.has_value() && *back == payload, "non-ascii round trip");
    }

    // Corrupt data: size mismatch -> dropped (nullopt)
    {
        store["sSettingsSlot5_0"] = "<[chars=99;bytes=99]{a:1}>";
        std::string log;
        auto r = ReadChunked("SettingsSlot5", 3, get, &log);
        Check(!r.has_value(), "size mismatch dropped");
    }

    // No data -> empty string (not an error)
    {
        std::string log;
        auto r = ReadChunked("SettingsSlot6", 3, get, &log);
        Check(r.has_value() && r->empty(), "missing slot reads as empty");
    }
}

int main() {
    TestStringify();
    TestParse();
    TestRemove();
    TestChunked();
    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d FAILURE(S)\n", g_failures);
    return 1;
}
