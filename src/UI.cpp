#include "UI.h"
#include "WindowManager.h"
#include <imgui.h>
#include <imgui_internal.h>  // FocusWindow / NavInitWindow for gamepad pane switching
#include "Renderer.h"
#include "Application.h"
#include "F4SEMenuFramework.h"
#include "Translations.h"
#include "GameLock.h"
#include "FontManager.h"
#include "Config.h"
#include "GamepadInput.h"
#include "GamepadGlyphs.h"
#include "MCM/MCMWidgetRenderer.h"
#include "MCM/MCMConflictCheck.h"
static ImGuiTextFilter filter;

// --- Gamepad pane focus state (main menu window) ---
// 0 = mod-list tree (left pane), 1 = settings content (right pane).
// LB/L1 focuses the list, RB/R1 focuses the content; B walks back through
// the cascade implemented in UI::HandleGamepadBack().
static int s_gamepadPane = 0;
static bool s_paneFocusRequest = false;  // consume on the frame the target child renders

UI::MenuTree* UI::RootMenu = new UI::MenuTree();


int frame = 0;

size_t item_current_idx = 0;
size_t node_id = 0;
UI::MenuTree* display_node;


static ImGuiTreeNodeFlags base_flags =
    ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
static int selection_mask = (1 << 2);

void DummyRenderer(std::pair<const std::string, UI::MenuTree*>& node) {
    ++node_id;
    for (auto& item : node.second->Children) {
        DummyRenderer(item);
    }
}

void RenderNode(std::pair<const std::string, UI::MenuTree*>& node) {
    ++node_id;
    ImGuiTreeNodeFlags node_flags = base_flags;
    // const bool is_selected = item_current_idx == i;
    if (item_current_idx == node_id) node_flags |= ImGuiTreeNodeFlags_Selected;

    if (node.second->Children.size() == 0) {
        node_flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    bool node_open = ImGui::TreeNodeEx((void*)(intptr_t)node_id, node_flags, node.first.c_str(), node_id);


    bool itemClicked = ImGui::IsItemClicked();
    bool itemToggledOpen = ImGui::IsItemToggledOpen();
    bool gamepadButtonPressed = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown);  // Typically A button
    bool itemIsFocused = ImGui::IsItemFocused();  // Check if the item is focused/highlighted by gamepad navigation

    if ((itemClicked || (gamepadButtonPressed && itemIsFocused)) && !itemToggledOpen) {
        if (node.second->Render) {
            item_current_idx = node_id;
            display_node = node.second;
        }
    }
    if (node_open && node.second->Children.size() != 0) {
        for (auto& item : node.second->SortedChildren) {
            RenderNode(item);
        }
        ImGui::TreePop();
    } else {
        for (auto& item : node.second->Children) {
            DummyRenderer(item);
        }
    }
}

