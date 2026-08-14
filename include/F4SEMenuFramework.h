#pragma once
#include "InputEventHandler.h"
#include "WindowManager.h"
#include "HudManager.h"
#include "HotkeyManager.h"
#include "imgui.h"
#include "Event.h"

#define FUNCTION_PREFIX extern "C" [[maybe_unused]] __declspec(dllexport)

FUNCTION_PREFIX void AddSectionItem(const char* path, RenderFunction rendererFunction);
FUNCTION_PREFIX WindowInterface* AddWindow(RenderFunction rendererFunction);
FUNCTION_PREFIX void CloseMenu();
FUNCTION_PREFIX void PushBig();
FUNCTION_PREFIX void PushDefault();
FUNCTION_PREFIX void PushSmall();
FUNCTION_PREFIX void PushSolid();
FUNCTION_PREFIX void PushRegular();
FUNCTION_PREFIX void PushBrands();
FUNCTION_PREFIX void Pop();
FUNCTION_PREFIX int64_t RegisterInpoutEvent(InputEventCallback callback);
FUNCTION_PREFIX void UnregisterInputEvent(uint64_t id);
FUNCTION_PREFIX int64_t RegisterHudElement(HudElementCallback callback);
FUNCTION_PREFIX void UnregisterHudElement(uint64_t id);
FUNCTION_PREFIX bool IsAnyBlockingWindowOpened();
FUNCTION_PREFIX ImTextureID LoadTexture(const char* texturePath, ImVec2* size);
FUNCTION_PREFIX void DisposeTexture(const char* texturePath);
FUNCTION_PREFIX int64_t RegisterEvent(Event::EventCallback callback);
FUNCTION_PREFIX int64_t RegisterEventPriority(Event::EventCallback callback, float priority);
FUNCTION_PREFIX void UnregisterEvent(int64_t id);
FUNCTION_PREFIX float GetMenuFrameworkVersion();
FUNCTION_PREFIX const char* GetToggleKeyName();

// --- Plugin Hotkey API ---
FUNCTION_PREFIX int64_t RegisterHotkey(const char* id, unsigned int defaultScanCode, HotkeyCallback callback);
FUNCTION_PREFIX int64_t RegisterGamepadHotkey(const char* id, unsigned int defaultConfigCode, HotkeyCallback callback);
FUNCTION_PREFIX void UnregisterHotkey(int64_t handle);
FUNCTION_PREFIX unsigned int GetHotkeyBinding(const char* id);
FUNCTION_PREFIX void SetHotkeyBinding(const char* id, unsigned int scanCode);
FUNCTION_PREFIX bool HasHotkeyConflict(unsigned int scanCode, const char* excludeId);

// Chord (Ctrl/Shift/Alt) variants — `modifiers` is a HotkeyMod bitmask; 0 is
// identical to the plain entry points above.
FUNCTION_PREFIX int64_t RegisterHotkeyWithModifiers(const char* id, unsigned int defaultScanCode, unsigned int modifiers, HotkeyCallback callback);
FUNCTION_PREFIX int64_t RegisterGamepadHotkeyWithModifiers(const char* id, unsigned int defaultConfigCode, unsigned int modifiers, HotkeyCallback callback);
FUNCTION_PREFIX unsigned int GetHotkeyModifiers(const char* id);
FUNCTION_PREFIX void SetHotkeyBindingWithModifiers(const char* id, unsigned int scanCode, unsigned int modifiers);
FUNCTION_PREFIX bool HasHotkeyConflictWithModifiers(unsigned int scanCode, unsigned int modifiers, const char* excludeId);

// --- Gamepad Query API ---
FUNCTION_PREFIX bool IsControllerConnected();

// --- Plugin Localization API ---
// Per-plugin JSON string tables loaded from
// Data/F4SE/Plugins/<pluginName>/Languages/<lang>.json. See
// include/PluginLocalization.h for the fallback semantics.
FUNCTION_PREFIX const char* GetPluginTranslation(const char* pluginName, const char* key);
FUNCTION_PREFIX int LoadPluginTranslations(const char* pluginName);
FUNCTION_PREFIX void ReloadPluginTranslations(const char* pluginName);
FUNCTION_PREFIX const char* GetGameLanguage();
