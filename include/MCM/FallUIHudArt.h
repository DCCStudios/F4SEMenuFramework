#pragma once

// Real widget artwork for the native FallUI HUD layout editor.
//
// Rasterizes the exported widget symbols out of the installed HUDMenu.swf
// (FallUI - HUD's, through the VFS) with SWFVectorMovie and caches one
// texture per widget. Only symbols whose static rasterization is actually
// usable are mapped — Flash art that is driven at runtime (meter fills,
// TextFields, code-placed compass markers) stays with the editor's
// mod-driven ImGui primitives.
//
// The offline falluitest harness shadows this header with a stub that
// always returns nullptr (no GPU there).

#include <string>

#include "imgui.h"

namespace FallUIHudArt {

    struct Art {
        ImTextureID tex = 0;
        // Content size in movie px (720-stage space, pre widget-scale).
        float w = 0.0f, h = 0.0f;
        // Offset of the art's top-left from the symbol's registration point
        // (movie px). Place at: widgetAnchor + origin * scale.
        float originX = 0.0f, originY = 0.0f;
        // True for white-authored vanilla art the game tints at runtime;
        // the editor multiplies it with the current HUD color.
        bool tintHud = true;
    };

    // Art for a FallUI HUD editor widget by instance name (e.g. "HPMeter_mc").
    // Returns nullptr when the widget has no usable rasterized art (caller
    // falls back to primitives). Cached after the first call; must be called
    // on the render thread (uploads textures on first use).
    const Art* Get(const std::string& widgetName);

}  // namespace FallUIHudArt