void __stdcall UI::RenderMenuWindow() {
    auto viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2{0.5f, 0.5f});
    ImGui::SetNextWindowSize(ImVec2{viewport->Size.x * 0.8f, viewport->Size.y * 0.8f}, ImGuiCond_Appearing);
    ImGuiWindowFlags window_flags = 0;
    window_flags |= ImGuiWindowFlags_NoCollapse;
    window_flags |= ImGuiWindowFlags_MenuBar;
    window_flags |= ImGuiWindowFlags_NoTitleBar;

    ImGui::Begin("#MCPMainWindow", nullptr, window_flags);

    // --- Gamepad shoulder-button pane switching + Y = open settings ---
    // Only react while the menu actually has input (game paused for us) and a
    // controller is connected; key events come from GamepadInput's ImGui bridge.
    const bool gamepadActive =
        GamepadInput::IsControllerConnected() && WindowManager::ShouldTheGameBePaused();
    if (gamepadActive) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) {
            s_gamepadPane = 0;
            s_paneFocusRequest = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) {
            s_gamepadPane = 1;
            s_paneFocusRequest = true;
        }
        // Y / Triangle opens the framework settings window (matches hint bar)
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) &&
            !WindowManager::ConfigInterface->IsOpen.load()) {
            WindowManager::ConfigInterface->IsOpen = true;
        }
    }

    if (ImGui::BeginMenuBar()) {
        PushSolid();
        if (ImGui::BeginMenu(Translations::Get("Options"))) {
            if (ImGui::MenuItem(Translations::Get("Options.ResumeGame"))) {
                WindowManager::MainInterface->BlockUserInput = false;
                WindowManager::ConfigInterface->BlockUserInput = false;
            }
            if (ImGui::MenuItem(Translations::Get("Options.OpenSettings"))) {
                WindowManager::ConfigInterface->IsOpen = true;
            }
            ImGui::EndMenu();
        }
        Pop();

        float barWidth = ImGui::GetWindowWidth();
        float barHeight = ImGui::GetFrameHeight();
        float textWidth = ImGui::CalcTextSize(Translations::Get("ModControlPanel")).x;

        float closeButtonSize = barHeight;
        float padding = ImGui::GetStyle().ItemSpacing.x;

        float availableWidth = barWidth - closeButtonSize - padding;
        float pos = (availableWidth * 0.5f) - (textWidth * 0.5f);
        ImGui::SameLine(pos);
        ImGui::Text(Translations::Get("ModControlPanel"));

        float closeButtonPos = barWidth - closeButtonSize - padding;
        ImGui::SameLine(closeButtonPos);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        if (ImGui::Button("X", ImVec2(closeButtonSize, closeButtonSize))) {
            WindowManager::Close();
        }
        ImGui::PopStyleVar();

        ImGui::EndMenuBar();
    }

    float filterHeight = 50.0f;
    float headerHeight = 41.0f;
    float headerOffsetY = 5.0f;



    // Filter section
    ImGui::BeginChild("TreeView2", ImVec2(ImGui::GetContentRegionAvail().x * 0.3f, filterHeight), ImGuiChildFlags_None);
    filter.Draw("##F4SEModControlPanelMenuFilter", -FLT_MIN);
    ImGui::EndChild();

    ImGui::SameLine();

    // Header section
    ImGui::BeginChild("F4SEModControlPanelModMenuHeader", ImVec2(0, headerHeight), ImGuiChildFlags_None);
    if (display_node) {
        auto windowWidth = ImGui::GetWindowSize().x;
        auto textWidth = ImGui::CalcTextSize(display_node->Title.c_str()).x;
        float offsetX = (windowWidth - textWidth) * 0.5f;
        ImGui::SetCursorPosX(offsetX);
        ImGui::SetCursorPosY(headerOffsetY);
        ImGui::Text("%s", display_node->Title.c_str());
    }
    ImGui::EndChild();

    // Active-pane highlight: when navigating by gamepad, the focused pane gets
    // the nav-highlight color as its border so the user can see where LB/RB
    // focus currently is.
    const ImVec4 paneHighlight = ImGui::GetStyleColorVec4(ImGuiCol_NavHighlight);

    // Tree view section
    const bool highlightTree = gamepadActive && s_gamepadPane == 0;
    if (highlightTree) ImGui::PushStyleColor(ImGuiCol_Border, paneHighlight);
    ImGui::BeginChild("F4SEModControlPanelTreeView", ImVec2(ImGui::GetContentRegionAvail().x * 0.3f, -FLT_MIN),
                      ImGuiChildFlags_Border);
    if (highlightTree) ImGui::PopStyleColor();

    // Apply a pending LB pane-switch: focus this child and init nav on its
    // first item so D-pad selection starts inside the list.
    if (s_paneFocusRequest && s_gamepadPane == 0) {
        s_paneFocusRequest = false;
        ImGuiWindow* treeWin = ImGui::GetCurrentWindow();
        ImGui::FocusWindow(treeWin);
        ImGui::NavInitWindow(treeWin, true);
    }
    node_id = 0;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 5.0f));

    // Name of the translated-MCM top-level group. Pinned to the top of the tree
    // (see below) and given a distinct accent + larger header.
    static constexpr const char* kMCMGroup = "MCM Mod Settings";

    // Renders a single top-level tree group (its CollapsingHeader + children).
    auto renderTopLevelGroup = [&](const std::pair<const std::string, UI::MenuTree*>& item) {
        // Translated MCM menus get a distinct accent so users can tell them
        // apart from native F4SE pages. The accent is derived from the active
        // theme's text color (blended toward amber) so it stays readable and
        // consistent-looking regardless of which style JSON is loaded.
        const bool isMCMGroup = (item.first == kMCMGroup);
        if (isMCMGroup) {
            const ImVec4 base = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            const ImVec4 accent{
                base.x * 0.35f + 1.00f * 0.65f,
                base.y * 0.35f + 0.72f * 0.65f,
                base.z * 0.35f + 0.25f * 0.65f,
                base.w};
            ImGui::PushStyleColor(ImGuiCol_Text, accent);
            // Make this header stand out from the native pages: a taller bar
            // (extra frame padding) and larger label text (window font scale).
            // Both are reset right after the header renders so nothing else in
            // the tree is affected.
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 9.0f));
            ImGui::SetWindowFontScale(1.25f);
        }
        const bool headerOpen =
            filter.PassFilter(item.first.c_str()) &&
            ImGui::CollapsingHeader(std::format("{}##{}", item.first, node_id).c_str());
        if (isMCMGroup) {
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }

        if (headerOpen) {
            for (auto node : item.second->SortedChildren) {
                RenderNode(node);
            }
        } else {
            for (auto node : item.second->Children) {
                DummyRenderer(node);
            }
        }
    };

    // Pin the translated "MCM Mod Settings" group to the very top of the list,
    // ahead of the alphabetically-ordered native pages. RootMenu->Children is a
    // std::map (alphabetical), so render the MCM group explicitly first, then
    // everything else while skipping it.
    if (auto mcmIt = RootMenu->Children.find(kMCMGroup); mcmIt != RootMenu->Children.end()) {
        renderTopLevelGroup(*mcmIt);
    }
    for (const auto& item : RootMenu->Children) {
        if (item.first == kMCMGroup) {
            continue;
        }
        renderTopLevelGroup(item);
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();

    ImGui::SameLine();

    // Content section
    const bool highlightContent = gamepadActive && s_gamepadPane == 1;
    if (highlightContent) ImGui::PushStyleColor(ImGuiCol_Border, paneHighlight);
    ImGui::BeginChild("F4SEModControlPanelMenuNode", ImVec2(0, -FLT_MIN), ImGuiChildFlags_Border);
    if (highlightContent) ImGui::PopStyleColor();

    // Apply a pending RB pane-switch: focus the content child and init nav on
    // its first widget so D-pad selection starts on the first setting.
    if (s_paneFocusRequest && s_gamepadPane == 1) {
        s_paneFocusRequest = false;
        ImGuiWindow* contentWin = ImGui::GetCurrentWindow();
        ImGui::FocusWindow(contentWin);
        ImGui::NavInitWindow(contentWin, true);
    }

    if (display_node) {
        display_node->Render();
    }
    ImGui::EndChild();

    // Capture the main window rect for the hint bar before End()
    const ImVec2 mainPos = ImGui::GetWindowPos();
    const ImVec2 mainSize = ImGui::GetWindowSize();

    ImGui::End();

    // Control-hint bar below the main window whenever a controller is connected
    if (GamepadInput::IsControllerConnected()) {
        GamepadGlyphs::RenderHintBar(mainPos, mainSize);
    }
}

