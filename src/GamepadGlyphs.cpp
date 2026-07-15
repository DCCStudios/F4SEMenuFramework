#include "GamepadGlyphs.h"
#include "TextureLoader.h"
#include "Config.h"
#include "MCM/MCMWidgetRenderer.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <string>

namespace GamepadGlyphs {

    // ------------------------------------------------------------------
    // Glyph directory resolution — the PNGs live NEXT TO THE DLL under
    // Data/F4SE/Plugins/F4SEMenuFramework/Gamepad/. Per the workspace
    // convention we resolve the module path with GetModuleHandleExW on an
    // address inside this module (never assume the process CWD).
    // ------------------------------------------------------------------

    static std::filesystem::path GetGlyphDirectory() {
        static std::filesystem::path s_dir = [] {
            HMODULE module = nullptr;
            // Use this function's address to find OUR module handle
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&GetGlyphDirectory), &module);

            wchar_t buffer[MAX_PATH]{};
            GetModuleFileNameW(module, buffer, MAX_PATH);

            // <...>/F4SE/Plugins/F4SEMenuFramework.dll -> <...>/F4SE/Plugins/F4SEMenuFramework/Gamepad
            std::filesystem::path dllPath(buffer);
            return dllPath.parent_path() / "F4SEMenuFramework" / "Gamepad";
        }();
        return s_dir;
    }

    // Maps a logical glyph + active style to its PNG filename.
    static const char* GlyphFileName(Glyph glyph, bool playstation) {
        switch (glyph) {
            case Glyph::FaceDown:  return playstation ? "ps_cross.png"    : "xbox_a.png";
            case Glyph::FaceRight: return playstation ? "ps_circle.png"   : "xbox_b.png";
            case Glyph::FaceLeft:  return playstation ? "ps_square.png"   : "xbox_x.png";
            case Glyph::FaceUp:    return playstation ? "ps_triangle.png" : "xbox_y.png";
            case Glyph::LB:        return playstation ? "ps_l1.png"       : "xbox_lb.png";
            case Glyph::RB:        return playstation ? "ps_r1.png"       : "xbox_rb.png";
            case Glyph::DPad:      return "pad_dpad.png";    // platform-neutral art
            case Glyph::LStick:    return "pad_lstick.png";  // platform-neutral art
            default:               return nullptr;
        }
    }

    ImTextureID Get(Glyph glyph) {
        const bool playstation = (Config::GamepadGlyphStyle == 1);
        const char* file = GlyphFileName(glyph, playstation);
        if (!file) return 0;

        auto path = GetGlyphDirectory() / file;
        // TextureLoader caches by path string, so repeated calls are cheap.
        return TextureLoader::GetTexture(path.string());
    }

    // ------------------------------------------------------------------
    // Hint bar rendering
    // ------------------------------------------------------------------

    // Draws one glyph + label pair on the current line.
    static void HintItem(Glyph glyph, const char* label, float glyphSize) {
        ImTextureID tex = Get(glyph);
        if (tex) {
            ImGui::Image(tex, ImVec2(glyphSize, glyphSize));
        } else {
            // Fallback when art is missing: bracketed text tag
            ImGui::TextUnformatted("[?]");
        }
        ImGui::SameLine(0.0f, 4.0f);
        // Vertically center the label against the glyph
        float offY = (glyphSize - ImGui::GetTextLineHeight()) * 0.5f;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offY);
        ImGui::TextUnformatted(label);
        ImGui::SameLine(0.0f, 18.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - offY);
    }

    // Draws two glyphs side by side followed by one label (e.g. LB+RB).
    static void HintItemPair(Glyph first, Glyph second, const char* label, float glyphSize) {
        ImTextureID texA = Get(first);
        ImTextureID texB = Get(second);
        if (texA) { ImGui::Image(texA, ImVec2(glyphSize, glyphSize)); ImGui::SameLine(0.0f, 2.0f); }
        if (texB) { ImGui::Image(texB, ImVec2(glyphSize, glyphSize)); }
        if (!texA && !texB) ImGui::TextUnformatted("[?]");
        ImGui::SameLine(0.0f, 4.0f);
        float offY = (glyphSize - ImGui::GetTextLineHeight()) * 0.5f;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offY);
        ImGui::TextUnformatted(label);
        ImGui::SameLine(0.0f, 18.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - offY);
    }

    void RenderHintBar(const ImVec2& mainWindowPos, const ImVec2& mainWindowSize) {
        const float glyphSize = ImGui::GetTextLineHeight() * 1.4f;

        // Position: directly below the main window, same width. NoNav keeps
        // gamepad focus inside the main window; NoFocusOnAppearing avoids
        // stealing focus from the menu.
        ImGui::SetNextWindowPos(ImVec2(mainWindowPos.x, mainWindowPos.y + mainWindowSize.y + 6.0f));
        ImGui::SetNextWindowSize(ImVec2(mainWindowSize.x, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.80f);

        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;

        if (ImGui::Begin("##F4SEMenuGamepadHints", nullptr, flags)) {
            // Button map row — logical actions in navigation order
            HintItemPair(Glyph::LB, Glyph::RB, "Switch Pane", glyphSize);
            HintItemPair(Glyph::DPad, Glyph::LStick, "Navigate", glyphSize);
            HintItem(Glyph::FaceDown, "Select", glyphSize);
            HintItem(Glyph::FaceRight, "Back / Close", glyphSize);
            HintItem(Glyph::FaceLeft, "Reset / Unbind", glyphSize);
            HintItem(Glyph::FaceUp, "Settings", glyphSize);
            ImGui::NewLine();

            // Help footer — mirrors the focused MCM control's help text, like
            // the real MCM's bottom help panel.
            const std::string& help = MCMWidgetRenderer::GetFocusedHelpText();
            if (!help.empty()) {
                ImGui::Separator();
                ImGui::TextWrapped("%s", help.c_str());
            }
        }
        ImGui::End();
    }

} // namespace GamepadGlyphs
