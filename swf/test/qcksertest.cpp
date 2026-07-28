// Offline validator for the M8rQckSer codec (MCM Categorizer's
// FastObjectStringifier format). Reads the user's real
// Data/MCM/Settings/MCMCategorizer.ini sCategories value when present and
// round-trips it; also runs synthetic and malformed-input cases.
//
// Build: build_qcksertest.bat   Run: qcksertest.exe [path-to-MCMCategorizer.ini]
#include "MCM/M8rQckSer.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using M8rQckSer::Value;

static int g_failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if (cond) {                                             \
            std::printf("  OK   %s\n", msg);                    \
        } else {                                                \
            std::printf("  FAIL %s\n", msg);                    \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

// Extracts "sCategories = <value>" from an INI file body.
static std::string ExtractCategories(const std::string& body) {
    std::istringstream in(body);
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        if (key == "sCategories") {
            std::string val = line.substr(eq + 1);
            val.erase(0, val.find_first_not_of(" \t"));
            if (!val.empty() && val.back() == '\r') val.pop_back();
            return val;
        }
    }
    return "";
}

static void TestRealData(const char* iniPath) {
    std::printf("[real data] %s\n", iniPath);
    std::ifstream f(iniPath, std::ios::binary);
    if (!f.is_open()) {
        std::printf("  SKIP file not found\n");
        return;
    }
    std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string raw = ExtractCategories(body);
    if (raw.empty()) {
        std::printf("  SKIP no sCategories value\n");
        return;
    }

    auto parsed = M8rQckSer::Parse(raw);
    CHECK(parsed.has_value(), "parses");
    if (!parsed) return;
    CHECK(parsed->IsArray(), "root is array");
    std::printf("       %zu categories\n", parsed->array.size());
    for (const auto& cat : parsed->array) {
        const Value* name = cat.Find("name");
        const Value* dir = cat.Find("dirName");
        const Value* mods = cat.Find("mods");
        const Value* id = cat.Find("id");
        if (!name || !dir || !mods || !id || !name->IsString() ||
            !dir->IsString() || !mods->IsArray() || !id->IsNumber()) {
            CHECK(false, "category has name/dirName/mods/id");
            continue;
        }
        std::printf("       id=%g dir=%s name=\"%s\" mods=%zu\n",
                    id->numVal, dir->strVal.c_str(), name->strVal.c_str(),
                    mods->array.size());
    }

    // Round trip: our stringify must be byte-identical to the original
    // (object key order is preserved by the insertion-ordered Value).
    std::string round = M8rQckSer::Stringify(*parsed);
    CHECK(round == raw, "stringify(parse(x)) == x (byte-identical)");
    if (round != raw) {
        // Find first difference for diagnosis.
        size_t i = 0;
        while (i < round.size() && i < raw.size() && round[i] == raw[i]) ++i;
        std::printf("       first diff at byte %zu: orig=0x%02X ours=0x%02X\n",
                    i,
                    i < raw.size() ? (unsigned char)raw[i] : 0,
                    i < round.size() ? (unsigned char)round[i] : 0);
    }
}

