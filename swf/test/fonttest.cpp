// Offline validator for the font-atlas overflow fix (BuildAtlasSafe logic).
// Reproduces the CTD precondition from the bug report — a CJK-capable font
// baked at the framework's three sizes (16/32/64) — and checks the atlas
// dimensions ImGui produces at each degrade level, without any game or GPU:
//   pass 0: full Chinese ranges, oversample 2 (pre-fix)   -> expect > 16384 px
//   pass 1: full Chinese ranges, oversample 1 (fix step 1) -> report
//   pass 2: common Chinese subset, oversample 1 (level 1)  -> expect to fit
// Usage: fonttest.exe [path-to-cjk-font.ttf/ttc]  (default: msyh.ttc)
#include "imgui.h"

#include <cstdio>

static void RunPass(const char* label, const char* fontPath, bool fullChinese, int oversampleH) {
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

    io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
    io.Fonts->TexDesiredWidth = 8192;

    ImFontConfig cfg;
    cfg.OversampleH = oversampleH;
    const ImWchar* ranges = fullChinese
        ? io.Fonts->GetGlyphRangesChineseFull()
        : io.Fonts->GetGlyphRangesChineseSimplifiedCommon();

    // The framework's three size buckets share one atlas.
    const float sizes[] = { 16.0f, 32.0f, 64.0f };
    bool loaded = true;
    for (float size : sizes) {
        if (!io.Fonts->AddFontFromFileTTF(fontPath, size, &cfg, ranges)) {
            loaded = false;
        }
    }

    if (!loaded) {
        std::printf("%-40s FAILED to load font '%s'\n", label, fontPath);
    } else {
        const bool built = io.Fonts->Build();
        std::printf("%-40s built=%d  atlas=%dx%d  %s\n",
            label, built ? 1 : 0, io.Fonts->TexWidth, io.Fonts->TexHeight,
            (built && io.Fonts->TexWidth <= 16384 && io.Fonts->TexHeight <= 16384)
                ? "FITS (<=16384)" : "OVERFLOW (would fail CreateTexture2D)");
    }

    ImGui::DestroyContext(ctx);
}

int main(int argc, char** argv) {
    const char* fontPath = argc > 1 ? argv[1] : "C:\\Windows\\Fonts\\msyh.ttc";
    std::printf("font: %s\n", fontPath);
    RunPass("full Chinese, oversample 2 (pre-fix)", fontPath, true, 2);
    RunPass("full Chinese, oversample 1 (fix)", fontPath, true, 1);
    RunPass("common Chinese, oversample 1 (level 1)", fontPath, false, 1);
    return 0;
}
