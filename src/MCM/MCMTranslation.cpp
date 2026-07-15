#include "MCM/MCMTranslation.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <fstream>
#include <regex>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <codecvt>
#include <locale>

namespace MCMTranslation {

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
            map[key] = value;
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

        // UTF-8 BOM: EF BB BF — or plain ASCII/UTF-8
        TranslationMap map;
        std::ifstream file(path);
        if (!file.is_open()) return map;

        // Skip UTF-8 BOM if present
        char first3[3] = {0};
        file.read(first3, 3);
        if (!(static_cast<unsigned char>(first3[0]) == 0xEF &&
              static_cast<unsigned char>(first3[1]) == 0xBB &&
              static_cast<unsigned char>(first3[2]) == 0xBF)) {
            file.seekg(0); // No BOM, rewind
        }

        std::string line;
        while (std::getline(file, line)) {
            ParseTranslationLine(line, map);
        }

        return map;
    }

    TranslationMap LoadDirectory(const std::filesystem::path& dir) {
        TranslationMap merged;

        if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
            return merged;
        }

        try {
            for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                if (!entry.is_regular_file()) continue;

                auto filename = entry.path().filename().string();
                // Load files ending in _en.txt or Translate_en.txt or similar
                auto ext = entry.path().extension().string();
                if (ext != ".txt" && ext != ".TXT") continue;

                auto fileMap = LoadFile(entry.path());
                for (auto& [k, v] : fileMap) {
                    merged[k] = std::move(v);
                }
            }
        } catch (const std::filesystem::filesystem_error&) {
            // Directory enumeration can fail under virtual filesystems (MO2 USVFS)
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
        // enumerating the directory (MO2 USVFS may not support enumeration)
        std::vector<std::string> candidates = {
            modName + "_en.txt",
            modName + "_EN.txt",
            "MCM_" + modName + "_en.txt",
            "MCM_" + modName + "_EN.txt",
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

        return map;
    }

} // namespace MCMTranslation