// Snapshot of "was a popup open / was a widget being edited" taken right
// before ImGui::NewFrame(). ImGui's nav-cancel handling runs INSIDE NewFrame
// and closes the popup / clears the active widget in response to B before
// HandleGamepadBack executes — checking live state there would always see
// "nothing open" and incorrectly fall through the cascade.
static bool s_hadPopupOrEditBeforeNewFrame = false;

void UI::PreNewFrameGamepadSnapshot() {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    s_hadPopupOrEditBeforeNewFrame =
        ctx && (ctx->OpenPopupStack.Size > 0 || ctx->ActiveId != 0);
}

bool UI::HandleGamepadBack() {
    // Cascade for the gamepad "back" button (B / Circle), most-specific first.

    // 1. A hotkey-rebind capture is in progress — cancel it, keep the menu.
    if (MCMWidgetRenderer::IsHotkeyCaptureActive()) {
        MCMWidgetRenderer::CancelHotkeyCapture();
        return true;
    }

    // 2./3. A popup was open or a widget was being edited when B was pressed —
    // ImGui's NavCancel already closed/cleared it inside NewFrame; the press
    // is consumed, don't also back out of the menu.
    if (s_hadPopupOrEditBeforeNewFrame) {
        return true;
    }

    // 4. The settings window is open — close just that window.
    if (WindowManager::ConfigInterface && WindowManager::ConfigInterface->IsOpen.load()) {
        WindowManager::ConfigInterface->IsOpen = false;
        return true;
    }

    // 5. Content pane is focused — step back to the mod list first.
    if (s_gamepadPane == 1) {
        s_gamepadPane = 0;
        s_paneFocusRequest = true;
        return true;
    }

    // 6. Nothing left to back out of — the caller closes the menu.
    return false;
}

