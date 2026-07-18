#pragma once

// Test-only shadow of MCM/FallUIHudArt.h: the offline falluitest harness has
// no D3D device, so widget art is simply never available and the editor
// always uses its primitive fallbacks.

#include <string>

#include "imgui.h"

namespace FallUIHudArt {

    struct Art {
        ImTextureID tex = 0;
        float w = 0.0f, h = 0.0f;
        float originX = 0.0f, originY = 0.0f;
        bool tintHud = true;
    };

    inline const Art* Get(const std::string&) { return nullptr; }

}  // namespace FallUIHudArt
