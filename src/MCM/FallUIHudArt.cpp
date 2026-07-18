#include "MCM/FallUIHudArt.h"

#include "MCM/SWFVectorMovie.h"
#include "TextureLoader.h"

#include <filesystem>
#include <map>
#include <optional>

namespace FallUIHudArt {

    namespace {

        // Widget instance name -> exported HUDMenu.swf symbol.
        //
        // Only symbols verified to produce a usable STATIC rasterization are
        // listed (checked offline with swftest against FallUI - HUD's
        // HUDMenu.swf). Everything else — meter bars that are fill tweens,
        // TextField-only widgets, bitmap-sequence Vault Boys, code-placed
        // compass markers — renders empty/wrong when flattened, so those
        // widgets keep the editor's mod-driven primitives.
        //
        // frame -1 = use the timeline's middle frame (for symbols whose
        // frame 0 is an empty "idle" state, like the hit indicator).
        struct SymbolRef {
            const char* className;
            bool tintHud;  // white-authored art the game tints -> HUD color
            int frame;
        };
        static const std::map<std::string, SymbolRef> kSymbols = {
            { "HUDCrosshair_mc",                { "HUDCrosshair", true, 0 } },
            { "ExplosiveAmmoCount_mc",          { "ExplosiveAmmoCount", true, 0 } },
            { "RolloverWidget_mc",              { "RolloverWidget", true, 0 } },
            { "TutorialText_mc",                { "TutorialText", true, 0 } },
            { "HitIndicator_mc",                { "HUDMenu_fla.HitIndicator_101", true, -1 } },
            { "DirectionalHitIndicatorBase_mc", { "DirectionalHitIndicator", true, 0 } },
            // The Perk/Quest Vault Boys are bitmap sequences we can't flatten;
            // the TutorialHeads symbol is the same character as real vector
            // art, so it stands in for them.
            { "PerkVaultBoy_mc",                { "HUDMenu_fla.TutorialHeads_125", true, -1 } },
            { "QuestVaultBoy_mc",               { "HUDMenu_fla.TutorialHeads_125", true, -1 } },
            // Background chrome; ticks/markers/texts are overlaid by the editor.
            { "CompassWidget_mc",               { "HUDCompassWidget", false, 0 } },
            { "StealthMeter_mc",                { "StealthMeter", false, 0 } },
        };

        // widgetName -> resolved art; nullopt = permanent failure (missing
        // SWF / symbol / empty render). A texture-upload failure (device not
        // ready yet) is NOT cached so it retries next frame.
        std::map<std::string, std::optional<Art>> s_cache;

    }  // namespace

    const Art* Get(const std::string& widgetName) {
        if (auto it = s_cache.find(widgetName); it != s_cache.end()) {
            return it->second ? &*it->second : nullptr;
        }

        auto sym = kSymbols.find(widgetName);
        if (sym == kSymbols.end()) {
            s_cache[widgetName] = std::nullopt;
            return nullptr;
        }

        // The winning loose file through the VFS — with FallUI - HUD active
        // (the only mod whose MCM reaches this editor) that is its HUDMenu.swf.
        const auto swfPath =
            std::filesystem::current_path() / "Data" / "Interface" / "HUDMenu.swf";

        auto movie = SWFVectorMovie::Load(swfPath, sym->second.className);
        if (!movie || movie->WidthPx() <= 0.0f || movie->HeightPx() <= 0.0f) {
            s_cache[widgetName] = std::nullopt;
            return nullptr;
        }

        int frame = sym->second.frame;
        if (frame < 0) {
            frame = movie->FrameCount() / 2;
        }
        // 2x movie px so the canvas can scale up without going blurry.
        auto img = movie->RenderFrame(frame, 2.0f);
        if ((!img || img->width <= 0) && frame != 0) {
            img = movie->RenderFrame(0, 2.0f);
        }
        if (!img || img->width <= 0 || img->height <= 0) {
            s_cache[widgetName] = std::nullopt;
            return nullptr;
        }

        ImTextureID tex = TextureLoader::GetTextureFromMemory(
            "falluihudart|" + widgetName, img->width, img->height, img->rgba.data());
        if (!tex) {
            return nullptr;  // device not ready — retry on a later frame
        }

        Art art;
        art.tex = tex;
        art.w = movie->WidthPx();
        art.h = movie->HeightPx();
        art.originX = movie->OriginXPx();
        art.originY = movie->OriginYPx();
        art.tintHud = sym->second.tintHud;
        s_cache[widgetName] = art;
        return &*s_cache[widgetName];
    }

}  // namespace FallUIHudArt
