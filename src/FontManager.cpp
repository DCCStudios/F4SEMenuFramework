#include "FontManager.h"
#include "Config.h"
#include "MCM/MCMTranslation.h"
#include "imgui_internal.h"
#include "imgui_impl_dx11.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <filesystem>

#define ICON_MIN_FA 0xe005
#define ICON_MAX_FA 0xf8ff

// Same resolution as MCMRegistry: Custom.ini / Fallout4.ini / GetINISetting.
static std::string GameLanguage() {
    const auto* setting = RE::GetINISetting("sLanguage:General");
    const std::string fromSetting = setting ? std::string(setting->GetString()) : "";
    return MCMTranslation::ResolveGameLanguage(fromSetting);
}

// Atlas overflow protection. D3D11 rejects textures larger than 16384 px per
// side; a font that actually CONTAINS CJK outlines (Noto/Sarasa/system fonts)
// bakes 20k+ glyphs per size across our three sizes and can push the atlas
// past that. CreateTexture2D then fails and, before the guard in the DX11
// backend, the null texture pointer crashed the game (verified from a Buffout
// crash log: E_INVALIDARG in RAX, null this in RCX, inside CreateFontsTexture
// called from PresentHook). s_atlasDegradeLevel shrinks the requested glyph
// coverage step by step until the atlas fits:
//   0 = full coverage (Chinese Full + everything requested)
//   1 = Chinese reduced to the "common" subset (~2.5k chars instead of ~21k)
//   2 = no CJK ranges at all (Latin + other small scripts still included)
static int s_atlasDegradeLevel = 0;

// Scripts observed in actually-loaded MCM text (MCMTranslation::ScriptMask
// bits). Covers the case the language setting can't: translation mods that
// ship non-Latin text in *_en.txt files on an sLanguage=en game (common for
// Korean translation packs).
static std::atomic<unsigned int> s_requestedScripts{ 0 };

void FontManager::RequestScriptCoverage(unsigned int scriptMask) {
    const unsigned int prev = s_requestedScripts.fetch_or(scriptMask);
    const unsigned int added = scriptMask & ~prev;
    if (added != 0) {
        logger::info("[FontManager] Detected script(s) 0x{:X} in loaded text — scheduling atlas rebuild for coverage", added);
        RequestReload();
    }
}

