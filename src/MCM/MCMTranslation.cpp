#include "MCM/MCMTranslation.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <fstream>
#include <regex>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <codecvt>
#include <locale>

namespace MCMTranslation {

    // ------------------------------------------------------------------
    // Active language
    //
    // Kept as injected state (set from MCMRegistry via the game's
    // sLanguage:General INI setting) because this translation unit is also
    // compiled standalone for the offline test harness and must stay free
    // of CommonLibF4 dependencies. Defaults to English.
    // ------------------------------------------------------------------
    static std::string s_language = "en";

    // Trim ASCII whitespace and lowercase — Custom.ini often has
    // "sLanguage = es" (spaces around =) which GetPrivateProfileString may
    // return with padding; suffix matching needs a clean code.
    static std::string NormalizeLanguage(std::string lang) {
        while (!lang.empty() && std::isspace(static_cast<unsigned char>(lang.front()))) {
            lang.erase(lang.begin());
        }
        while (!lang.empty() && std::isspace(static_cast<unsigned char>(lang.back()))) {
            lang.pop_back();
        }
        std::transform(lang.begin(), lang.end(), lang.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lang;
    }

    void SetLanguage(const std::string& lang) {
        const std::string lowered = NormalizeLanguage(lang);
        s_language = lowered.empty() ? "en" : lowered;
    }

    const std::string& GetLanguage() {
        return s_language;
    }

    static std::string ReadLanguageFromIniFile(const wchar_t* fileName) {
        wchar_t docs[MAX_PATH]{};
        // CSIDL_PERSONAL is the same Documents folder the game (and MO2 USVFS)
        // use — so Fallout4Custom.ini resolves to the active MO2 profile file.
        if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs))) {
            return {};
        }
        const auto path = std::filesystem::path(docs) / L"My Games" / L"Fallout4" / fileName;
        char buf[64]{};
        ::GetPrivateProfileStringA("General", "sLanguage", "", buf, static_cast<DWORD>(sizeof(buf)),
            path.string().c_str());
        return NormalizeLanguage(buf);
    }

    std::string ResolveGameLanguage(const std::string& optionalSettingFallback) {
        // Custom.ini first: that is where players (and MO2 profiles) put
        // sLanguage overrides. The in-memory Setting from GetINISetting often
        // still holds Fallout4.ini's "en" even when Custom says otherwise —
        // verified 2026-07-21: Spanish pause menu + English MCM load, log had
        // no "Game language 'es'" line despite profile Custom.ini sLanguage=es.
        if (auto custom = ReadLanguageFromIniFile(L"Fallout4Custom.ini"); !custom.empty()) {
            return custom;
        }
        if (auto base = ReadLanguageFromIniFile(L"Fallout4.ini"); !base.empty()) {
            return base;
        }
        auto fromSetting = NormalizeLanguage(optionalSettingFallback);
        if (!fromSetting.empty()) {
            return fromSetting;
        }
        return "en";
    }

    // Convert a UTF-16 LE wstring to a UTF-8 std::string
    static std::string WideToUtf8(const std::wstring& wstr) {
        if (wstr.empty()) return {};
        int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.data(),
            static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
        if (sizeNeeded <= 0) return {};
        std::string result(sizeNeeded, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wstr.data(),
            static_cast<int>(wstr.size()), result.data(), sizeNeeded, nullptr, nullptr);
        return result;
    }

    // ------------------------------------------------------------------
    // Legacy-codepage handling
    //
    // Community translation files (especially Korean ones, often shipped as
    // replacement *_en.txt) are frequently saved in the translator's ANSI
    // codepage (CP949) instead of UTF-8/UTF-16. Those bytes are invalid
    // UTF-8, so ImGui rendered every character as the U+FFFD replacement
    // diamond. EnsureUtf8 detects that and converts via the machine's ANSI
    // codepage — on the player's machine that matches the language of the
    // mods they install (a Korean player's Windows is CP949).
    // ------------------------------------------------------------------

    static unsigned int s_legacyCodepage = 0;  // 0 = CP_ACP (machine default)

    void SetLegacyCodepage(unsigned int codepage) {
        s_legacyCodepage = codepage;
    }

    std::string EnsureUtf8(const std::string& content) {
        if (content.empty()) return content;

        // Fast path: already valid UTF-8 (covers pure ASCII too). The OS
        // decoder with MB_ERR_INVALID_CHARS is the validation.
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                content.data(), static_cast<int>(content.size()), nullptr, 0) > 0) {
            return content;
        }

        const UINT cp = s_legacyCodepage != 0 ? s_legacyCodepage : CP_ACP;
        int wlen = MultiByteToWideChar(cp, 0, content.data(),
            static_cast<int>(content.size()), nullptr, 0);
        if (wlen <= 0) return content;  // can't convert — keep original bytes

        std::wstring wide(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(cp, 0, content.data(),
            static_cast<int>(content.size()), wide.data(), wlen);
        return WideToUtf8(wide);
    }

    unsigned int DetectScripts(const std::string& utf8) {
        unsigned int mask = 0;
        const auto* s = reinterpret_cast<const unsigned char*>(utf8.data());
        size_t i = 0;
        const size_t n = utf8.size();
        while (i < n) {
            unsigned int cp = 0;
            unsigned char c = s[i];
            if (c < 0x80) {
                cp = c;
                i += 1;
            } else if ((c & 0xE0) == 0xC0 && i + 1 < n) {
                cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu);
                i += 2;
            } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
                cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu);
                i += 3;
            } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
                cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) |
                     ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3Fu);
                i += 4;
            } else {
                i += 1;  // invalid byte — skip
                continue;
            }

            if (cp >= 0x0400 && cp <= 0x052F)      mask |= kScriptCyrillic;
            else if (cp >= 0x0370 && cp <= 0x03FF) mask |= kScriptGreek;
            else if (cp >= 0x0E00 && cp <= 0x0E7F) mask |= kScriptThai;
            else if (cp >= 0x3040 && cp <= 0x30FF) mask |= kScriptJapanese;   // kana
            else if ((cp >= 0x4E00 && cp <= 0x9FFF) ||
                     (cp >= 0x3400 && cp <= 0x4DBF)) mask |= kScriptChinese;  // ideographs (also kanji/hanja)
            else if ((cp >= 0xAC00 && cp <= 0xD7A3) ||
                     (cp >= 0x1100 && cp <= 0x11FF) ||
                     (cp >= 0x3130 && cp <= 0x318F)) mask |= kScriptKorean;   // hangul
            else if (cp >= 0x1E00 && cp <= 0x1EFF)  mask |= kScriptVietnamese;
        }
        return mask;
    }

    // Translation values use literal two-character escapes for formatting:
    // the game's Scaleform translator turns "\n" into a real line break when
    // it hands the string to Flash (e.g. UneducatedShooter_en.txt's landing
    // page text). Decode them here so ImGui receives real newlines/tabs
    // instead of printing "\n" verbatim. Unknown escapes are left untouched
    // (a backslash in prose or a path must survive as-is).
    static std::string DecodeEscapes(const std::string& in) {
        if (in.find('\\') == std::string::npos) return in;
        std::string out;
        out.reserve(in.size());
        for (size_t i = 0; i < in.size(); ++i) {
            if (in[i] == '\\' && i + 1 < in.size()) {
                const char next = in[i + 1];
                if (next == 'n') { out += '\n'; ++i; continue; }
                if (next == 't') { out += '\t'; ++i; continue; }
                if (next == 'r') { ++i; continue; }  // CR adds nothing in ImGui
            }
            out += in[i];
        }
        return out;
    }

    // Parse a single UTF-8 line in "$Key\tValue" format and insert into map
    static void ParseTranslationLine(const std::string& line, TranslationMap& map) {
        if (line.empty()) return;
        if (line[0] != '$') return;

        auto tabPos = line.find('\t');
        if (tabPos == std::string::npos) return;

        std::string key = line.substr(1, tabPos - 1);
        std::string value = line.substr(tabPos + 1);

        // Trim trailing \r or whitespace
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
            value.pop_back();

        if (!key.empty()) {
            map[key] = DecodeEscapes(value);
        }
    }

    // Load a UTF-16 LE file (with or without BOM already consumed)
    static TranslationMap LoadFileUTF16LE(const std::filesystem::path& path) {
        TranslationMap map;

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return map;

        // Read entire file as bytes
        file.seekg(0, std::ios::end);
        auto fileSize = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<char> buffer(static_cast<size_t>(fileSize));
        file.read(buffer.data(), fileSize);

        // Skip BOM if present (FF FE)
        size_t startOffset = 0;
        if (buffer.size() >= 2 &&
            static_cast<unsigned char>(buffer[0]) == 0xFF &&
            static_cast<unsigned char>(buffer[1]) == 0xFE) {
            startOffset = 2;
        }

        // Interpret as wchar_t (UTF-16 LE on Windows)
        size_t wcharCount = (buffer.size() - startOffset) / 2;
        const wchar_t* wdata = reinterpret_cast<const wchar_t*>(buffer.data() + startOffset);

        // Convert to UTF-8 string
        std::wstring wstr(wdata, wcharCount);
        std::string utf8Content = WideToUtf8(wstr);

        // Parse line by line
        std::istringstream stream(utf8Content);
        std::string line;
        while (std::getline(stream, line)) {
            ParseTranslationLine(line, map);
        }

        return map;
    }

    TranslationMap LoadFile(const std::filesystem::path& path) {
        // Detect encoding by reading first 2 bytes
        std::ifstream probe(path, std::ios::binary);
        if (!probe.is_open()) return {};

        unsigned char bom[2] = {0, 0};
        probe.read(reinterpret_cast<char*>(bom), 2);
        probe.close();

        // UTF-16 LE BOM: FF FE
        if (bom[0] == 0xFF && bom[1] == 0xFE) {
            return LoadFileUTF16LE(path);
        }

        // UTF-8 BOM: EF BB BF — or plain ASCII/UTF-8 — or a legacy ANSI
        // codepage (common for community Korean/Chinese translation files).
        TranslationMap map;
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return map;

        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

        // Skip UTF-8 BOM if present
        if (content.size() >= 3 &&
            static_cast<unsigned char>(content[0]) == 0xEF &&
            static_cast<unsigned char>(content[1]) == 0xBB &&
            static_cast<unsigned char>(content[2]) == 0xBF) {
            content.erase(0, 3);
        }

        // Legacy-codepage files (CP949 etc.) are converted to UTF-8 here;
        // valid UTF-8 passes through untouched.
        content = EnsureUtf8(content);

        std::istringstream stream(content);
        std::string line;
        while (std::getline(stream, line)) {
            ParseTranslationLine(line, map);
        }

        return map;
    }

    // Extracts the lowercase language suffix from "<name>_<lang>.txt"
    // (e.g. "UneducatedShooter_de.txt" -> "de"). Empty when the stem has no
    // plausible suffix (no underscore, or the tail isn't 2-5 letters).
    static std::string LanguageSuffixOf(const std::filesystem::path& file) {
        auto stem = file.stem().string();
        auto us = stem.rfind('_');
        if (us == std::string::npos) return {};
        std::string tail = stem.substr(us + 1);
        if (tail.size() < 2 || tail.size() > 5) return {};
        for (char& c : tail) {
            if (!std::isalpha(static_cast<unsigned char>(c))) return {};
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return tail;
    }

    TranslationMap LoadDirectory(const std::filesystem::path& dir) {
        TranslationMap merged;

        if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
            return merged;
        }

        // Mirror the game's per-language file selection: English files form
        // the base (files without a recognizable language suffix count as
        // base too), and — when the game runs in another language — that
        // language's files override per key, so partially translated files
        // still fall back to English for missing entries.
        std::vector<std::filesystem::path> baseFiles;
        std::vector<std::filesystem::path> langFiles;
        try {
            for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                if (!entry.is_regular_file()) continue;

                auto ext = entry.path().extension().string();
                if (ext != ".txt" && ext != ".TXT") continue;

                const std::string suffix = LanguageSuffixOf(entry.path());
                if (suffix.empty() || suffix == "en") {
                    baseFiles.push_back(entry.path());
                } else if (suffix == s_language) {
                    langFiles.push_back(entry.path());
                }
                // Other languages' files are skipped, as the game would.
            }
        } catch (const std::filesystem::filesystem_error&) {
            // Directory enumeration can fail under virtual filesystems (MO2 USVFS)
        }

        for (const auto* group : { &baseFiles, &langFiles }) {
            for (const auto& path : *group) {
                auto fileMap = LoadFile(path);
                for (auto& [k, v] : fileMap) {
                    merged[k] = std::move(v);
                }
            }
        }

        return merged;
    }

    std::string HumanizeFallback(const std::string& token) {
        if (token.empty()) return token;

        std::string key = token;

        // Strip leading $ if present
        if (key[0] == '$') {
            key = key.substr(1);
        }

        // Remove common MCM prefixes: ModName_MCM_Setting_, ModName_MCM_Section_, etc.
        // Pattern: anything before and including "_MCM_" followed by type prefix
        static const std::vector<std::string> prefixPatterns = {
            "_MCM_Setting_", "_MCM_Section_", "_MCM_Header_",
            "_MCM_Stepper_Option_", "_MCM_Page_"
        };

        for (const auto& prefix : prefixPatterns) {
            auto pos = key.find(prefix);
            if (pos != std::string::npos) {
                key = key.substr(pos + prefix.length());
                break;
            }
        }

        // Replace underscores with spaces
        std::replace(key.begin(), key.end(), '_', ' ');

        // Insert spaces before uppercase letters in CamelCase
        // (but not for consecutive uppercase like "FX" or "HUD")
        std::string result;
        for (size_t i = 0; i < key.size(); ++i) {
            char c = key[i];
            if (i > 0 && std::isupper(static_cast<unsigned char>(c))) {
                char prev = key[i - 1];
                // Insert space if previous char is lowercase, or if next char is lowercase
                // (handles "CamelCase" and "HTMLParser" → "HTML Parser")
                bool prevLower = std::islower(static_cast<unsigned char>(prev));
                bool nextLower = (i + 1 < key.size()) && std::islower(static_cast<unsigned char>(key[i + 1]));
                if (prevLower || (nextLower && !std::isspace(static_cast<unsigned char>(prev)))) {
                    result += ' ';
                }
            }
            result += c;
        }

        // Trim leading/trailing spaces
        auto start = result.find_first_not_of(' ');
        auto end = result.find_last_not_of(' ');
        if (start == std::string::npos) return result;
        return result.substr(start, end - start + 1);
    }

    std::string Resolve(const std::string& input, const TranslationMap& map) {
        if (input.empty()) return input;

        // Case 1: Entire string is a single "$Key" token (no spaces, no other content)
        if (input[0] == '$' && input.find(' ') == std::string::npos &&
            input.find('{') == std::string::npos && input.find('}') == std::string::npos) {
            std::string key = input.substr(1);
            auto it = map.find(key);
            if (it != map.end()) {
                return it->second;
            }
            // Fallback: humanize the token
            return HumanizeFallback(input);
        }

        // Case 2: String may contain ${Key} interpolations and/or bare $Key tokens
        std::string result = input;

        // First pass: resolve ${Key} interpolations
        static const std::regex bracePattern(R"(\$\{([^}]+)\})");
        std::string pass1;
        std::sregex_iterator it(result.begin(), result.end(), bracePattern);
        std::sregex_iterator end;
        size_t lastPos = 0;

        for (; it != end; ++it) {
            auto& match = *it;
            pass1 += result.substr(lastPos, match.position() - lastPos);

            std::string key = match[1].str();
            auto mapIt = map.find(key);
            if (mapIt != map.end()) {
                pass1 += mapIt->second;
            } else {
                // Try without $ prefix in case the map stored it differently
                pass1 += HumanizeFallback("$" + key);
            }
            lastPos = match.position() + match.length();
        }
        pass1 += result.substr(lastPos);
        result = pass1;

        // Second pass: resolve bare $Key tokens (word boundary: $followed by alnum/underscore)
        static const std::regex barePattern(R"(\$([A-Za-z_][A-Za-z0-9_]*))");
        std::string pass2;
        std::sregex_iterator it2(result.begin(), result.end(), barePattern);
        lastPos = 0;

        for (; it2 != end; ++it2) {
            auto& match = *it2;
            pass2 += result.substr(lastPos, match.position() - lastPos);

            std::string key = match[1].str();
            auto mapIt = map.find(key);
            if (mapIt != map.end()) {
                pass2 += mapIt->second;
            } else {
                pass2 += HumanizeFallback("$" + key);
            }
            lastPos = match.position() + match.length();
        }
        pass2 += result.substr(lastPos);

        return pass2;
    }

    std::string StripHTML(const std::string& input) {
        if (input.empty()) return input;

        // Remove all HTML/XML-style tags: <...> and </...>
        static const std::regex tagPattern(R"(<[^>]*>)");
        std::string result = std::regex_replace(input, tagPattern, "");

        // Convert {newline} to actual newline
        static const std::regex newlinePattern(R"(\{newline\})", std::regex::icase);
        result = std::regex_replace(result, newlinePattern, "\n");

        // Trim leading/trailing whitespace that might have been left behind
        auto start = result.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        auto end = result.find_last_not_of(" \t\r\n");
        return result.substr(start, end - start + 1);
    }

    std::string ResolveAndStrip(const std::string& input, const TranslationMap& map) {
        if (input.empty()) return input;
        std::string resolved = Resolve(input, map);
        return StripHTML(resolved);
    }

    TranslationMap LoadForMod(const std::string& modName) {
        TranslationMap map;
        auto baseDir = std::filesystem::current_path() / "Data" / "Interface" / "Translations";

        // Try multiple naming conventions — open files directly instead of
        // enumerating the directory (MO2 USVFS may not support enumeration).
        // English loads first as the base; the active game language (when not
        // English) overrides per key, so partial translations keep English
        // for their missing entries.
        std::vector<std::string> languages = { "en" };
        if (s_language != "en") {
            languages.push_back(s_language);
        }

        for (const auto& lang : languages) {
            std::string upper = lang;
            std::transform(upper.begin(), upper.end(), upper.begin(),
                [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

            const std::vector<std::string> candidates = {
                modName + "_" + lang + ".txt",
                modName + "_" + upper + ".txt",
                "MCM_" + modName + "_" + lang + ".txt",
                "MCM_" + modName + "_" + upper + ".txt",
            };

            for (const auto& filename : candidates) {
                auto path = baseDir / filename;
                std::ifstream test(path);
                if (test.is_open()) {
                    test.close();
                    auto fileMap = LoadFile(path);
                    for (auto& [k, v] : fileMap) {
                        map[k] = std::move(v);
                    }
                }
            }
        }

        return map;
    }

} // namespace MCMTranslation