void UI::AddToTree(UI::MenuTree* node, std::vector<std::string>& path, RenderFunction render, std::string title) {
    if (!path.empty()) {
        auto currentName = path.front();
        path.erase(path.begin());

        auto foundItem = node->Children.find(currentName);
        if (foundItem != node->Children.end()) {
            AddToTree(foundItem->second, path, render, title);
        } else {
            auto newItem = new UI::MenuTree();
            node->Children[currentName] = newItem;
            node->SortedChildren.push_back(std::pair<const std::string, UI::MenuTree*>(currentName, newItem));
            AddToTree(newItem, path, render, title);
        }
    } else {
        node->Render = render;
        node->Title = title;
    }
}

bool ToggleButton(const char* label, bool* v) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    float height = ImGui::GetFrameHeight();
    float width = height * 1.8f;
    float radius = height * 0.5f;

    // Use a unique ID for the button
    ImGui::PushID(label);

    // Use Selectable instead of InvisibleButton for gamepad navigation
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));

    bool clicked = ImGui::Selectable("##toggle", false, 0, ImVec2(width, height));

    ImGui::PopStyleColor(3);
    ImGui::PopID();

    if (clicked) {
        *v = !*v;
    }

    // Draw the toggle on top of the selectable
    ImVec2 p_min = ImVec2(p.x, p.y);
    ImVec2 p_max = ImVec2(p.x + width, p.y + height);

    float t = *v ? 1.0f : 0.0f;

    // Distinct colors: green when ON, dark gray when OFF
    ImU32 col_bg = *v ? IM_COL32(56, 142, 60, 255) : IM_COL32(80, 80, 80, 255);

    draw_list->AddRectFilled(p_min, p_max, col_bg, height * 0.5f);
    draw_list->AddCircleFilled(ImVec2(p.x + radius + t * (width - radius * 2.0f), p.y + radius), radius - 1.5f,
                               IM_COL32(255, 255, 255, 255));

    // Draw ON/OFF text inside the toggle track for clarity
    const char* stateText = *v ? "ON" : "OFF";
    ImVec2 textSize = ImGui::CalcTextSize(stateText);
    float textX;
    if (*v) {
        // "ON" text on the left side (where the knob came from)
        textX = p.x + radius * 0.4f;
    } else {
        // "OFF" text on the right side (where the knob would go)
        textX = p.x + width - textSize.x - radius * 0.4f;
    }
    float textY = p.y + (height - textSize.y) * 0.5f;
    draw_list->AddText(ImVec2(textX, textY), IM_COL32(255, 255, 255, 200), stateText);

    ImGui::SameLine();
    ImGui::Text("%s", label);

    return clicked;
}

