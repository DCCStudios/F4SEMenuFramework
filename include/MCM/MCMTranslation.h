#pragma once

#include <string>
#include <unordered_map>
#include <filesystem>

// Handles loading MCM translation files and resolving $Key / ${Key} tokens
// in display strings. Also provides HTML/Scaleform tag stripping for
// MCM text that uses Flash markup.
namespace MCMTranslation {

    // Map of translation keys (without leading $) to their resolved values
    using TranslationMap = std::unordered_map<std::string, std::string>;

    // Sets the active game language code (e.g. "en", "de", "ptbr") used by
    // LoadDirectory / LoadForMod file selection. Lowercased and trimmed
    // internally; defaults to "en" when never called. Injected from
    // MCMRegistry so this unit stays free of CommonLibF4 dependencies for
    // the offline test harness.
    void SetLanguage(const std::string& lang);
    const std::string& GetLanguage();

    // Resolves the language the player actually configured. Fallout4Custom.ini
    // often holds sLanguage while GetINISetting("sLanguage:General") still
    // reports the Fallout4.ini value ("en") — Custom.ini overrides are not
    // always reflected in the Setting collection. Order: Custom.ini, then
    // Fallout4.ini (both under Documents\My Games\Fallout4\, MO2-virtualized
    // when running through MO2), then optionalSettingFallback (typically
    // GetINISetting), then "en". Whitespace is trimmed.
    std::string ResolveGameLanguage(const std::string& optionalSettingFallback = {});

    // Returns content unchanged when it is already valid UTF-8. Otherwise the
    // bytes are legacy-codepage text (Korean translation files are commonly
    // saved as ANSI/CP949 on Korean Windows) and are converted to UTF-8 via
    // the configured legacy codepage — by default the machine's ANSI codepage
    // (CP_ACP), which on the player's machine matches the language of the
    // mods they install. Without this, ImGui renders every non-ASCII byte as
    // a U+FFFD replacement diamond.
    std::string EnsureUtf8(const std::string& content);

    // Overrides the codepage EnsureUtf8 converts from (0 = machine default).
    // Exists for the offline test harness; the game build uses the default.
    void SetLegacyCodepage(unsigned int codepage);

    // Script-coverage bits reported by DetectScripts. Values are shared with
    // FontManager::RequestScriptCoverage (kept here so this unit stays free
    // of ImGui/game dependencies for the offline harness).
    enum ScriptMask : unsigned int {
        kScriptCyrillic   = 1u << 0,  // U+0400-052F
        kScriptChinese    = 1u << 1,  // CJK unified ideographs (incl. kanji/hanja)
        kScriptJapanese   = 1u << 2,  // Hiragana/Katakana
        kScriptKorean     = 1u << 3,  // Hangul jamo + syllables
        kScriptThai       = 1u << 4,  // U+0E00-0E7F
        kScriptGreek      = 1u << 5,  // U+0370-03FF
        kScriptVietnamese = 1u << 6,  // Latin Extended Additional U+1E00-1EFF
    };

    // Scans a UTF-8 string and reports which non-Latin scripts appear in it.
    // Used to enable matching font glyph ranges even when the game language
    // setting doesn't announce them (e.g. Korean translation mods installed
    // on an sLanguage=en game).
    unsigned int DetectScripts(const std::string& utf8);

    // Load a single translation file.
    // Format: tab-separated lines of "$Key\tValue" (lines without tabs or
    // not starting with $ are skipped). Literal \n and \t escapes in values
    // are decoded to real newlines/tabs (matching the game's Scaleform
    // translator behavior).
    TranslationMap LoadFile(const std::filesystem::path& path);

    // Load translation files from a directory, merging into one map:
    // *_en.txt (and files with no language suffix) load first as the base,
    // then the active language's *_<lang>.txt files override per key.
    // Other languages' files are skipped.
    TranslationMap LoadDirectory(const std::filesystem::path& dir);

    // Resolve translation tokens in a string:
    //  - If the entire string is "$Key", look up Key in map and return value
    //  - Inline "${Key}" interpolations are replaced with their looked-up values
    //  - Bare "$Key" tokens at word boundaries are also resolved
    // Unresolved tokens fall through to HumanizeFallback.
    std::string Resolve(const std::string& input, const TranslationMap& map);

    // Strip HTML/Scaleform tags: <font ...>, </font>, <b>, </b>,
    // <p ...>, </p>, <br>, <br />, etc. Preserves inner text content.
    // Also converts {newline} to actual newlines.
    std::string StripHTML(const std::string& input);

    // Fallback for unresolved tokens: strips $ prefix, removes common
    // mod-name prefixes (e.g. "KARM_MCM_Setting_"), and converts
    // underscores/CamelCase to spaced words.
    std::string HumanizeFallback(const std::string& token);

    // Convenience: Resolve then StripHTML in one call
    std::string ResolveAndStrip(const std::string& input, const TranslationMap& map);

    // Try to load translation files for a specific mod by probing known naming
    // conventions under Data/Interface/Translations/ — English first, then the
    // active language's file overriding per key. Returns entries found
    // (empty map if no file could be opened).
    TranslationMap LoadForMod(const std::string& modName);

}
