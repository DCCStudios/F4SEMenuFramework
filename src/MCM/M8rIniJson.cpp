#include "MCM/M8rIniJson.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// Format ports reversed from the decompiled MCM Settings Manager AS3 —
// see the header for the format description and the _analysis folder under
// PluginTemplate/MCM Settings Manager/ for the decompiled sources.
namespace M8rIniJson {

    // The AS3 escape tokens. '§' is U+00A7 (0xC2 0xA7 in UTF-8) — written by
    // AS3's writeUTFBytes exactly like this.
    static constexpr const char* kQuotToken = "_\xC2\xA7QUOT\xC2\xA7_";
    static constexpr const char* kColonToken = "_\xC2\xA7" "CL\xC2\xA7_";

    // ------------------------------------------------------------------
    // Value tree helpers
    // ------------------------------------------------------------------

    const Value* Value::Find(const std::vector<std::string>& path) const {
        const Value* cur = this;
        for (const auto& seg : path) {
            if (!cur->IsObject()) return nullptr;
            auto it = cur->object.find(seg);
            if (it == cur->object.end()) return nullptr;
            cur = &it->second;
        }
        return cur;
    }

    Value* Value::Find(const std::vector<std::string>& path) {
        return const_cast<Value*>(static_cast<const Value*>(this)->Find(path));
    }

    void Value::Set(const std::vector<std::string>& path, Value leaf) {
        Value* cur = this;
        cur->kind = Kind::Object;
        for (size_t i = 0; i + 1 < path.size(); ++i) {
            Value& next = cur->object[path[i]];
            if (!next.IsObject()) next = MakeObject();
            cur = &next;
        }
        if (!path.empty()) {
            cur->object[path.back()] = std::move(leaf);
        }
    }

    void Value::Remove(const std::vector<std::string>& path) {
        if (path.empty()) return;
        // Delete the leaf, then walk back up removing parents that became
        // empty (mirrors McmSetting.removeFromSaved's prune loop).
        for (size_t depth = path.size(); depth >= 1; --depth) {
            std::vector<std::string> parentPath(path.begin(), path.begin() + (depth - 1));
            Value* parent = Find(parentPath);
            if (!parent || !parent->IsObject()) return;
            auto it = parent->object.find(path[depth - 1]);
            if (it == parent->object.end()) return;
            if (depth == path.size()) {
                parent->object.erase(it);        // the leaf itself
            } else if (it->second.IsObject() && it->second.object.empty()) {
                parent->object.erase(it);        // now-empty intermediate
            } else {
                return;                          // parent still has content
            }
        }
    }

    // ------------------------------------------------------------------
    // Number formatting (AS3 Number.toString)
    // ------------------------------------------------------------------