FontContainer FontManager::LoadFonts(ImGuiIO& io, float size) {
    auto result = FontContainer();

    logger::info("FontLoader: Begin loading process for font size {}.", size);

    // The manual Enable* INI flags remain as user overrides, but the range
    // matching the game's own language is force-enabled so translated MCM
    // text (now loaded per language by MCMTranslation) renders instead of
    // showing '?' boxes. FO4's official language codes: en/de/es/esmx/fr/it/
    // ja/pl/ptbr/ru/cn — the Latin-script ones are covered by the default +
    // Latin-Extended ranges below.
    const std::string gameLang = GameLanguage();
    const unsigned int scripts = s_requestedScripts.load(std::memory_order_relaxed);
    const bool wantChinese  = (Config::EnableChinese  || gameLang == "cn" || gameLang.starts_with("zh")
                               || (scripts & MCMTranslation::kScriptChinese)) && s_atlasDegradeLevel < 2;
    const bool wantJapanese = (Config::EnableJapanese || gameLang == "ja"
                               || (scripts & MCMTranslation::kScriptJapanese)) && s_atlasDegradeLevel < 2;
    const bool wantKorean   = (Config::EnableKorean   || gameLang == "ko" || gameLang == "kr"
                               || (scripts & MCMTranslation::kScriptKorean)) && s_atlasDegradeLevel < 2;
    const bool wantCyrillic = Config::EnableCyrillic || gameLang == "ru" || gameLang == "uk"
                               || (scripts & MCMTranslation::kScriptCyrillic);
    const bool wantThai     = Config::EnableThai     || gameLang == "th"
                               || (scripts & MCMTranslation::kScriptThai);

    // Check if a glyph range has already been constructed for this font size.
    if (persistentGlyphRanges.find(size) == persistentGlyphRanges.end()) {
        logger::info("FontLoader: No cached glyph ranges for size {}. Building new ones...", size);

        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(io.Fonts->GetGlyphRangesDefault());  // Basic English

        // Latin Extended-A/B: Polish, Czech, Turkish, Romanian, Hungarian...
        // ~330 glyphs, negligible atlas cost, so always baked.
        static const ImWchar latinExtRanges[] = { 0x0100, 0x024F, 0 };
        builder.AddRanges(latinExtRanges);

        if (wantChinese) {
            // Full coverage is ~21k codepoints; the "common" subset (~2.5k)
            // is the degraded fallback when the full atlas won't fit.
            builder.AddRanges(s_atlasDegradeLevel >= 1
                ? io.Fonts->GetGlyphRangesChineseSimplifiedCommon()
                : io.Fonts->GetGlyphRangesChineseFull());
        }
        if (wantJapanese) builder.AddRanges(io.Fonts->GetGlyphRangesJapanese());
        if (wantKorean) builder.AddRanges(io.Fonts->GetGlyphRangesKorean());
        if (wantCyrillic) builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
        if (wantThai) builder.AddRanges(io.Fonts->GetGlyphRangesThai());
        if (gameLang == "vi" || (scripts & MCMTranslation::kScriptVietnamese))
            builder.AddRanges(io.Fonts->GetGlyphRangesVietnamese());
        if (gameLang == "el" || (scripts & MCMTranslation::kScriptGreek))
            builder.AddRanges(io.Fonts->GetGlyphRangesGreek());

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

    // CJK atlases need far more texture space than the default width allows.
    // Oversampling is also dropped to 1x for them: the default 2x horizontal
    // oversample doubles every glyph's atlas footprint, which is noise for
    // dense ideographic glyphs but can push the atlas past the 16384-px D3D11
    // texture limit (CTD before BuildAtlasSafe existed).
    if (wantChinese || wantJapanese || wantKorean)
    {
        io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
        io.Fonts->TexDesiredWidth = 8192;
        font_config.OversampleH = 1;
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
    if (wantChinese || wantJapanese || wantKorean) {
        // Same 1x oversample as the primary font — the merged system
        // fallbacks below may carry the bulk of the CJK glyphs.
        merge_config.OversampleH = 1;
    }

    GetFont(io, "FalloutMenuFont.ttf", size, &merge_config, io.Fonts->GetGlyphRangesDefault());

    static const ImWchar icons_ranges[] = {ICON_MIN_FA, ICON_MAX_FA, 0};
    GetFont(io, "fa-solid-900.ttf", size, &merge_config, icons_ranges);
    GetFont(io, "fa-regular-400.ttf", size, &merge_config, icons_ranges);
    GetFont(io, "fa-brands-400.ttf", size, &merge_config, icons_ranges);

    // --- Fallback fonts for scripts the shipped fonts don't cover ---
    // Having the glyph RANGE in the atlas is not enough: if neither the
    // primary font nor FalloutMenuFont contains e.g. Hangul outlines, every
    // Korean character renders as '?'. Merge-mode fonts only contribute
    // glyphs that are still missing at this point, so these never override
    // the mod's chosen look for Latin text — they purely fill the gaps.
    //
    // Search order per candidate: the plugin's own Fonts folder first (so
    // users/modpacks can ship e.g. NotoSansKR and it wins — also the only
    // option under Proton/Wine, where the Windows font directory usually
    // lacks CJK fonts), then the real Windows font directory.
    {
        wchar_t winDirW[MAX_PATH]{};
        ::GetWindowsDirectoryW(winDirW, MAX_PATH);
        const std::filesystem::path systemFontsDir = std::filesystem::path(winDirW) / "Fonts";
        const std::filesystem::path pluginFontsDir = "Data/F4SE/Plugins/Fonts";

        auto mergeFallbackFont = [&](std::initializer_list<const char*> candidates) {
            for (const char* file : candidates) {
                for (const auto& dir : { pluginFontsDir, systemFontsDir }) {
                    auto path = dir / file;
                    if (std::filesystem::exists(path)) {
                        io.Fonts->AddFontFromFileTTF(path.string().c_str(), size, &merge_config,
                                                     persistentGlyphRanges.at(size).Data);
                        logger::info("FontLoader: merged fallback font '{}' for size {}", path.string(), size);
                        return;
                    }
                }
            }
        };

        // Noto names cover plugin-shipped fonts; the rest are Windows fonts.
        if (wantKorean)   mergeFallbackFont({ "NotoSansKR-Regular.ttf", "NotoSansKR-Light.ttf",
                                              "malgun.ttf", "gulim.ttc" });          // Malgun Gothic / Gulim
        if (wantChinese)  mergeFallbackFont({ "NotoSansSC-Regular.ttf", "NotoSansSC-Light.ttf",
                                              "NotoSansTC-Regular.ttf",
                                              "msyh.ttc", "msyh.ttf", "simhei.ttf" }); // YaHei / SimHei
        if (wantJapanese) mergeFallbackFont({ "NotoSansJP-Regular.ttf", "NotoSansJP-Light.ttf",
                                              "YuGothM.ttc", "meiryo.ttc", "msgothic.ttc" });
        if (wantThai)     mergeFallbackFont({ "NotoSansThai-Regular.ttf",
                                              "leelawui.ttf", "tahoma.ttf" });       // Leelawadee UI / Tahoma
        // Cyrillic, Greek, Vietnamese and Latin-Extended gaps: NotoSans if
        // shipped, else Segoe UI (present on every real Windows install).
        mergeFallbackFont({ "NotoSans-Regular.ttf", "segoeui.ttf" });
    }

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

std::vector<std::string> FontManager::GetAvailableFonts() {
    std::vector<std::string> fonts;
    const std::string dir = "Data/F4SE/Plugins/Fonts/";
    if (!std::filesystem::exists(dir)) {
        return fonts;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        // Case-insensitive .ttf / .otf check
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".ttf" || ext == ".otf") {
            fonts.push_back(entry.path().filename().string());
        }
    }
    std::sort(fonts.begin(), fonts.end());
    return fonts;
}

void FontManager::RequestReload() {
    reloadPending = true;
    logger::info("[FontManager] Font reload requested — will rebuild atlas next frame.");
}

bool FontManager::IsReloadPending() {
    return reloadPending;
}

void FontManager::PerformReload() {
    if (!reloadPending) return;
    reloadPending = false;

    logger::info("[FontManager] Performing font atlas rebuild with PrimaryFont='{}'", Config::PrimaryFont);

    auto& io = ImGui::GetIO();

    // A newly selected font may fit coverage a previous one couldn't (or
    // vice versa) — restart degradation from full coverage for the new font.
    s_atlasDegradeLevel = 0;

    // Clear all existing font data (atlas + glyph ranges cache).
    io.Fonts->Clear();
    persistentGlyphRanges.clear();
    fontSizes.clear();

    // Rebuild all three size buckets with the new primary font.
    auto regular = LoadFonts(io, Config::FontSizeMedium);
    io.FontDefault = regular.defaultFont;

    fontSizes["Big"]     = LoadFonts(io, Config::FontSizeBig);
    fontSizes["Small"]   = LoadFonts(io, Config::FontSizeSmall);
    fontSizes["Default"] = regular;

    BuildAtlasSafe(io);

    // Force the DX11 backend to recreate the font texture from the new atlas.
    ImGui_ImplDX11_InvalidateDeviceObjects();

    logger::info("[FontManager] Font atlas rebuild complete.");
}

void FontManager::BuildAtlasSafe(ImGuiIO& io) {
    // D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION for feature level 11: anything
    // bigger makes CreateTexture2D fail with E_INVALIDARG.
    constexpr int kMaxTextureDim = 16384;

    for (;;) {
        const bool built = io.Fonts->Build();
        if (built && io.Fonts->TexWidth <= kMaxTextureDim && io.Fonts->TexHeight <= kMaxTextureDim) {
            logger::info("[FontManager] Font atlas built: {}x{} px (degrade level {})",
                io.Fonts->TexWidth, io.Fonts->TexHeight, s_atlasDegradeLevel);
            return;
        }

        if (!built) {
            logger::error("[FontManager] Font atlas Build() failed outright (corrupt font file?)");
        } else {
            logger::warn("[FontManager] Font atlas {}x{} px exceeds the D3D11 texture limit ({})",
                io.Fonts->TexWidth, io.Fonts->TexHeight, kMaxTextureDim);
        }

        if (s_atlasDegradeLevel >= 2) {
            // Even the CJK-free atlas failed — the font file itself is the
            // problem. Last resort: ImGui's embedded ProggyClean, which
            // cannot fail. UI stays basic but the game stays alive.
            logger::error("[FontManager] Falling back to ImGui's embedded default font");
            io.Fonts->Clear();
            persistentGlyphRanges.clear();
            io.FontDefault = io.Fonts->AddFontDefault();
            for (auto& [name, container] : fontSizes) {
                container = FontContainer{};
                container.defaultFont = io.FontDefault;
            }
            io.Fonts->Build();
            return;
        }

        ++s_atlasDegradeLevel;
        logger::warn("[FontManager] Reducing glyph coverage to level {} and rebuilding "
                     "(1 = common Chinese subset, 2 = no CJK ranges)", s_atlasDegradeLevel);

        // Reload all size buckets with the reduced ranges, then loop to retry.
        io.Fonts->Clear();
        persistentGlyphRanges.clear();
        fontSizes.clear();

        auto regular = LoadFonts(io, Config::FontSizeMedium);
        io.FontDefault = regular.defaultFont;
        fontSizes["Big"]     = LoadFonts(io, Config::FontSizeBig);
        fontSizes["Small"]   = LoadFonts(io, Config::FontSizeSmall);
        fontSizes["Default"] = regular;
    }
}