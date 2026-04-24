#include "FontManager.h"
#include "Config.h"
#include "imgui_internal.h"

#define ICON_MIN_FA 0xe005
#define ICON_MAX_FA 0xf8ff

FontContainer FontManager::LoadFonts(ImGuiIO& io, float size) {
    auto result = FontContainer();

    logger::info("FontLoader: Begin loading process for font size {}.", size);

    // Check if a glyph range has already been constructed for this font size.
    if (persistentGlyphRanges.find(size) == persistentGlyphRanges.end()) {
        logger::info("FontLoader: No cached glyph ranges for size {}. Building new ones...", size);

        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(io.Fonts->GetGlyphRangesDefault());  // Basic English

        if (Config::EnableChinese) builder.AddRanges(io.Fonts->GetGlyphRangesChineseFull());
        if (Config::EnableJapanese) builder.AddRanges(io.Fonts->GetGlyphRangesJapanese());
        if (Config::EnableKorean) builder.AddRanges(io.Fonts->GetGlyphRangesKorean());
        if (Config::EnableCyrillic) builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
        if (Config::EnableThai) builder.AddRanges(io.Fonts->GetGlyphRangesThai());

        builder.BuildRanges(&persistentGlyphRanges[size]);

        logger::info("FontLoader: Glyph ranges for size {} successfully built and cached.", size);
    } else {
        logger::info("FontLoader: Using cached glyph ranges for size {}.", size);
    }

    // =========================================================================
    //                            Font loading section
    // =========================================================================

    ImFontConfig font_config;
    font_config.PixelSnapH = true;

    if (Config::EnableChinese) 
    {
        io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
        io.Fonts->TexDesiredWidth = 8192;
    }

    // Retrieve a pointer to the absolutely safe glyph range for the corresponding font size from the map.
    result.defaultFont =
        GetFont(io, Config::PrimaryFont.c_str(), size, &font_config, persistentGlyphRanges.at(size).Data);

    // rollback mechanism
    if (!result.defaultFont) {
        logger::warn("Primary font '{}' failed to load. Falling back to FalloutMenuFont.ttf.", Config::PrimaryFont);
        result.defaultFont = GetFont(io, "FalloutMenuFont.ttf", size, nullptr, io.Fonts->GetGlyphRangesDefault());
    }

    // Merge default font and Font Awesome icon
    ImFontConfig merge_config;
    merge_config.MergeMode = true;
    merge_config.PixelSnapH = true;

    GetFont(io, "FalloutMenuFont.ttf", size, &merge_config, io.Fonts->GetGlyphRangesDefault());

    static const ImWchar icons_ranges[] = {ICON_MIN_FA, ICON_MAX_FA, 0};
    GetFont(io, "fa-solid-900.ttf", size, &merge_config, icons_ranges);
    GetFont(io, "fa-regular-400.ttf", size, &merge_config, icons_ranges);
    GetFont(io, "fa-brands-400.ttf", size, &merge_config, icons_ranges);

    logger::info("Font loading process for size {} completed.", size);
    return result;
}
void FontManager::CleanFontStack() {
    auto ctx = ImGui::GetCurrentContext();
    while (ctx->FontStack.size() > 0) {
        ImGui::PopFont();
    }
}
void FontManager::CleanFont() {
    CleanFontStack();
    currentFont = Font::fontSizeDefault;
}

void FontManager::SetFont(Font font) {
    currentFont = font;
    ProcessFont();
}

void FontManager::ProcessFont() {
    FontContainer container;
    if (currentFont & Font::fontSizeSmall) {
        container = fontSizes["Small"];
    } else if (currentFont & Font::fontSizeBig) {
        container = fontSizes["Big"];
    } else if (currentFont & Font::fontSizeDefault) {
        container = fontSizes["Default"];
    }
    if (currentFont & Font::faSolid) {
        if (container.faSolid) {
            ImGui::PushFont(container.faSolid);
        }
    } else if (currentFont & Font::faRegular) {
        if (container.faRegular) {
            ImGui::PushFont(container.faRegular);
        }
    } else if (currentFont & Font::faBrands) {
        if (container.faBrands) {
            ImGui::PushFont(container.faBrands);
        }
    }
}

ImFont* FontManager::GetFont(ImGuiIO& io, std::string name, float size, const ImFontConfig* font_cfg = NULL,
                const ImWchar* glyph_ranges = NULL) {
    std::string path = "Data/F4SE/Plugins/Fonts/" + name;
    if (std::filesystem::exists(path)) {
        return io.Fonts->AddFontFromFileTTF(path.c_str(), size, font_cfg, glyph_ranges);
    }
    return nullptr;
}