static void TestSynthetic() {
    std::printf("[synthetic]\n");

    // Category tree like the categorizer builds (addCategory key order).
    Value cat = Value::MakeObject();
    cat.object.emplace_back("name", Value::MakeString("My Category"));
    Value mods = Value::MakeArray();
    mods.array.push_back(Value::MakeString("ModA"));
    mods.array.push_back(Value::MakeString("Mod with spaces"));
    cat.object.emplace_back("mods", std::move(mods));
    cat.object.emplace_back("id", Value::MakeNumber(7));
    cat.object.emplace_back("dirName", Value::MakeString("__CAT_7"));
    Value root = Value::MakeArray();
    root.array.push_back(std::move(cat));

    std::string s = M8rQckSer::Stringify(root);
    auto back = M8rQckSer::Parse(s);
    CHECK(back.has_value(), "synthetic round-trips");
    if (back) {
        CHECK(back->IsArray() && back->array.size() == 1, "structure preserved");
        const Value* name = back->array[0].Find("name");
        CHECK(name && name->strVal == "My Category", "string preserved");
        const Value* id = back->array[0].Find("id");
        CHECK(id && id->numVal == 7.0, "number preserved");
    }

    // All scalar kinds + newline escaping.
    Value all = Value::MakeObject();
    all.object.emplace_back("b1", Value::MakeBool(true));
    all.object.emplace_back("b0", Value::MakeBool(false));
    all.object.emplace_back("z", Value::MakeNull());
    all.object.emplace_back("n", Value::MakeNumber(-2.5));
    all.object.emplace_back("s", Value::MakeString("line1\nline2\rend"));
    std::string s2 = M8rQckSer::Stringify(all);
    CHECK(s2.find('\n') == std::string::npos && s2.find('\r') == std::string::npos,
          "no raw newlines in payload (INI-safe)");
    auto back2 = M8rQckSer::Parse(s2);
    CHECK(back2.has_value(), "scalar kinds round-trip");
    if (back2) {
        const Value* b1 = back2->Find("b1");
        const Value* b0 = back2->Find("b0");
        const Value* z = back2->Find("z");
        const Value* n = back2->Find("n");
        const Value* s3 = back2->Find("s");
        CHECK(b1 && b1->boolVal, "bool true");
        CHECK(b0 && !b0->boolVal, "bool false");
        CHECK(z && z->IsNull(), "null");
        CHECK(n && n->numVal == -2.5, "negative float");
        CHECK(s3 && s3->strVal == "line1\nline2\rend", "newlines escape/unescape");
    }

    // Separator inside a string must not corrupt the stream.
    Value sep = Value::MakeArray();
    sep.array.push_back(Value::MakeString(std::string("bad\x1F") + "name"));
    std::string s4 = M8rQckSer::Stringify(sep);
    auto back4 = M8rQckSer::Parse(s4);
    CHECK(back4.has_value() && back4->IsArray() && back4->array.size() == 1 &&
          back4->array[0].strVal == "bad name",
          "separator in string sanitized to space");
}

static void TestMalformed() {
    std::printf("[malformed]\n");
    const char SEP = '\x1F';
    auto join = [&](std::initializer_list<const char*> parts) {
        std::string out;
        for (const char* p : parts) {
            if (!out.empty()) out.push_back(SEP);
            out += p;
        }
        return out;
    };

    CHECK(!M8rQckSer::Parse("").has_value(), "empty input rejected");
    CHECK(!M8rQckSer::Parse("garbage").has_value(), "no header rejected");
    CHECK(!M8rQckSer::Parse(join({"m8rQckSerE", "0"})).has_value(), "extraction mode E rejected");
    CHECK(!M8rQckSer::Parse(join({"m8rQckSerS", "0"})).has_value(), "extraction mode S rejected");
    CHECK(!M8rQckSer::Parse(join({"m8rQckSerP", "A", "S", "x"})).has_value(), "unclosed array rejected");
    CHECK(!M8rQckSer::Parse(join({"m8rQckSerP", "O", "key"})).has_value(), "truncated object rejected");
    CHECK(!M8rQckSer::Parse(join({"m8rQckSerP", "N", "abc"})).has_value(), "non-numeric N rejected");
    CHECK(!M8rQckSer::Parse(join({"m8rQckSerP", "X"})).has_value(), "unknown tag rejected");
    CHECK(!M8rQckSer::Parse(join({"m8rQckSerP", "S", "a", "S", "b"})).has_value(), "trailing tokens rejected");
    CHECK(!M8rQckSer::Parse(join({"m8rQckSerP", "S"})).has_value(), "S without payload rejected");

    // Deep nesting bomb must not stack-overflow.
    std::string bomb = "m8rQckSerP";
    for (int i = 0; i < 5000; ++i) { bomb.push_back(SEP); bomb += "A"; }
    CHECK(!M8rQckSer::Parse(bomb).has_value(), "5000-deep nesting rejected");
}

int main(int argc, char** argv) {
    const char* iniPath = argc > 1
        ? argv[1]
        : "F:\\Modlists\\LoreOut\\mods\\LoreOut Patches\\MCM\\Settings\\MCMCategorizer.ini";

    TestRealData(iniPath);
    TestSynthetic();
    TestMalformed();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
