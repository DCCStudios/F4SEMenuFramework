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

    // Load a single translation file.
    // Format: tab-separated lines of "$Key\tValue" (lines without tabs or
    // not starting with $ are skipped).
    TranslationMap LoadFile(const std::filesystem::path& path);

    // Load all *_en.txt files from a directory, merging into one map.
    // Later files overwrite earlier entries on collision.
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

    // Try to load a translation file for a specific mod by probing known naming
    // conventions under Data/Interface/Translations/. Returns entries found
    // (empty map if no file could be opened).
    TranslationMap LoadForMod(const std::string& modName);

}
