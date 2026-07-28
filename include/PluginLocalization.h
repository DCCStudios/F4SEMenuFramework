#pragma once

#include <string>

// Per-plugin JSON string tables for third-party plugin menus (the public
// "Translate" API surfaced through F4SEMenuFramework.h).
//
// A plugin ships flat key/value JSON files at
//   Data/F4SE/Plugins/<PluginName>/Languages/<lang>.json
// and the framework serves lookups with this fallback chain:
//   active game language file -> en.json base -> the key itself.
//
// The key-itself fallthrough means en.json is OPTIONAL: authors who use
// English text as their keys ship no files at all and still render fine,
// while translators can later add es.json etc. mapping those English strings.
//
// This unit is intentionally logger-free and CommonLibF4-free (same policy
// as MCMTranslation.cpp) so the offline host test under swf/test/ can
// compile it standalone. The DLL export layer in F4SEMenuFramework.cpp does
// the logging.
namespace PluginLocalization {

    // Overrides the folder that holds the per-plugin Languages directories.
    // Default: "Data/F4SE/Plugins". Exists for the offline test harness.
    void SetRootPath(const std::string& root);

    // Overrides the active language code. Normally the code is resolved
    // lazily on first use via MCMTranslation::ResolveGameLanguage()
    // (Fallout4Custom.ini, then Fallout4.ini, then "en").
    void SetLanguage(const std::string& lang);

    // The active language code (resolving it first if needed). Returns a
    // reference to a static string, so .c_str() stays valid for the session.
    const std::string& GetLanguage();

    // Ensures a plugin's merged table is loaded from disk.
    // Returns the number of keys in the merged table, or -1 when no
    // translation file was found at all (neither en.json nor the active
    // language's file). -1 is informational, not an error: plugins that use
    // English text as keys legitimately ship no files.
    int Load(const std::string& pluginName);

    // Looks up a key for a plugin, lazy-loading its table on first use.
    // Fallback chain: active language value, then en.json value, then the
    // key itself. The returned pointer is stable until Reset() is called
    // for that plugin (missing keys are interned to guarantee this).
    const char* Get(const std::string& pluginName, const char* key);

    // Non-interning probe used by the automatic backend translation of
    // plugin ImGui calls: returns the translated value or nullptr on miss.
    // Unlike Get(), misses are NOT interned; the auto path sees arbitrary
    // per-frame dynamic strings and interning them would grow memory
    // without bound.
    const char* TryGet(const std::string& pluginName, const char* key);

    // Number of keys currently in a plugin's merged table (lazy-loads).
    // 0 means lookups can be skipped entirely (fast path).
    int KeyCount(const std::string& pluginName);

    // Monotonic counter bumped whenever any table content may have changed
    // (Load / Reset / SetLanguage / SetRootPath). Lets callers cache
    // per-plugin state and revalidate cheaply.
    unsigned long long Generation();

    // Drops one plugin's table so the next lookup re-reads from disk.
    // Invalidates pointers previously returned by Get() for that plugin.
    void Reset(const std::string& pluginName);

    // ---- Pure helpers (host-testable, no state) ----

    // True when `translated` consumes printf varargs exactly like
    // `original`: same ordered sequence of conversion specifiers ('*'
    // width/precision included, length modifiers included; flags and
    // literal widths may differ). Any '%n' on either side returns false.
    // Malformed '%' sequences are treated as opaque tokens that must match
    // positionally, so "100%" can translate to "100 %" but never to "%d".
    // Substituting a format string that fails this check would make
    // vsnprintf read garbage varargs, which is a crash, so callers must
    // fall back to `original` on false.
    bool FormatSpecsCompatible(const char* original, const char* translated);

    // Splits an ImGui label at its "##"/"###" ID suffix. Returns the
    // visible part; *suffix receives the "##..." remainder ("" when none).
    // A label that is only an ID ("##x") yields an empty visible part.
    std::string SplitVisibleLabel(const char* label, std::string* suffix);

}
