#pragma once

#include <string>

// Native ImGui recreation of FallUI's Flash-based in-MCM applications.
//
// The real FallUI mods embed full ActionScript 3 apps inside MCM "image"
// controls (className "M8r.Controller.FallUIHUD" is the drag-and-drop HUD
// layout editor; "M8r.View.FallUIIconLibrary" is the icon preset manager).
// Those apps require a live Scaleform stage and MCM's internal display list,
// which our translation layer doesn't provide — so we recreate them natively.
//
// Faithfulness notes (see FallUIHudEditor.cpp for the full port):
//  - The persisted data formats are IDENTICAL to the originals (verified
//    against shipped layout presets): widget lines use FallUI's packed
//    "on:<x>x<y>*<sx>*<sy>r<rot>:<k=v,...>" strings, and global/editor
//    settings use the M8r StringPacker "name;type;value;..." format. Layouts
//    made here load in the Flash editor and vice versa, and FallUI's runtime
//    HUD swf (which reads Data/MCM/Settings/FallUIHUD.ini directly) applies
//    them unmodified.
//  - The per-widget option model (every checkbox/slider/color the original
//    edit panel offers) is ported from the decompiled HUDLayoutOptions.as.
namespace FallUIHudEditor {

    // True when this MCM image control is one of the FallUI Flash apps we
    // replace with a native recreation.
    bool HandlesImageControl(const std::string& libName, const std::string& className);

    // Renders the recreation for the given (libName, className) pair inside
    // the current ImGui window. Call only when HandlesImageControl() is true.
    void RenderImageControl(const std::string& libName, const std::string& className);

    // Drops all cached editor state so the next render re-reads the settings
    // INIs from the value provider (called on menu close / RefreshMenu).
    void ResetSession();

}
