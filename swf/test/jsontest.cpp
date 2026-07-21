// Offline validator: runs MCMJson::SanitizeLenientJson over every MCM
// config.json / keybinds.json under one or more modlist roots and checks the
// result parses as strict JSON with nlohmann. Usage: jsontest <modsRoot>...
#include "../../include/MCM/MCMJsonSanitizer.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: jsontest <modsRoot> [<modsRoot>...]\n");
        return 2;
    }
    int total = 0, ok = 0, fixed = 0, failed = 0;
    for (int a = 1; a < argc; ++a) {
        for (const auto& mod : fs::directory_iterator(argv[a])) {
            const fs::path cfgRoot = mod.path() / "MCM" / "Config";
            if (!fs::is_directory(cfgRoot)) continue;
            for (const auto& sub : fs::directory_iterator(cfgRoot)) {
                for (const char* name : { "config.json", "keybinds.json" }) {
                    const fs::path p = sub.path() / name;
                    if (!fs::is_regular_file(p)) continue;
                    ++total;
                    std::ifstream f(p, std::ios::binary);
                    std::string text((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
                    bool strictOk = nlohmann::json::accept(text);
                    const std::string sane = MCMJson::SanitizeLenientJson(text);
                    if (nlohmann::json::accept(sane)) {
                        ++ok;
                        if (!strictOk) {
                            ++fixed;
                            std::printf("FIXED  %s\n", p.string().c_str());
                        } else if (nlohmann::json::parse(text) != nlohmann::json::parse(sane)) {
                            // Sanitizer must be a no-op (semantically) on valid input.
                            ++failed;
                            --ok;
                            std::printf("MUTATED %s\n", p.string().c_str());
                        }
                    } else {
                        ++failed;
                        // Re-parse with exceptions for a useful error message.
                        try {
                            (void)nlohmann::json::parse(sane);
                        } catch (const std::exception& e) {
                            std::printf("FAIL   %s\n       %s\n", p.string().c_str(), e.what());
                        }
                    }
                }
            }
        }
    }
    std::printf("\ntotal=%d ok=%d (sanitizer-fixed=%d) failed=%d\n", total, ok, fixed, failed);
    return failed == 0 ? 0 : 1;
}
