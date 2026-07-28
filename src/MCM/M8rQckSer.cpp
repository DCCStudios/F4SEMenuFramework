#include "MCM/M8rQckSer.h"
#include "MCM/M8rIniJson.h"  // NumberToString (AS3 Number.toString port)

#include <cstdlib>

namespace M8rQckSer {

    namespace {
        constexpr char kSep = '\x1F';     // token separator (FastObjectStringifier.separatorEntries)
        constexpr char kEsc = '\x1D';     // newline escape prefix (escapeChar)
        constexpr const char* kHeaderPlain = "m8rQckSerP";
        constexpr int kMaxDepth = 64;     // AS3 has a 1e6 loop guard; nesting this deep is garbage

        // Reverses the payload-wide newline escaping for one string token
        // (the AS3 safeUnescape loops replace() until stable, which for data
        // produced by the matching stringify is a single global replace).
        std::string Unescape(const std::string& s) {
            if (s.find(kEsc) == std::string::npos) return s;
            std::string out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size(); ++i) {
                if (s[i] == kEsc && i + 1 < s.size() && (s[i + 1] == 'n' || s[i + 1] == 'r')) {
                    out.push_back(s[i + 1] == 'n' ? '\n' : '\r');
                    ++i;
                } else {
                    out.push_back(s[i]);
                }
            }
            return out;
        }

        // Forward token cursor (the AS3 reverses the array and pops; same order).
        struct Cursor {
            const std::vector<std::string>* tokens = nullptr;
            size_t pos = 0;
            bool AtEnd() const { return pos >= tokens->size(); }
            const std::string& Peek() const { return (*tokens)[pos]; }
            const std::string& Take() { return (*tokens)[pos++]; }
        };

        // Recursive descent over the token stream. Returns nullopt on any
        // malformation; never throws.
        std::optional<Value> ParseValue(Cursor& c, int depth) {
            if (depth > kMaxDepth || c.AtEnd()) return std::nullopt;
            const std::string& tag = c.Take();

            if (tag == "S") {
                if (c.AtEnd()) return std::nullopt;
                return Value::MakeString(Unescape(c.Take()));
            }
            if (tag == "N") {
                if (c.AtEnd()) return std::nullopt;
                // AS3 Number(<token>): strtod covers the formats String(x)
                // emits; a non-numeric token becomes NaN there — treat that
                // as malformed instead (nothing legitimate produces it).
                const std::string& t = c.Take();
                char* end = nullptr;
                double d = std::strtod(t.c_str(), &end);
                if (t.empty() || end != t.c_str() + t.size()) return std::nullopt;
                return Value::MakeNumber(d);
            }
            if (tag == "B") {
                if (c.AtEnd()) return std::nullopt;
                return Value::MakeBool(c.Take() == "1");
            }
            if (tag == "Z") {
                return Value::MakeNull();
            }
            if (tag == "A") {
                Value arr = Value::MakeArray();
                while (!c.AtEnd() && c.Peek() != "U") {
                    auto v = ParseValue(c, depth + 1);
                    if (!v.has_value()) return std::nullopt;
                    arr.array.push_back(std::move(*v));
                }
                if (c.AtEnd()) return std::nullopt;  // missing closing U
                c.Take();
                return arr;
            }
            if (tag == "O") {
                Value obj = Value::MakeObject();
                while (!c.AtEnd() && c.Peek() != "U") {
                    std::string key = Unescape(c.Take());
                    auto v = ParseValue(c, depth + 1);
                    if (!v.has_value()) return std::nullopt;
                    obj.object.emplace_back(std::move(key), std::move(*v));
                }
                if (c.AtEnd()) return std::nullopt;
                c.Take();
                return obj;
            }
            return std::nullopt;  // AS3 throws "Invalid next"
        }

        // A raw separator inside a string or key would corrupt the token
        // stream (the AS3 has the same blind spot) — turn it into a space.
        std::string SanitizeToken(std::string s) {
            for (char& ch : s) {
                if (ch == kSep) ch = ' ';
            }
            return s;
        }

        void StringifyValue(const Value& v, std::vector<std::string>& out, int depth) {
            if (depth > kMaxDepth) return;  // cycle-proofing; Value trees can't cycle anyway
            switch (v.kind) {
                case Value::Kind::String:
                    out.push_back("S");
                    out.push_back(SanitizeToken(v.strVal));
                    break;
                case Value::Kind::Number:
                    out.push_back("N");
                    out.push_back(M8rIniJson::NumberToString(v.numVal));
                    break;
                case Value::Kind::Bool:
                    out.push_back("B");
                    out.push_back(v.boolVal ? "1" : "0");
                    break;
                case Value::Kind::Null:
                    out.push_back("Z");
                    break;
                case Value::Kind::Array:
                    out.push_back("A");
                    for (const auto& e : v.array) StringifyValue(e, out, depth + 1);
                    out.push_back("U");
                    break;
                case Value::Kind::Object:
                    out.push_back("O");
                    for (const auto& [k, e] : v.object) {
                        out.push_back(SanitizeToken(k));
                        StringifyValue(e, out, depth + 1);
                    }
                    out.push_back("U");
                    break;
            }
        }
    }

    const Value* Value::Find(const std::string& key) const {
        if (kind != Kind::Object) return nullptr;
        for (const auto& [k, v] : object) {
            if (k == key) return &v;
        }
        return nullptr;
    }

    void Value::Set(const std::string& key, Value v) {
        if (kind != Kind::Object) return;
        for (auto& [k, existing] : object) {
            if (k == key) {
                existing = std::move(v);
                return;
            }
        }
        object.emplace_back(key, std::move(v));
    }

    std::optional<Value> Parse(const std::string& text) {
        if (text.empty()) return std::nullopt;

        // Split on the separator (AS3 String.split keeps empty tokens).
        std::vector<std::string> tokens;
        size_t start = 0;
        while (true) {
            size_t sep = text.find(kSep, start);
            if (sep == std::string::npos) {
                tokens.push_back(text.substr(start));
                break;
            }
            tokens.push_back(text.substr(start, sep - start));
            start = sep + 1;
        }

        if (tokens.empty() || tokens[0] != kHeaderPlain) return std::nullopt;

        Cursor c{ &tokens, 1 };
        auto v = ParseValue(c, 0);
        if (!v.has_value()) return std::nullopt;
        // Trailing tokens mean the payload wasn't a single value — corrupt.
        if (!c.AtEnd()) return std::nullopt;
        return v;
    }

    std::string Stringify(const Value& v) {
        std::vector<std::string> tokens;
        tokens.emplace_back(kHeaderPlain);
        StringifyValue(v, tokens, 0);

        std::string joined;
        size_t total = 0;
        for (const auto& t : tokens) total += t.size() + 1;
        joined.reserve(total);
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (i) joined.push_back(kSep);
            joined += tokens[i];
        }

        // Payload-wide escaping, exactly like the AS3 (applied after the
        // join): \n -> ESC n, \r -> ESC r.
        std::string out;
        out.reserve(joined.size());
        for (char ch : joined) {
            if (ch == '\n') { out.push_back(kEsc); out.push_back('n'); }
            else if (ch == '\r') { out.push_back(kEsc); out.push_back('r'); }
            else out.push_back(ch);
        }
        return out;
    }

}
