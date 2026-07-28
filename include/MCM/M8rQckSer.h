#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

// Exact C++ port of m8r98a4f2's FastObjectStringifier ("m8rQckSer" format),
// reversed from the decompiled AS3 at
// PluginTemplate/MCM Categorizer/_analysis/main/scripts/M8r/Helper/
// FastObjectStringifier.as. MCM Categorizer persists its category tree with
// it (setting "sCategories:MCMCategorizer").
//
// Wire format (plain mode, the only mode the categorizer uses):
//   * The payload is a list of tokens joined by U+001F (unit separator).
//   * Token 0 is the header "m8rQckSerP" ("E"/"S" suffixes select the
//     string-extraction modes, which the categorizer never uses — we reject
//     them like the AS3 parser does when constructed for plain mode).
//   * A value is encoded as:
//       "S" <string token>     string (raw, see escaping below)
//       "N" <number token>     AS3 Number (String(x) formatting)
//       "B" <"1"|"0">          boolean
//       "Z"                    null
//       "A" value* "U"         array
//       "O" (<key token> value)* "U"   object (keys are raw tokens)
//   * After joining, the WHOLE payload has '\n' replaced by U+001D + "n" and
//     '\r' by U+001D + "r" (INI files are line-based). The parser reverses
//     that per string token.
//
// Because the categorizer setting is user data that any tool may have
// touched, Parse() is strictly defensive: any malformed input returns
// nullopt instead of throwing (a garbage INI value must never CTD the game).
//
// This unit has no game or framework dependencies so the codec can be
// verified by the offline host tests under swf/test/.
namespace M8rQckSer {

    // AS3-style dynamic value. Object keys keep INSERTION order (AS3 for..in
    // enumerates in insertion order), so re-serializing parsed data preserves
    // the original token sequence.
    class Value {
    public:
        enum class Kind { Null, Bool, Number, String, Array, Object };

        Kind kind = Kind::Null;
        bool boolVal = false;
        double numVal = 0.0;
        std::string strVal;  // UTF-8
        std::vector<Value> array;
        std::vector<std::pair<std::string, Value>> object;

        Value() = default;
        static Value MakeNull()                 { return Value{}; }
        static Value MakeBool(bool b)           { Value v; v.kind = Kind::Bool;   v.boolVal = b; return v; }
        static Value MakeNumber(double d)       { Value v; v.kind = Kind::Number; v.numVal = d; return v; }
        static Value MakeString(std::string s)  { Value v; v.kind = Kind::String; v.strVal = std::move(s); return v; }
        static Value MakeArray()                { Value v; v.kind = Kind::Array;  return v; }
        static Value MakeObject()               { Value v; v.kind = Kind::Object; return v; }

        bool IsNull()   const { return kind == Kind::Null; }
        bool IsArray()  const { return kind == Kind::Array; }
        bool IsObject() const { return kind == Kind::Object; }
        bool IsString() const { return kind == Kind::String; }
        bool IsNumber() const { return kind == Kind::Number; }

        // Object member lookup (nullptr when missing or not an object).
        const Value* Find(const std::string& key) const;

        // Sets/replaces an object member (keeps position when replacing).
        void Set(const std::string& key, Value v);
    };

    // Parses a plain-mode ("m8rQckSerP") payload. Returns nullopt for the
    // extraction-mode headers, a missing/garbled header, unbalanced
    // containers, truncated token streams, or excessive nesting.
    std::optional<Value> Parse(const std::string& text);

    // Serializes in plain mode, byte-compatible with the AS3 stringifier.
    // U+001F inside strings/keys would corrupt the token stream (the AS3
    // has the same blind spot), so it is replaced with a space.
    std::string Stringify(const Value& v);

}
