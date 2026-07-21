#pragma once

#include <cctype>
#include <string>

// Lenient-JSON sanitizer for MCM config.json / keybinds.json.
//
// The real MCM's UI parses these files with as3corelib's JSON decoder in
// NON-STRICT mode (MCM_0.1_AS3/com/adobe/serialization/json), which tolerates
// three things strict parsers reject — and shipped mods rely on all of them:
//   (a) unknown backslash escapes are passed through verbatim
//       ("...Documents\My Games\Fallout4..." — Active Effects on HUD),
//   (b) a trailing comma before ] or } (Workshop Plus keybinds.json),
//   (c) // and /* */ comments (Jump Grunt, Elzee Recoil Shake, ...).
// nlohmann::json's ignore_comments handles only (c), so this pre-pass rewrites
// the text into strict JSON: comments are removed, trailing commas dropped,
// and an invalid escape "\X" inside a string becomes "\\X" — a literal
// backslash + char, exactly the value AS3's pass-through produced.
//
// Header-only and std-only so the offline test harness can exercise it
// against real mod files without the game.
namespace MCMJson {

    inline std::string SanitizeLenientJson(const std::string& in) {
        std::string out;
        out.reserve(in.size());
        const std::size_t n = in.size();
        std::size_t i = 0;
        bool inStr = false;

        auto isHex = [](char c) {
            return std::isxdigit(static_cast<unsigned char>(c)) != 0;
        };

        while (i < n) {
            const char c = in[i];

            if (inStr) {
                if (c == '\\' && i + 1 < n) {
                    const char e = in[i + 1];
                    const bool valid =
                        e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' ||
                        e == 'n' || e == 'r' || e == 't' ||
                        (e == 'u' && i + 5 < n && isHex(in[i + 2]) && isHex(in[i + 3]) &&
                         isHex(in[i + 4]) && isHex(in[i + 5]));
                    if (valid) {
                        out += '\\';
                        out += e;
                    } else {
                        // Invalid escape -> literal backslash + char (AS3 behavior)
                        out += '\\';
                        out += '\\';
                        out += e;
                    }
                    i += 2;
                    continue;
                }
                if (c == '"') {
                    inStr = false;
                }
                out += c;
                ++i;
                continue;
            }

            if (c == '"') {
                inStr = true;
                out += c;
                ++i;
                continue;
            }

            // Comments (outside strings): drop entirely.
            if (c == '/' && i + 1 < n && in[i + 1] == '/') {
                while (i < n && in[i] != '\n') ++i;
                continue;
            }
            if (c == '/' && i + 1 < n && in[i + 1] == '*') {
                i += 2;
                while (i + 1 < n && !(in[i] == '*' && in[i + 1] == '/')) ++i;
                i = (i + 1 < n) ? i + 2 : n;
                continue;
            }

            // Trailing comma: look past whitespace/comments — if the next real
            // character closes the container, the comma must go.
            if (c == ',') {
                std::size_t j = i + 1;
                for (;;) {
                    while (j < n && std::isspace(static_cast<unsigned char>(in[j]))) ++j;
                    if (j + 1 < n && in[j] == '/' && in[j + 1] == '/') {
                        while (j < n && in[j] != '\n') ++j;
                        continue;
                    }
                    if (j + 1 < n && in[j] == '/' && in[j + 1] == '*') {
                        j += 2;
                        while (j + 1 < n && !(in[j] == '*' && in[j + 1] == '/')) ++j;
                        j = (j + 1 < n) ? j + 2 : n;
                        continue;
                    }
                    break;
                }
                if (j < n && (in[j] == ']' || in[j] == '}')) {
                    ++i;  // drop the comma; the closer is emitted on its own turn
                    continue;
                }
                out += c;
                ++i;
                continue;
            }

            out += c;
            ++i;
        }
        return out;
    }

}  // namespace MCMJson
