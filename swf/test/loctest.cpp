// Offline validator for the plugin localization module (PluginLocalization).
// Exercises load/merge/fallback logic against a scratch directory tree so
// the behavior is proven without a live game session.
//   Build: build_loctest.bat   Run: loctest.exe

#include "PluginLocalization.h"
#include "MCM/MCMTranslation.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int g_failures = 0;

static void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

static void WriteFile(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

int main() {
    const fs::path root = fs::path("_loctest_tmp");
    std::error_code ec;
    fs::remove_all(root, ec);

    PluginLocalization::SetRootPath(root.string());
    PluginLocalization::SetLanguage("es");

    // --- Plugin A: en.json base + es.json override -----------------------
    WriteFile(root / "PluginA/Languages/en.json",
              "{\n  \"Window.Title\": \"My Mod\",\n  \"Only.En\": \"English only\"\n}\n");
    WriteFile(root / "PluginA/Languages/es.json",
              "{\n  \"Window.Title\": \"Mi Mod\"\n}\n");

    std::printf("base + override merge:\n");
    Check(std::string(PluginLocalization::Get("PluginA", "Window.Title")) == "Mi Mod",
          "active language overrides English");
    Check(std::string(PluginLocalization::Get("PluginA", "Only.En")) == "English only",
          "missing key in language file falls back to English");
    Check(PluginLocalization::Load("PluginA") == 2, "Load returns merged key count");

    std::printf("missing key fallthrough:\n");
    const char* miss1 = PluginLocalization::Get("PluginA", "No.Such.Key");
    Check(std::string(miss1) == "No.Such.Key", "unknown key returns the key itself");
    {
        // Pass the key in a transient buffer: the returned pointer must not
        // alias it (misses are interned inside the table).
        char buf[32];
        std::strcpy(buf, "No.Such.Key");
        const char* miss2 = PluginLocalization::Get("PluginA", buf);
        Check(miss2 != buf && std::string(miss2) == "No.Such.Key",
              "interned miss pointer is stable, not the caller's buffer");
        Check(miss1 == miss2, "repeated misses return the same pointer");
    }

    // --- Language with no file: pure English -----------------------------
    std::printf("absent language file:\n");
    PluginLocalization::SetLanguage("fr");
    Check(std::string(PluginLocalization::Get("PluginA", "Window.Title")) == "My Mod",
          "falls back to en.json when fr.json is absent");
    PluginLocalization::SetLanguage("es");

    // --- Plugin B: no files at all (English-text-as-key convention) ------
    std::printf("no translation files:\n");
    Check(PluginLocalization::Load("PluginB") == -1, "Load returns -1 with no files");
    Check(std::string(PluginLocalization::Get("PluginB", "Enable feature")) == "Enable feature",
          "keys pass through unchanged");

    // --- Plugin C: language file only, no en.json (community translation) -
    std::printf("language file without en.json:\n");
    WriteFile(root / "PluginC/Languages/es.json",
              "{\n  \"Enable feature\": \"Activar funcion\"\n}\n");
    Check(PluginLocalization::Load("PluginC") == 1, "Load counts language-only table");
    Check(std::string(PluginLocalization::Get("PluginC", "Enable feature")) == "Activar funcion",
          "English-text key translated by es.json alone");
    Check(std::string(PluginLocalization::Get("PluginC", "Other text")) == "Other text",
          "untranslated English-text key passes through");

    // --- Plugin D: malformed language file must not poison the base ------
    std::printf("malformed JSON:\n");
    WriteFile(root / "PluginD/Languages/en.json", "{\n  \"K\": \"V\"\n}\n");
    WriteFile(root / "PluginD/Languages/es.json", "{ this is not json !!!");
    Check(PluginLocalization::Load("PluginD") == 1, "broken es.json skipped, en.json kept");
    Check(std::string(PluginLocalization::Get("PluginD", "K")) == "V", "base value survives");

    // --- Plugin E: ANSI-saved file converted to UTF-8 ---------------------
    std::printf("ANSI to UTF-8 conversion:\n");
    MCMTranslation::SetLegacyCodepage(1252);
    // "caf\xE9" is cp1252 for cafe with an accented e; invalid as UTF-8, so
    // EnsureUtf8 must convert it. Expected UTF-8: 63 61 66 C3 A9.
    WriteFile(root / "PluginE/Languages/en.json", "{ \"Coffee\": \"caf\xE9\" }");
    Check(std::string(PluginLocalization::Get("PluginE", "Coffee")) == "caf\xC3\xA9",
          "cp1252 byte converted to UTF-8 sequence");
    MCMTranslation::SetLegacyCodepage(0);

    // --- Reset re-reads from disk -----------------------------------------
    std::printf("reset/reload:\n");
    // Warm the cache first: the SetLanguage calls above invalidated it.
    (void)PluginLocalization::Get("PluginA", "Window.Title");
    WriteFile(root / "PluginA/Languages/es.json",
              "{\n  \"Window.Title\": \"Mi Mod v2\"\n}\n");
    Check(std::string(PluginLocalization::Get("PluginA", "Window.Title")) == "Mi Mod",
          "cached value served before Reset");
    PluginLocalization::Reset("PluginA");
    Check(std::string(PluginLocalization::Get("PluginA", "Window.Title")) == "Mi Mod v2",
          "Reset picks up the edited file");

    // --- Language accessor -------------------------------------------------
    Check(PluginLocalization::GetLanguage() == "es", "GetLanguage reports the active code");

    // --- Non-interning probe (auto-translate hot path) ---------------------
    std::printf("TryGet probe:\n");
    Check(std::string(PluginLocalization::TryGet("PluginA", "Window.Title")) == "Mi Mod v2",
          "TryGet returns translations");
    Check(PluginLocalization::TryGet("PluginA", "Dynamic string 12.3") == nullptr,
          "TryGet returns nullptr on miss (no interning)");
    Check(PluginLocalization::KeyCount("PluginA") == 2, "KeyCount reports merged table size");
    Check(PluginLocalization::KeyCount("PluginB") == 0, "KeyCount is 0 with no files");

    // --- Format-specifier guard (the CTD gate) ------------------------------
    std::printf("format-specifier guard:\n");
    using PluginLocalization::FormatSpecsCompatible;
    Check(FormatSpecsCompatible("Moved: %.1f / %.0f units", "Movido: %.2f de %f unidades"),
          "same conversions with different width/precision pass");
    Check(!FormatSpecsCompatible("Moved: %.1f / %.0f units", "Movido: %.1f unidades"),
          "dropped specifier rejected");
    Check(!FormatSpecsCompatible("Name: %s", "Nombre: %d"),
          "changed conversion rejected");
    Check(!FormatSpecsCompatible("Count: %d", "Cuenta: %lld"),
          "changed length modifier rejected");
    Check(FormatSpecsCompatible("Count: %d", "Cuenta: %05d"),
          "flags and literal width are cosmetic");
    Check(FormatSpecsCompatible("Static label", "Etiqueta estatica"),
          "no specifiers on either side passes");
    Check(!FormatSpecsCompatible("Static label", "Etiqueta %s"),
          "translation introducing a specifier rejected");
    Check(FormatSpecsCompatible("Progress 100%%", "Progreso 100%%"),
          "literal %% consumes nothing");
    Check(FormatSpecsCompatible("Done 100%", "Listo 100%"),
          "matching trailing bare % (malformed token) passes");
    Check(!FormatSpecsCompatible("Done 100%", "Listo %d"),
          "bare % vs real specifier rejected");
    Check(!FormatSpecsCompatible("ok", "bad %n bad"),
          "%n in translation always rejected");
    Check(FormatSpecsCompatible("%*d wide", "%*d ancho"),
          "star width consumes an int on both sides");
    Check(!FormatSpecsCompatible("%*d wide", "%d ancho"),
          "star width vs plain rejected");

    // --- Label ID-suffix splitting ------------------------------------------
    std::printf("label splitting:\n");
    {
        std::string suffix;
        Check(PluginLocalization::SplitVisibleLabel("Enable##opt1", &suffix) == "Enable" &&
                  suffix == "##opt1",
              "visible part split from ##id");
        Check(PluginLocalization::SplitVisibleLabel("Persist###fixed", &suffix) == "Persist" &&
                  suffix == "###fixed",
              "### suffix kept whole");
        Check(PluginLocalization::SplitVisibleLabel("##hidden", &suffix).empty() &&
                  suffix == "##hidden",
              "pure-ID label yields empty visible part");
        Check(PluginLocalization::SplitVisibleLabel("Plain", &suffix) == "Plain" &&
                  suffix.empty(),
              "label without suffix passes through");
    }

    fs::remove_all(root, ec);

    if (g_failures == 0) {
        std::printf("\nAll loctest checks passed.\n");
        return 0;
    }
    std::printf("\n%d loctest check(s) FAILED.\n", g_failures);
    return 1;
}