    std::string NumberToString(double v) {
        if (std::isnan(v)) return "NaN";
        if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
        // ECMAScript prints integral values without a decimal point.
        if (v == std::floor(v) && std::abs(v) < 9.007199254740992e15) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
            return buf;
        }
        // Shortest round-trip representation.
        char buf[64];
        auto res = std::to_chars(buf, buf + sizeof(buf), v);
        std::string s(buf, res.ptr);
        if (s.find('e') == std::string::npos && s.find('E') == std::string::npos) {
            return s;
        }
        // to_chars chose exponent notation (AS3 would too for such magnitudes),
        // but the original parser cannot read exponents — emit fixed instead.
        std::snprintf(buf, sizeof(buf), "%.17f", v);
        s = buf;
        // Trim trailing zeros (keep at least one digit after the point).
        auto dot = s.find('.');
        if (dot != std::string::npos) {
            auto last = s.find_last_not_of('0');
            if (last == dot) last++;         // "3." -> "3.0"
            s.erase(last + 1);
        }
        return s;
    }

    // ------------------------------------------------------------------
    // Stringify
    // ------------------------------------------------------------------

    static void ReplaceAll(std::string& s, std::string_view from, std::string_view to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    // Recursion cap for both parse and stringify. The Settings Manager's own
    // slot trees are ~5 levels deep; anything past this is corrupt or a
    // maliciously/accidentally nested blob (e.g. the MCM Categorizer's data
    // when the m8r save path produces pathological nesting). Bailing keeps
    // this codec from being the thing that stack-overflows — matching the
    // kMaxDepth guard M8rQckSer already has.
    constexpr int kMaxDepth = 64;

    static void StringifyInto(const Value& v, std::string& out, int depth = 0) {
        if (depth > kMaxDepth) return;  // don't recurse into a degenerate tree
        switch (v.kind) {
            case Value::Kind::Null:
                out += "null";
                break;
            case Value::Kind::Bool:
                out += v.boolVal ? "true" : "false";
                break;
            case Value::Kind::Int: {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%d", v.intVal);
                out += buf;
                break;
            }
            case Value::Kind::Double:
                out += NumberToString(v.dblVal);
                break;
            case Value::Kind::String: {
                std::string s = v.strVal;
                ReplaceAll(s, "\n", "\\n");        // literal backslash-n, like AS3
                ReplaceAll(s, "\"", kQuotToken);
                out += '"';
                out += s;
                out += '"';
                break;
            }
            case Value::Kind::Object: {
                out += '{';
                bool first = true;
                // std::map iterates keys in sorted (byte) order = AS3 sort().
                for (const auto& [key, child] : v.object) {
                    if (!first) out += ',';
                    first = false;
                    std::string k = key;
                    ReplaceAll(k, ":", kColonToken);
                    out += k;
                    out += ':';
                    StringifyInto(child, out, depth + 1);
                }
                out += '}';
                break;
            }
        }
    }

    std::string Stringify(const Value& v) {
        std::string out;
        StringifyInto(v, out);
        return out;
    }

    // ------------------------------------------------------------------
    // Parse
    // ------------------------------------------------------------------

    namespace {
        struct Cursor {
            std::string_view text;
            size_t pos = 0;
            bool failed = false;

            char Peek() const { return pos < text.size() ? text[pos] : '\0'; }
            bool AtEnd() const { return pos >= text.size(); }
            bool Consume(char c) {
                if (Peek() == c) { ++pos; return true; }
                return false;
            }
            bool ConsumeLiteral(std::string_view lit) {
                if (text.substr(pos, lit.size()) == lit) { pos += lit.size(); return true; }
                return false;
            }
        };

        Value ParseObject(Cursor& c, int depth);   // fwd

        // ^(-?\d+(?:\.\d+)?)(?=[,}\]]|$) — int when no '.', else Number.
        Value ParseNumber(Cursor& c) {
            const size_t start = c.pos;
            size_t p = c.pos;
            if (p < c.text.size() && c.text[p] == '-') ++p;
            const size_t digitsStart = p;
            while (p < c.text.size() && c.text[p] >= '0' && c.text[p] <= '9') ++p;
            if (p == digitsStart) { c.failed = true; return {}; }
            bool isFloat = false;
            if (p < c.text.size() && c.text[p] == '.') {
                const size_t fracStart = p + 1;
                size_t q = fracStart;
                while (q < c.text.size() && c.text[q] >= '0' && c.text[q] <= '9') ++q;
                if (q > fracStart) { isFloat = true; p = q; }
                // "1." with no fraction digits: regex leaves the dot behind —
                // the following lookahead check then fails, like the original.
            }
            // Lookahead: number must be followed by , } ] or end of input.
            if (p < c.text.size()) {
                const char next = c.text[p];
                if (next != ',' && next != '}' && next != ']') { c.failed = true; return {}; }
            }
            const std::string tok(c.text.substr(start, p - start));
            c.pos = p;
            if (isFloat) {
                return Value::MakeDouble(std::strtod(tok.c_str(), nullptr));
            }
            // AS3 int() is ToInt32 (modular wrap for out-of-range values).
            const long long ll = std::strtoll(tok.c_str(), nullptr, 10);
            return Value::MakeInt(static_cast<int>(static_cast<uint32_t>(ll)));
        }

        // "((?:\\"|[^"])*)" then _§QUOT§_ -> '"' (note: "\n" stays literal,
        // matching the original's asymmetric escaping).
        Value ParseString(Cursor& c) {
            ++c.pos;  // opening quote (caller checked)
            std::string content;
            while (!c.AtEnd()) {
                const char ch = c.text[c.pos];
                if (ch == '\\' && c.pos + 1 < c.text.size() && c.text[c.pos + 1] == '"') {
                    content += "\\\"";
                    c.pos += 2;
                    continue;
                }
                if (ch == '"') {
                    ++c.pos;
                    ReplaceAll(content, kQuotToken, "\"");
                    return Value::MakeString(std::move(content));
                }
                content += ch;
                ++c.pos;
            }
            c.failed = true;
            return {};
        }

        Value ParseElement(Cursor& c, int depth) {
            const char ch = c.Peek();
            if (ch == '-' || (ch >= '0' && ch <= '9')) return ParseNumber(c);
            if (ch == '"') return ParseString(c);
            if (ch == 't' || ch == 'f' || ch == 'n') {
                if (c.ConsumeLiteral("true"))  return Value::MakeBool(true);
                if (c.ConsumeLiteral("false")) return Value::MakeBool(false);
                if (c.ConsumeLiteral("null"))  return Value{};  // Kind::Null
                c.failed = true;
                return {};
            }
            if (ch == '{') { ++c.pos; return ParseObject(c, depth + 1); }
            if (ch == 'N' && c.ConsumeLiteral("NaN")) {
                return Value::MakeDouble(std::nan(""));
            }
            c.failed = true;
            return {};
        }

        // Called with the cursor just past '{'.
        Value ParseObject(Cursor& c, int depth) {
            if (depth > kMaxDepth) { c.failed = true; return {}; }  // degenerate nesting
            Value obj = Value::MakeObject();
            if (c.Consume('}')) return obj;
            while (!c.AtEnd()) {
                // Key: ^([^:]+): — everything up to the first colon.
                const size_t keyStart = c.pos;
                while (!c.AtEnd() && c.text[c.pos] != ':') ++c.pos;
                if (c.AtEnd() || c.pos == keyStart) { c.failed = true; return {}; }
                std::string key(c.text.substr(keyStart, c.pos - keyStart));
                ++c.pos;  // ':'
                ReplaceAll(key, kColonToken, ":");

                Value elem = ParseElement(c, depth);
                if (c.failed) return {};
                obj.object[std::move(key)] = std::move(elem);

                if (c.Consume(',')) continue;
                if (c.Consume('}')) return obj;
                c.failed = true;
                return {};
            }
            c.failed = true;
            return {};
        }
    }

    std::optional<Value> Parse(std::string_view text) {
        if (text.empty()) return std::nullopt;
        Cursor c{ text };
        if (text[0] == '{') {
            c.pos = 1;
            Value v = ParseObject(c, 0);
            if (c.failed) return std::nullopt;
            return v;
        }
        if (text[0] == '}') {
            return Value::MakeObject();  // the original returns an empty object here
        }
        return std::nullopt;
    }

    // ------------------------------------------------------------------
    // UTF measurement / slicing (AS3 String semantics on UTF-8 storage)
    // ------------------------------------------------------------------

    // Bytes in the UTF-8 sequence that starts with lead byte b (1 when the
    // byte is a continuation/invalid — safe forward progress).
    static size_t SeqLen(unsigned char b) {
        if (b < 0x80) return 1;
        if ((b & 0xE0) == 0xC0) return 2;
        if ((b & 0xF0) == 0xE0) return 3;
        if ((b & 0xF8) == 0xF0) return 4;
        return 1;
    }

    size_t Utf16Length(std::string_view utf8) {
        size_t units = 0;
        for (size_t i = 0; i < utf8.size();) {
            const size_t len = SeqLen(static_cast<unsigned char>(utf8[i]));
            units += (len == 4) ? 2 : 1;  // astral chars are surrogate pairs
            i += len;
        }
        return units;
    }

    // Byte offset of the UTF-16 unit index `units` from byte offset `from`.
    // Clamps to the end of the string. A surrogate pair is never split (the
    // pair counts once its lead unit is reached — chunk boundaries shift by
    // one unit at worst, which the format tolerates: the reader just
    // concatenates chunk contents).
    static size_t AdvanceUtf16(std::string_view s, size_t fromByte, size_t units) {
        size_t i = fromByte;
        size_t counted = 0;
        while (i < s.size() && counted < units) {
            const size_t len = SeqLen(static_cast<unsigned char>(s[i]));
            counted += (len == 4) ? 2 : 1;
            i += len;
        }
        return i;
    }

    // ------------------------------------------------------------------
    // IniChunked
    // ------------------------------------------------------------------

    static void AppendLog(std::string* log, const std::string& msg) {
        if (!log) return;
        if (!log->empty()) *log += " ";
        *log += msg;
    }

    std::optional<std::string> ReadChunked(
        const std::string& keyBase, int maxChunks,
        const std::function<std::string(const std::string&)>& getSetting,
        std::string* log) {

        if (maxChunks <= 0) return std::string{};

        std::string joined;
        int chunksRead = 0;
        for (int i = 0; i < maxChunks; ++i) {
            const std::string raw = getSetting("s" + keyBase + "_" + std::to_string(i));
            const size_t rawUnits = Utf16Length(raw);
            if (rawUnits > 1 && raw.front() == '<' && raw.back() == '>') {
                joined.append(raw, 1, raw.size() - 2);
            }
            if (rawUnits <= 2) break;  // empty or "<>" terminates the chain
            chunksRead = i + 1;
        }

        if (joined.empty()) return std::string{};

        // Header: [chars=<n>;bytes=<m>] within the first 50 characters.
        if (joined.front() != '[') {
            AppendLog(log, "Invalid data: No header start '['.");
            return std::nullopt;
        }
        const size_t close = joined.find(']');
        if (close == std::string::npos || close > 50) {
            AppendLog(log, "Invalid data: No header end ']'.");
            return std::nullopt;
        }
        unsigned long long chars = 0, bytes = 0;
        {
            const std::string header = joined.substr(0, close + 1);
            if (std::sscanf(header.c_str(), "[chars=%llu;bytes=%llu]", &chars, &bytes) != 2) {
                AppendLog(log, "Invalid data: Invalid header structure.");
                return std::nullopt;
            }
        }
        std::string payload = joined.substr(close + 1);
        const size_t payloadUnits = Utf16Length(payload);
        const size_t payloadBytes = payload.size();
        // The original's validity check, quirks included: "chars & bytes" is
        // a bitwise AND (both nonzero AND sharing at least one bit — always
        // true in practice for two nearly equal sizes).
        const bool valid = ((chars & bytes) != 0) &&
                           payloadUnits == chars && payloadBytes == bytes;
        AppendLog(log, "Read size: " + std::to_string(payloadUnits) +
                       " (Bytes: " + std::to_string(payloadBytes) + ") in " +
                       std::to_string(chunksRead) + " chunks. Check: chars=" +
                       std::to_string(chars) + ", bytes=" + std::to_string(bytes) +
                       ", result=" + (valid ? "VALID" : "FAILED") + ".");
        if (valid) return payload;
        AppendLog(log, "Dropped string with length=" + std::to_string(payloadUnits) + "!");
        return std::nullopt;
    }

    bool WriteChunked(
        const std::string& keyBase, int maxChunks, size_t maxBytes,
        const std::string& payload,
        const std::function<std::string(const std::string&)>& getSetting,
        const std::function<void(const std::string&, const std::string&)>& setSetting,
        std::string* log) {

        if (maxChunks <= 0) return false;

        // The original zeroes an "i<keyBase>Size" int first; keep that write
        // for byte-for-byte file compatibility.
        setSetting("i" + keyBase + "Size", "0");

        std::string full = "[chars=" + std::to_string(Utf16Length(payload)) +
                           ";bytes=" + std::to_string(payload.size()) + "]" + payload;

        size_t posByte = 0;
        int chunkIdx = 0;
        bool overflow = false;

        while (posByte < full.size()) {
            if (chunkIdx >= maxChunks) {
                overflow = true;
                break;
            }
            // Port of the original block-stepping split: take 500-UTF16-unit
            // blocks while the accumulated UTF-8 byte size stays under
            // maxBytes; the chunk is every complete block before the one
            // that crossed the limit. When nothing crosses (short remainder)
            // the scan runs to the 500*floor(maxBytes/500) cap, covering the
            // whole remainder.
            size_t goodUnits = 0;
            size_t scanByte = posByte;
            size_t accumulatedBytes = 0;
            for (size_t offUnits = 0; offUnits < maxBytes; offUnits += 500) {
                const size_t blockEnd = AdvanceUtf16(full, scanByte, 500);
                accumulatedBytes += blockEnd - scanByte;
                scanByte = blockEnd;
                if (accumulatedBytes >= maxBytes) break;
                goodUnits = offUnits;
            }
            if (goodUnits == 0) {
                // First block alone hit the limit — the original aborts here.
                AppendLog(log, "Chunk split failed (block too large).");
                return false;
            }
            const size_t chunkEnd = AdvanceUtf16(full, posByte, goodUnits);
            const size_t takeBytes = std::min(chunkEnd, full.size()) - posByte;
            const std::string value = "<" + full.substr(posByte, takeBytes) + ">";
            const std::string key = "s" + keyBase + "_" + std::to_string(chunkIdx);
            setSetting(key, value);
            if (getSetting(key) != value) {
                AppendLog(log, "Failed to write INI key '" + key + "'.");
                return false;
            }
            posByte += takeBytes;
            if (value.size() == 2) break;  // wrote "<>" — nothing left
            ++chunkIdx;
        }

        // Clear leftover chunks from a previous, longer value. (The original
        // intends this but its loop reuses the wrong index variable and only
        // clears the first stale chunk; readers stop at the first short
        // chunk either way, so clearing all of them is strictly compatible.)
        for (int i = chunkIdx; i < maxChunks; ++i) {
            const std::string key = "s" + keyBase + "_" + std::to_string(i);
            if (!getSetting(key).empty()) {
                setSetting(key, "");
            }
        }

        AppendLog(log, "Written size: " + std::to_string(Utf16Length(full)) +
                       " (Bytes: " + std::to_string(full.size()) + ") in " +
                       std::to_string(chunkIdx) + " chunks." +
                       (overflow ? " OVERFLOW: payload truncated!" : ""));
        return !overflow;
    }

}
