#include "F4SEMenuFramework.h"
#include "FontManager.h"
#include <imgui.h>
#include "Application.h"
#include "Config.h"
#include "Renderer.h"
#include "UI.h"
#include "TextureLoader.h"
#include "GamepadInput.h"
#include "PluginLocalization.h"
#include "AutoTranslate.h"

#define MENU_FRAMEWORK_VERSION 3.9f

void AddSectionItem(const char* path, RenderFunction rendererFunction) { 
    auto pathSplit = SplitString(path, '/');
    AddToTree(UI::RootMenu, pathSplit, rendererFunction, pathSplit.back());
    // The first path segment is the plugin's SetSection() name; record it as
    // the module's translation folder for automatic backend translation.
    AutoTranslate::NoteSectionPath(reinterpret_cast<void*>(rendererFunction), path);
}

WindowInterface* AddWindow(RenderFunction rendererFunction) { 

    auto newWindow = new Window();

    newWindow->Render = rendererFunction;

    WindowManager::Windows.push_back(newWindow);

    return newWindow->Interface;

}

void CloseMenu()
{
    // Reuse the framework's normal close transaction. This closes the main
    // panel and blocking plugin windows, dispatches kCloseMenu, and flushes
    // localization capture exactly as the framework hotkey and close button do.
    WindowManager::Close();
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

int64_t RegisterGamepadHotkey(const char* id, unsigned int defaultConfigCode, HotkeyCallback callback) {
    return HotkeyManager::RegisterGamepad(id, defaultConfigCode, callback);
}

// --- Chord (Ctrl/Shift/Alt) variants ---
// modifiers is a HotkeyMod bitmask (Ctrl=1, Shift=2, Alt=4); 0 == the plain
// entry points above. Kept as separate exports (not overloads) so the existing
// exported symbols are untouched and old plugins keep resolving them.

int64_t RegisterHotkeyWithModifiers(const char* id, unsigned int defaultScanCode, unsigned int modifiers, HotkeyCallback callback) {
    return HotkeyManager::RegisterWithModifiers(id, defaultScanCode, static_cast<uint8_t>(modifiers), callback);
}

int64_t RegisterGamepadHotkeyWithModifiers(const char* id, unsigned int defaultConfigCode, unsigned int modifiers, HotkeyCallback callback) {
    return HotkeyManager::RegisterGamepadWithModifiers(id, defaultConfigCode, static_cast<uint8_t>(modifiers), callback);
}

unsigned int GetHotkeyModifiers(const char* id) {
    return HotkeyManager::GetModifiers(id);
}

void SetHotkeyBindingWithModifiers(const char* id, unsigned int scanCode, unsigned int modifiers) {
    HotkeyManager::SetBindingWithModifiers(id, scanCode, static_cast<uint8_t>(modifiers));
}

bool HasHotkeyConflictWithModifiers(unsigned int scanCode, unsigned int modifiers, const char* excludeId) {
    return !HotkeyManager::GetConflicts(scanCode, static_cast<uint8_t>(modifiers), excludeId).empty();
}

// --- Gamepad Query API ---

bool IsControllerConnected() {
    return GamepadInput::IsControllerConnected();
}

// --- Plugin Localization API ---

const char* GetPluginTranslation(const char* pluginName, const char* key) {
    if (!key) return "";
    if (!pluginName || !*pluginName) return key;
    return PluginLocalization::Get(pluginName, key);
}

int LoadPluginTranslations(const char* pluginName) {
    if (!pluginName || !*pluginName) return -1;
    const int count = PluginLocalization::Load(pluginName);
    if (count >= 0) {
        logger::info("PluginLocalization: '{}' loaded {} key(s) (language '{}')",
                     pluginName, count, PluginLocalization::GetLanguage());
    } else {
        // Informational: plugins using English text as keys ship no files.
        logger::info("PluginLocalization: '{}' has no Languages files; keys pass through as-is",
                     pluginName);
    }
    return count;
}

void ReloadPluginTranslations(const char* pluginName) {
    if (!pluginName || !*pluginName) return;
    PluginLocalization::Reset(pluginName);
    logger::info("PluginLocalization: '{}' tables dropped; next lookup re-reads from disk",
                 pluginName);
}

const char* GetGameLanguage() {
    return PluginLocalization::GetLanguage().c_str();
}
