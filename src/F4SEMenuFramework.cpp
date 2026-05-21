#include "F4SEMenuFramework.h"
#include "FontManager.h"
#include <imgui.h>
#include "Application.h"
#include "Config.h"
#include "Renderer.h"
#include "UI.h"
#include "TextureLoader.h"

#define MENU_FRAMEWORK_VERSION 3.7f

void AddSectionItem(const char* path, RenderFunction rendererFunction) { 
    auto pathSplit = SplitString(path, '/');
    AddToTree(UI::RootMenu, pathSplit, rendererFunction, pathSplit.back());
}

WindowInterface* AddWindow(RenderFunction rendererFunction) { 

    auto newWindow = new Window();

    newWindow->Render = rendererFunction;

    WindowManager::Windows.push_back(newWindow);

    return newWindow->Interface;

}

void PushDefault() 
{
    FontManager::currentFont = (Font)(FontManager::currentFont & ~Font::fontSizeBig);
    FontManager::currentFont = (Font)(FontManager::currentFont & ~Font::fontSizeSmall);
    FontManager::currentFont = (Font)(FontManager::currentFont | Font::fontSizeDefault);
    FontManager::ProcessFont();
}
void PushBig() 
{
    FontManager::currentFont = (Font)(FontManager::currentFont & ~Font::fontSizeDefault);
    FontManager::currentFont = (Font)(FontManager::currentFont & ~Font::fontSizeSmall);
    FontManager::currentFont = (Font)(FontManager::currentFont | Font::fontSizeBig);
    FontManager::ProcessFont();
}
void PushSmall() 
{
    FontManager::currentFont = (Font)(FontManager::currentFont & ~Font::fontSizeDefault);
    FontManager::currentFont = (Font)(FontManager::currentFont & ~Font::fontSizeBig);
    FontManager::currentFont = (Font)(FontManager::currentFont | Font::fontSizeSmall);
    FontManager::ProcessFont();
}

void PushSolid() 
{
    FontManager::currentFont = (Font)(FontManager::currentFont & ~Font::faBrands);
    FontManager::currentFont = (Font)(FontManager::currentFont & ~Font::faRegular);
    FontManager::currentFont = (Font)(FontManager::currentFont | Font::faSolid);
    FontManager::ProcessFont();
}

void PushRegular() 
{
    FontManager::currentFont = (Font)(FontManager::currentFont & ~Font::faSolid);
    FontManager::currentFont = (Font)(FontManager::currentFont & ~Font::faBrands);
    FontManager::currentFont = (Font)(FontManager::currentFont | Font::faRegular);
    FontManager::ProcessFont();
}

void PushBrands() 
{
    FontManager::currentFont = (Font)(FontManager::currentFont & ~Font::faSolid);
    FontManager::currentFont = (Font)(FontManager::currentFont & ~Font::faRegular);
    FontManager::currentFont = (Font)(FontManager::currentFont | Font::faBrands);
    FontManager::ProcessFont();
}

void Pop() { FontManager::CleanFont(); }

int64_t RegisterInpoutEvent(InputEventCallback callback) { return InputEventHandler::Register(callback); }

void UnregisterInputEvent(uint64_t id) { InputEventHandler::Unregister(id); }

int64_t RegisterHudElement(HudElementCallback callback) { return HudManager::Register(callback); }

void UnregisterHudElement(uint64_t id) { HudManager::Unregister(id); }

bool IsAnyBlockingWindowOpened() { return WindowManager::ShouldTheGameBePaused(); }

const char* GetToggleKeyName() {
    static std::string keyName;
    keyName = GetKeyName(static_cast<int>(Config::ToggleKey), RE::INPUT_DEVICE::kKeyboard);
    return keyName.c_str();
}

ImTextureID LoadTexture(const char* texturePath, ImVec2* size) {
    return TextureLoader::GetTexture(texturePath, size ? *size : ImVec2{0,0});
}

void DisposeTexture(const char* texturePath) { 
    TextureLoader::DisposeTexture(texturePath);
}

int64_t RegisterEvent(Event::EventCallback callback) { return Event::AddEventListener(callback, 0); }

int64_t RegisterEventPriority(Event::EventCallback callback, float priority) {
    return Event::AddEventListener(callback, priority);
}

void UnregisterEvent(int64_t id) { Event::RemoveEventListener(id); }

float GetMenuFrameworkVersion() { return MENU_FRAMEWORK_VERSION; }

// --- Plugin Hotkey API ---

int64_t RegisterHotkey(const char* id, unsigned int defaultScanCode, HotkeyCallback callback) {
    return HotkeyManager::Register(id, defaultScanCode, callback);
}

void UnregisterHotkey(int64_t handle) {
    HotkeyManager::Unregister(handle);
}

unsigned int GetHotkeyBinding(const char* id) {
    return HotkeyManager::GetBinding(id);
}

void SetHotkeyBinding(const char* id, unsigned int scanCode) {
    HotkeyManager::SetBinding(id, scanCode);
}

bool HasHotkeyConflict(unsigned int scanCode, const char* excludeId) {
    return !HotkeyManager::GetConflicts(scanCode, excludeId).empty();
}
