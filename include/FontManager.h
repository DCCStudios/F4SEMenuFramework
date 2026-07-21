#pragma once

#include "imgui_internal.h"

enum Font {
    none = 0,
    faSolid = 1 << 0,
    faRegular = 1 << 1,
    faBrands = 1 << 2,
    fontSizeSmall = 1 << 3,
    fontSizeDefault = 1 << 4,
    fontSizeBig = 1 << 5,
};

struct FontContainer {
    ImFont* faSolid;
    ImFont* faRegular;
    ImFont* faBrands;
    ImFont* defaultFont;
};

class FontManager {
public:
    static void ProcessFont();
    static ImFont* GetFont(ImGuiIO& io, std::string name, float size, const ImFontConfig* font_cfg,
                    const ImWchar* glyph_ranges);
    static void SetFont(Font font);
    static FontContainer LoadFonts(ImGuiIO& io, float size);
    static void CleanFontStack();
    static void CleanFont();

    // Runtime font switching: enumerates .ttf files and triggers atlas rebuild.
    static std::vector<std::string> GetAvailableFonts();
    static void RequestReload();
    static bool IsReloadPending();
    static void PerformReload();

    // Builds the font atlas, degrading glyph coverage and retrying if the
    // result would exceed D3D11's 16384-px texture limit (a CJK-capable font
    // can bake 20k+ glyphs across three sizes and overflow it — that used to
    // CTD in the DX11 backend). Use this instead of io.Fonts->Build().
    static void BuildAtlasSafe(ImGuiIO& io);

    // Content-driven glyph coverage: request font ranges for scripts that
    // actually appear in loaded text (MCMTranslation::ScriptMask bits).
    // Needed when the game language doesn't announce them — e.g. Korean
    // translation mods installed on an sLanguage=en game. Triggers an atlas
    // rebuild if new scripts arrive after fonts were already built.
    static void RequestScriptCoverage(unsigned int scriptMask);

    static inline std::map<std::string, FontContainer> fontSizes;
    static inline Font currentFont = Font::fontSizeDefault;
    static inline std::map<float, ImVector<ImWchar>> persistentGlyphRanges;

private:
    static inline bool reloadPending = false;
};

