#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Exact C++ port of the two persistence formats used by m8r98a4f2's
// "MCM Settings Manager" (Nexus 56195), reversed from the decompiled AS3 in
// PluginTemplate/MCM Settings Manager/_analysis/as3/:
//
//   1. M8rJSON (M8r.McmSettingsManager.Helper.M8rJSON) — a JSON-like text
//      format that is NOT standard JSON:
//        * object keys are written UNQUOTED and sorted (AS3 Array.sort());
//          ':' inside a key is escaped as "_§CL§_"
//        * strings are double-quoted; '"' inside is escaped as "_§QUOT§_",
//          '\n' as the two characters "\n" (never unescaped on parse — the
//          original has the same asymmetry)
//        * numbers print like AS3: integral values have no decimal point
//        * bare NaN is a valid element; arrays can be written but the
//          original parser cannot read them (we never produce them)
//
//   2. IniChunked (M8r.Helper.IniChunked) — a container that splits one long
//      string across several MCM string settings ("s<Key>_0".."s<Key>_N"),
//      each chunk wrapped in '<'...'>', with a "[chars=<n>;bytes=<m>]" header
//      (UTF-16 code units / UTF-8 bytes of the payload) prepended for
//      validation. Slots written by us load in the original Flash manager
//      and vice versa.
//
// This unit has no game or framework dependencies so the codec can be
// verified by the offline host tests under swf/test/.
namespace M8rIniJson {

    // ------------------------------------------------------------------
    // Value tree
    // ------------------------------------------------------------------

    // std::map keeps keys in byte order, which matches AS3's default
    // Array.sort() (UTF-16 code unit order) for the ASCII keys MCM uses.
    class Value {
    public:
        enum class Kind { Null, Bool, Int, Double, String, Object };

        Kind kind = Kind::Null;
        bool boolVal = false;
        int intVal = 0;          // AS3 "int" (32-bit)
        double dblVal = 0.0;     // AS3 "Number" (may be NaN)
        std::string strVal;      // UTF-8
        std::map<std::string, Value> object;

        Value() = default;
        static Value MakeBool(bool b)                { Value v; v.kind = Kind::Bool;   v.boolVal = b; return v; }
        static Value MakeInt(int i)                  { Value v; v.kind = Kind::Int;    v.intVal = i; return v; }
        static Value MakeDouble(double d)            { Value v; v.kind = Kind::Double; v.dblVal = d; return v; }
        static Value MakeString(std::string s)       { Value v; v.kind = Kind::String; v.strVal = std::move(s); return v; }
        static Value MakeObject()                    { Value v; v.kind = Kind::Object; return v; }

        bool IsObject() const { return kind == Kind::Object; }
        bool IsNumber() const { return kind == Kind::Int || kind == Kind::Double; }
        double AsDouble() const { return kind == Kind::Int ? static_cast<double>(intVal) : dblVal; }

        // Deep lookup along a path of object keys; nullptr when any segment
        // is missing or a non-object is traversed (Helper.helperGetDeepProperty).
        const Value* Find(const std::vector<std::string>& path) const;
        Value* Find(const std::vector<std::string>& path);

        // Deep insert: creates intermediate objects (helperSetDeepProperty).
        void Set(const std::vector<std::string>& path, Value leaf);

        // Removes the leaf at path, then prunes now-empty parent objects
        // (McmSetting.removeFromSaved).
        void Remove(const std::vector<std::string>& path);
    };

    // AS3 Number.toString() equivalent (used for Kind::Double). Integral
    // values print with no decimal point ("3", not "3.0"). Where AS3 would
    // emit exponent notation (|x| >= 1e21 or < 1e-6) we emit fixed notation
    // instead, because the original parser's number regex rejects exponents
    // (writing them would produce data the Flash manager itself cannot read).
    std::string NumberToString(double v);

    // Serialize a tree in the exact M8rJSON wire format.
    std::string Stringify(const Value& v);

    // Parse M8rJSON. Only a top-level object is accepted (same as the
    // original). Returns nullopt on any syntax error.
    std::optional<Value> Parse(std::string_view text);

    // ------------------------------------------------------------------
    // AS3 string measurement helpers (UTF-8 in, AS3 semantics out)
    // ------------------------------------------------------------------

    // Number of UTF-16 code units (AS3 String.length) in a UTF-8 string.
    size_t Utf16Length(std::string_view utf8);

    // ------------------------------------------------------------------
    // IniChunked container
    // ------------------------------------------------------------------

    // Reads the chunked value stored under keyBase (e.g. "SettingsSlot3"):
    // concatenates "s<keyBase>_<i>" chunk contents, validates the
    // "[chars=;bytes=]" header, and returns the payload.
    //   ""       — no data stored (all chunks empty)
    //   nullopt  — data present but corrupt (header/size mismatch); the
    //              original logs and drops it, callers treat it as empty
    // getSetting receives the full key name (e.g. "sSettingsSlot3_0") and
    // must return "" for missing settings.
    std::optional<std::string> ReadChunked(
        const std::string& keyBase, int maxChunks,
        const std::function<std::string(const std::string&)>& getSetting,
        std::string* log = nullptr);

    // Writes payload as chunks under keyBase, prepending the size header and
    // wrapping each chunk in '<'...'>'. Clears leftover chunk keys from a
    // previous longer value (the original's cleanup loop has an index bug
    // and only reliably clears the first stale chunk; we clear all of them —
    // reads are compatible either way because the reader stops at the first
    // short chunk). Also writes "i<keyBase>Size" = 0 first, mirroring the
    // original. Returns false when the payload exceeds maxChunks capacity
    // (the original silently truncates; we report it so the UI can warn).
    bool WriteChunked(
        const std::string& keyBase, int maxChunks, size_t maxBytes,
        const std::string& payload,
        const std::function<std::string(const std::string&)>& getSetting,
        const std::function<void(const std::string&, const std::string&)>& setSetting,
        std::string* log = nullptr);

}
