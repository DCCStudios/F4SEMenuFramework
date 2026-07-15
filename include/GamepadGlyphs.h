#pragma once

#include "imgui.h"

// GamepadGlyphs — loads the console button glyph textures and renders the
// control-hint bar shown below the main F4SE Menu Framework window whenever a
// controller is connected.
//
// Glyph PNGs ship under Data/F4SE/Plugins/F4SEMenuFramework/Gamepad/ next to
// the DLL (resolved at runtime via GetModuleHandleExW + GetModuleFileNameW per
// the plugin-data-on-disk convention). Two art sets are provided: Xbox
// (default) and PlayStation, switched by Config::GamepadGlyphStyle.
namespace GamepadGlyphs {

    // Logical buttons the hint bar can display. The concrete art (A vs Cross,
    // LB vs L1, ...) is chosen from the active glyph style.
    enum class Glyph {
        FaceDown,   // A / Cross
        FaceRight,  // B / Circle
        FaceLeft,   // X / Square
        FaceUp,     // Y / Triangle
        LB,         // LB / L1
        RB,         // RB / R1
        DPad,       // shared art
        LStick      // shared art
    };

    // Returns the ImGui texture for a glyph in the active style, lazily
    // loading it through TextureLoader on first use (render thread only).
    // Returns 0 if the PNG is missing.
    ImTextureID Get(Glyph glyph);

    // Renders the hint bar as a separate borderless window directly below the
    // main menu window. Shows the button map plus the focused control's MCM
    // help text (if any). Call from the main window's render path each frame.
    void RenderHintBar(const ImVec2& mainWindowPos, const ImVec2& mainWindowSize);

}