void UI::RenderConfigWindow() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2{0.5f, 0.5f});
    // 0.46 = the original 0.4 default enlarged by 15%
    ImGui::SetNextWindowSize(ImVec2{viewport->Size.x * 0.46f, viewport->Size.y * 0.46f}, ImGuiCond_Appearing);
    ImGuiWindowFlags window_flags = 0;
    window_flags |= ImGuiWindowFlags_NoCollapse;
    window_flags |= ImGuiWindowFlags_MenuBar;
    window_flags |= ImGuiWindowFlags_NoTitleBar;

    if (ImGui::Begin("Settings##Window", nullptr,
                     window_flags)) {
        if (ImGui::BeginMenuBar()) {
            ImGui::Text(Translations::Get("Settings.Title"));
            float barWidth = ImGui::GetWindowWidth();
            float barHeight = ImGui::GetFrameHeight();
            float textWidth = ImGui::CalcTextSize(Translations::Get("Settings.Title")).x;

            float closeButtonSize = barHeight;
            float padding = ImGui::GetStyle().ItemSpacing.x;

            float closeButtonPos = barWidth - closeButtonSize - padding;
            ImGui::SameLine(closeButtonPos);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            if (ImGui::Button("X", ImVec2(closeButtonSize, closeButtonSize))) {
                WindowManager::ConfigInterface->IsOpen = false;
            }
            ImGui::PopStyleVar();
            ImGui::EndMenuBar();
        }

        float windowWidth = ImGui::GetContentRegionAvail().x;
        float contentWidth = windowWidth * 0.8f;
        float offsetX = (windowWidth - contentWidth) * 0.5f;

        if (offsetX > 0) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
        }

        ImGui::BeginGroup();
        ImGui::PushItemWidth(contentWidth);

        // ... all your combo boxes and settings ...

        std::vector<const char*> styleNames;
        styleNames.reserve(Config::MenuStyles.size());

        for (const auto& s : Config::MenuStyles) {
            styleNames.push_back(s.c_str());
        }

        ImGui::Text(Translations::Get("Settings.MenuStyle"));
        if (ImGui::Combo("##MenuStyleCombo", &Config::MenuStyle, styleNames.data(), styleNames.size())) {
            Config::LoadStyle();
            Config::Save();
        }

        // --- Font selection dropdown ---
        {
            static std::vector<std::string> availableFonts = FontManager::GetAvailableFonts();
            static int selectedFontIdx = -1;

            // Resolve current selection from Config::PrimaryFont on first use.
            if (selectedFontIdx < 0) {
                for (int i = 0; i < static_cast<int>(availableFonts.size()); ++i) {
                    if (availableFonts[i] == Config::PrimaryFont) {
                        selectedFontIdx = i;
                        break;
                    }
                }
                if (selectedFontIdx < 0) selectedFontIdx = 0;
            }

            if (!availableFonts.empty()) {
                std::vector<const char*> fontLabels;
                fontLabels.reserve(availableFonts.size());
                for (const auto& f : availableFonts) {
                    fontLabels.push_back(f.c_str());
                }

                ImGui::Text("Font");
                if (ImGui::Combo("##FontCombo", &selectedFontIdx, fontLabels.data(),
                                 static_cast<int>(fontLabels.size()))) {
                    Config::PrimaryFont = availableFonts[selectedFontIdx];
                    Config::Save();
                    FontManager::RequestReload();
                }
            }
        }

        if (ToggleButton(Translations::Get("Settings.FreezeTime"), &Config::FreezeTimeOnMenu)) {
            Config::Save();

            // If we're currently in Locked state, apply the change immediately
            // so the user sees the effect without closing and reopening the menu.
            if (GameLock::lastState == GameLock::State::Locked) {
                GameLock::SetGamePaused(Config::FreezeTimeOnMenu);
            }
        }

        if (ToggleButton(Translations::Get("Settings.BlurBackground"), &Config::BlurBackgroundOnMenu)) {
            Config::Save();

            // If we're currently in Locked state, apply the change immediately.
            if (GameLock::lastState == GameLock::State::Locked) {
                GameLock::SetBackgroundBlur(Config::BlurBackgroundOnMenu);
            }
        }

        const char* togleModeNames[] = {"SINGLEPRESS", "HOLD", "DOUBLEPRESS", "OFF"};
        int currentTogleMode = static_cast<int>(Config::ToggleMode);
        ImGui::Text(Translations::Get("Settings.ToggleMode.Keyboard"));
        if (ImGui::Combo("##ToggleModeCombo", &currentTogleMode, togleModeNames, IM_ARRAYSIZE(togleModeNames))) {
            Config::ToggleMode = currentTogleMode;
            Config::Save();
        }

        ImGui::Separator();

        ImGui::Text(Translations::Get("Settings.ToggleKey.Keyboard"));
        std::string currentKeyName = GetKeyName(Config::ToggleKey, RE::INPUT_DEVICE::kKeyboard);
        if (ImGui::BeginCombo("##ToggleKeyKeyboard", currentKeyName.c_str())) {
            const std::vector<std::pair<std::string, int>> keyboardKeys = {
                {"NONE", 0x00},       {"ESCAPE", 0x01},      {"1", 0x02},           {"2", 0x03},
                {"3", 0x04},          {"4", 0x05},           {"5", 0x06},           {"6", 0x07},
                {"7", 0x08},          {"8", 0x09},           {"9", 0x0A},           {"0", 0x0B},
                {"MINUS", 0x0C},      {"EQUALS", 0x0D},      {"BACKSPACE", 0x0E},   {"TAB", 0x0F},
                {"Q", 0x10},          {"W", 0x11},           {"E", 0x12},           {"R", 0x13},
                {"T", 0x14},          {"Y", 0x15},           {"U", 0x16},           {"I", 0x17},
                {"O", 0x18},          {"P", 0x19},           {"BRACKETLEFT", 0x1A}, {"BRACKETRIGHT", 0x1B},
                {"ENTER", 0x1C},      {"LEFTCONTROL", 0x1D}, {"A", 0x1E},           {"S", 0x1F},
                {"D", 0x20},          {"F", 0x21},           {"G", 0x22},           {"H", 0x23},
                {"J", 0x24},          {"K", 0x25},           {"L", 0x26},           {"SEMICOLON", 0x27},
                {"APOSTROPHE", 0x28}, {"TILDE", 0x29},       {"LEFTSHIFT", 0x2A},   {"BACKSLASH", 0x2B},
                {"Z", 0x2C},          {"X", 0x2D},           {"C", 0x2E},           {"V", 0x2F},
                {"B", 0x30},          {"N", 0x31},           {"M", 0x32},           {"COMMA", 0x33},
                {"PERIOD", 0x34},     {"SLASH", 0x35},       {"RIGHTSHIFT", 0x36},  {"KP_MULTIPLY", 0x37},
                {"LEFTALT", 0x38},    {"SPACEBAR", 0x39},    {"CAPSLOCK", 0x3A},    {"F1", 0x3B},
                {"F2", 0x3C},         {"F3", 0x3D},          {"F4", 0x3E},          {"F5", 0x3F},
                {"F6", 0x40},         {"F7", 0x41},          {"F8", 0x42},          {"F9", 0x43},
                {"F10", 0x44},        {"NUMLOCK", 0x45},     {"SCROLLLOCK", 0x46},  {"KP_7", 0x47},
                {"KP_8", 0x48},       {"KP_9", 0x49},        {"KP_SUBTRACT", 0x4A}, {"KP_4", 0x4B},
                {"KP_5", 0x4C},       {"KP_6", 0x4D},        {"KP_PLUS", 0x4E},     {"KP_1", 0x4F},
                {"KP_2", 0x50},       {"KP_3", 0x51},        {"KP_0", 0x52},        {"KP_DECIMAL", 0x53},
                {"F11", 0x57},        {"F12", 0x58},         {"KP_ENTER", 0x9C},    {"RIGHTCONTROL", 0x9D},
                {"KP_DIVIDE", 0xB5},  {"PRINTSCREEN", 0xB7}, {"RIGHTALT", 0xB8},    {"PAUSE", 0xC5},
                {"HOME", 0xC7},       {"UP", 0xC8},          {"PAGEUP", 0xC9},      {"LEFT", 0xCB},
                {"RIGHT", 0xCD},      {"END", 0xCF},         {"DOWN", 0xD0},        {"PAGEDOWN", 0xD1},
                {"INSERT", 0xD2},     {"DELETE", 0xD3},      {"LEFTWIN", 0xDB},     {"RIGHTWIN", 0xDC}};

            for (const auto& [name, code] : keyboardKeys) {
                bool isSelected = (Config::ToggleKey == code);
                if (ImGui::Selectable(name.c_str(), isSelected)) {
                    Config::ToggleKey = code;
                    Config::Save();
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Separator();

        int currentTogleModeGamepad = static_cast<int>(Config::ToggleModeGamePad);
        ImGui::Text(Translations::Get("Settings.ToggleMode.Gamepad"));
        if (ImGui::Combo("##ToggleModeComboGAmepad", &currentTogleModeGamepad, togleModeNames,
                         IM_ARRAYSIZE(togleModeNames))) {
            Config::ToggleModeGamePad = currentTogleModeGamepad;
            Config::Save();
        }

        ImGui::Text(Translations::Get("Settings.ToggleKey.Gamepad"));
        std::string currentButtonName = GetKeyName(Config::ToggleKeyGamePad, RE::INPUT_DEVICE::kGamepad);
        if (ImGui::BeginCombo("##ToggleKeyGamepad", currentButtonName.c_str())) {
            const std::vector<std::pair<std::string, int>> gamepadButtons = {
                {"NONE", 0},  {"DPAD_UP", 1}, {"DPAD_DOWN", 2}, {"DPAD_LEFT", 4}, {"DPAD_RIGHT", 8}, {"START", 16},
                {"BACK", 32}, {"LS", 64},     {"RS", 128},      {"LB", 256},      {"RB", 512},       {"LT", 9},
                {"RT", 10},   {"A", 4096},    {"B", 8192},      {"X", 16384},     {"Y", 32768}};

            for (const auto& [name, code] : gamepadButtons) {
                bool isSelected = (Config::ToggleKeyGamePad == code);
                if (ImGui::Selectable(name.c_str(), isSelected)) {
                    Config::ToggleKeyGamePad = code;
                    Config::Save();
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::PopItemWidth();  // ADDED: Pop the item width

        // --- MCM Compatibility Section ---
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Text("MCM Compatibility");
        ImGui::Spacing();

        // Deferred one frame so OpenPopup runs in the same ID scope every time.
        static bool s_mcmRestartPopupPending = false;

        if (ToggleButton("Enable MCM Compatibility Layer", &Config::MCMCompatEnabled)) {
            Config::Save();
            // Warn that the layer only (re)initializes during game startup.
            if (Config::MCMCompatEnabled) {
                s_mcmRestartPopupPending = true;
            }
        }
        bool mcmToggleHovered = ImGui::IsItemHovered();
        // Bold amber "[BETA]" tag after the toggle label. ImGui has no bold
        // font variant loaded, so fake the weight by overdrawing the text
        // with a sub-pixel horizontal offset.
        ImGui::SameLine();
        {
            const ImVec4 betaColor{1.0f, 0.72f, 0.25f, 1.0f};
            const ImVec2 betaPos = ImGui::GetCursorScreenPos();
            ImGui::TextColored(betaColor, "[BETA]");
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(betaPos.x + 0.7f, betaPos.y), ImGui::GetColorU32(betaColor), "[BETA]");
        }
        mcmToggleHovered |= ImGui::IsItemHovered();
        if (mcmToggleHovered) {
            ImGui::SetTooltip("Translates installed MCM mod configs into F4SE Menu pages.\nRequires game restart to take effect when toggled.");
        }

        if (s_mcmRestartPopupPending) {
            ImGui::OpenPopup("Restart Required##MCMCompat");
            s_mcmRestartPopupPending = false;
        }
        // Center the modal over the viewport
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2{0.5f, 0.5f});
        if (ImGui::BeginPopupModal("Restart Required##MCMCompat", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextWrapped("The MCM compatibility layer scans and translates MCM mod configs during game startup.");
            ImGui::Spacing();
            ImGui::TextWrapped("Restart the game for the MCM Mod Settings pages to appear.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            float btnWidth = 120.0f;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnWidth) * 0.5f);
            if (ImGui::Button("OK", ImVec2(btnWidth, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Only meaningful when the real MCM plugin is loaded alongside us —
        // by default our translation layer silently steps aside in that case.
        if (MCMConflictCheck::IsNativeMCMPresent()) {
            if (ToggleButton("Load MCM Mod Settings Even With MCM Installed", &Config::MCMCompatWhenNativePresent)) {
                Config::Save();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("The original MCM plugin is installed. By default this framework\n"
                                  "hides its own MCM Mod Settings pages to avoid conflicts. Enable to\n"
                                  "show them anyway (both write the same setting files).\n"
                                  "Requires game restart to take effect.");
            }
        }

        // --- Gamepad Section ---
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Text("Gamepad");
        ImGui::Spacing();

        ImGui::PushItemWidth(contentWidth);
        const char* glyphStyleNames[] = { "Xbox", "PlayStation" };
        ImGui::Text("Button Glyph Style");
        if (ImGui::Combo("##GamepadGlyphStyleCombo", &Config::GamepadGlyphStyle,
                         glyphStyleNames, IM_ARRAYSIZE(glyphStyleNames))) {
            Config::Save();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Which controller button art the hint bar uses.");
        }
        ImGui::PopItemWidth();

        ImGui::EndGroup();      // ADDED: End the group
    }
    ImGui::End();  // MOVED: Always call End() after Begin()
}
