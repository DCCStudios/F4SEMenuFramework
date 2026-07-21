// Offline validator for MCMTranslation: loads a real translation file and
// resolves tokens, so newline decoding, language selection, legacy-codepage
// conversion (CP949 etc.) and script detection can be checked without the
// game. Usage: trtest.exe <translation.txt|directory> [$Token] [lang] [codepage]
#include "MCM/MCMTranslation.h"

#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "usage: trtest <file-or-dir> [$Token] [lang] [codepage]\n";
        return 1;
    }
    if (argc >= 4) {
        MCMTranslation::SetLanguage(argv[3]);
    }
    if (argc >= 5) {
        // Force the legacy codepage (the game build uses the machine ANSI
        // codepage, which on this dev machine isn't CP949 — tests pass it).
        MCMTranslation::SetLegacyCodepage(static_cast<unsigned int>(std::stoul(argv[4])));
    }
    std::filesystem::path p = argv[1];
    auto map = std::filesystem::is_directory(p)
        ? MCMTranslation::LoadDirectory(p)
        : MCMTranslation::LoadFile(p);
    std::cout << map.size() << " entries (language " << MCMTranslation::GetLanguage() << ")\n";

    // Aggregate script detection over every value — mirrors what the
    // registry reports to FontManager.
    unsigned int mask = 0;
    for (const auto& [k, v] : map) {
        mask |= MCMTranslation::DetectScripts(v);
    }
    std::cout << "script mask: 0x" << std::hex << mask << std::dec
              << " (1=cyr 2=zh 4=ja 8=ko 10=th 20=el 40=vi)\n";

    if (argc >= 3) {
        auto resolved = MCMTranslation::ResolveAndStrip(argv[2], map);
        std::cout << "resolved: [" << resolved << "]\n";
        // Make embedded control characters visible
        std::cout << "escaped:  [";
        for (char c : resolved) {
            if (c == '\n') std::cout << "<NL>";
            else if (c == '\t') std::cout << "<TAB>";
            else std::cout << c;
        }
        std::cout << "]\n";
        // Byte dump so UTF-8 validity is checkable regardless of console codepage
        std::cout << "utf8 bytes:";
        for (unsigned char c : resolved) {
            std::printf(" %02X", c);
        }
        std::cout << "\n";
    }
    return 0;
}
