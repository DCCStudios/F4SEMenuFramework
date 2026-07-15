#pragma once

#include "WindowManager.h"

namespace UI {
    class MenuTree {
    public:
        std::map<std::string, MenuTree*> Children;
        std::vector<std::pair<const std::string, MenuTree*>> SortedChildren;
        RenderFunction Render;
        std::string Title;
    };
    extern UI::MenuTree* RootMenu;
    void __stdcall RenderMenuWindow();
    void AddToTree(UI::MenuTree* node, std::vector<std::string>& path, RenderFunction render, std::string title);
    void __stdcall RenderConfigWindow();

    // --- Gamepad navigation (main menu window) ---
    // Must be called right BEFORE ImGui::NewFrame() each frame: records
    // whether a popup was open or a widget was being edited. ImGui's own
    // nav-cancel handling (inside NewFrame) consumes the B press to close
    // popups / stop editing, destroying the evidence before HandleGamepadBack
    // runs — this snapshot preserves it so one B press doesn't fall through
    // and close the whole menu.
    void PreNewFrameGamepadSnapshot();

    // Handles one press of the gamepad "back" button (B / Circle) using a
    // cascade: cancel hotkey capture -> let ImGui close popups / cancel edits
    // -> close the settings window -> move focus from the content pane back
    // to the mod list -> finally close the menu. Returns true if the press
    // was consumed (menu must stay open); false means the caller should close
    // the window.
    bool HandleGamepadBack();
}