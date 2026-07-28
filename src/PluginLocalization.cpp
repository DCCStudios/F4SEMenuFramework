#include "PluginLocalization.h"

#include "MCM/MCMTranslation.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace PluginLocalization {

    namespace {

        // One plugin's merged string table. std::map node stability keeps the
        // const char* pointers handed out by Get() valid until Reset().
        struct Table {
            bool loaded = false;
            int loadResult = -1;                       // last Load() return value
            std::map<std::string, std::string> strings;
            std::map<std::string, std::string> misses; // interned unknown keys
        };

        std::mutex s_mutex;
        std::map<std::string, Table> s_tables;         // pluginName -> table
        std::string s_root = "Data/F4SE/Plugins";
        std::string s_language;                        // empty = not resolved yet
        std::atomic<unsigned long long> s_generation{1};

        // Reads a whole file as bytes; empty optional-style flag via bool.
        bool ReadFile(const std::filesystem::path& p, std::string& out) {
            std::ifstream f(p, std::ios::binary);
            if (!f.is_open()) return false;
            std::ostringstream ss;
            ss << f.rdbuf();
            out = ss.str();
            return true;
        }

        // Parses one flat key/value JSON file into `into`, overriding
        // existing keys. Returns false when the file is absent; malformed
        // content is swallowed (never crashes the game) and counts as
        // "present but contributed nothing".
        bool MergeFile(const std::filesystem::path& p,
                       std::map<std::string, std::string>& into) {
            std::string raw;
            if (!ReadFile(p, raw)) return false;

            // Community files are often saved in the machine's ANSI codepage;
            // convert so ImGui does not render replacement diamonds.
            raw = MCMTranslation::EnsureUtf8(raw);

            try {
                auto j = nlohmann::json::parse(raw); // handles a UTF-8 BOM
                if (!j.is_object()) return true;
                for (auto& [k, v] : j.items()) {
                    if (v.is_string()) {
                        into[k] = v.get<std::string>();
                    }
                }
            } catch (const std::exception&) {
                // Malformed JSON: skip the file, keep whatever else loaded.
            }
            return true;
        }

        // Resolves the active language once (from the game INIs) and caches
        // it. Caller holds s_mutex.
        const std::string& LanguageLocked() {
            if (s_language.empty()) {
                s_language = MCMTranslation::ResolveGameLanguage();
                if (s_language.empty()) s_language = "en";
            }
            return s_language;
        }

        // Loads (or reloads) one plugin's merged table. Caller holds s_mutex.
        void LoadLocked(const std::string& pluginName, Table& t) {
            t.strings.clear();
            t.misses.clear();

            const auto dir = std::filesystem::path(s_root) / pluginName / "Languages";
            const bool haveBase = MergeFile(dir / "en.json", t.strings);

            bool haveLang = false;
            const std::string& lang = LanguageLocked();
            if (!lang.empty() && lang != "en") {
                haveLang = MergeFile(dir / (lang + ".json"), t.strings);
            }

            t.loaded = true;
            t.loadResult = (haveBase || haveLang)
                               ? static_cast<int>(t.strings.size())
                               : -1;
            s_generation.fetch_add(1, std::memory_order_relaxed);
        }

        Table& EnsureLoaded(const std::string& pluginName) {
            auto& t = s_tables[pluginName];
            if (!t.loaded) LoadLocked(pluginName, t);
            return t;
        }

    }

    void SetRootPath(const std::string& root) {
        std::lock_guard lock(s_mutex);
        s_root = root;
        // Tables cached against the old root are stale.
        for (auto& [_, t] : s_tables) t.loaded = false;
        s_generation.fetch_add(1, std::memory_order_relaxed);
    }

    void SetLanguage(const std::string& lang) {
        std::lock_guard lock(s_mutex);
        s_language = lang.empty() ? "en" : lang;
        for (auto& [_, t] : s_tables) t.loaded = false;
        s_generation.fetch_add(1, std::memory_order_relaxed);
    }

    const std::string& GetLanguage() {
        std::lock_guard lock(s_mutex);
        return LanguageLocked();
    }

    int Load(const std::string& pluginName) {
        std::lock_guard lock(s_mutex);
        auto& t = s_tables[pluginName];
        LoadLocked(pluginName, t);
        return t.loadResult;
    }

    const char* Get(const std::string& pluginName, const char* key) {
        if (!key) return "";
        std::lock_guard lock(s_mutex);
        auto& t = EnsureLoaded(pluginName);

        if (auto it = t.strings.find(key); it != t.strings.end()) {
            return it->second.c_str();
        }
        // Intern the miss so the returned pointer outlives the caller's
        // argument (plugins may pass transient buffers, not just literals).
        auto [it, _] = t.misses.try_emplace(key, key);
        return it->second.c_str();
    }

    const char* TryGet(const std::string& pluginName, const char* key) {
        if (!key || !*key) return nullptr;
        std::lock_guard lock(s_mutex);
        auto& t = EnsureLoaded(pluginName);
        if (auto it = t.strings.find(key); it != t.strings.end()) {
            return it->second.c_str();
        }
        return nullptr;
    }

    int KeyCount(const std::string& pluginName) {
        std::lock_guard lock(s_mutex);
        return static_cast<int>(EnsureLoaded(pluginName).strings.size());
    }

    unsigned long long Generation() {
        return s_generation.load(std::memory_order_relaxed);
    }

    void Reset(const std::string& pluginName) {
        std::lock_guard lock(s_mutex);
        if (auto it = s_tables.find(pluginName); it != s_tables.end()) {
            it->second.loaded = false;
        }
        s_generation.fetch_add(1, std::memory_order_relaxed);
    }

    // ---- Pure helpers ----

    namespace {

        // Appends the printf-vararg signature of `s` to `sig`, one token per
        // argument-consuming or malformed '%' construct. Returns false when
        // the string contains '%n' (which writes through a pointer argument
        // and is never acceptable in a translation).
        bool AppendFormatSignature(const char* s, std::string& sig) {
            for (const char* p = s; *p; ++p) {
                if (*p != '%') continue;
                ++p;
                if (*p == '%') continue;        // literal %%: consumes nothing
                if (*p == '\0') {               // trailing bare '%': malformed
                    sig += "!;";
                    break;
                }
                // Flags (cosmetic, excluded from the signature).
                while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') ++p;
                // Width: '*' consumes an int argument, digits are cosmetic.
                if (*p == '*') { sig += '*'; ++p; }
                else while (*p >= '0' && *p <= '9') ++p;
                // Precision: same rule.
                if (*p == '.') {
                    ++p;
                    if (*p == '*') { sig += '*'; ++p; }
                    else while (*p >= '0' && *p <= '9') ++p;
                }
                // Length modifier: changes the argument size, so it is part
                // of the signature ("%d" vs "%lld" read different bytes).
                if (*p == 'h') { sig += 'h'; ++p; if (*p == 'h') { sig += 'h'; ++p; } }
                else if (*p == 'l') { sig += 'l'; ++p; if (*p == 'l') { sig += 'l'; ++p; } }
                else if (*p == 'j' || *p == 'z' || *p == 't' || *p == 'L' ||
                         *p == 'I' || *p == 'w') { sig += *p; ++p; }
                // Conversion character.
                if (*p == '\0') { sig += "!;"; break; }
                if (*p == 'n') return false;
                if (std::strchr("diouxXfFeEgGaAcsp", *p)) {
                    sig += *p;
                    sig += ';';
                } else {
                    // Unknown conversion: undefined printf behavior. Treat as
                    // an opaque token that must match positionally so a
                    // translation is never *more* dangerous than the original.
                    sig += '!';
                    sig += ';';
                }
            }
            return true;
        }

    }

    bool FormatSpecsCompatible(const char* original, const char* translated) {
        if (!original || !translated) return false;
        std::string a, b;
        if (!AppendFormatSignature(original, a)) return false;
        if (!AppendFormatSignature(translated, b)) return false;
        return a == b;
    }

    std::string SplitVisibleLabel(const char* label, std::string* suffix) {
        if (suffix) suffix->clear();
        if (!label) return {};
        // ImGui semantics: everything from the first "##" onward is the ID
        // portion ("###" also begins with "##") and is never displayed.
        if (const char* hash = std::strstr(label, "##")) {
            if (suffix) *suffix = hash;
            return std::string(label, hash);
        }
        return label;
    }

}
